#include "platform.hpp"

// General
// Windows SDK
#ifndef NOMINMAX
#define NOMINMAX
#define WIRESTEAM_PLATFORM_UNDEFINE_NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winsvc.h>
#ifdef WIRESTEAM_PLATFORM_UNDEFINE_NOMINMAX
#undef NOMINMAX
#undef WIRESTEAM_PLATFORM_UNDEFINE_NOMINMAX
#endif

// C++
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <utility>

// STL
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// Project
#include "config.hpp"
#include "internet_legacy_routes.hpp"
#include "internet_server.hpp"
#include "internet_worker.hpp"
#include "service_diagnostics.hpp"

namespace ws {
	namespace {
		constexpr DWORD kMaximumConfigurationBytes = 1024 * 1024;

		struct Handle {
			Handle() {
				m_hHandle = INVALID_HANDLE_VALUE;
			}

			explicit Handle(HANDLE hHandle) :
				m_hHandle(hHandle)
			{
			}

			~Handle() {
				Reset();
			}

			Handle(Handle&& Other) noexcept :
				m_hHandle(Other.Release())
			{
			}

			Handle(Handle const&) = delete;
			Handle& operator=(Handle const&) = delete;

			HANDLE Release() noexcept {
				HANDLE const hHandle = m_hHandle;
				m_hHandle = INVALID_HANDLE_VALUE;

				return hHandle;
			}

			void Reset(HANDLE hHandle = INVALID_HANDLE_VALUE) {
				if ((m_hHandle != INVALID_HANDLE_VALUE) && m_hHandle) {
					CloseHandle(m_hHandle);
				}

				m_hHandle = hHandle;
			}

			explicit operator bool() const {
				return (m_hHandle != INVALID_HANDLE_VALUE) && m_hHandle;
			}

			HANDLE m_hHandle;
		};

		struct ServiceHandle {
			explicit ServiceHandle(SC_HANDLE hHandle) :
				m_hHandle(hHandle)
			{
			}

			~ServiceHandle() {
				if (m_hHandle) {
					CloseServiceHandle(m_hHandle);
				}
			}

			ServiceHandle(ServiceHandle const&) = delete;
			ServiceHandle& operator=(ServiceHandle const&) = delete;

			SC_HANDLE m_hHandle;
		};

		struct CoTaskMemoryDeleter {
			void operator()(void* pMemory) const noexcept {
				CoTaskMemFree(pMemory);
			}
		};

		struct LocalMemoryDeleter {
			void operator()(void* pMemory) const noexcept {
				LocalFree(pMemory);
			}
		};

		struct COMScope {
			explicit COMScope(HRESULT nResult) :
				m_nResult(nResult)
			{
			}

			~COMScope() {
				if (SUCCEEDED(m_nResult)) {
					CoUninitialize();
				}
			}

			COMScope(COMScope const&) = delete;
			COMScope& operator=(COMScope const&) = delete;

			HRESULT m_nResult;
		};

		struct KeyEraseScope {
			explicit KeyEraseScope(Key& arrKey) :
				m_arrKey(arrKey)
			{
			}

			~KeyEraseScope() {
				SecureZeroMemory(m_arrKey.data(), m_arrKey.size());
			}

			KeyEraseScope(KeyEraseScope const&) = delete;
			KeyEraseScope& operator=(KeyEraseScope const&) = delete;

			Key& m_arrKey;
		};

		struct StringEraseScope {
			explicit StringEraseScope(std::string& strValue) :
				m_strValue(strValue)
			{
			}

			~StringEraseScope() {
				SecureZeroMemory(m_strValue.data(), m_strValue.size());
			}

			StringEraseScope(StringEraseScope const&) = delete;
			StringEraseScope& operator=(StringEraseScope const&) = delete;

			std::string& m_strValue;
		};

		struct MemoryEraseScope {
			MemoryEraseScope(void* pMemory, std::size_t unBytes) :
				m_pMemory(pMemory),
				m_unBytes(unBytes)
			{
			}

			~MemoryEraseScope() noexcept {
				SecureZeroMemory(m_pMemory, m_unBytes);
			}

			MemoryEraseScope(MemoryEraseScope const&) = delete;
			MemoryEraseScope& operator=(MemoryEraseScope const&) = delete;

			void* m_pMemory;
			std::size_t m_unBytes;
		};

		// Activate only after CREATE_NEW succeeds; Path and File outlive this scope.
		struct CreatedFileScope {
			CreatedFileScope(std::filesystem::path const& Path, Handle& File) :
				m_Path(Path),
				m_File(File)
			{
				m_bArmed = true;
			}

			~CreatedFileScope() {
				Rollback();
			}

			CreatedFileScope(CreatedFileScope const&) = delete;
			CreatedFileScope& operator=(CreatedFileScope const&) = delete;

			void Commit() noexcept {
				m_bArmed = false;
			}

			void Rollback() noexcept {
				if (!m_bArmed) {
					return;
				}

				m_bArmed = false;
				m_File.Reset();
				// Best effort after failure; retain the operation's original diagnostic.
				DeleteFileW(m_Path.c_str());
			}

			std::filesystem::path const& m_Path;
			Handle& m_File;
			bool m_bArmed;
		};

		struct ProcessAttributeListScope {
			explicit ProcessAttributeListScope(LPPROC_THREAD_ATTRIBUTE_LIST pAttributeList) :
				m_pAttributeList(pAttributeList)
			{
			}

			~ProcessAttributeListScope() {
				Reset();
			}

			ProcessAttributeListScope(ProcessAttributeListScope const&) = delete;
			ProcessAttributeListScope& operator=(ProcessAttributeListScope const&) = delete;

			void Reset() noexcept {
				if (m_pAttributeList) {
					DeleteProcThreadAttributeList(m_pAttributeList);
					m_pAttributeList = nullptr;
				}
			}

			LPPROC_THREAD_ATTRIBUTE_LIST m_pAttributeList;
		};

		// Process/Thread handles and input storage outlive this cleanup scope.
		struct ProcessExecutionScope {
			ProcessExecutionScope(HANDLE hProcess, std::thread& Thread, std::atomic_bool& bStopWriter, bool const& bFinished) :
				m_hProcess(hProcess),
				m_Thread(Thread),
				m_bStopWriter(bStopWriter),
				m_bFinished(bFinished)
			{
				m_bClosed = false;
			}

			~ProcessExecutionScope() {
				Close();
			}

			ProcessExecutionScope(ProcessExecutionScope const&) = delete;
			ProcessExecutionScope& operator=(ProcessExecutionScope const&) = delete;

			void Close() noexcept {
				constexpr DWORD kChildTerminationWaitMilliseconds = 5000;
				constexpr ULONGLONG kWriterCleanupTimeoutMilliseconds = 5000;
				constexpr DWORD kCancellationRetryMilliseconds = 5;

				if (m_bClosed) {
					return;
				}

				m_bClosed = true;

				if (!m_bFinished) {
					// Termination remains best effort; cancellation below also releases the writer.
					TerminateProcess(m_hProcess, ERROR_TIMEOUT);

					if (WaitForSingleObject(m_hProcess, kChildTerminationWaitMilliseconds) != WAIT_OBJECT_0) {
						OutputDebugStringA("WireSteam process cleanup could not confirm child termination.\n");
					}
				}

				m_bStopWriter.store(true);

				if (!m_Thread.joinable()) {
					return;
				}

				const ULONGLONG unCleanupStartTicks = GetTickCount64();
				bool bCancellationErrorReported = false;
				while (true) {
					if (!CancelSynchronousIo(m_Thread.native_handle())) {
						const DWORD unErrorCode = GetLastError();
						if ((unErrorCode != ERROR_NOT_FOUND) && !bCancellationErrorReported) {
							OutputDebugStringA("WireSteam process writer cancellation failed.\n");
							bCancellationErrorReported = true;
						}
					}

					const DWORD unWaitResult = WaitForSingleObject(m_Thread.native_handle(), kCancellationRetryMilliseconds);
					if (unWaitResult == WAIT_OBJECT_0) {
						break;
					}

					if ((unWaitResult != WAIT_TIMEOUT) || ((GetTickCount64() - unCleanupStartTicks) >= kWriterCleanupTimeoutMilliseconds)) {
						OutputDebugStringA("WireSteam process writer could not stop within its cleanup bound.\n");
						std::terminate();
					}

					// Cancellation may race the writer entering WriteFile after its stop check.
					// Retry against the owned thread handle; never detach borrowed input storage.
				}

				try {
					// The native thread is signaled, so this join cannot wait for further I/O.
					m_Thread.join();
				} catch (...) {
					OutputDebugStringA("WireSteam process writer join failed.\n");
					std::terminate();
				}
			}

