
// General
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <shellapi.h>
#include <tlhelp32.h>

// C++
#include <cstddef>
#include <cstring>
#include <utility>

// STL
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
	constexpr std::array<std::wstring_view, 7> kPayloadNames {
		L"WireSteam.UI.exe", L"WireSteam.exe", L"WireSteamService.exe",
		L"WireSteamTestClient.exe", L"WireSteamTestServer.exe",
		L"WireSteamSpeedClient.exe", L"WireSteamSpeedServer.exe"
	};
	constexpr std::wstring_view kServicePrefix = L"WireGuardTunnel$WireSteam-";
	constexpr std::wstring_view kQuitInstructions =
		L"Quit WireSteam using QUIT WIRESTEAM in its tray menu and wait for adapter cleanup to finish. "
		L"Stop CLI tunnels with Ctrl+C and wait for them to exit. Then retry setup. "
		L"If an adapter remains, run WireSteam.exe down --name <tunnel-name> and approve cleanup.";
	constexpr DWORD kMaximumPathCharacters = 32768;
	constexpr wchar_t kWireGuardUpgradeCode[] = L"{5E5A1DA5-BA36-404D-92EC-41050D1C799C}";
	constexpr wchar_t kWireGuardExecutableComponent[] = L"{C3508D23-3362-47CE-9220-321BDB1A1ACC}";
	constexpr std::size_t kProductCodeCharacters = 39;
	constexpr DWORD kMaximumRelatedProducts = 1024;
	constexpr DWORD kProcessStopMilliseconds = 10'000;
	constexpr DWORD kHelperCleanupMilliseconds = 60'000;
	constexpr DWORD kServiceStopMilliseconds = 15'000;
	constexpr DWORD kServicePollMilliseconds = 100;

	enum class ProcessAction {
		Check = 0,
		Terminate,
		WaitForCleanup
	};

	struct LocalArgumentsDeleter {
		void operator()(wchar_t** ppszArguments) const {
			LocalFree(ppszArguments);
		}
	};

	struct Handle {
		explicit Handle(HANDLE hHandle = nullptr) :
			m_hHandle(hHandle)
		{
		}

		~Handle() {
			if (m_hHandle && (m_hHandle != INVALID_HANDLE_VALUE)) {
				CloseHandle(m_hHandle);
			}
		}

		Handle(Handle const&) = delete;
		Handle& operator=(Handle const&) = delete;

		explicit operator bool() const {
			return m_hHandle && (m_hHandle != INVALID_HANDLE_VALUE);
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

	struct RegistryHandle {
		RegistryHandle() {
			m_hHandle = nullptr;
		}

		~RegistryHandle() {
			if (m_hHandle) {
				RegCloseKey(m_hHandle);
			}
		}

		RegistryHandle(RegistryHandle const&) = delete;
		RegistryHandle& operator=(RegistryHandle const&) = delete;

		HKEY m_hHandle;
	};

	bool AreOrdinallyEqual(std::wstring_view strvLeft, std::wstring_view strvRight) {
		return (strvLeft.size() == strvRight.size()) && (CompareStringOrdinal(strvLeft.data(), static_cast<int>(strvLeft.size()), strvRight.data(), static_cast<int>(strvRight.size()), TRUE) == CSTR_EQUAL);
	}

	bool IsPayloadName(std::wstring_view strvName) {
		return std::any_of(kPayloadNames.begin(), kPayloadNames.end(), [&](std::wstring_view strvExpected) -> bool {
			return AreOrdinallyEqual(strvName, strvExpected);
		});
	}

	bool IsWireSteamService(std::wstring_view strvName) {
		return (strvName.size() >= kServicePrefix.size()) && AreOrdinallyEqual(strvName.substr(0, kServicePrefix.size()), kServicePrefix);
	}

	bool IsFullRemoval(std::wstring_view strvRemove) {
		while (!strvRemove.empty() && ((strvRemove.front() == L' ') || (strvRemove.front() == L'\t'))) {
			strvRemove.remove_prefix(1);
		}

		while (!strvRemove.empty() && ((strvRemove.back() == L' ') || (strvRemove.back() == L'\t'))) {
			strvRemove.remove_suffix(1);
		}

		return AreOrdinallyEqual(strvRemove, L"ALL");
	}

	bool IsSupportedWindows(DWORD unMajor, DWORD unMinor, DWORD unBuild, BYTE unProductType) {
		return (unProductType == VER_NT_WORKSTATION) && ((unMajor > 10) || ((unMajor == 10) && ((unMinor > 0) || (unBuild >= 19045))));
	}

	std::wstring FormatWindowsFailure(std::wstring_view strvOperation, DWORD unCode) {
		return std::wstring(strvOperation) + L" (Windows error " + std::to_wstring(unCode) + L").";
	}

	UINT ReportFailure(MSIHANDLE hSession, wchar_t const* pszMessage) noexcept {
		MSIHANDLE hRecord = MsiCreateRecord(1);
		if (hRecord) {
			if ((MsiRecordSetStringW(hRecord, 0, L"[1]") == ERROR_SUCCESS) && (MsiRecordSetStringW(hRecord, 1, pszMessage) == ERROR_SUCCESS)) {
				MsiProcessMessage(hSession, INSTALLMESSAGE_ERROR, hRecord);
			}

			MsiCloseHandle(hRecord);
		}

		return ERROR_INSTALL_FAILURE;
	}

	void ReportInformation(MSIHANDLE hSession, wchar_t const* pszMessage) noexcept {
		MSIHANDLE hRecord = MsiCreateRecord(1);
		if (hRecord) {
			if ((MsiRecordSetStringW(hRecord, 0, L"[1]") == ERROR_SUCCESS) && (MsiRecordSetStringW(hRecord, 1, pszMessage) == ERROR_SUCCESS)) {
				MsiProcessMessage(hSession, INSTALLMESSAGE_INFO, hRecord);
			}

			MsiCloseHandle(hRecord);
		}
	}

	bool ReadProperty(MSIHANDLE hSession, wchar_t const* pszName, std::wstring& wstrValueOut, std::wstring& wstrErrorOut) {
		wchar_t szEmpty[1] {};
		DWORD unCount = 0;
		UINT unCode = MsiGetPropertyW(hSession, pszName, szEmpty, &unCount);
		if ((unCode == ERROR_SUCCESS) && (unCount == 0)) {
			wstrValueOut.clear();
			return true;
		}

		if (unCode != ERROR_MORE_DATA) {
			wstrErrorOut = FormatWindowsFailure(L"Setup could not read its installation settings", unCode);
			return false;
		}

		if (unCount >= kMaximumPathCharacters) {
			wstrErrorOut = L"An installation setting exceeds the supported Windows path length.";
			return false;
		}

		std::vector<wchar_t> vecBuffer(static_cast<std::size_t>(unCount) + 1);
		unCount = static_cast<DWORD>(vecBuffer.size());
		unCode = MsiGetPropertyW(hSession, pszName, vecBuffer.data(), &unCount);

		if (unCode != ERROR_SUCCESS) {
			wstrErrorOut = FormatWindowsFailure(L"Setup could not read its installation settings", unCode);
			return false;
		}

		wstrValueOut.assign(vecBuffer.data(), unCount);

		return true;
	}

	std::wstring RemoveExtendedPrefix(std::wstring wstrValue) {
		std::replace(wstrValue.begin(), wstrValue.end(), L'/', L'\\');

		if ((wstrValue.size() >= 8) && AreOrdinallyEqual(std::wstring_view(wstrValue).substr(0, 8), L"\\\\?\\UNC\\")) {
			return L"\\\\" + wstrValue.substr(8);
		}

		if ((wstrValue.size() >= 7) && wstrValue.starts_with(L"\\\\?\\") && (wstrValue[5] == L':') && (wstrValue[6] == L'\\')) {
			wstrValue.erase(0, 4);
		}

		return wstrValue;
	}

	bool IsAbsoluteDOSPath(std::wstring_view strvPath) {
		if ((strvPath.size() >= 3) && (((strvPath[0] >= L'A') && (strvPath[0] <= L'Z')) || ((strvPath[0] >= L'a') && (strvPath[0] <= L'z'))) && (strvPath[1] == L':') && (strvPath[2] == L'\\')) {
			return true;
		}

		if (!strvPath.starts_with(L"\\\\") || (strvPath.size() < 5) || (strvPath[2] == L'?') || (strvPath[2] == L'.')) {
			return false;
		}

		const std::size_t unSeparator = strvPath.find(L'\\', 2);

		return (unSeparator != std::wstring_view::npos) && (unSeparator > 2) && ((unSeparator + 1) < strvPath.size()) && (strvPath[unSeparator + 1] != L'\\');
	}

	void TrimDirectorySeparator(std::wstring& wstrPath) {
		while ((wstrPath.size() > 3) && (wstrPath.back() == L'\\')) {
			wstrPath.pop_back();
		}
	}

	bool NormalizeFullPath(std::wstring_view strvInput, std::wstring& wstrOutputOut, std::wstring& wstrErrorOut) {
		std::wstring wstrPath = RemoveExtendedPrefix(std::wstring(strvInput));
		if ((wstrPath.find(L'\0') != std::wstring::npos) || !IsAbsoluteDOSPath(wstrPath)) {
			wstrErrorOut = L"Setup could not validate an absolute installation directory.";
			return false;
		}

		DWORD unCount = GetFullPathNameW(wstrPath.c_str(), 0, nullptr, nullptr);
		if (!unCount) {
			wstrErrorOut = FormatWindowsFailure(L"Setup could not normalize an installation path", GetLastError());
			return false;
		}

		if (unCount > kMaximumPathCharacters) {
			wstrErrorOut = L"An installation path exceeds the supported Windows path length.";
			return false;
		}

		std::vector<wchar_t> vecBuffer(unCount);
		DWORD unWritten = GetFullPathNameW(wstrPath.c_str(), unCount, vecBuffer.data(), nullptr);
		if (!unWritten || (unWritten >= unCount)) {
			wstrErrorOut = L"Setup could not normalize an installation path reliably.";
			return false;
		}

		wstrOutputOut.assign(vecBuffer.data(), unWritten);
		TrimDirectorySeparator(wstrOutputOut);

		return true;
	}

	bool IsPathUnderDirectory(std::wstring_view strvImage, std::wstring_view strvDirectory) {
		return !strvDirectory.empty() && (strvImage.size() > strvDirectory.size()) && AreOrdinallyEqual(strvImage.substr(0, strvDirectory.size()), strvDirectory) && ((strvDirectory.back() == L'\\') || (strvImage[strvDirectory.size()] == L'\\'));
	}

	std::wstring BuildExtendedAPIPath(std::wstring_view strvPath) {
		// The MSI host's manifest must not impose MAX_PATH on read-only checks.
		return strvPath.starts_with(L"\\\\") ? (L"\\\\?\\UNC\\" + std::wstring(strvPath.substr(2))) : (L"\\\\?\\" + std::wstring(strvPath));
	}

	bool ResolveCanonicalPath(std::wstring const& wstrPath, bool bAllowMissing, std::wstring& wstrOutputOut, std::wstring& wstrErrorOut) {
		std::wstring wstrAPIPath = BuildExtendedAPIPath(wstrPath);
		Handle File(CreateFileW(wstrAPIPath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
		if (!File) {
			DWORD unCode = GetLastError();
			if (bAllowMissing && ((unCode == ERROR_FILE_NOT_FOUND) || (unCode == ERROR_PATH_NOT_FOUND))) {
				wstrOutputOut = wstrPath;
				return true;
			}

			wstrErrorOut = FormatWindowsFailure(L"Setup could not verify an installation path", unCode);
			return false;
		}

		DWORD unCount = GetFinalPathNameByHandleW(File.m_hHandle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (!unCount) {
			wstrErrorOut = FormatWindowsFailure(L"Setup could not resolve an installation path", GetLastError());
			return false;
		}

		if (unCount > kMaximumPathCharacters) {
			wstrErrorOut = L"A resolved installation path exceeds the supported Windows path length.";
			return false;
		}

		std::vector<wchar_t> vecBuffer(static_cast<std::size_t>(unCount) + 1);
		DWORD unWritten = GetFinalPathNameByHandleW(File.m_hHandle, vecBuffer.data(), static_cast<DWORD>(vecBuffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (!unWritten || (unWritten >= vecBuffer.size())) {
			wstrErrorOut = L"Setup could not resolve an installation path reliably.";
			return false;
		}

		return NormalizeFullPath(std::wstring_view(vecBuffer.data(), unWritten), wstrOutputOut, wstrErrorOut);
	}

	bool AddDirectory(std::wstring_view strvProperty, std::vector<std::wstring>& vecDirectories, std::wstring& wstrErrorOut) {
		if (strvProperty.empty()) {
			return true;
		}

		std::wstring wstrFull;
		std::wstring wstrCanonical;
		if (!NormalizeFullPath(strvProperty, wstrFull, wstrErrorOut) || !ResolveCanonicalPath(wstrFull, true, wstrCanonical, wstrErrorOut)) {
			return false;
		}

		for (auto const* pPath : { &wstrFull, &wstrCanonical }) {
			if (std::none_of(vecDirectories.begin(), vecDirectories.end(), [&](std::wstring const& wstrItem) -> bool {
				return AreOrdinallyEqual(wstrItem, *pPath);
			})) {
				vecDirectories.push_back(*pPath);
			}
		}

		return true;
	}

	bool IsInsideInstallationDirectories(std::wstring_view strvImage, std::vector<std::wstring> const& vecDirectories) {
		return std::any_of(vecDirectories.begin(), vecDirectories.end(), [&](std::wstring const& wstrDirectory) -> bool {
			return IsPathUnderDirectory(strvImage, wstrDirectory);
		});
	}

	bool ReadNativeProgramFiles(std::wstring& wstrPathOut, std::wstring& wstrErrorOut) {
		DWORD unBytes = kMaximumPathCharacters * sizeof(wchar_t);
		std::vector<wchar_t> vecBuffer(kMaximumPathCharacters);
		LSTATUS nCode = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion", L"ProgramFilesDir", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, vecBuffer.data(), &unBytes);
		if ((nCode != ERROR_SUCCESS) || (unBytes < sizeof(wchar_t)) || (unBytes > (vecBuffer.size() * sizeof(wchar_t)))) {
			wstrErrorOut = FormatWindowsFailure(L"Setup could not locate native Program Files", nCode);
			return false;
		}

		return NormalizeFullPath(vecBuffer.data(), wstrPathOut, wstrErrorOut);
	}

	bool IsExecutableFile(std::wstring const& wstrPath) {
		DWORD unBinaryType = 0;
		const DWORD unAttributes = GetFileAttributesW(BuildExtendedAPIPath(wstrPath).c_str());

		return (unAttributes != INVALID_FILE_ATTRIBUTES) && ((unAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) && GetBinaryTypeW(wstrPath.c_str(), &unBinaryType) && (unBinaryType == SCS_64BIT_BINARY);
	}

	bool ReadWireGuardInstallation(std::wstring& wstrPathOut, std::wstring& wstrErrorOut) {
		std::wstring wstrProgramFiles;
		if (!ReadNativeProgramFiles(wstrProgramFiles, wstrErrorOut)) {
			return false;
		}

		std::wstring const wstrExpected = wstrProgramFiles + L"\\WireGuard\\wireguard.exe";
		std::wstring const wstrTools = wstrProgramFiles + L"\\WireGuard\\wg.exe";
		wstrErrorOut = L"WireSteam requires the official x64 WireGuard for Windows installation. Install or repair WireGuard at " + wstrProgramFiles + L"\\WireGuard, then retry setup. Both wireguard.exe and wg.exe must be present. Download: https://www.wireguard.com/install/";
		if (!IsExecutableFile(wstrExpected) || !IsExecutableFile(wstrTools)) {
			return false;
		}

		std::wstring wstrCanonicalExpected;
		if (!ResolveCanonicalPath(wstrExpected, false, wstrCanonicalExpected, wstrErrorOut)) {
			return false;
		}

		for (DWORD unIndex = 0; unIndex < kMaximumRelatedProducts; ++unIndex) {
			wchar_t szProduct[kProductCodeCharacters] {};
			const UINT unCode = MsiEnumRelatedProductsW(kWireGuardUpgradeCode, 0, unIndex, szProduct);
			if (unCode == ERROR_NO_MORE_ITEMS) {
				break;
			}

			if (unCode != ERROR_SUCCESS) {
				wstrErrorOut = FormatWindowsFailure(L"Setup could not verify the installed WireGuard product", unCode);
				return false;
			}

			std::vector<wchar_t> vecPath(kMaximumPathCharacters);
			DWORD unCount = static_cast<DWORD>(vecPath.size());
			if (MsiGetComponentPathW(szProduct, kWireGuardExecutableComponent, vecPath.data(), &unCount) != INSTALLSTATE_LOCAL) {
				continue;
			}

			std::wstring wstrFull;
			std::wstring wstrCanonical;
			if (!NormalizeFullPath(vecPath.data(), wstrFull, wstrErrorOut) || !ResolveCanonicalPath(wstrFull, false, wstrCanonical, wstrErrorOut)) {
				return false;
			}

			if (AreOrdinallyEqual(wstrCanonical, wstrCanonicalExpected)) {
				wstrPathOut = wstrCanonicalExpected;
				return true;
			}
		}

		wstrErrorOut = L"Setup could not find a registered official x64 WireGuard installation in native Program Files\\WireGuard. Install or repair WireGuard for Windows, then retry setup. Download: https://www.wireguard.com/install/";

		return false;
	}

	bool AddRelatedProductDirectories(std::wstring_view strvProducts, std::vector<std::wstring>& vecDirectories, std::wstring& wstrErrorOut) {
		while (!strvProducts.empty()) {
			const std::size_t unSeparator = strvProducts.find(L';');
			std::wstring const wstrProduct = std::wstring(strvProducts.substr(0, unSeparator));
			std::vector<wchar_t> vecPath(kMaximumPathCharacters);
			DWORD unCount = static_cast<DWORD>(vecPath.size());
			const UINT unCode = MsiGetProductInfoW(wstrProduct.c_str(), L"InstallLocation", vecPath.data(), &unCount);
			if ((unCode != ERROR_SUCCESS) || (unCount == 0)) {
				wstrErrorOut = L"Setup could not determine an installed WireSteam version's directory. Repair or remove that version in Windows Installed apps, then retry setup.";
				return false;
			}

			if (!AddDirectory(std::wstring_view(vecPath.data(), unCount), vecDirectories, wstrErrorOut)) {
				return false;
			}

			if (unSeparator == std::wstring_view::npos) {
				break;
			}

			strvProducts.remove_prefix(unSeparator + 1);
		}

		return true;
	}

	std::wstring SerializeDirectories(std::vector<std::wstring> const& vecDirectories) {
		std::wstring wstrData;
		for (auto const& wstrDirectory : vecDirectories) {
			wstrData += std::to_wstring(wstrDirectory.size()) + L":" + wstrDirectory;
		}

		return wstrData;
	}

	bool DeserializeDirectories(std::wstring_view strvData, std::vector<std::wstring>& vecDirectoriesOut, std::wstring& wstrErrorOut) {
		std::vector<std::wstring> vecDirectories;
		while (!strvData.empty()) {
			const std::size_t unSeparator = strvData.find(L':');
			if ((unSeparator == std::wstring_view::npos) || (unSeparator == 0)) {
				wstrErrorOut = L"Setup received invalid WireSteam shutdown directory data.";
				return false;
			}

			std::size_t unLength = 0;
			for (auto const& wchCharacter : strvData.substr(0, unSeparator)) {
				if ((wchCharacter < L'0') || (wchCharacter > L'9') || (unLength > (kMaximumPathCharacters / 10))) {
					wstrErrorOut = L"Setup received invalid WireSteam shutdown directory data.";
					return false;
				}

				unLength = (unLength * 10) + static_cast<std::size_t>(wchCharacter - L'0');
			}

			strvData.remove_prefix(unSeparator + 1);
			if ((unLength == 0) || (unLength > kMaximumPathCharacters) || (unLength > strvData.size()) || !AddDirectory(strvData.substr(0, unLength), vecDirectories, wstrErrorOut)) {
				wstrErrorOut = L"Setup received invalid WireSteam shutdown directory data.";
				return false;
			}

			strvData.remove_prefix(unLength);
		}

		if (vecDirectories.empty()) {
			wstrErrorOut = L"Setup did not receive a WireSteam installation directory for shutdown.";
			return false;
		}

		vecDirectoriesOut = std::move(vecDirectories);

		return true;
	}

	std::wstring DescribeWindowsVersion(OSVERSIONINFOEXW const& Version) {
		return std::to_wstring(Version.dwMajorVersion) + L"." + std::to_wstring(Version.dwMinorVersion) + L"." + std::to_wstring(Version.dwBuildNumber) + L"; platform=" + std::to_wstring(Version.dwPlatformId) + L"; productType=" + std::to_wstring(Version.wProductType) + L"; structureBytes=" + std::to_wstring(Version.dwOSVersionInfoSize);
	}

	constexpr DWORD GetNativeBuildNumber(DWORD unBuild) {
		return unBuild & 0xFFFF;
	}

	bool IsNativeProductType(DWORD unProductType) {
		return (unProductType >= VER_NT_WORKSTATION) && (unProductType <= VER_NT_SERVER);
	}

	bool ReadWindowsVersion(OSVERSIONINFOEXW& VersionOut, std::wstring& wstrErrorOut, OSVERSIONINFOEXW* pCompatibilityReportOut = nullptr) {
		using fnReadVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
		using fnReadNativeNumbers = void(WINAPI*)(DWORD*, DWORD*, DWORD*);
		using fnReadNativeProductType = BOOLEAN(WINAPI*)(DWORD*);
		HMODULE hModule = GetModuleHandleW(L"ntdll.dll");
		FARPROC fnVersionAddress = hModule ? GetProcAddress(hModule, "RtlGetVersion") : nullptr;
		FARPROC fnNativeNumbersAddress = hModule ? GetProcAddress(hModule, "RtlGetNtVersionNumbers") : nullptr;
		FARPROC fnProductTypeAddress = hModule ? GetProcAddress(hModule, "RtlGetNtProductType") : nullptr;
		VersionOut = {};
		VersionOut.dwOSVersionInfoSize = sizeof(VersionOut);

		if (pCompatibilityReportOut) {
			*pCompatibilityReportOut = {};
		}

		if (!fnNativeNumbersAddress || !fnProductTypeAddress) {
			wstrErrorOut = L"Setup could not verify the native Windows version. Update Windows and retry setup.";
			return false;
		}

		fnReadNativeNumbers fnGetNativeNumbers = nullptr;
		fnReadNativeProductType fnGetNativeProductType = nullptr;

		static_assert(sizeof(fnGetNativeNumbers) == sizeof(fnNativeNumbersAddress));
		static_assert(sizeof(fnGetNativeProductType) == sizeof(fnProductTypeAddress));

		std::memcpy(&fnGetNativeNumbers, &fnNativeNumbersAddress, sizeof(fnGetNativeNumbers));
		std::memcpy(&fnGetNativeProductType, &fnProductTypeAddress, sizeof(fnGetNativeProductType));

		if (fnVersionAddress && pCompatibilityReportOut) {
			fnReadVersion fnGetVersion = nullptr;

			static_assert(sizeof(fnGetVersion) == sizeof(fnVersionAddress));

			std::memcpy(&fnGetVersion, &fnVersionAddress, sizeof(fnGetVersion));
			OSVERSIONINFOEXW Report {};
			Report.dwOSVersionInfoSize = sizeof(Report);

			if (fnGetVersion(reinterpret_cast<OSVERSIONINFOW*>(&Report)) == 0) {
				*pCompatibilityReportOut = Report;
			}
		}

		// Windows Installer compatibility shims can make RtlGetVersion report Vista.
		// Both native exports bypass that compatibility report, including the
		// product type used by the existing workstation-only policy.
		DWORD unMajor = 0;
		DWORD unMinor = 0;
		DWORD unBuild = 0;
		DWORD unProductType = 0;
		fnGetNativeNumbers(&unMajor, &unMinor, &unBuild);
		VersionOut.dwMajorVersion = unMajor;
		VersionOut.dwMinorVersion = unMinor;
		// The upper word contains native build flags rather than the build number.
		VersionOut.dwBuildNumber = GetNativeBuildNumber(unBuild);
		VersionOut.dwPlatformId = VER_PLATFORM_WIN32_NT;

		if (!fnGetNativeProductType(&unProductType) || !IsNativeProductType(unProductType) || !unMajor || !VersionOut.dwBuildNumber) {
			wstrErrorOut = L"Setup could not obtain valid native Windows version and product information. Update Windows and retry setup.";
			return false;
		}

		VersionOut.wProductType = static_cast<BYTE>(unProductType);

		return true;
	}

	bool CheckWindows(std::wstring& wstrErrorOut, OSVERSIONINFOEXW* pObservedOut = nullptr, OSVERSIONINFOEXW* pCompatibilityReportOut = nullptr) {
		OSVERSIONINFOEXW Version {};
		const bool bRead = ReadWindowsVersion(Version, wstrErrorOut, pCompatibilityReportOut);

		if (pObservedOut) {
			*pObservedOut = Version;
		}

		if (!bRead) {
			return false;
		}

		if (!IsSupportedWindows(Version.dwMajorVersion, Version.dwMinorVersion, Version.dwBuildNumber, Version.wProductType)) {
			wstrErrorOut = L"WireSteam requires Windows 10 22H2 (build 19045 or later) or Windows 11. Detected native Windows " + DescribeWindowsVersion(Version) + L". Update Windows before installing WireSteam.";
			return false;
		}

		return true;
	}

	bool HasProcessExited(HANDLE hProcess) {
		return WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0;
	}

	bool HasProcessExited(DWORD unID) {
		Handle Process(OpenProcess(SYNCHRONIZE, FALSE, unID));
		if (!Process) {
			return GetLastError() == ERROR_INVALID_PARAMETER;
		}

		return HasProcessExited(Process.m_hHandle);
	}

	bool CheckProcesses(std::vector<std::wstring> const& vecDirectories, std::wstring& wstrErrorOut, ProcessAction Action = ProcessAction::Check) {
		const ULONGLONG unCleanupDeadline = GetTickCount64() + kHelperCleanupMilliseconds;
		Handle Snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
		if (!Snapshot) {
			wstrErrorOut = FormatWindowsFailure(L"Setup could not check running applications", GetLastError());
			return false;
		}

		PROCESSENTRY32W Entry {};
		Entry.dwSize = sizeof(Entry);
		if (!Process32FirstW(Snapshot.m_hHandle, &Entry)) {
			const DWORD unCode = GetLastError();
			if (unCode == ERROR_NO_MORE_FILES) {
				return true;
			}

			wstrErrorOut = FormatWindowsFailure(L"Setup could not check running applications", unCode);
			return false;
		}

		do {
			const bool bCleanupHelper = AreOrdinallyEqual(Entry.szExeFile, L"WireSteamService.exe");
			if (!IsPayloadName(Entry.szExeFile) || ((Action == ProcessAction::Terminate) && bCleanupHelper) || ((Action == ProcessAction::WaitForCleanup) && !bCleanupHelper)) {
				continue;
			}

			Handle Process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, Entry.th32ProcessID));
			if (!Process) {
				const DWORD unCode = GetLastError();
				if ((unCode == ERROR_INVALID_PARAMETER) || HasProcessExited(Entry.th32ProcessID)) {
					continue;
				}

				wstrErrorOut = FormatWindowsFailure(L"Setup could not verify a running WireSteam component", unCode) + L" " + std::wstring(kQuitInstructions);
				return false;
			}

			if (HasProcessExited(Process.m_hHandle)) {
				continue;
			}

			std::vector<wchar_t> vecBuffer(kMaximumPathCharacters);
			DWORD unCount = static_cast<DWORD>(vecBuffer.size());
			if (!QueryFullProcessImageNameW(Process.m_hHandle, 0, vecBuffer.data(), &unCount)) {
				const DWORD unCode = GetLastError();
				if (HasProcessExited(Process.m_hHandle)) {
					continue;
				}

				wstrErrorOut = FormatWindowsFailure(L"Setup could not verify a running WireSteam component", unCode) + L" " + std::wstring(kQuitInstructions);
				return false;
			}

			std::wstring wstrFull;
			std::wstring wstrCanonical;
			if (!NormalizeFullPath(std::wstring_view(vecBuffer.data(), unCount), wstrFull, wstrErrorOut)) {
				return false;
			}

			if (!ResolveCanonicalPath(wstrFull, false, wstrCanonical, wstrErrorOut)) {
				if (HasProcessExited(Process.m_hHandle)) {
					continue;
				}

				wstrErrorOut += L" " + std::wstring(kQuitInstructions);
				return false;
			}

			if ((!IsInsideInstallationDirectories(wstrFull, vecDirectories) && !IsInsideInstallationDirectories(wstrCanonical, vecDirectories)) || HasProcessExited(Process.m_hHandle)) {
				continue;
			}

			if (Action == ProcessAction::Check) {
				wstrErrorOut = kQuitInstructions;
				return false;
			}

			if (Action == ProcessAction::WaitForCleanup) {
				// The elevated Internet helper restores networking after its engine dies.
				// Its recovery guard must survive a failed cleanup instead of being killed.
				const ULONGLONG unNow = GetTickCount64();
				const DWORD unRemaining = (unNow < unCleanupDeadline) ? static_cast<DWORD>(unCleanupDeadline - unNow) : 0;
				if (WaitForSingleObject(Process.m_hHandle, unRemaining) != WAIT_OBJECT_0) {
					wstrErrorOut = L"WireSteam Internet cleanup is still running. Setup has preserved its recovery helper and has not replaced application files. Finish tunnel recovery with the installed WireSteam version, then retry setup.";
					return false;
				}

				continue;
			}

			// Keep the query handle alive while opening termination rights, so the
			// verified process identity cannot be recycled into another process.
			Handle Terminable(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, Entry.th32ProcessID));
			if (!Terminable) {
				const DWORD unCode = GetLastError();
				if (HasProcessExited(Process.m_hHandle)) {
					continue;
				}

				wstrErrorOut = FormatWindowsFailure(L"Setup could not stop an installed WireSteam component", unCode);
				return false;
			}

			if (!TerminateProcess(Terminable.m_hHandle, ERROR_PROCESS_ABORTED)) {
				const DWORD unCode = GetLastError();
				if (HasProcessExited(Terminable.m_hHandle)) {
					continue;
				}

				wstrErrorOut = FormatWindowsFailure(L"Setup could not stop an installed WireSteam component", unCode);
				return false;
			}

			if (WaitForSingleObject(Terminable.m_hHandle, kProcessStopMilliseconds) != WAIT_OBJECT_0) {
				wstrErrorOut = L"An installed WireSteam component did not finish stopping. Retry setup after it exits.";
				return false;
			}
		} while (Process32NextW(Snapshot.m_hHandle, &Entry));

		const DWORD unCode = GetLastError();
		if (unCode != ERROR_NO_MORE_FILES) {
			wstrErrorOut = FormatWindowsFailure(L"Setup could not finish checking running applications", unCode);
			return false;
		}

		return true;
	}

	std::wstring FormatServiceCleanupMessage(std::wstring_view strvService) {
		constexpr std::size_t kMaximumTunnelNameCharacters = 20;

		std::wstring_view strvName = strvService.substr(kServicePrefix.size());
		bool bValid = !strvName.empty() && (strvName.size() <= kMaximumTunnelNameCharacters) && std::all_of(strvName.begin(), strvName.end(), [](wchar_t wchCharacter) -> bool {
			return ((wchCharacter >= L'a') && (wchCharacter <= L'z')) || ((wchCharacter >= L'A') && (wchCharacter <= L'Z')) || ((wchCharacter >= L'0') && (wchCharacter <= L'9')) || (wchCharacter == L'_') || (wchCharacter == L'-');
		});

		return L"A WireSteam WireGuard tunnel service still exists, including a possible stopped adapter. "
			L"Quit WireSteam using QUIT WIRESTEAM in its tray menu, or stop its CLI tunnel with Ctrl+C. "
			L"Then run WireSteam.exe down --name " +
			std::wstring(bValid ? strvName : L"<tunnel-name>") +
			L" and approve cleanup. Retry setup after the adapter has been removed.";
	}

	bool CheckServiceRegistry(std::wstring& wstrErrorOut, std::vector<std::wstring>* pvecNamesOut = nullptr) {
		constexpr std::size_t kRegistryNameBufferCharacters = 32768;
		constexpr DWORD kMaximumRegistryEntries = 1024 * 1024;

		// SCM enumeration silently omits services whose status ACL denies this
		// caller. Check the service namespace as well, without opening private data.
		RegistryHandle Services;
		LSTATUS nCode = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services", 0, KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &Services.m_hHandle);
		if (nCode != ERROR_SUCCESS) {
			wstrErrorOut = FormatWindowsFailure(L"Setup could not verify the tunnel-service namespace", nCode);
			return false;
		}

		std::vector<wchar_t> vecName(kRegistryNameBufferCharacters);
		for (DWORD unIndex = 0; unIndex < kMaximumRegistryEntries; ++unIndex) {
			DWORD unCount = static_cast<DWORD>(vecName.size());
			nCode = RegEnumKeyExW(Services.m_hHandle, unIndex, vecName.data(), &unCount, nullptr, nullptr, nullptr, nullptr);
			if (nCode == ERROR_NO_MORE_ITEMS) {
				return true;
			}

			if (nCode != ERROR_SUCCESS) {
				wstrErrorOut = FormatWindowsFailure(L"Setup could not verify the tunnel-service namespace", nCode);
				return false;
			}

			std::wstring_view strvService = std::wstring_view(vecName.data(), unCount);
			if (IsWireSteamService(strvService)) {
				if (pvecNamesOut) {
					pvecNamesOut->emplace_back(strvService);
					continue;
				}

				wstrErrorOut = FormatServiceCleanupMessage(strvService);
				return false;
			}
		}

		wstrErrorOut = L"Setup could not obtain a complete list of tunnel services. Retry setup.";

		return false;
	}

	bool CheckServices(std::wstring& wstrErrorOut) {
		constexpr std::size_t kServiceBufferBytes = 256 * 1024;
		constexpr std::size_t kMaximumServicePages = 1024;

		ServiceHandle Manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE));
		if (!Manager.m_hHandle) {
			wstrErrorOut = FormatWindowsFailure(L"Setup could not check WireSteam tunnel services", GetLastError());
			return false;
		}

		std::vector<BYTE> vecBuffer(kServiceBufferBytes);
		DWORD unResume = 0;
		for (std::size_t unPage = 0; unPage < kMaximumServicePages; ++unPage) {
			DWORD unNeeded = 0;
			DWORD unReturned = 0;
			DWORD unPrevious = unResume;
			BOOL nSuccess = EnumServicesStatusExW(Manager.m_hHandle, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, vecBuffer.data(), static_cast<DWORD>(vecBuffer.size()), &unNeeded, &unReturned, &unResume, nullptr);
			const DWORD unCode = nSuccess ? ERROR_SUCCESS : GetLastError();
			if ((unCode != ERROR_SUCCESS) && (unCode != ERROR_MORE_DATA)) {
				wstrErrorOut = FormatWindowsFailure(L"Setup could not check WireSteam tunnel services", unCode);
				return false;
			}

			if (unReturned > (vecBuffer.size() / sizeof(ENUM_SERVICE_STATUS_PROCESSW))) {
				wstrErrorOut = L"Windows returned incomplete tunnel-service information. Retry setup.";
				return false;
			}

			LPENUM_SERVICE_STATUS_PROCESSW pServices = reinterpret_cast<LPENUM_SERVICE_STATUS_PROCESSW>(vecBuffer.data());
			for (DWORD unIndex = 0; unIndex < unReturned; ++unIndex) {
				if (pServices[unIndex].lpServiceName && IsWireSteamService(pServices[unIndex].lpServiceName)) {
					wstrErrorOut = FormatServiceCleanupMessage(pServices[unIndex].lpServiceName);
					return false;
				}
			}

			if (nSuccess) {
				return CheckServiceRegistry(wstrErrorOut);
			}

			if (!unReturned || (unResume == unPrevious)) {
				break;
			}
		}

		wstrErrorOut = L"Setup could not obtain a complete list of tunnel services. Retry setup after Windows finishes changing its services.";

		return false;
	}

	bool IsValidOwnedServiceName(std::wstring_view strvService) {
		constexpr std::size_t kMaximumTunnelNameCharacters = 20;

		if (!IsWireSteamService(strvService)) {
			return false;
		}

		std::wstring_view const strvName = strvService.substr(kServicePrefix.size());

		return !strvName.empty() && (strvName.size() <= kMaximumTunnelNameCharacters) && std::all_of(strvName.begin(), strvName.end(), [](wchar_t wchCharacter) -> bool {
			return ((wchCharacter >= L'a') && (wchCharacter <= L'z')) || ((wchCharacter >= L'A') && (wchCharacter <= L'Z')) || ((wchCharacter >= L'0') && (wchCharacter <= L'9')) || (wchCharacter == L'_') || (wchCharacter == L'-');
		});
	}

	bool IsOwnedServiceCommand(std::wstring_view strvService, wchar_t const* pszCommand, std::wstring const& wstrWireGuard) {
		if (!IsValidOwnedServiceName(strvService) || !pszCommand) {
			return false;
		}

		int nCount = 0;
		std::unique_ptr<wchar_t*, LocalArgumentsDeleter> pArguments(CommandLineToArgvW(pszCommand, &nCount));
		if (!pArguments || (nCount != 3) || !AreOrdinallyEqual(pArguments.get()[1], L"/tunnelservice")) {
			return false;
		}

		std::wstring wstrImage;
		std::wstring wstrCanonicalImage;
		std::wstring wstrConfiguration;
		std::wstring wstrError;
		if (!NormalizeFullPath(pArguments.get()[0], wstrImage, wstrError) || !ResolveCanonicalPath(wstrImage, false, wstrCanonicalImage, wstrError) || !AreOrdinallyEqual(wstrCanonicalImage, wstrWireGuard) || !NormalizeFullPath(pArguments.get()[2], wstrConfiguration, wstrError)) {
			return false;
		}

		const std::size_t unSeparator = wstrConfiguration.find_last_of(L'\\');
		std::wstring const wstrExpected = std::wstring(strvService.substr(std::wstring_view(L"WireGuardTunnel$").size())) + L".conf";

		return (unSeparator != std::wstring::npos) && AreOrdinallyEqual(std::wstring_view(wstrConfiguration).substr(unSeparator + 1), wstrExpected);
	}

	bool ReadServiceStatus(SC_HANDLE hService, SERVICE_STATUS_PROCESS& StatusOut, std::wstring& wstrErrorOut) {
		DWORD unNeeded = 0;
		if (!QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&StatusOut), sizeof(StatusOut), &unNeeded)) {
			wstrErrorOut = FormatWindowsFailure(L"Setup could not query an owned WireSteam tunnel service", GetLastError());
			return false;
		}

		return true;
	}

	bool HasValidServiceProcessID(SERVICE_STATUS_PROCESS const& Status) {
		return (Status.dwProcessId != 0) && ((Status.dwCurrentState == SERVICE_RUNNING) || (Status.dwCurrentState == SERVICE_PAUSE_PENDING) || (Status.dwCurrentState == SERVICE_PAUSED) || (Status.dwCurrentState == SERVICE_CONTINUE_PENDING));
	}

	bool StopOwnedService(SC_HANDLE hManager, std::wstring const& wstrName, std::wstring const& wstrWireGuard, std::wstring& wstrErrorOut) {
		constexpr DWORD kMaximumServiceConfigurationBytes = 8192;

		ServiceHandle Service(OpenServiceW(hManager, wstrName.c_str(), SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE));
		if (!Service.m_hHandle) {
			const DWORD unCode = GetLastError();
			// The Internet helper may already be removing the same owned service.
			if ((unCode == ERROR_SERVICE_DOES_NOT_EXIST) || (unCode == ERROR_SERVICE_MARKED_FOR_DELETE)) {
				return true;
			}

			wstrErrorOut = FormatWindowsFailure(L"Setup could not open an owned WireSteam tunnel service", unCode);
			return false;
		}

		std::vector<BYTE> vecConfiguration(kMaximumServiceConfigurationBytes);
		DWORD unNeeded = 0;
		LPQUERY_SERVICE_CONFIGW pConfiguration = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(vecConfiguration.data());
		if (!QueryServiceConfigW(Service.m_hHandle, pConfiguration, static_cast<DWORD>(vecConfiguration.size()), &unNeeded)) {
			wstrErrorOut = FormatWindowsFailure(L"Setup could not verify an owned WireSteam tunnel service", GetLastError());
			return false;
		}

		if ((pConfiguration->dwServiceType != SERVICE_WIN32_OWN_PROCESS) || !IsOwnedServiceCommand(wstrName, pConfiguration->lpBinaryPathName, wstrWireGuard)) {
			wstrErrorOut = L"Setup refused to stop a service in the WireSteam namespace because its executable or tunnel configuration does not match WireSteam ownership. Resolve this service conflict before retrying setup.";
			return false;
		}

		SERVICE_STATUS_PROCESS Status {};
		if (!ReadServiceStatus(Service.m_hHandle, Status, wstrErrorOut)) {
			return false;
		}

		if (Status.dwCurrentState != SERVICE_STOPPED) {
			Handle Process;
			DWORD unVerifiedProcessID = 0;
			if (HasValidServiceProcessID(Status)) {
				// START_PENDING/STOP_PENDING PIDs are not valid Windows identities.
				// Retain an exact verified process before requesting a state change.
				Process.m_hHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, Status.dwProcessId);
				std::vector<wchar_t> vecImage(kMaximumPathCharacters);
				DWORD unCount = static_cast<DWORD>(vecImage.size());
				std::wstring wstrImage;
				std::wstring wstrCanonical;
				if (!Process || !QueryFullProcessImageNameW(Process.m_hHandle, 0, vecImage.data(), &unCount) || !NormalizeFullPath(std::wstring_view(vecImage.data(), unCount), wstrImage, wstrErrorOut) || !ResolveCanonicalPath(wstrImage, false, wstrCanonical, wstrErrorOut) || !AreOrdinallyEqual(wstrCanonical, wstrWireGuard)) {
					wstrErrorOut = L"Setup could not verify an owned WireSteam tunnel process. Finish tunnel cleanup and retry setup.";
					return false;
				}

				SERVICE_STATUS_PROCESS Confirmed {};
				if (!ReadServiceStatus(Service.m_hHandle, Confirmed, wstrErrorOut)) {
					return false;
				}

				if (HasValidServiceProcessID(Confirmed) && (Confirmed.dwProcessId == Status.dwProcessId)) {
					unVerifiedProcessID = Status.dwProcessId;
				}
			}

			SERVICE_STATUS StopStatus {};
			if (!ControlService(Service.m_hHandle, SERVICE_CONTROL_STOP, &StopStatus)) {
				const DWORD unCode = GetLastError();
				if ((unCode != ERROR_SERVICE_NOT_ACTIVE) && (unCode != ERROR_SERVICE_CANNOT_ACCEPT_CTRL)) {
					wstrErrorOut = FormatWindowsFailure(L"Setup could not stop an owned WireSteam tunnel service", unCode);
					return false;
				}
			}

			const ULONGLONG unDeadline = GetTickCount64() + kServiceStopMilliseconds;
			while (GetTickCount64() < unDeadline) {
				if (!ReadServiceStatus(Service.m_hHandle, Status, wstrErrorOut)) {
					return false;
				}

				if (Status.dwCurrentState == SERVICE_STOPPED) {
					break;
				}

				Sleep(kServicePollMilliseconds);
			}

			if (Status.dwCurrentState != SERVICE_STOPPED) {
				if (!Process || (unVerifiedProcessID == 0)) {
					wstrErrorOut = L"A WireSteam tunnel did not stop, and Windows did not provide a valid process identity before shutdown. Setup has preserved unrelated processes. Finish tunnel cleanup and retry setup.";
					return false;
				}

				SERVICE_STATUS_PROCESS Current {};
				if (!ReadServiceStatus(Service.m_hHandle, Current, wstrErrorOut)) {
					return false;
				}

				if ((Current.dwCurrentState != SERVICE_STOPPED) && HasValidServiceProcessID(Current) && (Current.dwProcessId != unVerifiedProcessID)) {
					wstrErrorOut = L"A WireSteam tunnel changed process while setup was stopping it. Finish tunnel cleanup and retry setup.";
					return false;
				}

				if (Current.dwCurrentState != SERVICE_STOPPED) {
					// Never reopen a pending-state PID: only the retained process
					// belonging to the verified own-process service can be killed.
					if (!TerminateProcess(Process.m_hHandle, ERROR_PROCESS_ABORTED) && !HasProcessExited(Process.m_hHandle)) {
						wstrErrorOut = L"Setup could not terminate an owned WireSteam tunnel that did not stop. Finish tunnel cleanup and retry setup.";
						return false;
					}

					if (WaitForSingleObject(Process.m_hHandle, kProcessStopMilliseconds) != WAIT_OBJECT_0) {
						wstrErrorOut = L"An owned WireSteam tunnel did not finish stopping. Retry setup after tunnel cleanup completes.";
						return false;
					}
				}
			}
		}

		if (!DeleteService(Service.m_hHandle)) {
			const DWORD unCode = GetLastError();
			if (unCode != ERROR_SERVICE_MARKED_FOR_DELETE) {
				wstrErrorOut = FormatWindowsFailure(L"Setup could not remove an owned WireSteam tunnel service", unCode);
				return false;
			}
		}

		return true;
	}

	bool StopOwnedServices(std::wstring const& wstrWireGuard, std::wstring& wstrErrorOut) {
		std::vector<std::wstring> vecNames;
		if (!CheckServiceRegistry(wstrErrorOut, &vecNames)) {
			return false;
		}

		ServiceHandle Manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
		if (!Manager.m_hHandle) {
			wstrErrorOut = FormatWindowsFailure(L"Setup could not connect to WireSteam tunnel services", GetLastError());
			return false;
		}

		for (auto const& wstrName : vecNames) {
			if (!StopOwnedService(Manager.m_hHandle, wstrName, wstrWireGuard, wstrErrorOut)) {
				return false;
			}
		}

		return true;
	}

	bool WaitForOwnedServicesRemoved(std::wstring& wstrErrorOut) {
		const ULONGLONG unDeadline = GetTickCount64() + kProcessStopMilliseconds;
		do {
			if (CheckServices(wstrErrorOut)) {
				return true;
			}

			// DeleteService completes only after the stopped service's final
			// handle closes, including handles released by concurrent cleanup.
			Sleep(kServicePollMilliseconds);
		} while (GetTickCount64() < unDeadline);

		wstrErrorOut = L"WireSteam tunnel service removal is still pending. Close service-management windows and finish tunnel cleanup, then retry setup. " + wstrErrorOut;

		return false;
	}

	template <typename TProcessOperation, typename TServiceOperation, typename TServiceCheck>
	bool RunComponentShutdown(TProcessOperation const& ProcessOperation, TServiceOperation const& ServiceOperation, TServiceCheck const& ServiceCheck) {
		// The Internet helper must restore forwarding/NAT while its adapter is
		// still present. Leftover service removal is permitted only afterward.
		return ProcessOperation(ProcessAction::Terminate) && ProcessOperation(ProcessAction::WaitForCleanup) && ServiceOperation() && ProcessOperation(ProcessAction::Check) && ServiceCheck();
	}
} // namespace

