#include "internet_worker.hpp"

// General
#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <shellapi.h>

// C++
#include <cstddef>
#include <cstring>
#include <exception>

// STL
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

// Project
#include "internet_routing.hpp"
#include "internet_server.hpp"
#include "platform.hpp"

namespace ws {
	namespace {
		typedef struct _INTERNET_WORKER_STATUS {
			DWORD m_unMagic;
			volatile LONG m_nPhase; // 0 starting, 1 underlay prepared, 2 adapter ready, 3 stopped; negative = failure.
			volatile LONG m_nSequence;
			char m_szMessage[4096];
		} Status, *PStatus;

		constexpr DWORD kStatusMagic = 0x57534932;
		constexpr std::size_t kModulePathCapacity = 32768;
		constexpr std::size_t kStatusReadAttempts = 20;

		static_assert(std::is_standard_layout_v<Status> && std::is_trivially_copyable_v<Status>);
		static_assert(sizeof(Status) == 0x100C);
		static_assert(alignof(Status) == alignof(LONG));
		static_assert(offsetof(Status, m_nPhase) == 0x04);
		static_assert(offsetof(Status, m_nSequence) == 0x08);
		static_assert(offsetof(Status, m_szMessage) == 0x0C);
		static_assert(std::atomic_ref<char>::is_always_lock_free);
		static_assert(std::atomic_ref<char>::required_alignment == 0x01);

		struct Handle {
			Handle() {
				m_hValue = nullptr;
			}

			explicit Handle(HANDLE hValue) :
				m_hValue(hValue)
			{
			}

			~Handle() {
				Reset();
			}

			Handle(Handle const&) = delete;
			Handle& operator=(Handle const&) = delete;

			void Reset(HANDLE hValue = nullptr) noexcept {
				if (m_hValue == hValue) {
					return;
				}

				if (m_hValue && (m_hValue != INVALID_HANDLE_VALUE)) {
					// Terminal best-effort release of this owner's exact handle.
					CloseHandle(m_hValue);
				}

				m_hValue = hValue;
			}

			explicit operator bool() const noexcept {
				return m_hValue && (m_hValue != INVALID_HANDLE_VALUE);
			}

			HANDLE m_hValue;
		};

		ULONGLONG MakeDeadline(ULONGLONG unNow, ULONGLONG unMilliseconds) noexcept {
			constexpr ULONGLONG kMaximumTickCount = std::numeric_limits<ULONGLONG>::max();

			return (unMilliseconds > (kMaximumTickCount - unNow)) ? kMaximumTickCount : (unNow + unMilliseconds);
		}

		bool HasTimeRemaining(ULONGLONG unNow, ULONGLONG unDeadline, ULONGLONG unMilliseconds) noexcept {
			return (unNow < unDeadline) && ((unDeadline - unNow) <= unMilliseconds);
		}

		std::string FormatWindowsFailure(char const* pszOperation, const DWORD unError) {
			return std::string(pszOperation) + " (Windows error " + std::to_string(unError) + ")";
		}

		std::string FormatWindowsFailure(char const* pszOperation) {
			const DWORD unError = GetLastError();

			return FormatWindowsFailure(pszOperation, unError);
		}

		std::filesystem::path GetModulePath() {
			std::wstring wstrPath = std::wstring(kModulePathCapacity, L'\0');
			const DWORD unSize = GetModuleFileNameW(nullptr, wstrPath.data(), static_cast<DWORD>(wstrPath.size()));
			if (!unSize || (unSize >= wstrPath.size())) {
				throw std::runtime_error("Cannot locate the Internet helper executable");
			}

			wstrPath.resize(unSize);

			return wstrPath;
		}

		std::wstring WidenASCII(std::string const& strText) {
			return { strText.begin(), strText.end() };
		}

		bool IsValidAdapterName(std::string const& strName) {
			constexpr std::size_t kPrefixCharacters = sizeof("WireSteam-") - 1;
			constexpr std::size_t kMaximumNameCharacters = 30;

			return strName.starts_with("WireSteam-") && (strName.size() > kPrefixCharacters) && (strName.size() <= kMaximumNameCharacters) && std::all_of(strName.begin() + kPrefixCharacters, strName.end(), [](unsigned char unCharacter) -> bool {
				return ((unCharacter >= 'a') && (unCharacter <= 'z')) || ((unCharacter >= 'A') && (unCharacter <= 'Z')) || ((unCharacter >= '0') && (unCharacter <= '9')) || (unCharacter == '_') || (unCharacter == '-');
			});
		}

		bool IsValidChannel(std::wstring const& wstrValue) {
			constexpr wchar_t kChannelPrefix[] = L"Local\\WireSteamInternet-";
			constexpr std::size_t kPrefixCharacters = (sizeof(kChannelPrefix) / sizeof(wchar_t)) - 1;
			constexpr std::size_t kIdentityCharacters = 32;

			return wstrValue.starts_with(kChannelPrefix) && (wstrValue.size() == (kPrefixCharacters + kIdentityCharacters)) && std::all_of(wstrValue.begin() + kPrefixCharacters, wstrValue.end(), [](wchar_t wchCharacter) -> bool {
				return ((wchCharacter >= L'0') && (wchCharacter <= L'9')) || ((wchCharacter >= L'a') && (wchCharacter <= L'f'));
			});
		}

		struct LocalMemoryDeleter {
			void operator()(void* pMemory) const noexcept {
				// LocalFree is the matching terminal release for these Windows allocations.
				LocalFree(pMemory);
			}
		};

		struct Security {
			Security() {
				m_Attributes = { sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE };
			}

			Security(Security const&) = delete;
			Security& operator=(Security const&) = delete;

			bool Initialize(std::string& strErrorOut) {
				HANDLE hRaw = nullptr;
				if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hRaw)) {
					strErrorOut = FormatWindowsFailure("Cannot identify IPC owner");
					return false;
				}

				Handle Token(hRaw);
				DWORD unBytes = 0;
				// The sizing call normally fails with ERROR_INSUFFICIENT_BUFFER.
				const BOOL bSized = GetTokenInformation(Token.m_hValue, TokenUser, nullptr, 0, &unBytes);
				const DWORD unSizeError = bSized ? ERROR_SUCCESS : GetLastError();
				if ((!bSized && (unSizeError != ERROR_INSUFFICIENT_BUFFER)) || (unBytes < sizeof(TOKEN_USER))) {
					SetLastError(unSizeError);
					strErrorOut = FormatWindowsFailure("Cannot read IPC owner");
					return false;
				}

				std::vector<unsigned char> vecInformation(unBytes);
				if (!GetTokenInformation(Token.m_hValue, TokenUser, vecInformation.data(), unBytes, &unBytes)) {
					strErrorOut = FormatWindowsFailure("Cannot read IPC owner");
					return false;
				}

				if ((unBytes < sizeof(TOKEN_USER)) || (unBytes > vecInformation.size())) {
					strErrorOut = "Cannot read IPC owner";
					return false;
				}