			HANDLE m_hProcess;
			std::thread& m_Thread;
			std::atomic_bool& m_bStopWriter;
			bool const& m_bFinished;
			bool m_bClosed;
		};

		std::string FormatWindowsError(char const* pszOperation, DWORD unErrorCode = GetLastError()) {
			return std::string(pszOperation) + " failed (Windows error " + std::to_string(unErrorCode) + ")";
		}

		std::wstring ConvertToWideName(std::string const& strValue) {
			return { strValue.begin(), strValue.end() };
		}

		bool IsValidServiceName(std::string const& strName) {
			constexpr char kServiceNamePrefix[] = "WireSteam-";
			constexpr std::size_t kPrefixBytes = sizeof(kServiceNamePrefix) - 1;
			constexpr std::size_t kMaximumServiceNameBytes = 30;

			if ((strName.rfind(kServiceNamePrefix, 0) != 0) || (strName.size() <= kPrefixBytes) || (strName.size() > kMaximumServiceNameBytes)) {
				return false;
			}

			return std::all_of(strName.begin() + kPrefixBytes, strName.end(), [](unsigned char chCharacter) -> bool {
				return ((chCharacter >= 'a') && (chCharacter <= 'z')) || ((chCharacter >= 'A') && (chCharacter <= 'Z')) || ((chCharacter >= '0') && (chCharacter <= '9')) || (chCharacter == '_') || (chCharacter == '-');
			});
		}

		std::filesystem::path FindProgramFilesWireGuard(wchar_t const* pszFileName) {
			PWSTR pszKnownFolder = nullptr;
			const HRESULT nResult = SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &pszKnownFolder);
			std::unique_ptr<wchar_t, CoTaskMemoryDeleter> pKnownFolder(pszKnownFolder);
			if (FAILED(nResult)) {
				return {};
			}

			std::filesystem::path Result = std::filesystem::path(pKnownFolder.get()) / L"WireGuard" / pszFileName;
			pKnownFolder.reset();

			const DWORD unAttributes = GetFileAttributesW(Result.c_str());
			if ((unAttributes == INVALID_FILE_ATTRIBUTES) || (unAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
				return {};
			}

			return Result;
		}

		std::filesystem::path ResolveModulePath() {
			constexpr std::size_t kModulePathCharacters = 32768;

			std::wstring wstrBuffer = std::wstring(kModulePathCharacters, L'\0');
			const DWORD unCharacters = GetModuleFileNameW(nullptr, wstrBuffer.data(), static_cast<DWORD>(wstrBuffer.size()));
			if ((unCharacters == 0) || (unCharacters >= wstrBuffer.size())) {
				throw std::runtime_error("Cannot determine executable path");
			}

			wstrBuffer.resize(unCharacters);

			return wstrBuffer;
		}

		bool ValidateConfigurationParents(std::filesystem::path const& Path, std::string& strErrorOut) {
			std::filesystem::path CurrentPath = Path.parent_path();
			while (!CurrentPath.empty()) {
				const DWORD unAttributes = GetFileAttributesW(CurrentPath.c_str());
				if ((unAttributes == INVALID_FILE_ATTRIBUTES) || !(unAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
					strErrorOut = "The configuration parent directory must already exist";
					return false;
				}

				if (unAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
					strErrorOut = "Configuration paths cannot contain reparse points";
					return false;
				}

				std::filesystem::path const ParentPath = CurrentPath.parent_path();
				if (ParentPath == CurrentPath) {
					break;
				}

				CurrentPath = ParentPath;
			}

			return true;
		}

		// A successful caller owns pDescriptorOut and must release it with LocalFree.
		bool CreatePrivateSecurityDescriptor(PSECURITY_DESCRIPTOR& pDescriptorOut, std::string& strErrorOut) {
			Handle Token;
			HANDLE hToken = nullptr;
			if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
				strErrorOut = FormatWindowsError("OpenProcessToken");
				return false;
			}

			Token.Reset(hToken);
			DWORD unBytes = 0;
			// The size probe normally reports insufficient storage; reject zero before allocation.
			GetTokenInformation(Token.m_hHandle, TokenUser, nullptr, 0, &unBytes);
			const DWORD unProbeError = GetLastError();
			if (unBytes == 0) {
				strErrorOut = FormatWindowsError("GetTokenInformation", unProbeError);
				return false;
			}

			std::vector<unsigned char> vecData(unBytes);
			if (!GetTokenInformation(Token.m_hHandle, TokenUser, vecData.data(), unBytes, &unBytes)) {
				strErrorOut = FormatWindowsError("GetTokenInformation");
				return false;
			}

			LPWSTR pszSID = nullptr;
			const BOOL nConverted = ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(vecData.data())->User.Sid, &pszSID);
			const DWORD unConvertError = GetLastError();
			std::unique_ptr<wchar_t, LocalMemoryDeleter> pSID(pszSID);
			if (!nConverted) {
				strErrorOut = FormatWindowsError("ConvertSidToStringSid", unConvertError);
				return false;
			}

			std::wstring const wstrSDDL = L"O:" + std::wstring(pSID.get()) + L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;" + pSID.get() + L")";
			pSID.reset();