extern "C" __declspec(dllexport) UINT __stdcall WireSteamValidateSession(MSIHANDLE hSession) noexcept {
	try {
		std::wstring wstrRemove;
		std::wstring wstrCurrent;
		std::wstring wstrPrevious;
		std::wstring wstrInstalled;
		std::wstring wstrRelated;
		std::wstring wstrError;
		if (!ReadProperty(hSession, L"REMOVE", wstrRemove, wstrError)) {
			return ReportFailure(hSession, wstrError.c_str());
		}

		if (!IsFullRemoval(wstrRemove)) {
			OSVERSIONINFOEXW Observed {};
			OSVERSIONINFOEXW CompatibilityReport {};
			const bool bSupported = CheckWindows(wstrError, &Observed, &CompatibilityReport);
			std::wstring const wstrDescription = L"WireSteam native Windows version: " + DescribeWindowsVersion(Observed);
			ReportInformation(hSession, wstrDescription.c_str());

			std::wstring const wstrCompatibilityDescription = L"WireSteam Windows compatibility report: " + DescribeWindowsVersion(CompatibilityReport);
			ReportInformation(hSession, wstrCompatibilityDescription.c_str());

			if (!bSupported) {
				return ReportFailure(hSession, wstrError.c_str());
			}
		}

		if (!IsFullRemoval(wstrRemove)) {
			std::wstring wstrWireGuard;
			if (!ReadWireGuardInstallation(wstrWireGuard, wstrError)) {
				return ReportFailure(hSession, wstrError.c_str());
			}
		}

		if (!ReadProperty(hSession, L"INSTALLFOLDER", wstrCurrent, wstrError) || !ReadProperty(hSession, L"WIRESTEAM_PREVIOUSINSTALLDIR", wstrPrevious, wstrError) || !ReadProperty(hSession, L"Installed", wstrInstalled, wstrError) || !ReadProperty(hSession, L"WIX_UPGRADE_DETECTED", wstrRelated, wstrError)) {
			return ReportFailure(hSession, wstrError.c_str());
		}

		std::vector<std::wstring> vecDirectories;
		if (!AddDirectory(wstrCurrent, vecDirectories, wstrError) || !AddDirectory(wstrPrevious, vecDirectories, wstrError)) {
			return ReportFailure(hSession, wstrError.c_str());
		}

		if (vecDirectories.empty()) {
			return ReportFailure(hSession, L"Setup could not determine the WireSteam installation directory. Retry setup.");
		}

		const bool bReplacing = wstrInstalled.empty() && !wstrRelated.empty() && !IsFullRemoval(wstrRemove);
		if (bReplacing) {
			if (!AddRelatedProductDirectories(wstrRelated, vecDirectories, wstrError)) {
				return ReportFailure(hSession, wstrError.c_str());
			}

			std::wstring const wstrData = SerializeDirectories(vecDirectories);
			if (wstrData.size() >= kMaximumPathCharacters) {
				return ReportFailure(hSession, L"The installed WireSteam directory list exceeds the supported shutdown data length.");
			}

			const UINT unCode = MsiSetPropertyW(hSession, L"WireSteamStopComponents", wstrData.c_str());
			if (unCode != ERROR_SUCCESS) {
				wstrError = FormatWindowsFailure(L"Setup could not prepare WireSteam component shutdown", unCode);
				return ReportFailure(hSession, wstrError.c_str());
			}

			return ERROR_SUCCESS;
		}

		if (!CheckProcesses(vecDirectories, wstrError) || !CheckServices(wstrError)) {
			return ReportFailure(hSession, wstrError.c_str());
		}

		return ERROR_SUCCESS;
	} catch (...) {
		return ReportFailure(hSession, L"Setup could not complete its prerequisite and session checks. Retry setup after repairing the required WireGuard installation and closing WireSteam.");
	}
}