				// The successful TokenUser API returns an aligned TOKEN_USER and its SID in this owned allocation.
				TOKEN_USER User {};
				std::memcpy(&User, vecInformation.data(), sizeof(User));
				LPWSTR pszSID = nullptr;
				if (!ConvertSidToStringSidW(User.User.Sid, &pszSID)) {
					strErrorOut = FormatWindowsFailure("Cannot encode IPC owner");
					return false;
				}

				std::unique_ptr<wchar_t, LocalMemoryDeleter> pSID(pszSID);
				std::wstring const wstrACL = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + std::wstring(pSID.get()) + L")";
				pSID.reset();

				PSECURITY_DESCRIPTOR pDescriptor = nullptr;
				if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(wstrACL.c_str(), SDDL_REVISION_1, &pDescriptor, nullptr)) {
					strErrorOut = FormatWindowsFailure("Cannot protect Internet IPC");
					return false;
				}

				m_pDescriptor.reset(pDescriptor);
				m_Attributes.lpSecurityDescriptor = m_pDescriptor.get();

				return true;
			}

			std::unique_ptr<void, LocalMemoryDeleter> m_pDescriptor;
			SECURITY_ATTRIBUTES m_Attributes;
		};

		struct SensitiveStringWiper {
			explicit SensitiveStringWiper(std::string& strValue) :
				m_strValue(strValue)
			{
			}

			~SensitiveStringWiper() {
				Wipe();
			}

			SensitiveStringWiper(SensitiveStringWiper const&) = delete;
			SensitiveStringWiper& operator=(SensitiveStringWiper const&) = delete;

			void Wipe() noexcept {
				if (!m_strValue.empty()) {
					SecureZeroMemory(m_strValue.data(), m_strValue.size());
				}
			}

			std::string& m_strValue;
		};

		struct SensitiveStreamWiper {
			explicit SensitiveStreamWiper(std::stringbuf& Buffer) noexcept :
				m_Buffer(Buffer)
			{
			}

			~SensitiveStreamWiper() noexcept {
				// This borrows a non-const owned stream and wipes its current initialized buffer.
				// Previously released growth allocations are outside this cleanup guarantee.
				std::string_view const strvContents = m_Buffer.view();
				if (!strvContents.empty()) {
					SecureZeroMemory(const_cast<char*>(strvContents.data()), strvContents.size());
				}
			}

			SensitiveStreamWiper(SensitiveStreamWiper const&) = delete;
			SensitiveStreamWiper& operator=(SensitiveStreamWiper const&) = delete;

			std::stringbuf& m_Buffer;
		};

		struct IPC {
			IPC() {
				m_pStatus = nullptr;
				m_bOwnsChannel = false;
			}

			~IPC() {
				Reset();
			}

			IPC(IPC const&) = delete;
			IPC& operator=(IPC const&) = delete;

			bool Initialize(std::wstring const& wstrChannel, bool bCreate, std::string& strErrorOut) {
				if (!IsValidChannel(wstrChannel)) {
					strErrorOut = "Invalid Internet helper channel";
					return false;
				}

				// A previous successful creator attempt may still have a late launcher.
				// Borrowed/opened channels and failed uniqueness checks are never signalled here.
				if (m_bOwnsChannel && !SetEvent(m_Stop.m_hValue)) {
					strErrorOut = FormatWindowsFailure("Cannot signal previous Internet setup cancellation");
					return false;
				}

				Reset();

				Security SecurityValue;
				if (bCreate && !SecurityValue.Initialize(strErrorOut)) {
					return false;
				}

				std::wstring const wstrStatusName = wstrChannel + L"-status";
				DWORD unMappingError = ERROR_SUCCESS;
				if (bCreate) {
					const HANDLE hMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &SecurityValue.m_Attributes, PAGE_READWRITE, 0, sizeof(Status), wstrStatusName.c_str());
					const DWORD unError = GetLastError();
					m_Mapping.Reset(hMapping);

					if (!m_Mapping || (unError == ERROR_ALREADY_EXISTS)) {
						strErrorOut = "Cannot create a unique Internet status channel";
						return false;
					}
				} else {
					const HANDLE hMapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, wstrStatusName.c_str());
					unMappingError = hMapping ? ERROR_SUCCESS : GetLastError();
					m_Mapping.Reset(hMapping);
				}

				if (!m_Mapping) {
					strErrorOut = FormatWindowsFailure("Cannot open Internet status channel", unMappingError);
					return false;
				}

				m_pStatus = static_cast<PStatus>(MapViewOfFile(m_Mapping.m_hValue, FILE_MAP_READ | FILE_MAP_WRITE, 0x00, 0x00, sizeof(Status)));
				if (!m_pStatus) {
					strErrorOut = FormatWindowsFailure("Cannot map Internet status channel");
					return false;
				}

				if (bCreate) {
					// Initialization finishes before the helper is launched or any reader is published.
					std::memset(m_pStatus, 0, sizeof(Status));
					m_pStatus->m_unMagic = kStatusMagic;
				}

				if (m_pStatus->m_unMagic != kStatusMagic) {
					strErrorOut = "Invalid Internet status channel version";
					return false;
				}

				DWORD unControlError = ERROR_SUCCESS;
				auto const OpenControlEvent = [&](Handle& HandleValue, wchar_t const* pszSuffix) -> bool {
					std::wstring const wstrEventName = wstrChannel + pszSuffix;
					if (bCreate) {
						const HANDLE hEvent = CreateEventW(&SecurityValue.m_Attributes, TRUE, FALSE, wstrEventName.c_str());
						const DWORD unError = GetLastError();
						unControlError = unError;
						HandleValue.Reset(hEvent);

						if (HandleValue && (unError == ERROR_ALREADY_EXISTS)) {
							SetLastError(unError);
							return false;
						}
					} else {
						const HANDLE hEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, wstrEventName.c_str());
						unControlError = hEvent ? ERROR_SUCCESS : GetLastError();
						HandleValue.Reset(hEvent);
					}

					return static_cast<bool>(HandleValue);
				};
				if (!OpenControlEvent(m_Routing, L"-routing") || !OpenControlEvent(m_Install, L"-install") || !OpenControlEvent(m_Adapter, L"-adapter") || !OpenControlEvent(m_Stop, L"-stop")) {
					strErrorOut = FormatWindowsFailure("Cannot open Internet control events", unControlError);
					return false;
				}

				m_bOwnsChannel = bCreate;

				return true;
			}

			void Reset() noexcept {
				if (m_pStatus) {
					// Release the mapped view before its exact mapping/control handles.
					UnmapViewOfFile(m_pStatus);
					m_pStatus = nullptr;
				}

				m_Stop.Reset();
				m_Adapter.Reset();
				m_Install.Reset();
				m_Routing.Reset();
				m_Mapping.Reset();
				m_bOwnsChannel = false;
			}

			void Report(LONG nPhase, std::string_view strvMessage) noexcept {
				constexpr unsigned char kUTF8ContinuationMask = 0xC0;
				constexpr unsigned char kUTF8ContinuationMark = 0x80;

				if (!m_pStatus) {
					return;
				}

				std::size_t unMessageBytes = strvMessage.size();
				if (unMessageBytes >= sizeof(m_pStatus->m_szMessage)) {
					unMessageBytes = sizeof(m_pStatus->m_szMessage) - 1;
					while (unMessageBytes && ((static_cast<unsigned char>(strvMessage[unMessageBytes]) & kUTF8ContinuationMask) == kUTF8ContinuationMark)) {
						--unMessageBytes;
					}
				}

				// Exactly one helper thread writes. Interlocked operations provide Windows full barriers;
				// lock-free byte accesses also make concurrent same-process payload reads race-free.
				InterlockedIncrement(&m_pStatus->m_nSequence);
				for (std::size_t unIndex = 0; unIndex < sizeof(m_pStatus->m_szMessage); ++unIndex) {
					std::atomic_ref<char> Character(m_pStatus->m_szMessage[unIndex]);
					Character.store((unIndex < unMessageBytes) ? strvMessage[unIndex] : '\0', std::memory_order_relaxed);
				}

				InterlockedExchange(&m_pStatus->m_nPhase, nPhase);
				InterlockedIncrement(&m_pStatus->m_nSequence);
			}

			LONG ReadPhase() const {
				return m_pStatus ? InterlockedCompareExchange(&m_pStatus->m_nPhase, 0, 0) : -1;
			}

			std::string ReadMessage() const {
				if (!m_pStatus) {
					return "Internet helper unavailable";
				}

				char szCopy[sizeof(Status::m_szMessage)] {};
				for (std::size_t unAttempt = 0; unAttempt < kStatusReadAttempts; ++unAttempt) {
					const LONG nBefore = InterlockedCompareExchange(&m_pStatus->m_nSequence, 0, 0);
					if (nBefore & 0x01) {
						SwitchToThread();
						continue;
					}

					for (std::size_t unIndex = 0; unIndex < sizeof(szCopy); ++unIndex) {
						std::atomic_ref<char> Character(m_pStatus->m_szMessage[unIndex]);
						szCopy[unIndex] = Character.load(std::memory_order_relaxed);
					}

					if (InterlockedCompareExchange(&m_pStatus->m_nSequence, 0, 0) == nBefore) {
						return std::string(szCopy, strnlen_s(szCopy, sizeof(szCopy)));
					}
				}

				return "Internet helper is updating its status";
			}

			Handle m_Mapping;
			Handle m_Routing;
			Handle m_Install;
			Handle m_Adapter;
			Handle m_Stop;
			PStatus m_pStatus;
			bool m_bOwnsChannel;
		};

		struct StopOnFailure {
			explicit StopOnFailure(HANDLE hStop) noexcept :
				m_hStop(hStop)
			{
				m_bPending = true;
			}

			~StopOnFailure() noexcept {
				// Preserve the primary failure while signalling only this controller's own attempt.
				if (m_bPending && !SetEvent(m_hStop)) {
					OutputDebugStringA("WireSteam: cannot signal Internet setup failure cleanup.\n");
				}
			}

			StopOnFailure(StopOnFailure const&) = delete;
			StopOnFailure& operator=(StopOnFailure const&) = delete;

			void Dismiss() noexcept {
				m_bPending = false;
			}

			HANDLE m_hStop;
			bool m_bPending;
		};

		struct Launch {
			Launch() {
				m_unError = 0;
				m_hProcess = nullptr;
			}

			~Launch() {
				if (m_hProcess) {
					// Terminal best-effort release; this state owns the returned launch handle.
					CloseHandle(m_hProcess);
				}
			}

			Launch(Launch const&) = delete;
			Launch& operator=(Launch const&) = delete;

			std::promise<void> m_Done;
			DWORD m_unError;
			HANDLE m_hProcess;
		};

		bool VerifyParentProcess(DWORD unProcessID, Handle& HandleValue, std::string& strErrorOut) {
			HandleValue.Reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, unProcessID));

			if (!HandleValue) {
				strErrorOut = FormatWindowsFailure("Cannot open the normal-user Internet controller");
				return false;
			}

			std::wstring wstrPath = std::wstring(kModulePathCapacity, L'\0');
			DWORD unCount = static_cast<DWORD>(wstrPath.size());
			if (!QueryFullProcessImageNameW(HandleValue.m_hValue, 0, wstrPath.data(), &unCount)) {
				strErrorOut = FormatWindowsFailure("Cannot verify Internet controller");
				return false;
			}

			if (unCount > wstrPath.size()) {
				strErrorOut = "Cannot verify Internet controller";
				return false;
			}

			wstrPath.resize(unCount);
			std::filesystem::path const ExpectedPath = GetModulePath().parent_path() / L"WireSteam.exe";
			if (_wcsicmp(wstrPath.c_str(), ExpectedPath.c_str()) != 0) {
				strErrorOut = "The Internet controller must be the adjacent WireSteam.exe";
				return false;
			}

			const DWORD unWait = WaitForSingleObject(HandleValue.m_hValue, 0);
			if (unWait == WAIT_FAILED) {
				strErrorOut = FormatWindowsFailure("Cannot verify Internet controller");
				return false;
			}

			return unWait == WAIT_TIMEOUT;
		}

		std::string ReadInterfaceAddress(std::string const& strConfiguration) {
			constexpr std::size_t kAddressPrefixBytes = sizeof("Address = ") - 1;

			bool bInInterface = false;

			std::istringstream Lines(strConfiguration);
			SensitiveStreamWiper StreamWiper(*Lines.rdbuf());

			std::string strLine;
			SensitiveStringWiper LineWiper(strLine);
			// The caller-validated input length bounds every line before any secret is read.
			strLine.reserve(strConfiguration.size());
			for (;;) {
				LineWiper.Wipe();

				if (!std::getline(Lines, strLine)) {
					break;
				}

				if ((strLine == "[Interface]") || (strLine == "[Interface]\r")) {
					bInInterface = true;
					continue;
				}

				if (strLine.starts_with('[')) {
					bInInterface = false;
				}

				if (bInInterface && strLine.starts_with("Address = ")) {
					std::string strAddress = strLine.substr(kAddressPrefixBytes);
					while (!strAddress.empty() && ((strAddress.back() == '\r') || (strAddress.back() == ' '))) {
						strAddress.pop_back();
					}

					return strAddress;
				}
			}

			return {};
		}
	} // namespace

	struct InternetWorker::Implementation {
		Implementation() {
			m_bStopAttempted = false;
		}

		bool WaitForPhase(HANDLE hReady, LONG nPhase, unsigned int unMilliseconds, std::string& strErrorOut, std::function<bool()> const& fnCancelled = {}) {
			constexpr DWORD kPollMilliseconds = 100;

			const ULONGLONG unUntil = MakeDeadline(GetTickCount64(), unMilliseconds);
			HANDLE arrEvents[] = { hReady, m_Process.m_hValue };
			while (HasTimeRemaining(GetTickCount64(), unUntil, unMilliseconds)) {
				if (fnCancelled && fnCancelled()) {
					if (!SetEvent(m_IPC.m_Stop.m_hValue)) {
						strErrorOut = FormatWindowsFailure("Cannot signal Internet adapter cancellation");
						return false;
					}

					strErrorOut = "Internet adapter setup cancelled";
					return false;
				}

				const LONG nCurrent = m_IPC.ReadPhase();
				if (nCurrent < 0) {
					strErrorOut = m_IPC.ReadMessage();
					return false;
				}

				if ((nCurrent == nPhase) && (WaitForSingleObject(m_Process.m_hValue, 0) == WAIT_TIMEOUT)) {
					return true;
				}

				const DWORD unWait = WaitForMultipleObjects(2, arrEvents, FALSE, kPollMilliseconds);
				if (unWait == WAIT_FAILED) {
					strErrorOut = FormatWindowsFailure("Cannot wait for Internet adapter setup");
					return false;
				}

				if (unWait == (WAIT_OBJECT_0 + 1)) {
					strErrorOut = m_IPC.ReadMessage();
					if (strErrorOut.empty()) {
						strErrorOut = "The Internet helper exited before setup completed";
					}

					return false;
				}
			}

			strErrorOut = "Timed out waiting for Internet adapter setup. Resolve the administrator prompt and try again.";

			return false;
		}

		IPC m_IPC;
		Handle m_Process;
		std::string m_strName;
		bool m_bStopAttempted;
	};

	InternetWorker::InternetWorker() {
		m_pImplementation = std::make_unique<Implementation>();
	}

	InternetWorker::~InternetWorker() {
		try {
			if (!m_pImplementation->m_bStopAttempted && m_pImplementation->m_Process && IsAlive()) {
				std::string strIgnoredError;
				// Stop is best effort here; the independent helper retains its own recovery state.
				Stop(strIgnoredError);
			}
		} catch (...) {
			OutputDebugStringA("WireSteam: Internet worker destructor cleanup failed.\n");
		}
	}

	bool InternetWorker::Prepare(std::filesystem::path const& CompiledConfiguration, std::string const& strName, InternetMode Mode, std::string const& strSubnet, std::string& strErrorOut, std::function<bool()> const& fnCancelled) {
		constexpr ULONGLONG kApprovalMilliseconds = 30000;
		constexpr unsigned int kRoutingMilliseconds = 60000;
		constexpr int kGUIDCapacity = 40;
		constexpr std::chrono::milliseconds kPollInterval = std::chrono::milliseconds(100);

		if (!IsValidAdapterName(strName) || (Mode == InternetMode::Off) || m_pImplementation->m_Process) {
			strErrorOut = "Invalid Internet worker setup";
			return false;
		}

		if ((Mode == InternetMode::Client) && !InternetClientRouting::IsSupported(strErrorOut)) {
			return false;
		}

		GUID GUIDValue {};
		if (FAILED(CoCreateGuid(&GUIDValue))) {
			strErrorOut = "Cannot create Internet worker identity";
			return false;
		}

		wchar_t szGUID[kGUIDCapacity] {};
		if (!StringFromGUID2(GUIDValue, szGUID, kGUIDCapacity)) {
			strErrorOut = "Cannot create Internet worker identity";
			return false;
		}

		std::wstring wstrChannel = L"Local\\WireSteamInternet-";
		for (auto const& wchCharacter : std::wstring(szGUID)) {
			if ((wchCharacter != L'{') && (wchCharacter != L'}') && (wchCharacter != L'-')) {
				wstrChannel += static_cast<wchar_t>(towlower(wchCharacter));
			}
		}

		if (!m_pImplementation->m_IPC.Initialize(wstrChannel, true, strErrorOut)) {
			return false;
		}

		StopOnFailure FailureStop(m_pImplementation->m_IPC.m_Stop.m_hValue);
		// IPC initialization and immutable magic publication precede both launch threads/processes.
		m_pImplementation->m_strName = strName;
		std::filesystem::path const HelperPath = GetModulePath().parent_path() / L"WireSteamService.exe";
		const ULONGLONG unDeadline = MakeDeadline(GetTickCount64(), kApprovalMilliseconds);
		std::wstring wstrArguments = L"internet-worker --channel " + QuoteArgument(wstrChannel) + L" --name " + QuoteArgument(WidenASCII(strName)) + L" --config " + QuoteArgument(std::filesystem::absolute(CompiledConfiguration).wstring()) + L" --mode " + WidenASCII(InternetModeName(Mode)) + L" --parent " + std::to_wstring(GetCurrentProcessId()) + L" --deadline " + std::to_wstring(unDeadline);

		if (!strSubnet.empty()) {
			wstrArguments += L" --subnet " + QuoteArgument(WidenASCII(strSubnet));
		}

		std::shared_ptr<Launch> pLaunch = std::make_shared<Launch>();
		std::future<void> Done = pLaunch->m_Done.get_future();
		// All captures own their state; cancellation may return while the UAC dialog remains open.
		std::thread Launcher([pLaunch, HelperPath, wstrArguments]() -> void {
			try {
				const HRESULT nCOMResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
				if (FAILED(nCOMResult)) {
					pLaunch->m_unError = ERROR_DLL_INIT_FAILED;
				} else {
					SHELLEXECUTEINFOW Execute {};
					Execute.cbSize = sizeof(Execute);
					Execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
					Execute.lpVerb = L"runas";
					Execute.lpFile = HelperPath.c_str();
					Execute.lpParameters = wstrArguments.c_str();
					Execute.nShow = SW_HIDE;
					if (!ShellExecuteExW(&Execute)) {
						pLaunch->m_unError = GetLastError();
					} else {
						pLaunch->m_hProcess = Execute.hProcess;
					}

					CoUninitialize();
				}
			} catch (...) {
				pLaunch->m_unError = ERROR_UNHANDLED_EXCEPTION;
				OutputDebugStringA("WireSteam: Internet helper launcher failed.\n");
			}

			try {
				pLaunch->m_Done.set_value();
			} catch (...) {
				// The only producer must publish exactly once before its shared launch state is released.
				OutputDebugStringA("WireSteam: cannot publish Internet helper launch completion.\n");
				std::terminate();
			}
		});
		try {
			Launcher.detach();
		} catch (...) {
			// A failed detach leaves a joinable wrapper; joining can block on an unresolved UAC dialog.
			OutputDebugStringA("WireSteam: cannot detach the Internet helper launcher.\n");
			std::terminate();
		}

		while (Done.wait_for(kPollInterval) != std::future_status::ready) {
			if (fnCancelled && fnCancelled()) {
				if (!SetEvent(m_pImplementation->m_IPC.m_Stop.m_hValue)) {
					strErrorOut = FormatWindowsFailure("Cannot signal Internet setup cancellation");
					return false;
				}

				strErrorOut = "Internet setup approval cancelled";
				return false;
			}

			if (!HasTimeRemaining(GetTickCount64(), unDeadline, kApprovalMilliseconds)) {
				if (!SetEvent(m_pImplementation->m_IPC.m_Stop.m_hValue)) {
					strErrorOut = FormatWindowsFailure("Cannot signal Internet setup cancellation");
					return false;
				}

				strErrorOut = "Internet setup approval timed out; late approval will be refused";
				return false;
			}
		}

		Done.get();

		if (pLaunch->m_unError || !pLaunch->m_hProcess) {
			strErrorOut = (pLaunch->m_unError == ERROR_CANCELLED) ? "Administrator approval was cancelled" : "Cannot launch Internet helper (Windows error " + std::to_string(pLaunch->m_unError) + ")";
			return false;
		}

		m_pImplementation->m_Process.Reset(pLaunch->m_hProcess);
		pLaunch->m_hProcess = nullptr;

		const bool bReady = m_pImplementation->WaitForPhase(m_pImplementation->m_IPC.m_Routing.m_hValue, 1, kRoutingMilliseconds, strErrorOut, fnCancelled);
		if (bReady) {
			FailureStop.Dismiss();
		}

		return bReady;
	}

	bool InternetWorker::Install(std::string& strErrorOut, std::function<bool()> const& fnCancelled) {
		constexpr unsigned int kAdapterMilliseconds = 120000;

		if (!m_pImplementation->m_Process) {
			strErrorOut = "Internet helper has not been prepared";
			return false;
		}

		StopOnFailure FailureStop(m_pImplementation->m_IPC.m_Stop.m_hValue);

		if (!SetEvent(m_pImplementation->m_IPC.m_Install.m_hValue)) {
			strErrorOut = FormatWindowsFailure("Cannot signal Internet adapter installation");
			return false;
		}

		const bool bReady = m_pImplementation->WaitForPhase(m_pImplementation->m_IPC.m_Adapter.m_hValue, 2, kAdapterMilliseconds, strErrorOut, fnCancelled);
		if (bReady) {
			FailureStop.Dismiss();
		}

		return bReady;
	}

	bool InternetWorker::Stop(std::string& strErrorOut) {
		constexpr ULONGLONG kCleanupMilliseconds = 90000;
		constexpr DWORD kPollMilliseconds = 100;

		m_pImplementation->m_bStopAttempted = true;

		if (!m_pImplementation->m_Process) {
			return true;
		}

		if (!SetEvent(m_pImplementation->m_IPC.m_Stop.m_hValue)) {
			strErrorOut = FormatWindowsFailure("Cannot signal Internet helper stop");
			return false;
		}

		const ULONGLONG unUntil = MakeDeadline(GetTickCount64(), kCleanupMilliseconds);
		while (HasTimeRemaining(GetTickCount64(), unUntil, kCleanupMilliseconds)) {
			const DWORD unWait = WaitForSingleObject(m_pImplementation->m_Process.m_hValue, kPollMilliseconds);
			if (unWait == WAIT_FAILED) {
				strErrorOut = FormatWindowsFailure("Cannot wait for Internet helper cleanup");
				return false;
			}

			if (unWait == WAIT_OBJECT_0) {
				if ((m_pImplementation->m_IPC.ReadPhase() == -2) || ServiceExists(m_pImplementation->m_strName)) {
					strErrorOut = m_pImplementation->m_IPC.ReadMessage();
					return false;
				}

				return true;
			}

			if (m_pImplementation->m_IPC.ReadPhase() == -2) {
				strErrorOut = m_pImplementation->m_IPC.ReadMessage();
				return false;
			}
		}

		strErrorOut = "Internet helper is still cleaning up. The worker was not killed; use down with this tunnel name to recover.";

		return false;
	}

	bool InternetWorker::IsAlive() const {
		return m_pImplementation->m_Process && (WaitForSingleObject(m_pImplementation->m_Process.m_hValue, 0) == WAIT_TIMEOUT) && (m_pImplementation->m_IPC.ReadPhase() >= 0);
	}

	std::string InternetWorker::Message() const {
		return m_pImplementation->m_IPC.ReadMessage();
	}

	int InternetWorkerHelperMain(int nArgumentCount, wchar_t** ppszArguments) {
		constexpr ULONGLONG kApprovalMilliseconds = 30000;
		constexpr DWORD kInstallMilliseconds = 180000;
		constexpr ULONGLONG kBindMilliseconds = 15000;
		constexpr DWORD kBindPollMilliseconds = 100;
		constexpr DWORD kCleanupPollMilliseconds = 1000;

		if (!IsElevated()) {
			std::cerr << "Internet setup requires administrator approval\n";
			return 1;
		}

		std::wstring wstrChannel;
		std::filesystem::path ConfigurationPath;
		std::string strName;
		std::string strSubnet;
		InternetMode Mode = InternetMode::Off;
		DWORD unParentID = 0;
		ULONGLONG unDeadline = 0;
		std::set<std::wstring> setSeenOptions;
		try {
			const std::size_t unArgumentCount = (nArgumentCount > 0) ? static_cast<std::size_t>(nArgumentCount) : 0;
			for (std::size_t unArgumentIndex = 2; unArgumentIndex < unArgumentCount; ++unArgumentIndex) {
				std::wstring const wstrOption = ppszArguments[unArgumentIndex];
				if (!setSeenOptions.insert(wstrOption).second || (++unArgumentIndex >= unArgumentCount)) {
					throw std::runtime_error("Duplicate or missing Internet helper argument");
				}

				std::wstring const wstrValue = ppszArguments[unArgumentIndex];
				if (wstrOption == L"--channel") {
					wstrChannel = wstrValue;
				} else if (wstrOption == L"--config") {
					ConfigurationPath = wstrValue;
				} else if ((wstrOption == L"--name") || (wstrOption == L"--subnet") || (wstrOption == L"--mode")) {
					if (std::any_of(wstrValue.begin(), wstrValue.end(), [](wchar_t wchCharacter) -> bool {
						return wchCharacter > 0x7F;
					})) {
						throw std::runtime_error("Invalid Internet helper argument");
					}

					std::string strASCII;
					for (auto const& wchCharacter : wstrValue) {
						strASCII.push_back(static_cast<char>(wchCharacter));
					}

					if (wstrOption == L"--name") {
						strName = strASCII;
					} else if (wstrOption == L"--subnet") {
						strSubnet = strASCII;
					} else if (strASCII == "client") {
						Mode = InternetMode::Client;
					} else if (strASCII == "server") {
						Mode = InternetMode::Server;
					} else {
						throw std::runtime_error("Internet helper mode must be client or server");
					}
				} else if ((wstrOption == L"--parent") || (wstrOption == L"--deadline")) {
					if (wstrValue.empty() || !std::all_of(wstrValue.begin(), wstrValue.end(), [](wchar_t wchCharacter) -> bool {
						return (wchCharacter >= L'0') && (wchCharacter <= L'9');
					})) {
						throw std::runtime_error("Invalid Internet helper numeric argument");
					}

					const unsigned long long unNumber = std::stoull(wstrValue);
					if (wstrOption == L"--parent") {
						if (!unNumber || (unNumber > MAXDWORD)) {
							throw std::runtime_error("Invalid controller PID");
						}

						unParentID = static_cast<DWORD>(unNumber);
					} else {
						unDeadline = unNumber;
					}
				} else {
					throw std::runtime_error("Unknown Internet helper option");
				}
			}
		} catch (std::exception const& Exception) {
			std::cerr << Exception.what() << '\n';
			return 1;
		}

		IPC IPCValue;
		std::string strError;
		if (!IPCValue.Initialize(wstrChannel, false, strError)) {
			std::cerr << strError << '\n';
			return 1;
		}

		auto const ReportFailure = [&](std::string_view strvText) noexcept -> int {
			IPCValue.Report(-1, strvText);
			// The controller also polls phase; these readiness signals are advisory.
			SetEvent(IPCValue.m_Routing.m_hValue);
			SetEvent(IPCValue.m_Adapter.m_hValue);

			return 1;
		};

		const ULONGLONG unNow = GetTickCount64();
		if (!IsValidAdapterName(strName) || (Mode == InternetMode::Off) || !unParentID || !unDeadline || (unNow >= unDeadline) || ((unDeadline - unNow) > kApprovalMilliseconds)) {
			return ReportFailure("Internet setup approval expired or arguments are invalid");
		}

		Handle Parent;
		if (!VerifyParentProcess(unParentID, Parent, strError)) {
			return ReportFailure(strError);
		}

		if (ServiceExists(strName)) {
			return ReportFailure("A service with this tunnel name already exists; stop it before Internet setup");
		}

		if (!ConfigurationPath.is_absolute() || (ConfigurationPath.filename().wstring() != (WidenASCII(strName) + L".conf"))) {
			return ReportFailure("Invalid Internet runtime configuration path");
		}

		std::string strContents;
		SensitiveStringWiper ContentsWiper(strContents);

		if (!ReadConfigFile(ConfigurationPath, strContents, strError) || !ValidateCompiledConfig(strContents, strError, Mode, strSubnet)) {
			return ReportFailure(strError);
		}

		std::string const strAddress = ReadInterfaceAddress(strContents);
		ContentsWiper.Wipe();

		InternetClientRouting Routing;
		InternetServerGateway Server;
		bool bRecoveryHandedOff = false;
		auto Cleanup = [&]() noexcept -> bool {
			try {
				if (bRecoveryHandedOff) {
					return false;
				}

				Server.Stop();
				std::string const strServerError = Server.CleanupError();
				if (!strServerError.empty()) {
					std::string strHandoffError;
					if ((Mode == InternetMode::Server) && Server.HandOffRecovery(strHandoffError)) {
						bRecoveryHandedOff = true;
						IPCValue.Report(-2, strServerError + "; gateway recovery preserved. Run down with this tunnel name to retry cleanup.");
					} else {
						IPCValue.Report(-2, strServerError + "; gateway ownership retained while cleanup is retried");
					}

					return false;
				}

				bool bRemoved = true;

				if (ServiceExists(strName)) {
					if (!ServiceMatchesConfig(strName, ConfigurationPath, strError)) {
						// A competing installation is never ours to remove.
						IPCValue.Report(-2, "Adapter ownership verification failed; routing protection retained: " + strError);
						return false;
					}

					bRemoved = ServiceAction("remove", {}, strName, strError);
				}

				if (!bRemoved) {
					IPCValue.Report(-2, "Adapter cleanup failed: " + strError);
					return false;
				}

				Routing.Stop();

				return true;
			} catch (...) {
				// Keep live adapter/routing owners in this retry loop even if diagnostics allocate.
				IPCValue.Report(-2, "Internet cleanup failed; routing protection retained while cleanup is retried");
				return false;
			}
		};

		try {
			if (WaitForSingleObject(IPCValue.m_Stop.m_hValue, 0) == WAIT_OBJECT_0) {
				IPCValue.Report(3, "Internet setup cancelled");
				return 0;
			}

			if ((Mode == InternetMode::Client) && !Routing.Start(GetModulePath().parent_path() / L"WireSteam.exe", strAddress, strError, true)) {
				return ReportFailure(strError);
			}

			IPCValue.Report(1, (Mode == InternetMode::Client) ? "Steam underlay prepared: " + Routing.UplinkDescription() : "Internet server helper prepared");
			// Readiness is also observed by phase polling if this advisory signal fails.
			SetEvent(IPCValue.m_Routing.m_hValue);

			HANDLE arrWaitInstall[] = { IPCValue.m_Install.m_hValue, IPCValue.m_Stop.m_hValue, Parent.m_hValue };
			const DWORD unCommand = WaitForMultipleObjects(3, arrWaitInstall, FALSE, kInstallMilliseconds);
			if (unCommand != WAIT_OBJECT_0) {
				while (!Cleanup()) {
					if (bRecoveryHandedOff) {
						return 1;
					}

					Sleep(kCleanupPollMilliseconds);
				}

				IPCValue.Report(3, "Internet setup cancelled before adapter installation");
				return 0;
			}

			if ((Mode == InternetMode::Client) && !Routing.EnableGuard(strError)) {
				return ReportFailure(strError);
			}

			if (!ServiceAction("install", ConfigurationPath, strName, strError, Mode, strSubnet)) {
				std::string const strOriginal = strError;

				if (Cleanup()) {
					return ReportFailure(strOriginal);
				}
			} else {
				if (Mode == InternetMode::Server) {
					if (!Server.Start(strName, strSubnet, strError)) {
						std::string const strOriginal = strError;

						if (Cleanup()) {
							return ReportFailure(strOriginal);
						}
					}
				} else {
					bool bBound = false;
					const ULONGLONG unUntil = MakeDeadline(GetTickCount64(), kBindMilliseconds);
					while (!bBound && HasTimeRemaining(GetTickCount64(), unUntil, kBindMilliseconds) && (WaitForSingleObject(Parent.m_hValue, 0) == WAIT_TIMEOUT) && (WaitForSingleObject(IPCValue.m_Stop.m_hValue, 0) == WAIT_TIMEOUT)) {
						if (!Routing.TryBindAdapter(WidenASCII(strName), bBound, strError)) {
							break;
						}

						if (!bBound) {
							Sleep(kBindPollMilliseconds);
						}
					}

					if (!bBound) {
						std::string const strOriginal = strError.empty() ? "Timed out waiting for the Internet VPN interface" : strError;

						if (Cleanup()) {
							return ReportFailure(strOriginal);
						}
					}
				}

				if (IPCValue.ReadPhase() != -2) {
					IPCValue.Report(2, (Mode == InternetMode::Client) ? "IPv4 Internet routing active; IPv6 bypass blocked" : "Internet gateway active: " + strSubnet);
					// Readiness is also observed by phase polling if this advisory signal fails.
					SetEvent(IPCValue.m_Adapter.m_hValue);
				}
			}

			HANDLE arrStop[] = { IPCValue.m_Stop.m_hValue, Parent.m_hValue };
			std::string strRuntimeFailure;
			for (;;) {
				if (bRecoveryHandedOff) {
					return 1;
				}

				if (IPCValue.ReadPhase() != -2) {
					const DWORD unSignal = WaitForMultipleObjects(2, arrStop, FALSE, kCleanupPollMilliseconds);
					if (unSignal == WAIT_TIMEOUT) {
						if ((Mode != InternetMode::Client) || Routing.Healthy(strRuntimeFailure)) {
							continue;
						}
					}
				}

				if (Cleanup()) {
					if (!strRuntimeFailure.empty()) {
						return ReportFailure(strRuntimeFailure);
					}

					IPCValue.Report(3, "Internet adapter and owned networking state removed");
					return 0;
				}

				if (bRecoveryHandedOff) {
					return 1;
				}

				// Retain the routing guard while a failed adapter removal is
				// retried or an explicit `down` command performs recovery.
				Sleep(kCleanupPollMilliseconds);
			}
		} catch (std::exception const& Exception) {
			// The exception remains alive throughout cleanup retries; do not allocate another copy.
			char const* const pszOriginal = Exception.what();

			if (Cleanup()) {
				return ReportFailure(pszOriginal);
			}

			// A live service must keep its guard. The parent will show recovery.
			while (!Cleanup()) {
				if (bRecoveryHandedOff) {
					return 1;
				}

				Sleep(kCleanupPollMilliseconds);
			}

			return ReportFailure(pszOriginal);
		}
	}

	void RunInternetWorkerTests() {
		constexpr int kGUIDCapacity = 40;
		constexpr DWORD kRoutingWaitMilliseconds = 5000;
		constexpr DWORD kAdapterWaitMilliseconds = 2000;
		constexpr unsigned int kChildMilliseconds = 8000;
		constexpr std::size_t kConcurrentReports = 128;
		constexpr ULONGLONG kConcurrentMilliseconds = 3000;
		constexpr ULONGLONG kMaximumTickCount = std::numeric_limits<ULONGLONG>::max();

		SetLastError(ERROR_FILE_NOT_FOUND);
		const DWORD unSavedError = GetLastError();
		SetLastError(ERROR_ACCESS_DENIED);
		std::string const strSavedDiagnostic = FormatWindowsFailure("Saved IPC error", unSavedError);
		if (strSavedDiagnostic != ("Saved IPC error (Windows error " + std::to_string(ERROR_FILE_NOT_FOUND) + ")")) {
			throw std::runtime_error("Internet worker error formatting lost the captured Windows status");
		}

		if ((MakeDeadline(123, 77) != 200) || (MakeDeadline(kMaximumTickCount - 1, 10) != kMaximumTickCount) || (MakeDeadline(kMaximumTickCount, 0) != kMaximumTickCount)) {
			throw std::runtime_error("Internet worker deadline overflow protection failed");
		}

		if (!HasTimeRemaining(100, 200, 100) || !HasTimeRemaining(199, 200, 100) || HasTimeRemaining(200, 200, 100) || HasTimeRemaining(100, 101, 0) || !HasTimeRemaining(kMaximumTickCount - 1, kMaximumTickCount, 10) || HasTimeRemaining(0, kMaximumTickCount, 10) || HasTimeRemaining(201, 200, 100) || HasTimeRemaining(kMaximumTickCount, kMaximumTickCount, 10)) {
			throw std::runtime_error("Internet worker deadline wrap protection failed");
		}

		wchar_t const* const pszValid = L"Local\\WireSteamInternet-0123456789abcdef0123456789abcdef";
		if (!IsValidChannel(pszValid) || IsValidChannel(L"Global\\WireSteamInternet-0123456789abcdef0123456789abcdef") || IsValidChannel(L"Local\\WireSteamInternet-../unsafe") || !IsValidAdapterName("WireSteam-ui-test") || IsValidAdapterName("WireSteam-../unsafe")) {
			throw std::runtime_error("Internet worker identity validation failed");
		}

		if (ReadInterfaceAddress("[Interface]\nAddress = 10.77.0.2/32\n[Peer]\nAllowedIPs = 0.0.0.0/1\n") != "10.77.0.2/32") {
			throw std::runtime_error("Internet worker address selection failed");
		}

		std::string const strLongConfiguration = "[Interface]\r\nPrivateKey = " + std::string(4096, 'K') + "\r\nAddress = 10.77.0.2/32  \r\n[Peer]\r\nAddress = ignored\r\n";
		if (ReadInterfaceAddress(strLongConfiguration) != "10.77.0.2/32") {
			throw std::runtime_error("Internet worker address selection after a longer line failed");
		}

		Handle FailureEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		if (!FailureEvent) {
			throw std::runtime_error(FormatWindowsFailure("Cannot create Internet worker failure signal test"));
		}

		try {
			StopOnFailure FailureStop(FailureEvent.m_hValue);
			throw std::runtime_error("Owned Internet worker cancellation callback failure");
		} catch (std::runtime_error const& Exception) {
			if ((std::strcmp(Exception.what(), "Owned Internet worker cancellation callback failure") != 0) || (WaitForSingleObject(FailureEvent.m_hValue, 0) != WAIT_OBJECT_0)) {
				throw std::runtime_error("Internet worker failure signal lost its primary exception");
			}
		}

		if (!ResetEvent(FailureEvent.m_hValue)) {
			throw std::runtime_error(FormatWindowsFailure("Cannot reset Internet worker failure signal test"));
		}

		{
			StopOnFailure FailureStop(FailureEvent.m_hValue);
			FailureStop.Dismiss();
		}

		if (WaitForSingleObject(FailureEvent.m_hValue, 0) != WAIT_TIMEOUT) {
			throw std::runtime_error("Internet worker committed setup was cancelled");
		}

		GUID GUIDValue {};
		if (FAILED(CoCreateGuid(&GUIDValue))) {
			throw std::runtime_error("Cannot create IPC test identity");
		}

		wchar_t szGUID[kGUIDCapacity] {};
		if (!StringFromGUID2(GUIDValue, szGUID, kGUIDCapacity)) {
			throw std::runtime_error("Cannot create IPC test identity");
		}

		std::wstring wstrChannel = L"Local\\WireSteamInternet-";
		for (auto const& wchCharacter : std::wstring(szGUID)) {
			if ((wchCharacter != L'{') && (wchCharacter != L'}') && (wchCharacter != L'-')) {
				wstrChannel += static_cast<wchar_t>(towlower(wchCharacter));
			}
		}

		IPC IPCValue;
		std::string strError;
		if (!IPCValue.Initialize(wstrChannel, true, strError)) {
			throw std::runtime_error(strError);
		}

		// Reinitialization cancels only a successfully created prior attempt and releases its mapped view.
		std::wstring const wstrPreviousChannel = wstrChannel;
		HANDLE hPreviousStop = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), IPCValue.m_Stop.m_hValue, GetCurrentProcess(), &hPreviousStop, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
			throw std::runtime_error(FormatWindowsFailure("Cannot retain IPC retry test stop event"));
		}

		Handle PreviousStop(hPreviousStop);
		wstrChannel.back() = (wstrChannel.back() == L'0') ? L'1' : L'0';
		if (!IPCValue.Initialize(wstrChannel, true, strError) || (WaitForSingleObject(PreviousStop.m_hValue, 0) != WAIT_OBJECT_0)) {
			throw std::runtime_error("Internet worker IPC retry did not stop its previous attempt");
		}

		std::wstring const wstrPreviousMappingName = wstrPreviousChannel + L"-status";
		const HANDLE hPreviousMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, wstrPreviousMappingName.c_str());
		const DWORD unPreviousMappingError = GetLastError();
		Handle PreviousMapping(hPreviousMapping);
		if (PreviousMapping || (unPreviousMappingError != ERROR_FILE_NOT_FOUND)) {
			throw std::runtime_error("Internet worker IPC retry retained its previous mapped view");
		}

		{
			IPC BorrowedIPC;
			std::string const strExpectedMappingError = "Cannot open Internet status channel (Windows error " + std::to_string(ERROR_FILE_NOT_FOUND) + ")";
			if (!BorrowedIPC.Initialize(wstrChannel, false, strError) || BorrowedIPC.Initialize(wstrPreviousChannel, false, strError) || (strError != strExpectedMappingError) || (WaitForSingleObject(IPCValue.m_Stop.m_hValue, 0) != WAIT_TIMEOUT)) {
				throw std::runtime_error("Internet worker borrowed IPC retry cancelled its creator");
			}
		}

		// Initialization precedes thread publication; exactly one writer updates complete atomic-byte records.
		std::string const strFirst = std::string(sizeof(Status::m_szMessage) - 1, 'A');
		std::string const strSecond = std::string(sizeof(Status::m_szMessage) - 1, 'B');
		IPCValue.Report(0, strFirst);

		{
			std::atomic<bool> bFinished;
			std::atomic<bool> bFailed;
			std::jthread Writer([&](std::stop_token StopToken) -> void {
				try {
					for (std::size_t unReport = 0; (unReport < kConcurrentReports) && !StopToken.stop_requested(); ++unReport) {
						IPCValue.Report(0, (unReport & 0x01) ? strFirst : strSecond);
					}
				} catch (...) {
					bFailed.store(true, std::memory_order_relaxed);
				}

				// Publishes completion and failure after the payload writes.
				bFinished.store(true, std::memory_order_release);
			});
			const ULONGLONG unUntil = MakeDeadline(GetTickCount64(), kConcurrentMilliseconds);
			while (!bFinished.load(std::memory_order_acquire) && HasTimeRemaining(GetTickCount64(), unUntil, kConcurrentMilliseconds)) {
				std::string const strMessage = IPCValue.ReadMessage();
				if ((strMessage != strFirst) && (strMessage != strSecond) && (strMessage != "Internet helper is updating its status")) {
					throw std::runtime_error("Internet worker status snapshot was torn");
				}
			}

			Writer.request_stop();
			Writer.join();

			if (bFailed.load(std::memory_order_relaxed) || !bFinished.load(std::memory_order_acquire)) {
				throw std::runtime_error("Internet worker status writer failed");
			}
		}

		std::string const strUTF8Prefix = std::string(sizeof(Status::m_szMessage) - 2, 'a');
		IPCValue.Report(0, strUTF8Prefix + "\xE2\x82\xAC");

		if (IPCValue.ReadMessage() != strUTF8Prefix) {
			throw std::runtime_error("Internet worker status truncation split UTF-8");
		}

		std::filesystem::path const HelperPath = GetModulePath().parent_path() / L"WireSteamService.exe";
		std::wstring const wstrArguments = L"internet-ipc-test " + QuoteArgument(wstrChannel) + L" " + std::to_wstring(GetCurrentProcessId());
		std::future<ProcessResult> Child = std::async(std::launch::async, [HelperPath, wstrArguments]() -> ProcessResult {
			return RunProcess(HelperPath, wstrArguments, {}, kChildMilliseconds);
		});

		struct StopTest {
			explicit StopTest(HANDLE hEvent) :
				m_hEvent(hEvent)
			{
			}

			~StopTest() {
				// Best-effort terminal signal; RunProcess also bounds this owned child to eight seconds.
				SetEvent(m_hEvent);
			}

			StopTest(StopTest const&) = delete;
			StopTest& operator=(StopTest const&) = delete;

			HANDLE m_hEvent;
		} StopTestValue(IPCValue.m_Stop.m_hValue);

		if ((WaitForSingleObject(IPCValue.m_Routing.m_hValue, kRoutingWaitMilliseconds) != WAIT_OBJECT_0) || (IPCValue.ReadPhase() != 1) || (IPCValue.ReadMessage() != "IPC ROUTING READY")) {
			throw std::runtime_error("Cross-process Internet routing readiness IPC failed");
		}

		if (!SetEvent(IPCValue.m_Install.m_hValue)) {
			throw std::runtime_error(FormatWindowsFailure("Cannot signal IPC test installation"));
		}

		if ((WaitForSingleObject(IPCValue.m_Adapter.m_hValue, kAdapterWaitMilliseconds) != WAIT_OBJECT_0) || (IPCValue.ReadPhase() != 2) || (IPCValue.ReadMessage() != "IPC ADAPTER READY")) {
			throw std::runtime_error("Cross-process Internet installation IPC failed");
		}

		if (!SetEvent(IPCValue.m_Stop.m_hValue)) {
			throw std::runtime_error(FormatWindowsFailure("Cannot signal IPC test stop"));
		}

		ProcessResult const Result = Child.get();
		if (Result.m_nExitCode || !Result.m_strError.empty() || (IPCValue.ReadPhase() != 3)) {
			throw std::runtime_error("Cross-process Internet stop IPC failed");
		}

		std::cout << "PASS Internet worker: protected cross-process readiness/install/stop IPC and validated identities (no elevation/network changes)\n";
	}

	int InternetIpcTestHelperMain(int nArgumentCount, wchar_t** ppszArguments) {
		constexpr std::size_t kProcessIDCharacters = 10;
		constexpr DWORD kWaitMilliseconds = 4000;

		// Deliberately unprivileged: this test exercises only the private IPC
		// contract. It has no service, routing, firewall, NAT or config actions.
		if ((nArgumentCount != 4) || !IsValidChannel(ppszArguments[2])) {
			return 1;
		}

		std::wstring const wstrValue = ppszArguments[3];
		if (wstrValue.empty() || (wstrValue.size() > kProcessIDCharacters) || !std::all_of(wstrValue.begin(), wstrValue.end(), [](wchar_t wchCharacter) -> bool {
			return (wchCharacter >= L'0') && (wchCharacter <= L'9');
		})) {
			return 1;
		}

		const unsigned long long unProcessID = _wcstoui64(wstrValue.c_str(), nullptr, 10);
		if (!unProcessID || (unProcessID > MAXDWORD)) {
			return 1;
		}

		Handle Parent;
		std::string strError;
		if (!VerifyParentProcess(static_cast<DWORD>(unProcessID), Parent, strError)) {
			return 1;
		}

		IPC IPCValue;
		if (!IPCValue.Initialize(ppszArguments[2], false, strError)) {
			return 1;
		}

		// The creator initialized this record before process creation; this thread is its sole writer.
		IPCValue.Report(1, "IPC ROUTING READY");

		if (!SetEvent(IPCValue.m_Routing.m_hValue)) {
			return 1;
		}

		if (WaitForSingleObject(IPCValue.m_Install.m_hValue, kWaitMilliseconds) != WAIT_OBJECT_0) {
			return 1;
		}

		IPCValue.Report(2, "IPC ADAPTER READY");

		if (!SetEvent(IPCValue.m_Adapter.m_hValue)) {
			return 1;
		}

		if (WaitForSingleObject(IPCValue.m_Stop.m_hValue, kWaitMilliseconds) != WAIT_OBJECT_0) {
			return 1;
		}

		IPCValue.Report(3, "IPC STOPPED");

		return 0;
	}
} // namespace ws
