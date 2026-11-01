
// C++
#include <cwchar>

// STL
#include <iostream>
#include <stdexcept>

// Project
#include "InstallerGuard.cpp"

// Compile this translation unit alone: including the implementation lets the
// executable test the exact matching helpers without opening an MSI session.
namespace {
	void Require(bool bCondition, char const* pszMessage) {
		if (!bCondition) {
			throw std::runtime_error(pszMessage);
		}
	}

	std::wstring NormalizeTestPath(std::wstring_view strvPath) {
		std::wstring wstrResult;
		std::wstring wstrError;
		Require(NormalizeFullPath(strvPath, wstrResult, wstrError), "An absolute guard-test path did not normalize");

		return wstrResult;
	}

	std::wstring_view GetEnvironmentName(std::wstring_view strvEntry) {
		// Drive-current-directory variables have names such as "=C:".
		return strvEntry.substr(0, strvEntry.find(L'=', 1));
	}

	std::vector<wchar_t> BuildVistaChildEnvironment() {
		std::vector<std::wstring> vecEntries;
		wchar_t* pszInherited = GetEnvironmentStringsW();
		Require(pszInherited != nullptr, "The test could not copy its process environment");

		try {
			for (wchar_t const* pszEntry = pszInherited; *pszEntry; pszEntry += std::wcslen(pszEntry) + 1) {
				std::wstring_view strvItem = pszEntry;
				if (!AreOrdinallyEqual(GetEnvironmentName(strvItem), L"__COMPAT_LAYER")) {
					vecEntries.emplace_back(strvItem);
				}
			}

			FreeEnvironmentStringsW(pszInherited);
			pszInherited = nullptr;

			vecEntries.emplace_back(L"__COMPAT_LAYER=VISTARTM");
			std::sort(vecEntries.begin(), vecEntries.end(), [](std::wstring const& wstrLeft, std::wstring const& wstrRight) -> bool {
				std::wstring_view const strvLeftName = GetEnvironmentName(wstrLeft);
				std::wstring_view const strvRightName = GetEnvironmentName(wstrRight);

				return CompareStringOrdinal(strvLeftName.data(), static_cast<int>(strvLeftName.size()), strvRightName.data(), static_cast<int>(strvRightName.size()), TRUE) == CSTR_LESS_THAN;
			});

			std::vector<wchar_t> vecEnvironment;
			for (auto const& wstrEntry : vecEntries) {
				vecEnvironment.insert(vecEnvironment.end(), wstrEntry.begin(), wstrEntry.end());
				vecEnvironment.push_back(L'\0');
			}

			vecEnvironment.push_back(L'\0');

			return vecEnvironment;
		} catch (...) {
			if (pszInherited) {
				FreeEnvironmentStringsW(pszInherited);
			}

			throw;
		}
	}