extern "C" __declspec(dllexport) UINT __stdcall WireSteamStopComponents(MSIHANDLE hSession) noexcept {
	try {
		std::wstring wstrData;
		std::wstring wstrWireGuard;
		std::wstring wstrError;
		std::vector<std::wstring> vecDirectories;
		// Validate again immediately before shutdown. A missing prerequisite must
		// never tear down a working session or remove a previous application.
		if (!ReadWireGuardInstallation(wstrWireGuard, wstrError) || !ReadProperty(hSession, L"CustomActionData", wstrData, wstrError) || !DeserializeDirectories(wstrData, vecDirectories, wstrError)) {
			return ReportFailure(hSession, wstrError.c_str());
		}

		ReportInformation(hSession, L"Stopping installed WireSteam components and owned tunnel services before application file replacement.");
		auto const ProcessOperation = [&](ProcessAction Action) -> bool {
			return CheckProcesses(vecDirectories, wstrError, Action);
		};
		auto const ServiceOperation = [&]() -> bool {
			return StopOwnedServices(wstrWireGuard, wstrError);
		};
		auto const ServiceCheck = [&]() -> bool {
			return WaitForOwnedServicesRemoved(wstrError);
		};
		if (!RunComponentShutdown(ProcessOperation, ServiceOperation, ServiceCheck)) {
			return ReportFailure(hSession, wstrError.c_str());
		}

		ReportInformation(hSession, L"Installed WireSteam components and owned tunnel services stopped. Internet cleanup completed.");

		return ERROR_SUCCESS;
	} catch (...) {
		return ReportFailure(hSession, L"Setup could not finish stopping the installed WireSteam version. Its components may remain stopped. Finish tunnel recovery with that version, then retry setup.");
	}
}