			if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(wstrSDDL.c_str(), SDDL_REVISION_1, &pDescriptorOut, nullptr)) {
				strErrorOut = FormatWindowsError("Create private ACL");
				return false;
			}

			return true;
		}

		bool CreatePrivateParentDirectories(std::filesystem::path const& Path, SECURITY_ATTRIBUTES& Attributes, std::string& strErrorOut) {
			std::vector<std::filesystem::path> vecMissing;
			std::filesystem::path CurrentPath = Path.parent_path();
			while (!CurrentPath.empty()) {
				const DWORD unAttributes = GetFileAttributesW(CurrentPath.c_str());
				if (unAttributes != INVALID_FILE_ATTRIBUTES) {
					break;
				}

				const DWORD unErrorCode = GetLastError();
				if ((unErrorCode != ERROR_PATH_NOT_FOUND) && (unErrorCode != ERROR_FILE_NOT_FOUND)) {
					strErrorOut = FormatWindowsError("Inspect configuration directory", unErrorCode);
					return false;
				}

				vecMissing.push_back(CurrentPath);
				std::filesystem::path const ParentPath = CurrentPath.parent_path();
				if (ParentPath == CurrentPath) {
					break;
				}

				CurrentPath = ParentPath;
			}

			// Validate existing ancestors before creating anything. Existing directory
			// ACLs are preserved; only directories created here receive a private ACL.
			if (!ValidateConfigurationParents(CurrentPath / L"placeholder", strErrorOut)) {
				return false;
			}

			for (auto itPath = vecMissing.rbegin(); itPath != vecMissing.rend(); ++itPath) {
				if (!CreateDirectoryW(itPath->c_str(), &Attributes)) {
					const DWORD unErrorCode = GetLastError();
					if (unErrorCode != ERROR_ALREADY_EXISTS) {
						strErrorOut = FormatWindowsError("Create private configuration directory", unErrorCode);
						return false;
					}
				}

				const DWORD unAttributes = GetFileAttributesW(itPath->c_str());
				if ((unAttributes == INVALID_FILE_ATTRIBUTES) || !(unAttributes & FILE_ATTRIBUTE_DIRECTORY) || (unAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
					strErrorOut = "Configuration directory is not a regular directory";
					return false;
				}
			}

			return ValidateConfigurationParents(Path, strErrorOut);
		}

		bool ReadConfigurationHandle(HANDLE hFile, std::string& strContentsOut, std::string& strErrorOut) {
			LARGE_INTEGER Size {};
			if (!GetFileSizeEx(hFile, &Size) || (Size.QuadPart < 0) || (Size.QuadPart > kMaximumConfigurationBytes)) {
				strErrorOut = "Configuration must be at most 1 MiB";
				return false;
			}

			SecureZeroMemory(strContentsOut.data(), strContentsOut.size());
			strContentsOut.assign(static_cast<std::size_t>(Size.QuadPart), '\0');
			std::size_t unOffset = 0;
			while (unOffset < strContentsOut.size()) {
				DWORD unReadBytes = 0;
				if (!ReadFile(hFile, strContentsOut.data() + unOffset, static_cast<DWORD>(strContentsOut.size() - unOffset), &unReadBytes, nullptr) || (unReadBytes == 0)) {
					const DWORD unErrorCode = GetLastError();
					SecureZeroMemory(strContentsOut.data(), strContentsOut.size());
					strContentsOut.clear();
					strErrorOut = FormatWindowsError("Read configuration", unErrorCode);
					return false;
				}

				unOffset += unReadBytes;
			}

			return true;
		}

		bool ArePathsEqual(std::wstring const& wstrFirst, std::wstring const& wstrSecond) {
			return _wcsicmp(wstrFirst.c_str(), wstrSecond.c_str()) == 0;
		}

		DWORD GetRemainingActionMilliseconds(const ULONGLONG unDeadlineTicks, const ULONGLONG unNowTicks, const DWORD unBudgetMilliseconds) {
			if ((unNowTicks >= unDeadlineTicks) || ((unDeadlineTicks - unNowTicks) > unBudgetMilliseconds)) {
				return 0;
			}

			return static_cast<DWORD>(unDeadlineTicks - unNowTicks);
		}

		bool QueryOwnedService(std::string const& strName, std::filesystem::path const* pExactConfiguration, std::string& strErrorOut) {
			constexpr int kExpectedArgumentCount = 3;

			ServiceHandle Manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
			if (!Manager.m_hHandle) {
				strErrorOut = FormatWindowsError("OpenSCManager");
				return false;
			}

			std::wstring const wstrServiceName = L"WireGuardTunnel$" + ConvertToWideName(strName);
			ServiceHandle Service(OpenServiceW(Manager.m_hHandle, wstrServiceName.c_str(), SERVICE_QUERY_CONFIG));
			if (!Service.m_hHandle) {
				strErrorOut = FormatWindowsError("OpenService");
				return false;
			}

			DWORD unNeededBytes = 0;
			// The checked second query consumes this size probe; no payload is expected yet.
			QueryServiceConfigW(Service.m_hHandle, nullptr, 0, &unNeededBytes);
			const DWORD unProbeError = GetLastError();
			if ((unNeededBytes == 0) || (unNeededBytes > kMaximumConfigurationBytes)) {
				strErrorOut = FormatWindowsError("QueryServiceConfig", unProbeError);
				return false;
			}

			std::vector<unsigned char> vecData(unNeededBytes);
			LPQUERY_SERVICE_CONFIGW pConfiguration = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(vecData.data());
			if (!QueryServiceConfigW(Service.m_hHandle, pConfiguration, unNeededBytes, &unNeededBytes)) {
				strErrorOut = FormatWindowsError("QueryServiceConfig");
				return false;
			}

			int nArgumentCount = 0;
			LPWSTR* const ppszArguments = CommandLineToArgvW(pConfiguration->lpBinaryPathName, &nArgumentCount);
			std::unique_ptr<wchar_t*, LocalMemoryDeleter> pArguments(ppszArguments);
			bool bOwned = ppszArguments && (nArgumentCount == kExpectedArgumentCount) && ArePathsEqual(ppszArguments[0], FindWireGuard().wstring()) && (_wcsicmp(ppszArguments[1], L"/tunnelservice") == 0);
			if (bOwned) {
				std::filesystem::path const ActualPath = ppszArguments[2];
				bOwned = ActualPath.is_absolute() && (ActualPath.filename().wstring() == (ConvertToWideName(strName) + L".conf"));

				if (bOwned && pExactConfiguration) {
					bOwned = ArePathsEqual(ActualPath.lexically_normal().wstring(), pExactConfiguration->lexically_normal().wstring());
				}
			}

			pArguments.reset();

			if (!bOwned) {
				strErrorOut = "Refusing to modify a service that is not owned by this WireSteam tunnel";
			}

			return bOwned;
		}

		bool WaitForServiceState(std::string const& strName, bool bRunning, unsigned unTimeoutMilliseconds, std::string& strErrorOut) {
			constexpr DWORD kPollIntervalMilliseconds = 100;

			ServiceHandle Manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
			if (!Manager.m_hHandle) {
				strErrorOut = FormatWindowsError("OpenSCManager");
				return false;
			}

			std::wstring const wstrServiceName = L"WireGuardTunnel$" + ConvertToWideName(strName);
			const ULONGLONG unStartTicks = GetTickCount64();
			while ((GetTickCount64() - unStartTicks) < unTimeoutMilliseconds) {
				ServiceHandle Service(OpenServiceW(Manager.m_hHandle, wstrServiceName.c_str(), SERVICE_QUERY_STATUS));
				if (!Service.m_hHandle) {
					const DWORD unErrorCode = GetLastError();
					if (!bRunning && (unErrorCode == ERROR_SERVICE_DOES_NOT_EXIST)) {
						return true;
					}

					if ((unErrorCode != ERROR_SERVICE_DOES_NOT_EXIST) && (unErrorCode != ERROR_SERVICE_MARKED_FOR_DELETE)) {
						strErrorOut = FormatWindowsError("OpenService", unErrorCode);
						return false;
					}

				} else {
					SERVICE_STATUS_PROCESS Status {};
					DWORD unNeededBytes = 0;
					if (!QueryServiceStatusEx(Service.m_hHandle, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&Status), sizeof(Status), &unNeededBytes)) {
						strErrorOut = FormatWindowsError("QueryServiceStatusEx");
						return false;
					}

					if (bRunning && (Status.dwCurrentState == SERVICE_RUNNING)) {
						return true;
					}

					if (bRunning && (Status.dwCurrentState == SERVICE_STOPPED)) {
						strErrorOut = "WireGuard service stopped during startup (exit " + std::to_string(Status.dwWin32ExitCode) + ")";
						return false;
					}
				}

				Sleep(kPollIntervalMilliseconds);
			}

			strErrorOut = bRunning ? "Timed out waiting for WireGuard service startup" : "Timed out waiting for WireGuard service removal";

			return false;
		}

		bool SetServiceDemandStart(std::string const& strName, std::filesystem::path const& ExactConfigurationPath, std::string& strErrorOut) {
			if (!QueryOwnedService(strName, &ExactConfigurationPath, strErrorOut)) {
				return false;
			}

			ServiceHandle Manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
			if (!Manager.m_hHandle) {
				strErrorOut = FormatWindowsError("OpenSCManager");
				return false;
			}

			std::wstring const wstrServiceName = L"WireGuardTunnel$" + ConvertToWideName(strName);
			ServiceHandle Service(OpenServiceW(Manager.m_hHandle, wstrServiceName.c_str(), SERVICE_CHANGE_CONFIG));
			if (!Service.m_hHandle) {
				strErrorOut = FormatWindowsError("OpenService");
				return false;
			}

			// This adapter belongs to an interactive Steam bridge. It must not start
			// after reboot before the signed-in user's bridge exists.
			if (!ChangeServiceConfigW(Service.m_hHandle, SERVICE_NO_CHANGE, SERVICE_DEMAND_START, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
				strErrorOut = FormatWindowsError("Set WireSteam service to manual startup");
				return false;
			}

			return true;
		}

		std::string FormatDiagnosticStartTime() {
			constexpr std::size_t kTimestampBufferBytes = 32;

			SYSTEMTIME Time {};
			GetLocalTime(&Time);

			char szText[kTimestampBufferBytes] {};
			// The numeric timestamp fits this storage for valid SYSTEMTIME fields.
			std::snprintf(szText, sizeof(szText), "%04u-%02u-%02u %02u:%02u:%02u", static_cast<unsigned>(Time.wYear), static_cast<unsigned>(Time.wMonth), static_cast<unsigned>(Time.wDay), static_cast<unsigned>(Time.wHour), static_cast<unsigned>(Time.wMinute), static_cast<unsigned>(Time.wSecond));

			return szText;
		}

		std::string ReadStartupDiagnostics(std::string const& strName, std::string const& strSince) {
			constexpr unsigned kTimeoutMilliseconds = 2500;
			constexpr unsigned kMaximumOutputBytes = 2 * kMaximumConfigurationBytes;

			std::filesystem::path const WireGuardPath = FindWireGuard();
			if (WireGuardPath.empty()) {
				return {};
			}

			ProcessResult Result = RunProcess(WireGuardPath, L"/dumplog", {}, kTimeoutMilliseconds, kMaximumOutputBytes);
			StringEraseScope OutputScope(Result.m_strOutput);
			// Best effort only. Do not turn a log access error into a second failure.
			if (!Result.m_strError.empty() || (Result.m_nExitCode != 0)) {
				return {};
			}

			return SelectServiceDiagnostics(Result.m_strOutput, strName, strSince);
		}

		void Require(bool bCondition, char const* pszMessage) {
			if (!bCondition) {
				throw std::runtime_error(pszMessage);
			}
		}
	} // namespace

	bool IsElevated() {
		HANDLE hToken = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
			return false;
		}

		Handle Token(hToken);
		TOKEN_ELEVATION Elevation {};
		DWORD unBytes = 0;

		return GetTokenInformation(Token.m_hHandle, TokenElevation, &Elevation, sizeof(Elevation), &unBytes) && Elevation.TokenIsElevated;
	}

	std::filesystem::path FindWireGuard() {
		return FindProgramFilesWireGuard(L"wireguard.exe");
	}

	std::filesystem::path FindWg() {
		return FindProgramFilesWireGuard(L"wg.exe");
	}

	std::wstring QuoteArgument(std::wstring const& wstrValue) {
		constexpr wchar_t kEscapeCharacter = L'\\';
		constexpr wchar_t kQuoteCharacter = L'"';
		constexpr std::size_t kEscapeMultiplier = 2;

		std::wstring wstrResult = L"\"";
		std::size_t unSlashes = 0;
		for (auto const& wchCharacter : wstrValue) {
			if (wchCharacter == kEscapeCharacter) {
				++unSlashes;
				continue;
			}

			if (wchCharacter == kQuoteCharacter) {
				wstrResult.append(unSlashes * kEscapeMultiplier + 1, kEscapeCharacter);
				wstrResult += wchCharacter;

			} else {
				wstrResult.append(unSlashes, kEscapeCharacter);
				wstrResult += wchCharacter;
			}

			unSlashes = 0;
		}

		wstrResult.append(unSlashes * kEscapeMultiplier, kEscapeCharacter);
		wstrResult += kQuoteCharacter;

		return wstrResult;
	}

	ProcessResult RunProcess(std::filesystem::path const& ExecutablePath, std::wstring const& wstrArguments, std::string const& strInput, unsigned unTimeoutMilliseconds, unsigned unMaximumOutputBytes) {
		constexpr unsigned kMaximumProcessOutputBytes = 16 * kMaximumConfigurationBytes;
		constexpr DWORD kPipeBufferBytes = 65536;
		constexpr DWORD kProcessAttributeCount = 1;
		constexpr std::size_t kOutputChunkBytes = 4096;
		constexpr DWORD kPollIntervalMilliseconds = 5;

		static_assert(std::is_nothrow_move_constructible_v<ProcessResult>);

		ProcessResult Result;
		StringEraseScope OutputScope(Result.m_strOutput);

		if (ExecutablePath.empty() || !ExecutablePath.is_absolute()) {
			Result.m_strError = "Executable must be an absolute installed path";
			return ProcessResult(std::move(Result));
		}

		if (strInput.size() > kMaximumConfigurationBytes) {
			Result.m_strError = "Process input exceeds 1 MiB";
			return ProcessResult(std::move(Result));
		}

		if ((unMaximumOutputBytes == 0) || (unMaximumOutputBytes > kMaximumProcessOutputBytes)) {
			Result.m_strError = "Invalid process output limit";
			return ProcessResult(std::move(Result));
		}

		// Capture may contain private keys; reserve its bounded storage before output, with one chunk minimum to avoid SSO.
		Result.m_strOutput.reserve(std::max<std::size_t>(unMaximumOutputBytes, kOutputChunkBytes));

		SECURITY_ATTRIBUTES InheritableAttributes { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
		HANDLE hRead = nullptr;
		HANDLE hWrite = nullptr;
		if (!CreatePipe(&hRead, &hWrite, &InheritableAttributes, kPipeBufferBytes)) {
			Result.m_strError = FormatWindowsError("CreatePipe");
			return ProcessResult(std::move(Result));
		}

		Handle OutputRead(hRead);
		Handle OutputWrite(hWrite);
		if (!SetHandleInformation(OutputRead.m_hHandle, HANDLE_FLAG_INHERIT, 0)) {
			Result.m_strError = FormatWindowsError("SetHandleInformation");
			return ProcessResult(std::move(Result));
		}

		if (!CreatePipe(&hRead, &hWrite, &InheritableAttributes, kPipeBufferBytes)) {
			Result.m_strError = FormatWindowsError("CreatePipe");
			return ProcessResult(std::move(Result));
		}

		Handle InputRead(hRead);
		Handle InputWrite(hWrite);
		if (!SetHandleInformation(InputWrite.m_hHandle, HANDLE_FLAG_INHERIT, 0)) {
			Result.m_strError = FormatWindowsError("SetHandleInformation");
			return ProcessResult(std::move(Result));
		}

		SIZE_T unAttributeBytes = 0;
		// This probe discovers storage; the following initialization checks success.
		InitializeProcThreadAttributeList(nullptr, kProcessAttributeCount, 0, &unAttributeBytes);
		std::vector<unsigned char> vecAttributes(unAttributeBytes);
		LPPROC_THREAD_ATTRIBUTE_LIST pAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(vecAttributes.data());
		if (!InitializeProcThreadAttributeList(pAttributeList, kProcessAttributeCount, 0, &unAttributeBytes)) {
			Result.m_strError = FormatWindowsError("Initialize process attributes");
			return ProcessResult(std::move(Result));
		}

		ProcessAttributeListScope AttributeScope(pAttributeList);
		HANDLE arrInheritedHandles[] { InputRead.m_hHandle, OutputWrite.m_hHandle };
		if (!UpdateProcThreadAttribute(pAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, arrInheritedHandles, sizeof(arrInheritedHandles), nullptr, nullptr)) {
			Result.m_strError = FormatWindowsError("Set inherited handles");
			AttributeScope.Reset();
			return ProcessResult(std::move(Result));
		}

		STARTUPINFOEXW StartupInformation {};
		StartupInformation.StartupInfo.cb = sizeof(StartupInformation);
		StartupInformation.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		StartupInformation.StartupInfo.wShowWindow = SW_HIDE;
		StartupInformation.StartupInfo.hStdInput = InputRead.m_hHandle;
		StartupInformation.StartupInfo.hStdOutput = OutputWrite.m_hHandle;
		StartupInformation.StartupInfo.hStdError = OutputWrite.m_hHandle;
		StartupInformation.lpAttributeList = pAttributeList;
		std::wstring wstrCommand = QuoteArgument(ExecutablePath.wstring()) + (wstrArguments.empty() ? L"" : (L" " + wstrArguments));
		PROCESS_INFORMATION ProcessInformation {};
		std::filesystem::path const ParentPath = ExecutablePath.parent_path();
		const bool bStarted = CreateProcessW(ExecutablePath.c_str(), wstrCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, ParentPath.c_str(), &StartupInformation.StartupInfo, &ProcessInformation) != FALSE;
		const DWORD unStartError = GetLastError();
		AttributeScope.Reset();

		if (!bStarted) {
			Result.m_strError = FormatWindowsError("CreateProcess", unStartError);
			return ProcessResult(std::move(Result));
		}

		Handle Process(ProcessInformation.hProcess);
		Handle MainThread(ProcessInformation.hThread);
		OutputWrite.Reset();
		InputRead.Reset();

		// A writer prevents deadlock when a child emits output while consuming input.
		const HANDLE hStandardInput = InputWrite.m_hHandle;
		std::atomic_bool bStopWriter = false;
		std::thread Writer;
		bool bFinished = false;
		ProcessExecutionScope ExecutionScope = ProcessExecutionScope(Process.m_hHandle, Writer, bStopWriter, bFinished);
		Writer = std::thread([hStandardInput, &strInput, &bStopWriter]() noexcept -> void {
			constexpr std::size_t kWriteChunkBytes = 65536;

			Handle OwnedInput(hStandardInput);
			std::size_t unOffset = 0;
			while ((unOffset < strInput.size()) && !bStopWriter.load()) {
				DWORD unWrittenBytes = 0;
				if (!WriteFile(OwnedInput.m_hHandle, strInput.data() + unOffset, static_cast<DWORD>(std::min<std::size_t>(kWriteChunkBytes, strInput.size() - unOffset)), &unWrittenBytes, nullptr) || (unWrittenBytes == 0)) {
					break;
				}

				unOffset += unWrittenBytes;
			}
		});
		// Successful construction commits ownership; releasing this local owner cannot throw.
		InputWrite.Release();

		const ULONGLONG unStartTicks = GetTickCount64();
		while (!bFinished) {
			if ((GetTickCount64() - unStartTicks) >= unTimeoutMilliseconds) {
				Result.m_strError = "Process timed out";
				break;
			}

			DWORD unAvailableBytes = 0;
			// Readiness failures fall through to the authoritative child-exit/deadline checks.
			if (PeekNamedPipe(OutputRead.m_hHandle, nullptr, 0, nullptr, &unAvailableBytes, nullptr) && (unAvailableBytes != 0)) {
				char arrOutputBytes[kOutputChunkBytes] {};
				MemoryEraseScope OutputChunkScope = MemoryEraseScope(arrOutputBytes, sizeof(arrOutputBytes));
				DWORD unReadBytes = 0;
				if (ReadFile(OutputRead.m_hHandle, arrOutputBytes, std::min<DWORD>(unAvailableBytes, static_cast<DWORD>(sizeof(arrOutputBytes))), &unReadBytes, nullptr) && (unReadBytes != 0)) {
					if (unReadBytes > (unMaximumOutputBytes - Result.m_strOutput.size())) {
						Result.m_strError = "Process output exceeded its bounded limit";
						break;
					}

					Result.m_strOutput.append(arrOutputBytes, unReadBytes);
				}

				continue;
			}

			if (WaitForSingleObject(Process.m_hHandle, 0) == WAIT_OBJECT_0) {
				bFinished = true;
				break;
			}

			Sleep(kPollIntervalMilliseconds);
		}

		// Preserve normal child termination and writer cleanup before reading the exit code.
		ExecutionScope.Close();

		if (bFinished) {
			DWORD unExitCode = 0;
			if (GetExitCodeProcess(Process.m_hHandle, &unExitCode)) {
				Result.m_nExitCode = static_cast<int>(unExitCode);

			} else {
				Result.m_strError = FormatWindowsError("GetExitCodeProcess");
			}
		}

		return ProcessResult(std::move(Result));
	}

	bool DerivePublicKey(std::string const& strPrivateKey, std::string& strPublicKeyOut, std::string& strErrorOut) {
		Key arrDecodedKey {};
		KeyEraseScope DecodedKeyScope(arrDecodedKey);
		if (!DecodeKey(strPrivateKey, arrDecodedKey)) {
			strErrorOut = "Private key must be a base64-encoded 32-byte WireGuard key";
			return false;
		}

		SecureZeroMemory(arrDecodedKey.data(), arrDecodedKey.size());
		std::filesystem::path const ToolPath = FindWg();
		if (ToolPath.empty()) {
			strErrorOut = "Install official WireGuard for Windows to generate or derive keys";
			return false;
		}

		ProcessResult Result;
		{
			std::string strInput = strPrivateKey + "\n";
			StringEraseScope InputScope(strInput);
			Result = RunProcess(ToolPath, L"pubkey", strInput);
		}

		if (!Result.m_strError.empty() || (Result.m_nExitCode != 0)) {
			strErrorOut = Result.m_strError.empty() ? "WireGuard public-key derivation failed" : Result.m_strError;
			return false;
		}

		while (!Result.m_strOutput.empty() && ((Result.m_strOutput.back() == '\r') || (Result.m_strOutput.back() == '\n'))) {
			Result.m_strOutput.pop_back();
		}

		if (!DecodeKey(Result.m_strOutput, arrDecodedKey)) {
			strErrorOut = "WireGuard returned an invalid public key";
			return false;
		}

		strPublicKeyOut = Result.m_strOutput;

		return true;
	}

	bool GenerateKeyPair(std::string& strPrivateKeyOut, std::string& strPublicKeyOut, std::string& strErrorOut) {
		std::filesystem::path const ToolPath = FindWg();
		if (ToolPath.empty()) {
			strErrorOut = "Install official WireGuard for Windows before generating keys";
			return false;
		}

		ProcessResult Result = RunProcess(ToolPath, L"genkey");
		StringEraseScope PrivateOutputScope(Result.m_strOutput);
		if (!Result.m_strError.empty() || (Result.m_nExitCode != 0)) {
			strErrorOut = Result.m_strError.empty() ? "WireGuard key generation failed" : Result.m_strError;
			return false;
		}

		while (!Result.m_strOutput.empty() && ((Result.m_strOutput.back() == '\r') || (Result.m_strOutput.back() == '\n'))) {
			Result.m_strOutput.pop_back();
		}

		Key arrKey {};
		KeyEraseScope KeyScope(arrKey);
		if (!DecodeKey(Result.m_strOutput, arrKey)) {
			strErrorOut = "WireGuard returned an invalid private key";
			return false;
		}

		SecureZeroMemory(arrKey.data(), arrKey.size());
		std::string strDerivedPublicKey;
		if (!DerivePublicKey(Result.m_strOutput, strDerivedPublicKey, strErrorOut)) {
			SecureZeroMemory(Result.m_strOutput.data(), Result.m_strOutput.size());
			return false;
		}

		// Preserve the caller's independent output storage; only the intermediate is erased.
		strPrivateKeyOut = Result.m_strOutput;
		strPublicKeyOut = strDerivedPublicKey;
		SecureZeroMemory(Result.m_strOutput.data(), Result.m_strOutput.size());

		return true;
	}

	bool WritePrivateFile(std::filesystem::path const& RequestedPath, std::string const& strContents, bool bOverwrite, std::string& strErrorOut) {
		constexpr int kGUIDCharacters = 40;

		if (strContents.size() > kMaximumConfigurationBytes) {
			strErrorOut = "Configuration exceeds 1 MiB";
			return false;
		}

		std::error_code ErrorCode;
		std::filesystem::path const Path = std::filesystem::absolute(RequestedPath, ErrorCode).lexically_normal();
		if (ErrorCode || Path.filename().empty()) {
			strErrorOut = "Invalid configuration file path";
			return false;
		}

		const DWORD unAttributes = GetFileAttributesW(Path.c_str());
		if ((unAttributes != INVALID_FILE_ATTRIBUTES) && ((unAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || !bOverwrite)) {
			strErrorOut = "Destination already exists or is a reparse point; refusing to overwrite";
			return false;
		}

		PSECURITY_DESCRIPTOR pRawDescriptor = nullptr;
		if (!CreatePrivateSecurityDescriptor(pRawDescriptor, strErrorOut)) {
			return false;
		}

		std::unique_ptr<void, LocalMemoryDeleter> pDescriptor(pRawDescriptor);
		SECURITY_ATTRIBUTES Attributes { sizeof(SECURITY_ATTRIBUTES), pDescriptor.get(), FALSE };
		if (!CreatePrivateParentDirectories(Path, Attributes, strErrorOut)) {
			pDescriptor.reset();
			return false;
		}

		std::filesystem::path DestinationPath = Path;
		if (bOverwrite) {
			GUID Identifier {};
			if (FAILED(CoCreateGuid(&Identifier))) {
				pDescriptor.reset();
				strErrorOut = "Cannot create a temporary file identifier";
				return false;
			}

			wchar_t szIdentifier[kGUIDCharacters] {};
			// A GUID including braces and its terminator occupies 39 characters.
			StringFromGUID2(Identifier, szIdentifier, kGUIDCharacters);
			DestinationPath += std::wstring(L".") + szIdentifier + L".tmp";
		}

		const HANDLE hFile = CreateFileW(DestinationPath.c_str(), GENERIC_WRITE, 0, &Attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		const DWORD unCreateError = GetLastError();
		Handle File(hFile);
		pDescriptor.reset();

		if (!File) {
			strErrorOut = FormatWindowsError("Create private configuration", unCreateError);
			return false;
		}

		CreatedFileScope FileScope = CreatedFileScope(DestinationPath, File);
		bool bOkay = true;
		std::size_t unOffset = 0;
		while (unOffset < strContents.size()) {
			DWORD unWrittenBytes = 0;
			if (!WriteFile(File.m_hHandle, strContents.data() + unOffset, static_cast<DWORD>(strContents.size() - unOffset), &unWrittenBytes, nullptr) || (unWrittenBytes == 0)) {
				strErrorOut = FormatWindowsError("Write configuration");
				bOkay = false;
				break;
			}

			unOffset += unWrittenBytes;
		}

		if (bOkay && !FlushFileBuffers(File.m_hHandle)) {
			strErrorOut = FormatWindowsError("Flush configuration");
			bOkay = false;
		}

		File.Reset();

		if (bOkay && bOverwrite) {
			const DWORD unExistingAttributes = GetFileAttributesW(Path.c_str());
			if ((unExistingAttributes != INVALID_FILE_ATTRIBUTES) && (unExistingAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
				strErrorOut = "Destination became a reparse point; refusing to overwrite";
				bOkay = false;

			} else if (!MoveFileExW(DestinationPath.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				strErrorOut = FormatWindowsError("Replace configuration");
				bOkay = false;
			}
		}

		if (bOkay) {
			FileScope.Commit();

		} else {
			FileScope.Rollback();
		}

		return bOkay;
	}

	bool ReadConfigFile(std::filesystem::path const& Path, std::string& strContentsOut, std::string& strErrorOut) {
		Handle File(CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
		if (!File) {
			strErrorOut = FormatWindowsError("Open configuration");
			return false;
		}

		BY_HANDLE_FILE_INFORMATION Information {};
		if (!GetFileInformationByHandle(File.m_hHandle, &Information) || (Information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
			strErrorOut = "Configuration must be a regular file";
			return false;
		}

		return ReadConfigurationHandle(File.m_hHandle, strContentsOut, strErrorOut);
	}

	bool ServiceExists(std::string const& strName) {
		ServiceHandle Manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
		// Conservatively preserve a runtime file if service state is inaccessible.
		// Only ERROR_SERVICE_DOES_NOT_EXIST proves that cleanup may remove it.
		if (!Manager.m_hHandle) {
			return true;
		}

		std::wstring const wstrServiceName = L"WireGuardTunnel$" + ConvertToWideName(strName);
		ServiceHandle Service(OpenServiceW(Manager.m_hHandle, wstrServiceName.c_str(), SERVICE_QUERY_STATUS));
		if (Service.m_hHandle) {
			return true;
		}

		const DWORD unErrorCode = GetLastError();

		return unErrorCode != ERROR_SERVICE_DOES_NOT_EXIST;
	}

	bool ServiceMatchesConfig(std::string const& strName, std::filesystem::path const& ConfigurationPath, std::string& strErrorOut) {
		return IsValidServiceName(strName) && QueryOwnedService(strName, &ConfigurationPath, strErrorOut);
	}

	bool ServiceAction(std::string const& strAction, std::filesystem::path const& ConfigurationPath, std::string const& strTunnelName, std::string& strErrorOut, InternetMode ModeValue, std::string const& strSubnet) {
		constexpr DWORD kRemovalBudgetMilliseconds = 90000;
		constexpr DWORD kInstallationBudgetMilliseconds = 30000;

		if (((strAction != "install") && (strAction != "remove")) || !IsValidServiceName(strTunnelName)) {
			strErrorOut = "Invalid WireSteam service action or tunnel name";
			return false;
		}

		std::filesystem::path const HelperPath = ResolveModulePath().parent_path() / L"WireSteamService.exe";
		const DWORD unAttributes = GetFileAttributesW(HelperPath.c_str());
		if ((unAttributes == INVALID_FILE_ATTRIBUTES) || (unAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
			strErrorOut = "WireSteamService.exe must be present beside WireSteam.exe";
			return false;
		}

		std::wstring wstrArguments = ConvertToWideName(strAction) + L" --name " + QuoteArgument(ConvertToWideName(strTunnelName));
		if (strAction == "install") {
			wstrArguments += L" --config " + QuoteArgument(std::filesystem::absolute(ConfigurationPath).wstring());
		}

		if ((strAction == "install") && (ModeValue != InternetMode::Off)) {
			wstrArguments += L" --internet-mode " + ConvertToWideName(InternetModeName(ModeValue));

			if (!strSubnet.empty()) {
				wstrArguments += L" --internet-subnet " + QuoteArgument(ConvertToWideName(strSubnet));
			}
		}

		std::string const strDiagnosticStart = FormatDiagnosticStartTime();
		const DWORD unBudgetMilliseconds = (strAction == "remove") ? kRemovalBudgetMilliseconds : kInstallationBudgetMilliseconds;
		const ULONGLONG unStartTicks = GetTickCount64();
		if (unStartTicks > (std::numeric_limits<ULONGLONG>::max() - unBudgetMilliseconds)) {
			strErrorOut = "Cannot establish the WireSteam service action deadline";
			return false;
		}

		const ULONGLONG unDeadlineTicks = unStartTicks + unBudgetMilliseconds;
		wstrArguments += L" --deadline " + std::to_wstring(unDeadlineTicks);

		if (IsElevated()) {
			ProcessResult const Result = RunProcess(HelperPath, wstrArguments, {}, unBudgetMilliseconds);
			if ((Result.m_nExitCode == 0) && Result.m_strError.empty()) {
				return true;
			}

			strErrorOut = !Result.m_strError.empty() ? Result.m_strError : ("WireSteam service helper failed: " + Result.m_strOutput);

			if (strAction == "install") {
				strErrorOut += ReadStartupDiagnostics(strTunnelName, strDiagnosticStart);
			}

			return false;
		}

		// UAC can wait indefinitely for a human. The caller remains bounded, and the
		// helper refuses a late approval using the boot-wide monotonic deadline.
		struct ElevationState {
			std::promise<std::pair<DWORD, DWORD>> m_Promise;
		};

		std::shared_ptr<ElevationState> pState = std::make_shared<ElevationState>();
		std::future<std::pair<DWORD, DWORD>> Future = pState->m_Promise.get_future();
		// All captures own their data so the worker can safely outlive this caller.
		std::thread ElevationThread([pState, HelperPath, wstrArguments, unDeadlineTicks, unBudgetMilliseconds]() noexcept -> void {
			constexpr DWORD kInvalidExitCode = std::numeric_limits<DWORD>::max();

			try {
				const HRESULT nCOMResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
				COMScope Apartment(nCOMResult);
				if (FAILED(nCOMResult)) {
					pState->m_Promise.set_value({ ERROR_DLL_INIT_FAILED, kInvalidExitCode });
					return;
				}

				SHELLEXECUTEINFOW Execute {};
				Execute.cbSize = sizeof(Execute);
				Execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
				Execute.lpVerb = L"runas";
				Execute.lpFile = HelperPath.c_str();
				Execute.lpParameters = wstrArguments.c_str();
				Execute.nShow = SW_HIDE;
				if (!ShellExecuteExW(&Execute)) {
					const DWORD unErrorCode = GetLastError();
					pState->m_Promise.set_value({ unErrorCode, kInvalidExitCode });
					return;
				}

				Handle Process(Execute.hProcess);
				const ULONGLONG unNowTicks = GetTickCount64();
				const DWORD unRemainingMilliseconds = GetRemainingActionMilliseconds(unDeadlineTicks, unNowTicks, unBudgetMilliseconds);
				if (!Process || (WaitForSingleObject(Process.m_hHandle, unRemainingMilliseconds) != WAIT_OBJECT_0)) {
					pState->m_Promise.set_value({ ERROR_TIMEOUT, kInvalidExitCode });
					return;
				}

				DWORD unExitCode = kInvalidExitCode;
				if (!GetExitCodeProcess(Process.m_hHandle, &unExitCode)) {
					const DWORD unErrorCode = GetLastError();
					pState->m_Promise.set_value({ unErrorCode, kInvalidExitCode });
					return;
				}

				pState->m_Promise.set_value({ ERROR_SUCCESS, unExitCode });
			} catch (...) {
				try {
					pState->m_Promise.set_value({ ERROR_UNHANDLED_EXCEPTION, kInvalidExitCode });
				} catch (...) {
					OutputDebugStringA("WireSteam elevation worker could not publish its failure.\n");
				}
			}
		});

		try {
			ElevationThread.detach();
		} catch (...) {
			OutputDebugStringA("WireSteam elevation worker could not detach.\n");
			std::terminate();
		}

		if (Future.wait_for(std::chrono::milliseconds(unBudgetMilliseconds)) != std::future_status::ready) {
			strErrorOut = "Timed out waiting for administrator approval or WireGuard service action; late approval will be refused";
			return false;
		}

		std::pair<DWORD, DWORD> Result {};
		try {
			Result = Future.get();
		} catch (...) {
			strErrorOut = "WireSteam service helper result could not be read";
			return false;
		}

		const DWORD unErrorCode = Result.first;
		const DWORD unExitCode = Result.second;
		if (unErrorCode == ERROR_CANCELLED) {
			strErrorOut = "Administrator approval was cancelled";

		} else if (unErrorCode == ERROR_TIMEOUT) {
			strErrorOut = "Timed out waiting for the WireSteam service helper";

		} else if (unErrorCode != 0) {
			strErrorOut = FormatWindowsError("Elevate WireSteam service helper", unErrorCode);

		} else if (unExitCode != 0) {
			strErrorOut = "WireSteam service helper failed (exit " + std::to_string(unExitCode) + "); check configuration validation and WireGuard installation";

			if (strAction == "install") {
				strErrorOut += ReadStartupDiagnostics(strTunnelName, strDiagnosticStart);
			}

		} else {
			return true;
		}

		return false;
	}

	int ServiceHelperMain(int nArgumentCount, wchar_t** ppszArguments) {
		constexpr int kMinimumArgumentCount = 2;
		constexpr wchar_t kMaximumASCIICharacter = 0x7F;
		constexpr int kDecimalBase = 10;
		constexpr ULONGLONG kDefaultDeadlineMilliseconds = 30000;
		constexpr DWORD kServiceCommandTimeoutMilliseconds = 12000;
		constexpr DWORD kServiceStateTimeoutMilliseconds = 10000;

		if ((nArgumentCount >= kMinimumArgumentCount) && (std::wstring(ppszArguments[1]) == L"internet-ipc-test")) {
			return InternetIpcTestHelperMain(nArgumentCount, ppszArguments);
		}

		if ((nArgumentCount >= kMinimumArgumentCount) && (std::wstring(ppszArguments[1]) == L"internet-worker")) {
			return InternetWorkerHelperMain(nArgumentCount, ppszArguments);
		}

		auto Fail = [](std::string const& strError) -> int {
			std::cerr << "WireSteam service: " << strError << '\n';

			return 1;
		};

		if (!IsElevated()) {
			return Fail("Administrator privileges are required");
		}

		if (nArgumentCount < kMinimumArgumentCount) {
			return Fail("Expected install or remove");
		}

		const std::size_t unArgumentCount = static_cast<std::size_t>(nArgumentCount);
		std::wstring const wstrAction = ppszArguments[1];
		std::string strName;
		std::filesystem::path ConfigurationPath;
		const ULONGLONG unStartTicks = GetTickCount64();
		if (unStartTicks > (std::numeric_limits<ULONGLONG>::max() - kDefaultDeadlineMilliseconds)) {
			return Fail("Cannot establish the WireSteam service action deadline");
		}

		ULONGLONG unDeadlineTicks = unStartTicks + kDefaultDeadlineMilliseconds;
		InternetMode ModeValue = InternetMode::Off;
		std::string strInternetSubnet;
		for (std::size_t unArgumentIndex = static_cast<std::size_t>(kMinimumArgumentCount); unArgumentIndex < unArgumentCount; ++unArgumentIndex) {
			std::wstring const wstrOption = ppszArguments[unArgumentIndex];
			if (++unArgumentIndex >= unArgumentCount) {
				return Fail("Missing service argument value");
			}

			if (wstrOption == L"--name") {
				std::wstring const wstrValue = ppszArguments[unArgumentIndex];
				if (std::any_of(wstrValue.begin(), wstrValue.end(), [](wchar_t wchCharacter) -> bool {
					return wchCharacter > kMaximumASCIICharacter;
				})) {
					return Fail("Invalid tunnel name");
				}

				strName.clear();
				for (auto const& wchCharacter : wstrValue) {
					strName.push_back(static_cast<char>(wchCharacter));
				}

			} else if (wstrOption == L"--config") {
				ConfigurationPath = ppszArguments[unArgumentIndex];

			} else if (wstrOption == L"--internet-mode") {
				std::wstring const wstrValue = ppszArguments[unArgumentIndex];
				if (wstrValue == L"client") {
					ModeValue = InternetMode::Client;

				} else if (wstrValue == L"server") {
					ModeValue = InternetMode::Server;

				} else {
					return Fail("Invalid Internet mode authorization");
				}

			} else if (wstrOption == L"--internet-subnet") {
				std::wstring const wstrValue = ppszArguments[unArgumentIndex];
				if (std::any_of(wstrValue.begin(), wstrValue.end(), [](wchar_t wchCharacter) -> bool {
					return wchCharacter > kMaximumASCIICharacter;
				})) {
					return Fail("Invalid Internet subnet");
				}

				strInternetSubnet.clear();
				for (auto const& wchCharacter : wstrValue) {
					strInternetSubnet.push_back(static_cast<char>(wchCharacter));
				}

			} else if (wstrOption == L"--deadline") {
				wchar_t* pszEnd = nullptr;
				unDeadlineTicks = _wcstoui64(ppszArguments[unArgumentIndex], &pszEnd, kDecimalBase);
				if (!pszEnd || *pszEnd || !unDeadlineTicks) {
					return Fail("Invalid action deadline");
				}

			} else {
				return Fail("Unknown service option");
			}
		}

		if (!IsValidServiceName(strName)) {
			return Fail("Tunnel name must match WireSteam-[A-Za-z0-9_-]{1,20}");
		}

		if (GetTickCount64() >= unDeadlineTicks) {
			return Fail("Administrator approval expired; run the command again");
		}

		std::filesystem::path const WireGuardPath = FindWireGuard();
		if (wstrAction == L"remove") {
			if (!ConfigurationPath.empty()) {
				return Fail("Remove does not accept a configuration path");
			}

			if ((ModeValue != InternetMode::Off) || !strInternetSubnet.empty()) {
				return Fail("Remove does not accept Internet mode arguments");
			}

			std::string strRecoveryError;
			if (!LegacyInternetRoutes::RecoverSettings(strRecoveryError)) {
				return Fail(strRecoveryError);
			}

			if (!InternetServerGateway::Recover(strName, strRecoveryError)) {
				return Fail(strRecoveryError);
			}

			if (!ServiceExists(strName)) {
				return 0;
			}

			if (WireGuardPath.empty()) {
				return Fail("Install official WireGuard for Windows in Program Files");
			}

			std::string strError;
			if (!QueryOwnedService(strName, nullptr, strError)) {
				return Fail(strError);
			}

			ProcessResult const Result = RunProcess(WireGuardPath, L"/uninstalltunnelservice " + QuoteArgument(ConvertToWideName(strName)), {}, kServiceCommandTimeoutMilliseconds);
			if (!Result.m_strError.empty() || (Result.m_nExitCode != 0)) {
				return Fail(Result.m_strError.empty() ? "WireGuard service removal failed" : Result.m_strError);
			}

			if (!WaitForServiceState(strName, false, kServiceStateTimeoutMilliseconds, strError)) {
				return Fail(strError);
			}

			return 0;
		}

		if (wstrAction != L"install") {
			return Fail("Expected install or remove");
		}

		if (WireGuardPath.empty()) {
			return Fail("Install official WireGuard for Windows in Program Files");
		}

		if (!ConfigurationPath.is_absolute() || (ConfigurationPath.filename().wstring() != (ConvertToWideName(strName) + L".conf"))) {
			return Fail("Configuration must be absolute and its basename must equal the tunnel name plus .conf");
		}

		ConfigurationPath = ConfigurationPath.lexically_normal();
		std::string strError;
		StringEraseScope ErrorScope(strError);
		if (!ValidateConfigurationParents(ConfigurationPath, strError)) {
			return Fail(strError);
		}

		if (ServiceExists(strName)) {
			return Fail("A service with this tunnel name already exists; refusing to replace it");
		}

		// Keep this handle open through startup so the validated contents cannot be
		// rewritten or replaced before the SYSTEM tunnel service reads them.
		Handle FrozenConfiguration(CreateFileW(ConfigurationPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
		if (!FrozenConfiguration) {
			return Fail(FormatWindowsError("Open compiled configuration"));
		}

		BY_HANDLE_FILE_INFORMATION Information {};
		if (!GetFileInformationByHandle(FrozenConfiguration.m_hHandle, &Information) || (Information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
			return Fail("Compiled configuration must be a regular file");
		}

		std::string strText;
		StringEraseScope TextScope(strText);
		if (!ReadConfigurationHandle(FrozenConfiguration.m_hHandle, strText, strError) || !ValidateCompiledConfig(strText, strError, ModeValue, strInternetSubnet)) {
			return Fail(strError);
		}

		SecureZeroMemory(strText.data(), strText.size());

		if (GetTickCount64() >= unDeadlineTicks) {
			return Fail("Service action expired before installation");
		}

		ProcessResult const Result = RunProcess(WireGuardPath, L"/installtunnelservice " + QuoteArgument(ConfigurationPath.wstring()), {}, kServiceCommandTimeoutMilliseconds);
		bool bOkay = Result.m_strError.empty() && (Result.m_nExitCode == 0);
		if (bOkay) {
			bOkay = SetServiceDemandStart(strName, ConfigurationPath, strError);
		}

		if (bOkay) {
			bOkay = WaitForServiceState(strName, true, kServiceStateTimeoutMilliseconds, strError);
		}

		if (!bOkay) {
			// Only undo an installation that appeared after our absent preflight and
			// whose exact binary/configuration path matches this invocation.
			std::string strOwnershipError;
			if (ServiceExists(strName) && QueryOwnedService(strName, &ConfigurationPath, strOwnershipError)) {
				// Rollback is best effort; retain the original installation failure below.
				RunProcess(WireGuardPath, L"/uninstalltunnelservice " + QuoteArgument(ConvertToWideName(strName)), {}, kServiceStateTimeoutMilliseconds);
			}

			return Fail(!strError.empty() ? strError : !Result.m_strError.empty() ? Result.m_strError : "WireGuard service installation failed");
		}

		return 0;
	}

	void RunPlatformTests() {
		constexpr int kExpectedArgumentCount = 2;
		constexpr DWORD kTemporaryPathCharacters = MAX_PATH;
		constexpr int kGUIDCharacters = 40;
		constexpr WORD kExpectedPrivateACLCount = 3;
		constexpr ULONGLONG kMaximumTicks = std::numeric_limits<ULONGLONG>::max();
		constexpr DWORD kActionBudgetMilliseconds = 30000;
		constexpr ULONGLONG kOrdinaryDeadlineTicks = 100;
		constexpr DWORD kOrdinaryRemainingMilliseconds = 25;

		Require(GetRemainingActionMilliseconds(0, 0, 0) == 0, "Zero action deadline extended wait");
		Require(GetRemainingActionMilliseconds(kOrdinaryDeadlineTicks, kOrdinaryDeadlineTicks, kActionBudgetMilliseconds) == 0, "Expired action deadline extended wait");
		Require(GetRemainingActionMilliseconds(kOrdinaryDeadlineTicks, kOrdinaryDeadlineTicks - kOrdinaryRemainingMilliseconds, kActionBudgetMilliseconds) == kOrdinaryRemainingMilliseconds, "Ordinary action deadline changed wait");
		Require(GetRemainingActionMilliseconds(kMaximumTicks, kMaximumTicks - 1, kActionBudgetMilliseconds) == 1, "Maximum action deadline changed wait");
		Require(GetRemainingActionMilliseconds(kMaximumTicks, 0, kActionBudgetMilliseconds) == 0, "Wrapped action clock extended wait");

		RunServiceDiagnosticTests();

		for (auto const& pszValue : { L"", L"plain", L"two words", L"C:\\space path\\", L"a\\\"b", L"\"\\", L"Unicode \u0416" }) {
			std::wstring const wstrValue = pszValue;
			std::wstring const wstrCommand = L"program " + QuoteArgument(wstrValue);
			int nArgumentCount = 0;
			LPWSTR* const ppszArguments = CommandLineToArgvW(wstrCommand.c_str(), &nArgumentCount);
			std::unique_ptr<wchar_t*, LocalMemoryDeleter> pArguments(ppszArguments);
			const bool bOkay = pArguments && (nArgumentCount == kExpectedArgumentCount) && (std::wstring(pArguments.get()[1]) == wstrValue);
			pArguments.reset();
			Require(bOkay, "Windows argument quoting round trip failed");
		}

		std::string strPrivateKey;
		StringEraseScope PrivateKeyScope(strPrivateKey);
		std::string strPublicKey;
		std::string strDerivedKey;
		std::string strError;
		if (!GenerateKeyPair(strPrivateKey, strPublicKey, strError)) {
			throw std::runtime_error(strError);
		}

		if (!DerivePublicKey(strPrivateKey, strDerivedKey, strError)) {
			throw std::runtime_error(strError);
		}

		Require((strPublicKey == strDerivedKey) && (strPublicKey != strPrivateKey), "WireGuard key derivation mismatch");

		wchar_t szTemporaryPath[kTemporaryPathCharacters + 1] {};
		const DWORD unTemporaryCharacters = GetTempPathW(kTemporaryPathCharacters, szTemporaryPath);
		Require((unTemporaryCharacters != 0) && (unTemporaryCharacters < kTemporaryPathCharacters), "Cannot obtain temporary directory");
		GUID Identifier {};
		Require(SUCCEEDED(CoCreateGuid(&Identifier)), "Cannot generate temporary filename");
		wchar_t szIdentifier[kGUIDCharacters] {};
		// The fixed GUID buffer includes braces and the terminating null character.
		StringFromGUID2(Identifier, szIdentifier, kGUIDCharacters);
		std::filesystem::path const FilePath = std::filesystem::path(szTemporaryPath) / (std::wstring(L"WireSteam-test-") + szIdentifier + L".conf");

		struct FileCleanup {
			explicit FileCleanup(std::filesystem::path const& Path) noexcept :
				m_Path(Path)
			{
			}

			~FileCleanup() {
				// Best effort for this fixture's successfully created file only.
				DeleteFileW(m_Path.c_str());
			}

			FileCleanup(FileCleanup const&) = delete;
			FileCleanup& operator=(FileCleanup const&) = delete;

			std::filesystem::path const& m_Path;
		};

		if (!WritePrivateFile(FilePath, "private-test-data", false, strError)) {
			throw std::runtime_error(strError);
		}

		FileCleanup FileScope(FilePath);
		ProcessResult const InvalidExecutable = RunProcess(FilePath, L"");
		Require((InvalidExecutable.m_nExitCode == -1) && (InvalidExecutable.m_strError == FormatWindowsError("CreateProcess", ERROR_BAD_EXE_FORMAT)), "Invalid executable launch lost the Windows error");

		std::string strRead;
		Require(ReadConfigFile(FilePath, strRead, strError) && (strRead == "private-test-data"), "Private configuration round trip failed");
		Require(!WritePrivateFile(FilePath, "must-not-truncate", false, strError), "Existing file was overwritten without authorization");
		Require(ReadConfigFile(FilePath, strRead, strError) && (strRead == "private-test-data"), "Refused overwrite modified the file");

		if (!WritePrivateFile(FilePath, "replacement", true, strError)) {
			throw std::runtime_error(strError);
		}

		Require(ReadConfigFile(FilePath, strRead, strError) && (strRead == "replacement"), "Atomic configuration replacement failed");
		DWORD unRequiredBytes = 0;
		// The first call probes the variable-size descriptor; the checked call follows.
		GetFileSecurityW(FilePath.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &unRequiredBytes);
		std::vector<unsigned char> vecDescriptor(unRequiredBytes);
		Require(unRequiredBytes && GetFileSecurityW(FilePath.c_str(), DACL_SECURITY_INFORMATION, vecDescriptor.data(), unRequiredBytes, &unRequiredBytes), "Cannot inspect private file ACL");
		SECURITY_DESCRIPTOR_CONTROL unControl {};
		DWORD unRevision = 0;
		Require(GetSecurityDescriptorControl(vecDescriptor.data(), &unControl, &unRevision) && (unControl & SE_DACL_PROTECTED), "Private file ACL inherits public access");
		PACL pACL = nullptr;
		BOOL nPresent = FALSE;
		BOOL nDefaulted = FALSE;
		Require(GetSecurityDescriptorDacl(vecDescriptor.data(), &nPresent, &pACL, &nDefaulted) && nPresent && pACL && (pACL->AceCount == kExpectedPrivateACLCount), "Unexpected private file access entries");

		std::filesystem::path const DirectoryPath = std::filesystem::path(szTemporaryPath) / (std::wstring(L"WireSteam-directory-test-") + szIdentifier);
		std::filesystem::path const NestedPath = DirectoryPath / L"nested" / L"new.conf";
		std::filesystem::path const ParentPath = NestedPath.parent_path();

		struct DirectoryCleanup {
			DirectoryCleanup(std::filesystem::path const& FilePath, std::filesystem::path const& ParentPath, std::filesystem::path const& RootPath) noexcept :
				m_FilePath(FilePath),
				m_ParentPath(ParentPath),
				m_RootPath(RootPath)
			{
			}

			~DirectoryCleanup() {
				// Best effort for this fixture's successfully created file and directories.
				DeleteFileW(m_FilePath.c_str());
				RemoveDirectoryW(m_ParentPath.c_str());
				RemoveDirectoryW(m_RootPath.c_str());
			}

			DirectoryCleanup(DirectoryCleanup const&) = delete;
			DirectoryCleanup& operator=(DirectoryCleanup const&) = delete;

			std::filesystem::path const& m_FilePath;
			std::filesystem::path const& m_ParentPath;
			std::filesystem::path const& m_RootPath;
		};

		if (!WritePrivateFile(NestedPath, "nested-private-data", false, strError)) {
			throw std::runtime_error(strError);
		}

		DirectoryCleanup DirectoryScope = DirectoryCleanup(NestedPath, ParentPath, DirectoryPath);
		unRequiredBytes = 0;
		// Probe before the checked descriptor read, as for the file above.
		GetFileSecurityW(ParentPath.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &unRequiredBytes);
		vecDescriptor.resize(unRequiredBytes);
		Require(unRequiredBytes && GetFileSecurityW(ParentPath.c_str(), DACL_SECURITY_INFORMATION, vecDescriptor.data(), unRequiredBytes, &unRequiredBytes), "Cannot inspect new directory ACL");
		Require(GetSecurityDescriptorControl(vecDescriptor.data(), &unControl, &unRevision) && (unControl & SE_DACL_PROTECTED), "New private directory inherits public access");
		SecureZeroMemory(strPrivateKey.data(), strPrivateKey.size());
		std::cout << "PASS platform: Windows quoting, official WireGuard keys, private ACL files and atomic replacement\n";
	}
} // namespace ws