	void TestVistaChild(OSVERSIONINFOEXW const& Expected) {
		constexpr DWORD kChildTimeoutMilliseconds = 10'000;

		std::vector<wchar_t> vecImage(kMaximumPathCharacters);
		DWORD unCount = GetModuleFileNameW(nullptr, vecImage.data(), static_cast<DWORD>(vecImage.size()));
		Require((unCount > 0) && (unCount < vecImage.size()), "The guard test could not locate its own executable");
		std::wstring wstrExecutable = std::wstring(vecImage.data(), unCount);
		std::wstring wstrCommand = L"\"" + wstrExecutable + L"\" --probe-os " + std::to_wstring(Expected.dwMajorVersion) + L" " + std::to_wstring(Expected.dwMinorVersion) + L" " + std::to_wstring(Expected.dwBuildNumber) + L" " + std::to_wstring(Expected.wProductType);
		std::vector<wchar_t> vecEnvironment = BuildVistaChildEnvironment();
		STARTUPINFOW Startup {};
		Startup.cb = sizeof(Startup);
		PROCESS_INFORMATION Child {};
		Require(CreateProcessW(wstrExecutable.c_str(), wstrCommand.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, vecEnvironment.data(), nullptr, &Startup, &Child) != FALSE, "The guard test could not start its own isolated Vista-compatibility probe");
		Handle Process(Child.hProcess);
		Handle Thread(Child.hThread);
		const DWORD unWaitResult = WaitForSingleObject(Process.m_hHandle, kChildTimeoutMilliseconds);
		if (unWaitResult != WAIT_OBJECT_0) {
			// This handle belongs only to the exact probe created above. Reap that
			// owned child before reporting its timeout or failed wait.
			const BOOL nTerminationStarted = TerminateProcess(Process.m_hHandle, ERROR_PROCESS_ABORTED);
			Require(nTerminationStarted || (WaitForSingleObject(Process.m_hHandle, 0) == WAIT_OBJECT_0), "The isolated Vista-compatibility probe could not be stopped after its failed wait");
			Require(WaitForSingleObject(Process.m_hHandle, INFINITE) == WAIT_OBJECT_0, "The isolated Vista-compatibility probe could not be joined after its failed wait");

			Require(false, "The isolated Vista-compatibility probe did not finish");
		}

		DWORD unExitCode = 0;
		Require(GetExitCodeProcess(Process.m_hHandle, &unExitCode) && (unExitCode == 0), "The native Windows guard failed its live Vista-compatibility child regression");

		std::wcout << L"PASS child __COMPAT_LAYER=VISTARTM: compatibility report=6.0.6000; native policy=" << Expected.dwMajorVersion << L'.' << Expected.dwMinorVersion << L'.' << Expected.dwBuildNumber << L"; productType=" << static_cast<unsigned>(Expected.wProductType) << L'\n';
	}

	struct OwnedProbe {
		OwnedProbe(std::wstring const& wstrDirectory, std::wstring const& wstrSource);
		~OwnedProbe();

		OwnedProbe(OwnedProbe const&) = delete;
		OwnedProbe& operator=(OwnedProbe const&) = delete;

		std::wstring m_wstrDirectory;
		std::wstring m_wstrImage;
		Handle m_Process;
	};

	OwnedProbe::OwnedProbe(std::wstring const& wstrDirectory, std::wstring const& wstrSource) :
		m_wstrDirectory(wstrDirectory),
		m_wstrImage(wstrDirectory + L"\\WireSteam.exe")
	{
		Require(CreateDirectoryW(m_wstrDirectory.c_str(), nullptr) != FALSE, "The test could not create its isolated process directory");
		if (!CopyFileW(wstrSource.c_str(), m_wstrImage.c_str(), TRUE)) {
			RemoveDirectoryW(m_wstrDirectory.c_str());
			Require(false, "The test could not copy its isolated executable");
		}

		std::wstring wstrCommand = L"\"" + m_wstrImage + L"\" --guard-idle";
		STARTUPINFOW Startup {};
		Startup.cb = sizeof(Startup);
		PROCESS_INFORMATION Child {};
		if (!CreateProcessW(m_wstrImage.c_str(), wstrCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, m_wstrDirectory.c_str(), &Startup, &Child)) {
			DeleteFileW(m_wstrImage.c_str());
			RemoveDirectoryW(m_wstrDirectory.c_str());
			Require(false, "The test could not start its isolated executable");
		}

		m_Process.m_hHandle = Child.hProcess;
		Handle Thread(Child.hThread);
	}

	OwnedProbe::~OwnedProbe() {
		if (m_Process && !HasProcessExited(m_Process.m_hHandle)) {
			if (!TerminateProcess(m_Process.m_hHandle, ERROR_PROCESS_ABORTED) && !HasProcessExited(m_Process.m_hHandle)) {
				OutputDebugStringA("WireSteam guard test: isolated child cleanup failed.\n");
			}

			WaitForSingleObject(m_Process.m_hHandle, kProcessStopMilliseconds);
		}

		DeleteFileW(m_wstrImage.c_str());
		RemoveDirectoryW(m_wstrDirectory.c_str());
	}

	void TestShutdownOrdering() {
		std::vector<int> vecSteps;
		bool bCleanupSucceeds = true;
		auto const ProcessOperation = [&](ProcessAction Action) -> bool {
			vecSteps.push_back(static_cast<int>(Action));

			return (Action != ProcessAction::WaitForCleanup) || bCleanupSucceeds;
		};
		auto const ServiceOperation = [&]() -> bool {
			vecSteps.push_back(3);
			return true;
		};
		auto const ServiceCheck = [&]() -> bool {
			vecSteps.push_back(4);
			return true;
		};
		Require(RunComponentShutdown(ProcessOperation, ServiceOperation, ServiceCheck) && (vecSteps == std::vector<int> { 1, 2, 3, 0, 4 }), "Shutdown did not restore helper-owned networking before service removal");
		vecSteps.clear();
		bCleanupSucceeds = false;
		Require(!RunComponentShutdown(ProcessOperation, ServiceOperation, ServiceCheck) && (vecSteps == std::vector<int> { 1, 2 }), "Pending Internet recovery allowed service removal or post-cleanup mutation");
	}

	void TestScopedTermination() {
		std::vector<wchar_t> vecImage(kMaximumPathCharacters);
		DWORD unCount = GetModuleFileNameW(nullptr, vecImage.data(), static_cast<DWORD>(vecImage.size()));
		Require((unCount > 0) && (unCount < vecImage.size()), "The process fixture could not locate its source executable");
		std::wstring const wstrSource = std::wstring(vecImage.data(), unCount);
		std::vector<wchar_t> vecTemporary(kMaximumPathCharacters);
		unCount = GetTempPathW(static_cast<DWORD>(vecTemporary.size()), vecTemporary.data());
		Require((unCount > 0) && (unCount < vecTemporary.size()), "The process fixture could not locate temporary storage");
		std::wstring const wstrDirectory = std::wstring(vecTemporary.data(), unCount) + L"WireSteamGuard-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
		OwnedProbe Installed(wstrDirectory, wstrSource);
		OwnedProbe Portable(wstrDirectory + L"-portable", wstrSource);
		std::vector<std::wstring> vecDirectories;
		std::wstring wstrError;
		Require(AddDirectory(wstrDirectory, vecDirectories, wstrError), "The process fixture could not resolve its isolated directory");
		Require(!CheckProcesses(vecDirectories, wstrError), "Read-only validation failed to detect its isolated installed child");
		Require(!HasProcessExited(Installed.m_Process.m_hHandle) && !HasProcessExited(Portable.m_Process.m_hHandle), "Read-only validation stopped an isolated child");
		Require(CheckProcesses(vecDirectories, wstrError, ProcessAction::Terminate), "Scoped shutdown failed for its isolated installed child");
		Require(HasProcessExited(Installed.m_Process.m_hHandle), "Scoped shutdown left its isolated installed child running");
		Require(!HasProcessExited(Portable.m_Process.m_hHandle), "Scoped shutdown touched its isolated portable neighbor");
		Require(CheckProcesses(vecDirectories, wstrError), "Post-stop validation still found its isolated installed child");
	}
} // namespace

int main(int nArgumentCount, char** ppszArguments) {
	if ((nArgumentCount == 2) && (std::string_view(ppszArguments[1]) == "--guard-idle")) {
		Sleep(30'000);
		return 0;
	}

	try {
		using fnReadVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
		using fnReadNativeNumbers = void(WINAPI*)(DWORD*, DWORD*, DWORD*);
		using fnReadNativeProduct = BOOLEAN(WINAPI*)(DWORD*);

		FARPROC fnVersionAddress = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
		FARPROC fnNumbersAddress = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetNtVersionNumbers");
		FARPROC fnProductAddress = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetNtProductType");
		Require((fnVersionAddress != nullptr) && (fnNumbersAddress != nullptr) && (fnProductAddress != nullptr), "The live native Windows version APIs are unavailable");

		fnReadVersion fnNativeVersion = nullptr;
		fnReadNativeNumbers fnNativeNumbers = nullptr;
		fnReadNativeProduct fnNativeProduct = nullptr;
		std::memcpy(&fnNativeVersion, &fnVersionAddress, sizeof(fnNativeVersion));
		std::memcpy(&fnNativeNumbers, &fnNumbersAddress, sizeof(fnNativeNumbers));
		std::memcpy(&fnNativeProduct, &fnProductAddress, sizeof(fnNativeProduct));

		OSVERSIONINFOEXW Live {};
		Live.dwOSVersionInfoSize = sizeof(Live);
		LONG nStatus = fnNativeVersion(reinterpret_cast<OSVERSIONINFOW*>(&Live));
		DWORD unNativeMajor = 0;
		DWORD unNativeMinor = 0;
		DWORD unNativeBuild = 0;
		fnNativeNumbers(&unNativeMajor, &unNativeMinor, &unNativeBuild);
		DWORD unProductType = 0;
		const BOOLEAN unValidProduct = fnNativeProduct(&unProductType);
		std::wstring wstrLiveError;
		OSVERSIONINFOEXW Production {};
		OSVERSIONINFOEXW CompatibilityReport {};
		bool bLiveSupported = CheckWindows(wstrLiveError, &Production, &CompatibilityReport);

		std::wcout << L"LIVE RtlGetVersion: size=" << sizeof(Live) << L" status=" << nStatus << L" major=" << Live.dwMajorVersion << L" minor=" << Live.dwMinorVersion << L" build=" << Live.dwBuildNumber << L" platform=" << Live.dwPlatformId << L" productType=" << static_cast<unsigned>(Live.wProductType) << L" suite=" << Live.wSuiteMask << L" CheckWindows=" << bLiveSupported << L" error=" << wstrLiveError << L'\n';
		std::wcout << L"LIVE RtlGetNtVersionNumbers: major=" << unNativeMajor << L" minor=" << unNativeMinor << L" rawBuild=" << unNativeBuild << L" build=" << GetNativeBuildNumber(unNativeBuild) << L'\n';

		Require((nStatus == 0) && (Live.dwPlatformId == VER_PLATFORM_WIN32_NT) && (Live.wProductType >= VER_NT_WORKSTATION) && (Live.wProductType <= VER_NT_SERVER),
			"The live native Windows probe returned invalid version/platform/product fields");
		Require((unNativeMajor != 0) && (GetNativeBuildNumber(unNativeBuild) != 0) && unValidProduct && IsNativeProductType(unProductType),
			"The live native version/product APIs returned invalid fields");
		Require((Production.dwMajorVersion == unNativeMajor) && (Production.dwMinorVersion == unNativeMinor) &&
			(Production.dwBuildNumber == (unNativeBuild & 0xFFFF)) && (Production.dwBuildNumber <= 65535) &&
			(Production.dwPlatformId == VER_PLATFORM_WIN32_NT) && (Production.wProductType == unProductType),
			"The production Windows check did not preserve native version/product fields");
		Require((CompatibilityReport.dwMajorVersion == Live.dwMajorVersion) && (CompatibilityReport.dwMinorVersion == Live.dwMinorVersion) &&
			(CompatibilityReport.dwBuildNumber == Live.dwBuildNumber) && (CompatibilityReport.wProductType == Live.wProductType),
			"The production Windows check lost its diagnostic compatibility report");
		Require(bLiveSupported == IsSupportedWindows(unNativeMajor, unNativeMinor, unNativeBuild & 0xFFFF, static_cast<BYTE>(unProductType)),
			"The actual production Windows check disagreed with its supported-version policy");

		if (nArgumentCount != 1) {
			Require((nArgumentCount == 6) && (std::string_view(ppszArguments[1]) == "--probe-os"), "Unexpected guard-test child arguments");
			Require((Live.dwMajorVersion == 6) && (Live.dwMinorVersion == 0) && (Live.dwBuildNumber == 6000),
				"The isolated child did not activate the Vista compatibility shim");
			Require((Production.dwMajorVersion == std::stoul(ppszArguments[2])) && (Production.dwMinorVersion == std::stoul(ppszArguments[3])) &&
				(Production.dwBuildNumber == std::stoul(ppszArguments[4])) && (Production.wProductType == std::stoul(ppszArguments[5])),
				"The compatibility shim changed the guard's actual Windows version/product type");
			return 0;
		}

		Require((GetEnvironmentName(L"=C:=C:\\test") == L"=C:") && (GetEnvironmentName(L"ProgramFiles=C:\\test") == L"ProgramFiles") &&
			(GetEnvironmentName(L"ProgramFiles(x86)=C:\\test") == L"ProgramFiles(x86)"),
			"The isolated child environment lost a variable-name boundary");
		TestVistaChild(Production);
		Require(!IsNativeProductType(0) && !IsNativeProductType(4) && !IsNativeProductType(0x10001) &&
			IsNativeProductType(VER_NT_WORKSTATION) && IsNativeProductType(VER_NT_DOMAIN_CONTROLLER) && IsNativeProductType(VER_NT_SERVER),
			"An invalid native product type could be narrowed into workstation metadata");
		Require((GetNativeBuildNumber(0xF0004A64) == 19044) && !IsSupportedWindows(10, 0, GetNativeBuildNumber(0xF0004A64), VER_NT_WORKSTATION),
			"Native build flags incorrectly allowed Windows 10 before 22H2");
		Require((GetNativeBuildNumber(0xC0004A65) == 19045) && IsSupportedWindows(10, 0, GetNativeBuildNumber(0xC0004A65), VER_NT_WORKSTATION),
			"Native build flags incorrectly rejected Windows 10 22H2");
		Require(!IsSupportedWindows(10, 0, 19044, VER_NT_WORKSTATION), "Windows 10 before 22H2 was accepted");
		Require(IsSupportedWindows(10, 0, 19045, VER_NT_WORKSTATION), "Windows 10 22H2 was rejected");
		Require(IsSupportedWindows(10, 0, 26200, VER_NT_WORKSTATION), "Windows 11 was rejected");
		Require(IsSupportedWindows(11, 0, 100, VER_NT_WORKSTATION), "A later client Windows version was rejected");
		Require(!IsSupportedWindows(6, 3, 9600, VER_NT_WORKSTATION), "An older Windows version was accepted");
		Require(!IsSupportedWindows(10, 0, 26100, VER_NT_SERVER), "Windows Server was accepted as Windows 11");
		Require(IsFullRemoval(L"ALL") && IsFullRemoval(L" all\t") && !IsFullRemoval(L"MainFeature") && !IsFullRemoval(L"ALL,MainFeature"),
			"Complete-uninstall OS exemption matching failed");

		for (auto const& strvName : kPayloadNames) {
			Require(IsPayloadName(strvName), "A packaged executable escaped process-name matching");
		}

		Require(IsPayloadName(L"wiresteam.ui.EXE") && !IsPayloadName(L"OtherWireSteam.exe") && !IsPayloadName(L"WireSteam.exe.bak") && !IsPayloadName(L"Steam.exe"),
			"Payload process-name matching was too broad or case-sensitive");

		std::wstring const wstrFolder = NormalizeTestPath(L"C:\\Program Files\\WireSteam\\");
		Require(wstrFolder == L"C:\\Program Files\\WireSteam", "Trailing directory separator was not removed");
		Require(IsPathUnderDirectory(NormalizeTestPath(L"c:\\PROGRAM FILES\\wiresteam\\WireSteam.UI.exe"), wstrFolder), "Case-insensitive installed process matching failed");
		Require(IsPathUnderDirectory(NormalizeTestPath(L"C:\\Program Files\\WireSteam\\tools\\WireSteam.exe"), wstrFolder), "Installed subtree matching failed");
		Require(!IsPathUnderDirectory(NormalizeTestPath(L"C:\\Program Files\\WireSteam-old\\WireSteam.exe"), wstrFolder), "A prefix-neighbor installation was matched");
		Require(!IsPathUnderDirectory(wstrFolder, wstrFolder), "A directory itself was treated as a child image");
		Require(NormalizeTestPath(L"C:/Program Files/WireSteam/../WireSteam/WireSteam.exe") == L"C:\\Program Files\\WireSteam\\WireSteam.exe", "Dot/slash normalization failed");
		Require(NormalizeTestPath(L"\\\\?\\C:\\Program Files\\WireSteam\\") == wstrFolder, "Extended drive path normalization failed");
		Require((BuildExtendedAPIPath(wstrFolder) == L"\\\\?\\C:\\Program Files\\WireSteam") &&
			(BuildExtendedAPIPath(L"\\\\server\\share\\WireSteam") == L"\\\\?\\UNC\\server\\share\\WireSteam"),
			"Long-path file queries must not depend on the MSI host manifest");
		Require(IsPathUnderDirectory(NormalizeTestPath(L"\\\\?\\UNC\\server\\share\\WireSteam\\WireSteam.exe"), NormalizeTestPath(L"\\\\server\\share\\WireSteam\\")),
			"Extended UNC installation matching failed");
		Require(!IsPathUnderDirectory(NormalizeTestPath(L"\\\\server\\share\\WireSteam-extra\\WireSteam.exe"), NormalizeTestPath(L"\\\\server\\share\\WireSteam\\")),
			"UNC prefix-neighbor matching failed");
		Require(IsPathUnderDirectory(NormalizeTestPath(L"C:\\WireSteam.exe"), NormalizeTestPath(L"C:\\")), "Drive-root boundary matching failed");

		std::wstring wstrResult;
		std::wstring wstrError;
		for (auto const& pszInvalid : { L"WireSteam", L"C:WireSteam", L"\\WireSteam", L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1", L"\\\\server" }) {
			Require(!NormalizeFullPath(pszInvalid, wstrResult, wstrError), "A relative/device/incomplete installation path was accepted");
		}

		std::vector<std::wstring> const vecDirectories { wstrFolder, NormalizeTestPath(L"D:\\Previous WireSteam\\") };
		Require(IsInsideInstallationDirectories(NormalizeTestPath(L"D:\\Previous WireSteam\\WireSteam.exe"), vecDirectories), "Previous installation directory was ignored");
		Require(!IsInsideInstallationDirectories(NormalizeTestPath(L"D:\\Portable\\WireSteam.exe"), vecDirectories), "An unrelated portable process was matched");

		std::vector<std::wstring> vecDecoded;
		std::vector<std::wstring> const vecSerialized { wstrFolder, L"D:\\Previous; WireSteam", L"D:\\Unicode-\u03a9" };
		Require(DeserializeDirectories(SerializeDirectories(vecSerialized), vecDecoded, wstrError) && (vecDecoded == vecSerialized), "Deferred shutdown data lost directory boundaries or Unicode");
		for (auto const& pszInvalid : { L"", L":C:\\x", L"0:", L"99999999999999999999999999:x", L"-1:x", L"4:abc", L"3:abc", L"4:C:\\xgarbage" }) {
			vecDecoded = { L"unchanged" };
			Require(!DeserializeDirectories(pszInvalid, vecDecoded, wstrError) && (vecDecoded == std::vector<std::wstring> { L"unchanged" }), "Malformed shutdown data changed its output or was accepted");
		}

		SERVICE_STATUS_PROCESS ServiceIdentity {};
		ServiceIdentity.dwProcessId = 123;
		for (DWORD unState : { SERVICE_RUNNING, SERVICE_PAUSE_PENDING, SERVICE_PAUSED, SERVICE_CONTINUE_PENDING }) {
			ServiceIdentity.dwCurrentState = unState;
			Require(HasValidServiceProcessID(ServiceIdentity), "A documented valid service process state was rejected");
		}

		for (DWORD unState : { SERVICE_START_PENDING, SERVICE_STOP_PENDING, SERVICE_STOPPED, 0 }) {
			ServiceIdentity.dwCurrentState = unState;
			Require(!HasValidServiceProcessID(ServiceIdentity), "A pending or stopped service PID was eligible for forced process shutdown");
		}

		ServiceIdentity.dwCurrentState = SERVICE_RUNNING;
		ServiceIdentity.dwProcessId = 0;
		Require(!HasValidServiceProcessID(ServiceIdentity), "A zero service PID was treated as a process identity");

		Require(IsValidOwnedServiceName(L"WireGuardTunnel$WireSteam-host") && IsValidOwnedServiceName(L"WireGuardTunnel$WireSteam-ui-123456abcdef"), "Valid owned service names were rejected");
		for (auto const& pszInvalid : { L"WireGuardManager", L"WireGuardTunnel$OtherVPN", L"WireGuardTunnel$WireSteam-", L"WireGuardTunnel$WireSteam-../../private", L"WireGuardTunnel$WireSteam-name with space", L"WireGuardTunnel$WireSteam-123456789012345678901" }) {
			Require(!IsValidOwnedServiceName(pszInvalid), "An unrelated or unsafe service name was eligible for shutdown");
		}

		std::vector<wchar_t> vecTestImage(kMaximumPathCharacters);
		const DWORD unTestImageCount = GetModuleFileNameW(nullptr, vecTestImage.data(), static_cast<DWORD>(vecTestImage.size()));
		Require((unTestImageCount > 0) && (unTestImageCount < vecTestImage.size()), "The service ownership test could not locate its executable");
		std::wstring wstrCanonicalTestImage;
		Require(ResolveCanonicalPath(std::wstring(vecTestImage.data(), unTestImageCount), false, wstrCanonicalTestImage, wstrError), "The service ownership test could not resolve its executable");
		std::wstring const wstrCommandPrefix = L"\"" + wstrCanonicalTestImage + L"\" ";
		Require(IsOwnedServiceCommand(L"WireGuardTunnel$WireSteam-host", (wstrCommandPrefix + L"/tunnelservice \"C:\\private\\WireSteam-host.conf\"").c_str(), wstrCanonicalTestImage), "A valid exact service command was rejected");
		for (auto const& pszArguments : { L"/managerservice", L"/tunnelservice C:\\private\\OtherVPN.conf", L"/tunnelservice C:\\private\\WireSteam-host.conf extra", L"/tunnelservice relative\\WireSteam-host.conf" }) {
			Require(!IsOwnedServiceCommand(L"WireGuardTunnel$WireSteam-host", (wstrCommandPrefix + pszArguments).c_str(), wstrCanonicalTestImage), "An unrelated service command was eligible for shutdown");
		}

		Require(IsExecutableFile(wstrCanonicalTestImage) && !IsExecutableFile(wstrCanonicalTestImage + L".missing") && !IsExecutableFile(wstrCanonicalTestImage.substr(0, wstrCanonicalTestImage.find_last_of(L'\\'))), "Executable prerequisite checks accepted a missing runtime or directory");
		TestShutdownOrdering();
		TestScopedTermination();

		Require(IsWireSteamService(L"WireGuardTunnel$WireSteam-host") && IsWireSteamService(L"wireguardtunnel$wiresteam-ui-123456abcdef") &&
			IsWireSteamService(L"WireGuardTunnel$WireSteam-"),
			"WireSteam service namespace matching failed");
		Require(!IsWireSteamService(L"WireGuardTunnel$OtherVPN") && !IsWireSteamService(L"WireGuardTunnel$WireSteamOther-host"),
			"An unrelated WireGuard service was matched");
		Require(FormatServiceCleanupMessage(L"WireGuardTunnel$WireSteam-ui-123456abcdef").find(L"--name ui-123456abcdef") != std::wstring::npos,
			"Owned service cleanup guidance omitted its safe tunnel alias");
		Require(FormatServiceCleanupMessage(L"WireGuardTunnel$WireSteam-private\\unsafe").find(L"private") == std::wstring::npos,
			"An untrusted service suffix leaked into cleanup guidance");
		std::cout << "PASS installer guard: actual-version policy, uninstall exemption, seven payload names, current/previous directory boundaries, DOS/UNC normalization and all-state service namespace matching, service PID validity, missing-runtime boundaries, cleanup ordering and scoped process termination. Only isolated child fixtures were stopped; no installed processes, services, adapters or profiles were changed.\n";

		return 0;
	} catch (std::exception const& Error) {
		std::cerr << Error.what() << '\n';
		return 1;
	}
}
