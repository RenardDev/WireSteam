#include "internet_routing.hpp"

// General
// Windows SDK
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A00000E
#define WIRESTEAM_ROUTING_OWNS_NTDDI_VERSION
#endif
#include <winsock2.h>
// ws2tcpip enables the IP route records exposed by netioapi.
#include <ws2tcpip.h>
#include <Windows.h>
#include <fwpmu.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <objbase.h>
#include <tlhelp32.h>

// C++
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <utility>

// STL
#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <vector>

// Project
#include "internet_legacy_routes.hpp"

namespace ws {
	namespace {
		using fnAddPolicy = DWORD(WINAPI*)(HANDLE, FWPM_PROVIDER_CONTEXT3 const*, FWP_IP_VERSION, UINT64, UINT32, FWPM_FILTER_CONDITION0 const*, PSECURITY_DESCRIPTOR);

		constexpr UINT8 kVPNPermitWeight = 15;
		constexpr UINT8 kLegacyVPNPermitWeight = 13;
		constexpr UINT8 kLegacyTransportBlockWeight = 14;
		constexpr UINT16 kDHCPClientPort = 68;
		constexpr UINT16 kDHCPServerPort = 67;
		constexpr UINT8 kDHCPPermitWeight = 15;
		constexpr UINT8 kLoopbackPermitWeight = 15;
		constexpr UINT8 kOutsideBlockWeight = 1;

		struct Module {
			Module() {
				m_hModule = LoadLibraryExW(L"fwpuclnt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			}

			Module(Module const&) = delete;
			Module& operator=(Module const&) = delete;

			~Module() {
				if (m_hModule) {
					// Terminal release of this LoadLibrary reference is best effort.
					FreeLibrary(m_hModule);
				}
			}

			fnAddPolicy GetAddPolicy() const {
				// GetProcAddress exposes an untyped procedure; the SDK signature/calling convention is exact.
				return m_hModule ? reinterpret_cast<fnAddPolicy>(GetProcAddress(m_hModule, "FwpmConnectionPolicyAdd0")) : nullptr;
			}

			HMODULE m_hModule;
		};

		struct Handle {
			explicit Handle(HANDLE const hHandle = nullptr) :
				m_hValue(hHandle)
			{
			}

			Handle(Handle const&) = delete;
			Handle& operator=(Handle const&) = delete;

			~Handle() {
				if (m_hValue && (m_hValue != INVALID_HANDLE_VALUE)) {
					// This wrapper releases only its acquired handle; a failed close cannot be retried safely.
					CloseHandle(m_hValue);
				}
			}

			HANDLE m_hValue;
		};

		struct ApplicationID {
			ApplicationID() {
				m_pValue = nullptr;
			}

			ApplicationID(ApplicationID const&) = delete;
			ApplicationID& operator=(ApplicationID const&) = delete;

			~ApplicationID() {
				if (m_pValue) {
					void* pMemory = m_pValue;
					FwpmFreeMemory0(&pMemory);
				}
			}

			FWP_BYTE_BLOB* m_pValue;
		};

		struct InterfaceTableDeleter {
			void operator()(MIB_IF_TABLE2* pTable) const noexcept {
				if (pTable) {
					FreeMibTable(pTable);
				}
			}
		};

		class PolicyTransaction {
		public:
			explicit PolicyTransaction(HANDLE const hEngine) noexcept :
				m_hEngine(hEngine)
			{
				m_bActive = true;
			}

			PolicyTransaction(PolicyTransaction const&) = delete;
			PolicyTransaction& operator=(PolicyTransaction const&) = delete;

			~PolicyTransaction() {
				if (m_bActive) {
					// Best effort: the parent session retains any failed abort until Stop closes it.
					FwpmTransactionAbort0(m_hEngine);
				}
			}

			void Release() noexcept {
				m_bActive = false;
			}

		private:
			// Caller owns the session and holds its filter mutex through this guard.
			HANDLE m_hEngine;
			bool m_bActive;
		};

		struct Uplink {
			Uplink() noexcept {
				m_LUID = {};
				m_unSource = 0;
				m_unNextHop = 0;
			}

			NET_LUID m_LUID;
			ULONG m_unSource;
			ULONG m_unNextHop;
			std::wstring m_wstrName;
		};

		static_assert(std::is_nothrow_default_constructible_v<Uplink>);
		static_assert(std::is_nothrow_move_assignable_v<Uplink>);
		static_assert(sizeof(ULONG) == 0x04);
		static_assert(sizeof(wchar_t) == 0x02);

		std::string FormatWindowsFailure(char const* pszOperation, const DWORD unCode) {
			return std::string(pszOperation) + " failed (Windows error " + std::to_string(unCode) + ")";
		}

		std::string GetMissingRoutePolicyReason(const DWORD unBuild, const DWORD unRevision) {
			constexpr DWORD kFirstWindows11Build = 22000;

			std::string strPlatform = "this Windows version";
			if (unBuild) {
				strPlatform = (unBuild >= kFirstWindows11Build) ? "Windows 11" : "Windows 10";
				strPlatform += " (10.0." + std::to_string(unBuild) + "." + std::to_string(unRevision) + ")";
			}

			return "IPv4 Internet-client mode is unavailable on " + strPlatform + ": Windows has neither connection-event routing nor per-application route policy support. "
				"Private-network profiles and Internet-server profiles remain available. "
				"Internet-client mode requires Windows 10 or later with Windows Filtering Platform available.";
		}

		std::string GetMissingRoutePolicyReason() {
			constexpr std::size_t kVersionCharacters = 32;
			constexpr DWORD kDecimalBase = 10;

			static_assert((kVersionCharacters * sizeof(wchar_t)) <= std::numeric_limits<DWORD>::max());

			// Diagnostic only: the actual API export decides availability, independently of OS/manifest version.
			std::array<wchar_t, kVersionCharacters> szVersionText {};
			DWORD unBytes = static_cast<DWORD>(sizeof(szVersionText));
			DWORD unBuild = 0;
			DWORD unRevision = 0;
			const LSTATUS nBuildStatus = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, szVersionText.data(), &unBytes);
			if ((nBuildStatus == ERROR_SUCCESS) && (unBytes <= sizeof(szVersionText)) && ((unBytes % sizeof(wchar_t)) == 0)) {
				auto itEnd = std::find(szVersionText.begin(), szVersionText.end(), L'\0');
				if ((itEnd != szVersionText.begin()) && (itEnd != szVersionText.end()) && std::all_of(szVersionText.begin(), itEnd, [](const wchar_t wchCharacter) -> bool {
					return (wchCharacter >= L'0') && (wchCharacter <= L'9');
				})) {
					for (auto itCharacter = szVersionText.begin(); itCharacter != itEnd; ++itCharacter) {
						const DWORD unDigit = static_cast<DWORD>(*itCharacter - L'0');
						if (unBuild > ((std::numeric_limits<DWORD>::max() - unDigit) / kDecimalBase)) {
							unBuild = 0;
							break;
						}

						unBuild = (unBuild * kDecimalBase) + unDigit;
					}
				}
			}

			unBytes = sizeof(unRevision);
			const LSTATUS nRevisionStatus = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr, &unRevision, &unBytes);
			if ((nRevisionStatus != ERROR_SUCCESS) || (unBytes != sizeof(unRevision))) {
				unRevision = 0;
			}

			return GetMissingRoutePolicyReason(unBuild, unRevision);
		}

		std::string EncodeUTF8(std::wstring const& wstrText) {
			if (wstrText.empty()) {
				return {};
			}

			if (wstrText.size() > std::numeric_limits<int>::max()) {
				throw std::length_error("Windows UTF-8 conversion input exceeds the API limit");
			}

			const int nInputCharacters = static_cast<int>(wstrText.size());
			const int nCount = WideCharToMultiByte(CP_UTF8, 0, wstrText.data(), nInputCharacters, nullptr, 0, nullptr, nullptr);
			if (nCount <= 0) {
				return {};
			}

			std::string strResult = std::string(static_cast<std::size_t>(nCount), '\0');
			if (WideCharToMultiByte(CP_UTF8, 0, wstrText.data(), nInputCharacters, strResult.data(), nCount, nullptr, nullptr) != nCount) {
				return {};
			}

			return strResult;
		}

		std::wstring ToLowercase(std::wstring wstrText) {
			std::transform(wstrText.begin(), wstrText.end(), wstrText.begin(), [](const wchar_t wchCharacter) -> wchar_t {
				return static_cast<wchar_t>(std::towlower(wchCharacter));
			});

			return wstrText;
		}

		bool IsValidExecutable(std::filesystem::path const& Executable) {
			if (!Executable.is_absolute() || (Executable.extension() != L".exe")) {
				return false;
			}

			const DWORD unAttributes = GetFileAttributesW(Executable.c_str());

			return (unAttributes != INVALID_FILE_ATTRIBUTES) && !(unAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
		}

		bool IsUsableSource(const ULONG unNetworkOrder) {
			constexpr unsigned kFirstOctetShift = 0x18;
			constexpr unsigned kTwoOctetShift = 0x10;
			constexpr unsigned kHighNibbleShift = 0x1C;
			constexpr ULONG kBroadcastAddress = 0xFFFFFFFF;
			constexpr ULONG kLoopbackOctet = 0x7F;
			constexpr ULONG kLinkLocalPrefix = 0xA9FE;
			constexpr ULONG kMulticastNibble = 0x0E;

			const ULONG unHostAddress = ntohl(unNetworkOrder);

			return ((unHostAddress >> kFirstOctetShift) != 0x00) && (unHostAddress != kBroadcastAddress) && ((unHostAddress >> kFirstOctetShift) != kLoopbackOctet) && ((unHostAddress >> kTwoOctetShift) != kLinkLocalPrefix) && ((unHostAddress >> kHighNibbleShift) < kMulticastNibble);
		}

		bool ParseOverlayIPv4(std::string const& strText, ULONG& unAddressOut) {
			constexpr std::size_t kMaximumAddressCharacters = 15;
			constexpr std::size_t kMaximumPrefixCharacters = 2;
			constexpr int kMaximumPrefix = 32;
			constexpr unsigned kFirstOctetShift = 0x18;
			constexpr unsigned kPrivate172Shift = 0x14;
			constexpr unsigned kTwoOctetShift = 0x10;
			constexpr ULONG kPrivate10Prefix = 0x0A;
			constexpr ULONG kPrivate172Prefix = 0xAC1;
			constexpr ULONG kPrivate192Prefix = 0xC0A8;

			const std::size_t unSlash = strText.find('/');
			std::string const strBare = strText.substr(0, unSlash);
			if (strBare.empty() || (strBare.size() > kMaximumAddressCharacters) || (strBare.find('\0') != std::string::npos)) {
				return false;
			}

			if (unSlash != std::string::npos) {
				std::string const strPrefix = strText.substr(unSlash + 1);
				if (strPrefix.empty() || (strPrefix.size() > kMaximumPrefixCharacters) || !std::all_of(strPrefix.begin(), strPrefix.end(), [](const char chCharacter) -> bool {
					return (chCharacter >= '0') && (chCharacter <= '9');
				}) ||
					(std::stoi(strPrefix) > kMaximumPrefix)) {
					return false;
				}
			}

			IN_ADDR Parsed {};
			if (InetPtonA(AF_INET, strBare.c_str(), &Parsed) != 1) {
				return false;
			}

			const ULONG unHostAddress = ntohl(Parsed.S_un.S_addr);
			if (((unHostAddress >> kFirstOctetShift) != kPrivate10Prefix) && ((unHostAddress >> kPrivate172Shift) != kPrivate172Prefix) && ((unHostAddress >> kTwoOctetShift) != kPrivate192Prefix)) {
				return false;
			}

			unAddressOut = Parsed.S_un.S_addr;

			return true;
		}

		bool CaptureUplink(Uplink& UplinkOut, std::string& strErrorOut, const ULONG unInterfaceIndex = 0) {
			constexpr ULONG kRouteProbeAddress = 0x08080808;
			constexpr UINT8 kMaximumDefaultPrefix = 1;

			SOCKADDR_INET Target {};
			SOCKADDR_INET Source {};
			Target.si_family = AF_INET;
			Target.Ipv4.sin_addr.S_un.S_addr = htonl(kRouteProbeAddress);
			MIB_IPFORWARD_ROW2 Route {};
			DWORD unCode = GetBestRoute2(nullptr, unInterfaceIndex, nullptr, &Target, 0, &Route, &Source);
			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Find the current IPv4 Internet uplink", unCode);
				return false;
			}

			if ((Source.si_family != AF_INET) || (Route.NextHop.si_family != AF_INET) || !IsUsableSource(Source.Ipv4.sin_addr.S_un.S_addr) || !Route.InterfaceLuid.Value) {
				strErrorOut = "The current IPv4 Internet uplink has no usable source address";
				return false;
			}

			// A public destination-specific exception is not a trustworthy default
			// uplink. A normal default and an existing VPN's split /1 defaults are.
			if (Route.DestinationPrefix.PrefixLength > kMaximumDefaultPrefix) {
				strErrorOut = "The current Internet route is destination-specific; an IPv4 default or split-default uplink is required";
				return false;
			}

			MIB_IF_ROW2 InterfaceRow {};
			InterfaceRow.InterfaceLuid = Route.InterfaceLuid;
			unCode = GetIfEntry2(&InterfaceRow);
			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Read the IPv4 uplink interface", unCode);
				return false;
			}

			std::wstring const wstrAlias = ToLowercase(InterfaceRow.Alias);
			std::wstring const wstrDescription = ToLowercase(InterfaceRow.Description);
			if ((InterfaceRow.OperStatus != IfOperStatusUp) || (InterfaceRow.Type == IF_TYPE_SOFTWARE_LOOPBACK) || (wstrAlias.find(L"wiresteam") != std::wstring::npos) || (wstrDescription.find(L"wiresteam") != std::wstring::npos)) {
				strErrorOut = "The current Internet uplink is down, loopback, or another WireSteam tunnel";
				return false;
			}

			if (!InterfaceRow.InterfaceAndOperStatusFlags.HardwareInterface) {
				strErrorOut = "Internet-client mode requires a direct physical Internet uplink; disable WARP or another VPN before activating it";
				return false;
			}

			Uplink Pending;
			Pending.m_LUID = Route.InterfaceLuid;
			Pending.m_unSource = Source.Ipv4.sin_addr.S_un.S_addr;
			Pending.m_unNextHop = Route.NextHop.Ipv4.sin_addr.S_un.S_addr;
			Pending.m_wstrName = InterfaceRow.Alias;
			UplinkOut = std::move(Pending);

			return true;
		}

		std::vector<std::filesystem::path> FindSteamExecutables(std::string& strErrorOut) {
			constexpr DWORD kMaximumPathCharacters = 32768;

			static_assert(sizeof(PROCESSENTRY32W) <= std::numeric_limits<DWORD>::max());

			std::vector<std::filesystem::path> vecPaths;
			Handle Snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
			if (Snapshot.m_hValue == INVALID_HANDLE_VALUE) {
				const DWORD unCode = GetLastError();
				strErrorOut = FormatWindowsFailure("Find the running Steam client", unCode);
				return {};
			}

			PROCESSENTRY32W Process {};
			Process.dwSize = sizeof(Process);
			if (!Process32FirstW(Snapshot.m_hValue, &Process)) {
				const DWORD unCode = GetLastError();
				strErrorOut = FormatWindowsFailure("Enumerate the running Steam client", unCode);
				return {};
			}

			do {
				if (_wcsicmp(Process.szExeFile, L"steam.exe") != 0) {
					continue;
				}

				Handle ProcessHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Process.th32ProcessID));
				if (!ProcessHandle.m_hValue) {
					continue;
				}

				std::wstring wstrImage = std::wstring(kMaximumPathCharacters, L'\0');
				DWORD unLength = static_cast<DWORD>(wstrImage.size());
				if (!QueryFullProcessImageNameW(ProcessHandle.m_hValue, 0, wstrImage.data(), &unLength) || (unLength > wstrImage.size())) {
					continue;
				}

				wstrImage.resize(unLength);
				std::filesystem::path const Executable = wstrImage;
				if (!IsValidExecutable(Executable)) {
					continue;
				}

				if (std::none_of(vecPaths.begin(), vecPaths.end(), [&Executable](std::filesystem::path const& Existing) -> bool {
					return _wcsicmp(Existing.c_str(), Executable.c_str()) == 0;
				})) {
					vecPaths.push_back(Executable);
				}
			} while (Process32NextW(Snapshot.m_hValue, &Process));

			if (vecPaths.empty()) {
				strErrorOut = "No readable running Steam client was found; start and sign in to Steam first";
			}

			return vecPaths;
		}

		bool EnableActiveRoutePolicies(std::string& strErrorOut, const bool bIPv6 = false) {
			constexpr UINT kMaximumPathCharacters = 32768;
			constexpr DWORD kPolicyTimeoutMilliseconds = 10000;
			constexpr DWORD kTerminationTimeoutMilliseconds = 5000;
			constexpr UINT kTerminationExitCode = 1;

			static_assert(sizeof(STARTUPINFOW) <= std::numeric_limits<DWORD>::max());

			std::wstring wstrDirectory = std::wstring(kMaximumPathCharacters, L'\0');
			const UINT unSize = GetSystemDirectoryW(wstrDirectory.data(), static_cast<UINT>(wstrDirectory.size()));
			const DWORD unDirectoryError = !unSize ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
			if (!unSize || (unSize >= wstrDirectory.size())) {
				strErrorOut = FormatWindowsFailure("Find Windows netsh", unDirectoryError);
				return false;
			}

			wstrDirectory.resize(unSize);
			std::filesystem::path const Executable = std::filesystem::path(wstrDirectory) / L"netsh.exe";
			std::wstring wstrArguments = L"\"" + Executable.wstring() + L"\" interface " + (bIPv6 ? L"ipv6" : L"ipv4") + L" set global routepolicies=enabled store=active";
			STARTUPINFOW Startup {};
			Startup.cb = sizeof(Startup);
			Startup.dwFlags = STARTF_USESHOWWINDOW;
			Startup.wShowWindow = SW_HIDE;
			PROCESS_INFORMATION Process {};
			if (!CreateProcessW(Executable.c_str(), wstrArguments.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Startup, &Process)) {
				const DWORD unCode = GetLastError();
				strErrorOut = FormatWindowsFailure("Enable active IPv4 route policies", unCode);
				return false;
			}

			Handle ThreadHandle(Process.hThread);
			Handle ProcessHandle(Process.hProcess);
			const DWORD unWaitStatus = WaitForSingleObject(ProcessHandle.m_hValue, kPolicyTimeoutMilliseconds);
			const DWORD unWaitError = (unWaitStatus == WAIT_FAILED) ? GetLastError() : ERROR_SUCCESS;
			if (unWaitStatus != WAIT_OBJECT_0) {
				// Stop only the exact child created above; no process enumeration or shared process is targeted.
				const BOOL bTerminated = TerminateProcess(ProcessHandle.m_hValue, kTerminationExitCode);
				const DWORD unTerminationError = !bTerminated ? GetLastError() : ERROR_SUCCESS;
				const DWORD unCleanupStatus = WaitForSingleObject(ProcessHandle.m_hValue, kTerminationTimeoutMilliseconds);
				const DWORD unCleanupError = (unCleanupStatus == WAIT_FAILED) ? GetLastError() : ERROR_SUCCESS;
				if (unCleanupStatus != WAIT_OBJECT_0) {
					if (!bTerminated) {
						strErrorOut = FormatWindowsFailure("Stop the owned route-policy helper", unTerminationError);

					} else if (unCleanupStatus == WAIT_FAILED) {
						strErrorOut = FormatWindowsFailure("Wait for the owned route-policy helper to stop", unCleanupError);

					} else {
						strErrorOut = "The owned route-policy helper did not exit after termination";
					}

					strErrorOut += "; child completion was not confirmed (wait status " + std::to_string(unCleanupStatus) + ")";
					return false;
				}

				strErrorOut = (unWaitStatus == WAIT_FAILED) ? FormatWindowsFailure("Wait for active IPv4 route policies", unWaitError) : "Windows did not finish enabling active IPv4 route policies in time";
				return false;
			}

			DWORD unExitCode = 0;
			if (!GetExitCodeProcess(ProcessHandle.m_hValue, &unExitCode)) {
				const DWORD unCode = GetLastError();
				strErrorOut = FormatWindowsFailure("Read the route-policy helper exit status", unCode);
				return false;
			}

			if (unExitCode != 0) {
				strErrorOut = "Windows rejected IPv4 route policies; Internet-client mode requires administrator approval and Windows 11 24H2 or later";
				return false;
			}

			return true;
		}

		FWPM_FILTER_CONDITION0 BuildNonLoopbackCondition() {
			FWPM_FILTER_CONDITION0 Condition {};
			Condition.fieldKey = FWPM_CONDITION_FLAGS;
			Condition.matchType = FWP_MATCH_FLAGS_NONE_SET;
			Condition.conditionValue.type = FWP_UINT32;
			Condition.conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;

			return Condition;
		}

		FWP_BYTE_ARRAY16 BuildMappedIPv4Address(const ULONG unNetworkOrder) {
			constexpr std::size_t kMarkerOffset = 0x0A;
			constexpr std::size_t kAddressOffset = 0x0C;

			static_assert((kAddressOffset + sizeof(ULONG)) == sizeof(FWP_BYTE_ARRAY16));

			FWP_BYTE_ARRAY16 Result {};
			Result.byteArray16[kMarkerOffset] = 0xFF;
			Result.byteArray16[kMarkerOffset + 0x01] = 0xFF;
			std::memcpy(Result.byteArray16 + kAddressOffset, &unNetworkOrder, sizeof(unNetworkOrder));

			return Result;
		}

		FWP_V6_ADDR_AND_MASK BuildMappedIPv4Prefix() {
			constexpr std::size_t kMarkerOffset = 0x0A;
			constexpr UINT8 kPrefixLength = 96;

			FWP_V6_ADDR_AND_MASK Prefix {};
			Prefix.addr[kMarkerOffset] = 0xFF;
			Prefix.addr[kMarkerOffset + 0x01] = 0xFF;
			Prefix.prefixLength = kPrefixLength;

			return Prefix;
		}

		FWPM_FILTER_CONDITION0 BuildMappedRemoteCondition(FWP_V6_ADDR_AND_MASK* pPrefix) {
			// The returned SDK condition borrows pPrefix through filter registration.
			FWPM_FILTER_CONDITION0 Condition {};
			Condition.fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
			Condition.matchType = FWP_MATCH_EQUAL;
			Condition.conditionValue.type = FWP_V6_ADDR_MASK;
			Condition.conditionValue.v6AddrMask = pPrefix;

			return Condition;
		}

		bool PinApplication(HANDLE hEngine, fnAddPolicy fnAddConnectionPolicy, std::filesystem::path const& Executable, Uplink const& UplinkValue, std::string& strErrorOut) {
			constexpr std::size_t kConditionCount = 3;
			constexpr UINT32 kBaseConditionCount = 2;
			constexpr std::size_t kSettingCount = 3;
			constexpr UINT32 kDirectSettingCount = 2;
			// ULL is required for this decimal priority to retain its unsigned 64-bit literal type.
			constexpr UINT64 kPolicyWeight = 17293822569102704640ULL;

			static_assert(kConditionCount <= std::numeric_limits<UINT32>::max());
			static_assert(kSettingCount <= std::numeric_limits<UINT32>::max());

			if (!hEngine || !fnAddConnectionPolicy) {
				strErrorOut = "The transport route policy session or export is missing";
				return false;
			}

			ApplicationID Identity;
			DWORD unCode = FwpmGetAppIdFromFileName0(Executable.c_str(), &Identity.m_pValue);
			if (unCode != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Identify an Internet transport executable", unCode);
				return false;
			}

			if (!Identity.m_pValue || !Identity.m_pValue->data || !Identity.m_pValue->size) {
				strErrorOut = FormatWindowsFailure("Identify an Internet transport executable", ERROR_INVALID_DATA);
				return false;
			}

			std::array<FWPM_FILTER_CONDITION0, kConditionCount> arrConditions {};
			arrConditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
			arrConditions[0].matchType = FWP_MATCH_EQUAL;
			arrConditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
			arrConditions[0].conditionValue.byteBlob = Identity.m_pValue;
			arrConditions[1] = BuildNonLoopbackCondition();
			UINT64 unInterfaceLUID = UplinkValue.m_LUID.Value;
			std::array<FWPM_NETWORK_CONNECTION_POLICY_SETTING0, kSettingCount> arrSettings {};
			arrSettings[0].type = FWP_NETWORK_CONNECTION_POLICY_NEXT_HOP_INTERFACE;
			arrSettings[0].value.type = FWP_UINT64;
			arrSettings[0].value.uint64 = &unInterfaceLUID;
			arrSettings[1].type = FWP_NETWORK_CONNECTION_POLICY_SOURCE_ADDRESS;
			arrSettings[1].value.type = FWP_UINT32;
			arrSettings[1].value.uint32 = UplinkValue.m_unSource;
			arrSettings[2].type = FWP_NETWORK_CONNECTION_POLICY_NEXT_HOP;
			arrSettings[2].value.type = FWP_UINT32;
			arrSettings[2].value.uint32 = UplinkValue.m_unNextHop;
			FWPM_NETWORK_CONNECTION_POLICY_SETTINGS0 Policies {};
			// An unspecified next hop is resolved on the pinned interface.
			Policies.numSettings = UplinkValue.m_unNextHop ? static_cast<UINT32>(arrSettings.size()) : kDirectSettingCount;
			Policies.settings = arrSettings.data();
			FWPM_PROVIDER_CONTEXT3 Context {};
			if (CoCreateGuid(&Context.providerContextKey) != S_OK) {
				strErrorOut = "Cannot create the transport route policy identity";
				return false;
			}

			// WFP copies the display text and all call-local policy/condition carriers.
			Context.displayData.name = const_cast<wchar_t*>(L"WireSteam Internet transport uplink");
			Context.displayData.description = const_cast<wchar_t*>(L"Keeps Steam signaling and encrypted relay transport outside the WireSteam Internet tunnel.");
			Context.type = FWPM_NETWORK_CONNECTION_POLICY_CONTEXT;
			Context.networkConnectionPolicy = &Policies;
			unCode = fnAddConnectionPolicy(hEngine, &Context, FWP_IP_VERSION_V4, kPolicyWeight, kBaseConditionCount, arrConditions.data(), nullptr);
			if (unCode != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Install the per-application transport route", unCode);
				return false;
			}

			// ALE can classify an IPv4 flow on the V6 layer when its application uses
			// a true dual-stack socket. Cover that representation explicitly instead
			// of assuming that a V4-only app policy will see every IPv4 socket.
			// Routing policy source/nexthop addresses use network byte order; the
			// mapped representation retains the original four wire-order bytes.
			FWP_BYTE_ARRAY16 MappedSource = BuildMappedIPv4Address(UplinkValue.m_unSource);
			FWP_BYTE_ARRAY16 MappedNextHop = BuildMappedIPv4Address(UplinkValue.m_unNextHop);
			FWP_V6_ADDR_AND_MASK MappedPrefix = BuildMappedIPv4Prefix();
			arrConditions[2] = BuildMappedRemoteCondition(&MappedPrefix);
			arrSettings[1].value.type = FWP_BYTE_ARRAY16_TYPE;
			arrSettings[1].value.byteArray16 = &MappedSource;
			arrSettings[2].value.type = FWP_BYTE_ARRAY16_TYPE;
			arrSettings[2].value.byteArray16 = &MappedNextHop;

			if (CoCreateGuid(&Context.providerContextKey) != S_OK) {
				strErrorOut = "Cannot create the dual-stack transport route identity";
				return false;
			}

			Context.displayData.name = const_cast<wchar_t*>(L"WireSteam IPv4-mapped transport uplink");
			unCode = fnAddConnectionPolicy(hEngine, &Context, FWP_IP_VERSION_V6, kPolicyWeight, static_cast<UINT32>(arrConditions.size()), arrConditions.data(), nullptr);
			if (unCode != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Install the IPv4-mapped per-application transport route", unCode);
				return false;
			}

			return true;
		}

		bool BlockIPv6(HANDLE hEngine, GUID const& Sublayer, std::string& strErrorOut) {
			constexpr UINT8 kBlockWeight = 1;

			std::array<GUID, 2> const arrLayers { FWPM_LAYER_ALE_AUTH_CONNECT_V6, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6 };

			FWPM_FILTER_CONDITION0 Condition = BuildNonLoopbackCondition();
			FWPM_FILTER0 Filter {};
			// The SDK copies this immutable display text during filter registration.
			Filter.displayData.name = const_cast<wchar_t*>(L"WireSteam IPv4 Internet mode: prevent IPv6 bypass");
			Filter.subLayerKey = Sublayer;
			Filter.weight.type = FWP_UINT8;
			Filter.weight.uint8 = kBlockWeight;
			Filter.action.type = FWP_ACTION_BLOCK;
			Filter.numFilterConditions = 1;
			Filter.filterCondition = &Condition;
			for (auto const& Layer : arrLayers) {
				Filter.layerKey = Layer;
				const DWORD unCode = FwpmFilterAdd0(hEngine, &Filter, nullptr, nullptr);
				if (unCode != ERROR_SUCCESS) {
					strErrorOut = FormatWindowsFailure("Prevent IPv6 bypass during IPv4 Internet mode", unCode);
					return false;
				}
			}

			return true;
		}

		bool AddIPFilters(HANDLE hEngine, GUID const& Sublayer, wchar_t const* pszName, const FWP_ACTION_TYPE unAction, const UINT8 unWeight, FWPM_FILTER_CONDITION0* pConditions, const UINT32 unCount, std::string& strErrorOut, const bool bIPv6 = false) {
			if ((unCount != 0) && !pConditions) {
				strErrorOut = "The Internet-client egress guard conditions are missing";
				return false;
			}

			FWPM_FILTER0 Filter {};
			// WFP copies the borrowed name/condition carriers before this call returns.
			Filter.displayData.name = const_cast<wchar_t*>(pszName);
			Filter.subLayerKey = Sublayer;
			Filter.weight.type = FWP_UINT8;
			Filter.weight.uint8 = unWeight;
			Filter.action.type = unAction;
			Filter.numFilterConditions = unCount;
			Filter.filterCondition = pConditions;
			// Receive-accept guards prevent an unsolicited physical-interface flow
			// from authorizing replies that would bypass the outbound-connect layer.
			std::array<GUID, 2> const arrLayers = bIPv6 ? std::array<GUID, 2> { FWPM_LAYER_ALE_AUTH_CONNECT_V6, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6 } : std::array<GUID, 2> { FWPM_LAYER_ALE_AUTH_CONNECT_V4, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4 };
			for (auto const& Layer : arrLayers) {
				Filter.layerKey = Layer;
				const DWORD unCode = FwpmFilterAdd0(hEngine, &Filter, nullptr, nullptr);
				if (unCode != ERROR_SUCCESS) {
					strErrorOut = FormatWindowsFailure("Install the Internet-client IPv4 egress guard", unCode);
					return false;
				}
			}

			return true;
		}

		FWPM_FILTER_CONDITION0 BuildInterfaceCondition(UINT64* punInterfaceLUID) {
			// The returned SDK condition borrows this scalar through registration.
			FWPM_FILTER_CONDITION0 Condition {};
			Condition.fieldKey = FWPM_CONDITION_IP_LOCAL_INTERFACE;
			Condition.matchType = FWP_MATCH_EQUAL;
			Condition.conditionValue.type = FWP_UINT64;
			Condition.conditionValue.uint64 = punInterfaceLUID;

			return Condition;
		}

		bool PermitMappedTransport(HANDLE hEngine, GUID const& Sublayer, std::vector<std::filesystem::path> const& vecApplications, Uplink const& UplinkValue, std::string& strErrorOut, const UINT8 unWeight = kVPNPermitWeight) {
			constexpr std::size_t kTransportConditionCount = 4;
			constexpr std::size_t kDHCPConditionCount = 5;

			static_assert(kTransportConditionCount <= std::numeric_limits<UINT32>::max());
			static_assert(kDHCPConditionCount <= std::numeric_limits<UINT32>::max());

			FWP_V6_ADDR_AND_MASK Prefix = BuildMappedIPv4Prefix();
			UINT64 unInterfaceLUID = UplinkValue.m_LUID.Value;
			for (auto const& Executable : vecApplications) {
				ApplicationID Identity;
				const DWORD unCode = FwpmGetAppIdFromFileName0(Executable.c_str(), &Identity.m_pValue);
				if (unCode != ERROR_SUCCESS) {
					strErrorOut = FormatWindowsFailure("Identify a dual-stack transport executable", unCode);
					return false;
				}

				if (!Identity.m_pValue || !Identity.m_pValue->data || !Identity.m_pValue->size) {
					strErrorOut = FormatWindowsFailure("Identify a dual-stack transport executable", ERROR_INVALID_DATA);
					return false;
				}

				std::array<FWPM_FILTER_CONDITION0, kTransportConditionCount> arrConditions {};
				arrConditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
				arrConditions[0].matchType = FWP_MATCH_EQUAL;
				arrConditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
				arrConditions[0].conditionValue.byteBlob = Identity.m_pValue;
				arrConditions[1] = BuildInterfaceCondition(&unInterfaceLUID);
				arrConditions[2] = BuildMappedRemoteCondition(&Prefix);
				FWP_BYTE_ARRAY16 PhysicalSource = BuildMappedIPv4Address(UplinkValue.m_unSource);
				arrConditions[3].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
				arrConditions[3].matchType = FWP_MATCH_EQUAL;
				arrConditions[3].conditionValue.type = FWP_BYTE_ARRAY16_TYPE;
				arrConditions[3].conditionValue.byteArray16 = &PhysicalSource;
				if (!AddIPFilters(hEngine, Sublayer, L"WireSteam: allow only IPv4-mapped Steam transport on its physical uplink", FWP_ACTION_PERMIT, unWeight, arrConditions.data(), static_cast<UINT32>(arrConditions.size()), strErrorOut, true)) {
					return false;
				}
			}

			std::array<FWPM_FILTER_CONDITION0, kDHCPConditionCount> arrDHCPConditions {};
			arrDHCPConditions[0] = BuildInterfaceCondition(&unInterfaceLUID);
			arrDHCPConditions[1] = BuildMappedRemoteCondition(&Prefix);
			arrDHCPConditions[2].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
			arrDHCPConditions[2].matchType = FWP_MATCH_EQUAL;
			arrDHCPConditions[2].conditionValue.type = FWP_UINT8;
			arrDHCPConditions[2].conditionValue.uint8 = IPPROTO_UDP;
			arrDHCPConditions[3].fieldKey = FWPM_CONDITION_IP_LOCAL_PORT;
			arrDHCPConditions[3].matchType = FWP_MATCH_EQUAL;
			arrDHCPConditions[3].conditionValue.type = FWP_UINT16;
			arrDHCPConditions[3].conditionValue.uint16 = kDHCPClientPort;
			arrDHCPConditions[4].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
			arrDHCPConditions[4].matchType = FWP_MATCH_EQUAL;
			arrDHCPConditions[4].conditionValue.type = FWP_UINT16;
			arrDHCPConditions[4].conditionValue.uint16 = kDHCPServerPort;

			return AddIPFilters(hEngine, Sublayer, L"WireSteam: allow IPv4-mapped DHCP on its physical uplink", FWP_ACTION_PERMIT, kDHCPPermitWeight, arrDHCPConditions.data(), static_cast<UINT32>(arrDHCPConditions.size()), strErrorOut, true);
		}

		bool PermitPhysicalTransport(HANDLE hEngine, GUID const& Sublayer, std::vector<std::filesystem::path> const& vecApplications, Uplink const& UplinkValue, std::string& strErrorOut, const UINT8 unWeight = kVPNPermitWeight) {
			constexpr std::size_t kConditionCount = 3;

			static_assert(kConditionCount <= std::numeric_limits<UINT32>::max());

			UINT64 unInterfaceLUID = UplinkValue.m_LUID.Value;
			for (auto const& Executable : vecApplications) {
				ApplicationID Identity;
				const DWORD unCode = FwpmGetAppIdFromFileName0(Executable.c_str(), &Identity.m_pValue);
				if (unCode != ERROR_SUCCESS) {
					strErrorOut = FormatWindowsFailure("Identify an Internet transport executable", unCode);
					return false;
				}

				if (!Identity.m_pValue || !Identity.m_pValue->data || !Identity.m_pValue->size) {
					strErrorOut = FormatWindowsFailure("Identify an Internet transport executable", ERROR_INVALID_DATA);
					return false;
				}

				std::array<FWPM_FILTER_CONDITION0, kConditionCount> arrPermitConditions {};
				arrPermitConditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
				arrPermitConditions[0].matchType = FWP_MATCH_EQUAL;
				arrPermitConditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
				arrPermitConditions[0].conditionValue.byteBlob = Identity.m_pValue;
				arrPermitConditions[1] = BuildInterfaceCondition(&unInterfaceLUID);
				arrPermitConditions[2].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
				arrPermitConditions[2].matchType = FWP_MATCH_EQUAL;
				arrPermitConditions[2].conditionValue.type = FWP_UINT32;
				arrPermitConditions[2].conditionValue.uint32 = ntohl(UplinkValue.m_unSource);
				if (!AddIPFilters(hEngine, Sublayer, L"WireSteam Internet mode: allow Steam transport on its physical uplink", FWP_ACTION_PERMIT, unWeight, arrPermitConditions.data(), static_cast<UINT32>(arrPermitConditions.size()), strErrorOut)) {
					return false;
				}
			}

			return true;
		}

		bool GuardIPv4(HANDLE hEngine, GUID const& Sublayer, std::vector<std::filesystem::path> const& vecApplications, Uplink const& UplinkValue, std::string& strErrorOut, const UINT8 unTransportWeight = kVPNPermitWeight) {
			constexpr std::size_t kDHCPConditionCount = 4;

			static_assert(kDHCPConditionCount <= std::numeric_limits<UINT32>::max());

			FWPM_FILTER_CONDITION0 Loopback = BuildNonLoopbackCondition();
			Loopback.matchType = FWP_MATCH_FLAGS_ALL_SET;
			if (!AddIPFilters(hEngine, Sublayer, L"WireSteam Internet mode: allow local loopback", FWP_ACTION_PERMIT, kLoopbackPermitWeight, &Loopback, 1, strErrorOut)) {
				return false;
			}

			if (!PermitPhysicalTransport(hEngine, Sublayer, vecApplications, UplinkValue, strErrorOut, unTransportWeight)) {
				return false;
			}

			UINT64 unInterfaceLUID = UplinkValue.m_LUID.Value;
			std::array<FWPM_FILTER_CONDITION0, kDHCPConditionCount> arrDHCPConditions {};
			arrDHCPConditions[0] = BuildInterfaceCondition(&unInterfaceLUID);
			arrDHCPConditions[1].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
			arrDHCPConditions[1].matchType = FWP_MATCH_EQUAL;
			arrDHCPConditions[1].conditionValue.type = FWP_UINT8;
			arrDHCPConditions[1].conditionValue.uint8 = IPPROTO_UDP;
			arrDHCPConditions[2].fieldKey = FWPM_CONDITION_IP_LOCAL_PORT;
			arrDHCPConditions[2].matchType = FWP_MATCH_EQUAL;
			arrDHCPConditions[2].conditionValue.type = FWP_UINT16;
			arrDHCPConditions[2].conditionValue.uint16 = kDHCPClientPort;
			arrDHCPConditions[3].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
			arrDHCPConditions[3].matchType = FWP_MATCH_EQUAL;
			arrDHCPConditions[3].conditionValue.type = FWP_UINT16;
			arrDHCPConditions[3].conditionValue.uint16 = kDHCPServerPort;
			if (!AddIPFilters(hEngine, Sublayer, L"WireSteam Internet mode: retain IPv4 DHCP on its physical uplink", FWP_ACTION_PERMIT, kDHCPPermitWeight, arrDHCPConditions.data(), static_cast<UINT32>(arrDHCPConditions.size()), strErrorOut)) {
				return false;
			}

			FWPM_FILTER_CONDITION0 Outside = BuildNonLoopbackCondition();

			return AddIPFilters(hEngine, Sublayer, L"WireSteam Internet mode: block untunneled IPv4 and DNS", FWP_ACTION_BLOCK, kOutsideBlockWeight, &Outside, 1, strErrorOut);
		}

		bool AddVPNPermit(HANDLE hEngine, GUID const& Sublayer, UINT64 unInterfaceLUID, const ULONG unOverlayAddress, std::string& strErrorOut, const UINT8 unWeight = kVPNPermitWeight) {
			constexpr std::size_t kConditionCount = 2;
			constexpr std::size_t kMappedConditionCount = 3;

			static_assert(kConditionCount <= std::numeric_limits<UINT32>::max());
			static_assert(kMappedConditionCount <= std::numeric_limits<UINT32>::max());

			std::array<FWPM_FILTER_CONDITION0, kConditionCount> arrConditions {};
			arrConditions[0] = BuildInterfaceCondition(&unInterfaceLUID);
			arrConditions[1].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
			arrConditions[1].matchType = FWP_MATCH_EQUAL;
			arrConditions[1].conditionValue.type = FWP_UINT32;
			arrConditions[1].conditionValue.uint32 = ntohl(unOverlayAddress);
			// Ordinary WFP V4 address matching uses host byte order. IPv6-mapped
			// matching uses the sixteen wire-order address bytes instead.
			if (!AddIPFilters(hEngine, Sublayer, L"WireSteam Internet mode: allow authenticated VPN egress", FWP_ACTION_PERMIT, unWeight, arrConditions.data(), static_cast<UINT32>(arrConditions.size()), strErrorOut)) {
				return false;
			}

			FWP_BYTE_ARRAY16 MappedSource = BuildMappedIPv4Address(unOverlayAddress);
			FWP_V6_ADDR_AND_MASK MappedPrefix = BuildMappedIPv4Prefix();
			std::array<FWPM_FILTER_CONDITION0, kMappedConditionCount> arrMappedConditions {};
			arrMappedConditions[0] = BuildInterfaceCondition(&unInterfaceLUID);
			arrMappedConditions[1].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
			arrMappedConditions[1].matchType = FWP_MATCH_EQUAL;
			arrMappedConditions[1].conditionValue.type = FWP_BYTE_ARRAY16_TYPE;
			arrMappedConditions[1].conditionValue.byteArray16 = &MappedSource;
			arrMappedConditions[2] = BuildMappedRemoteCondition(&MappedPrefix);

			return AddIPFilters(hEngine, Sublayer, L"WireSteam: allow IPv4-mapped VPN egress through its exact interface and address", FWP_ACTION_PERMIT, unWeight, arrMappedConditions.data(), static_cast<UINT32>(arrMappedConditions.size()), strErrorOut, true);
		}

		std::array<FWPM_FILTER_CONDITION0, 2> BuildApplicationInterfaceConditions(FWP_BYTE_BLOB* pApplication, UINT64* punInterfaceLUID) {
			// These SDK fields borrow both carriers through the caller's filter registration.
			std::array<FWPM_FILTER_CONDITION0, 2> arrConditions {};
			arrConditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
			arrConditions[0].matchType = FWP_MATCH_EQUAL;
			arrConditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
			arrConditions[0].conditionValue.byteBlob = pApplication;
			arrConditions[1] = BuildInterfaceCondition(punInterfaceLUID);

			return arrConditions;
		}

		bool BlockLegacyTransportOnVPN(HANDLE hEngine, GUID const& Sublayer, std::vector<std::filesystem::path> const& vecApplications, UINT64 unInterfaceLUID, std::string& strErrorOut) {
			constexpr std::array<bool, 2> kIPv6Modes { false, true };

			for (auto const& Executable : vecApplications) {
				ApplicationID Application;
				const DWORD unCode = FwpmGetAppIdFromFileName0(Executable.c_str(), &Application.m_pValue);
				if (unCode != ERROR_SUCCESS) {
					strErrorOut = FormatWindowsFailure("Identify the Steam transport recursion guard", unCode);
					return false;
				}

				if (!Application.m_pValue || !Application.m_pValue->data || !Application.m_pValue->size) {
					strErrorOut = FormatWindowsFailure("Identify the Steam transport recursion guard", ERROR_INVALID_DATA);
					return false;
				}

				std::array<FWPM_FILTER_CONDITION0, 2> arrConditions = BuildApplicationInterfaceConditions(Application.m_pValue, &unInterfaceLUID);
				for (auto const& bIPv6 : kIPv6Modes) {
					if (!AddIPFilters(hEngine, Sublayer, L"WireSteam Windows 10: prevent Steam transport from entering its own VPN", FWP_ACTION_BLOCK, kLegacyTransportBlockWeight, arrConditions.data(), static_cast<UINT32>(arrConditions.size()), strErrorOut, bIPv6)) {
						return false;
					}
				}
			}

			return true;
		}

		bool LearnLegacyUDP(HANDLE hEngine, GUID const& Sublayer, std::vector<std::filesystem::path> const& vecApplications, std::string& strErrorOut) {
			constexpr std::size_t kConditionCount = 3;
			constexpr std::array<bool, 2> kIPv6Modes { false, true };

			static_assert(kConditionCount <= std::numeric_limits<UINT32>::max());

			for (auto const& Executable : vecApplications) {
				ApplicationID Application;
				const DWORD unCode = FwpmGetAppIdFromFileName0(Executable.c_str(), &Application.m_pValue);
				if (unCode != ERROR_SUCCESS) {
					strErrorOut = FormatWindowsFailure("Identify the Steam UDP endpoint learner", unCode);
					return false;
				}

				if (!Application.m_pValue || !Application.m_pValue->data || !Application.m_pValue->size) {
					strErrorOut = FormatWindowsFailure("Identify the Steam UDP endpoint learner", ERROR_INVALID_DATA);
					return false;
				}

				std::array<FWPM_FILTER_CONDITION0, kConditionCount> arrConditions {};
				arrConditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
				arrConditions[0].matchType = FWP_MATCH_EQUAL;
				arrConditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
				arrConditions[0].conditionValue.byteBlob = Application.m_pValue;
				arrConditions[1].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
				arrConditions[1].matchType = FWP_MATCH_EQUAL;
				arrConditions[1].conditionValue.type = FWP_UINT8;
				arrConditions[1].conditionValue.uint8 = IPPROTO_UDP;
				arrConditions[2] = BuildNonLoopbackCondition();
				for (auto const& bIPv6 : kIPv6Modes) {
					if (!AddIPFilters(hEngine, Sublayer, L"WireSteam Windows 10: learn unknown Steam UDP endpoints before permitting transport", FWP_ACTION_BLOCK, kLegacyTransportBlockWeight, arrConditions.data(), static_cast<UINT32>(arrConditions.size()), strErrorOut, bIPv6)) {
						return false;
					}
				}
			}

			return true;
		}
	} // namespace

	struct InternetClientRouting::Implementation {
		Implementation() {
			m_hEngine = nullptr;
			m_Sublayer = {};
			m_unOverlayAddress = 0;
			m_unAdapterLUID = 0;
			m_bGuardEnabled = false;
		}

		~Implementation() {
			if (m_hEngine) {
				try {
					std::lock_guard<std::mutex> FilterLock = std::lock_guard<std::mutex>(m_FilterMutex);
					// Terminal abort is best effort; session close is the final dynamic-policy cleanup.
					FwpmTransactionAbort0(m_hEngine);
				} catch (...) {
					std::fputs("WireSteam: cannot serialize routing session cleanup.\n", stderr);
					std::terminate();
				}
			}

			// Release the filter mutex before waiting for the worker, which may need it.
			if (m_pLegacyRoutes) {
				m_pLegacyRoutes->Stop();
				m_pLegacyRoutes.reset();
			}

			if (m_hEngine) {
				// Callback/worker state is already quiescent; handle close is terminal best effort.
				FwpmEngineClose0(m_hEngine);
			}
		}

		// Keep the module loaded until its engine and optional legacy worker are released.
		Module m_Module;
		HANDLE m_hEngine;
		std::string m_strUplinkDescription;
		GUID m_Sublayer;
		ULONG m_unOverlayAddress;
		UINT64 m_unAdapterLUID;
		Uplink m_Underlay;
		std::vector<std::filesystem::path> m_vecApplications;
		bool m_bGuardEnabled;
		std::mutex m_FilterMutex;
		std::unique_ptr<LegacyInternetRoutes> m_pLegacyRoutes;
	};

	InternetClientRouting::InternetClientRouting() {
		m_pImplementation = std::make_unique<Implementation>();
	}

	InternetClientRouting::~InternetClientRouting() {
	}

	bool InternetClientRouting::IsSupported(std::string& strReasonOut) {
		Module RoutingModule;
		if (!RoutingModule.GetAddPolicy() && !LegacyInternetRoutes::IsSupported()) {
			strReasonOut = GetMissingRoutePolicyReason();
			return false;
		}

		strReasonOut.clear();

		return true;
	}

	bool InternetClientRouting::CheckPreflight(std::string& strReasonOut) {
		if (!IsSupported(strReasonOut)) {
			return false;
		}

		Uplink CapturedUplink;
		if (!CaptureUplink(CapturedUplink, strReasonOut)) {
			return false;
		}

		strReasonOut.clear();

		return true;
	}

	bool InternetClientRouting::ProbeRegistration(std::filesystem::path const& DiagnosticExecutable, const unsigned unPhysicalInterfaceIndex, std::string& strErrorOut) {
		constexpr UINT16 kSublayerWeight = 65535;
		constexpr ULONG kDiagnosticOverlayAddress = 0x0A4D00FE;

		struct AbortedSession {
			AbortedSession() {
				m_hEngine = nullptr;
				m_bTransaction = false;
			}

			AbortedSession(AbortedSession const&) = delete;
			AbortedSession& operator=(AbortedSession const&) = delete;

			~AbortedSession() {
				// This diagnostic never commits. Abort/close are terminal best-effort cleanup.
				if (m_bTransaction) {
					FwpmTransactionAbort0(m_hEngine);
				}

				if (m_hEngine) {
					FwpmEngineClose0(m_hEngine);
				}
			}

			HANDLE m_hEngine;
			bool m_bTransaction;
		};

		strErrorOut.clear();
		Module RoutingModule;
		const fnAddPolicy fnAddConnectionPolicy = RoutingModule.GetAddPolicy();
		if (!fnAddConnectionPolicy && !LegacyInternetRoutes::IsSupported()) {
			strErrorOut = "Route registration diagnostics require Windows 10 or later with connection-event routing support";
			return false;
		}

		if (!unPhysicalInterfaceIndex || !IsValidExecutable(DiagnosticExecutable)) {
			strErrorOut = "Registration diagnostics require an existing absolute executable and an explicit physical interface index";
			return false;
		}

		Uplink CapturedUplink;
		if (!CaptureUplink(CapturedUplink, strErrorOut, unPhysicalInterfaceIndex)) {
			return false;
		}

		AbortedSession OwnedSession;
		FWPM_SESSION0 Session {};
		// WFP copies its immutable display text and record fields during registration.
		Session.displayData.name = const_cast<wchar_t*>(L"WireSteam route API validation: transaction is never committed");
		Session.flags = FWPM_SESSION_FLAG_DYNAMIC;
		DWORD unCode = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &Session, &OwnedSession.m_hEngine);
		if (unCode != ERROR_SUCCESS) {
			strErrorOut = FormatWindowsFailure("Open administrator-only route registration diagnostics", unCode);
			return false;
		}

		unCode = FwpmTransactionBegin0(OwnedSession.m_hEngine, 0);
		if (unCode != ERROR_SUCCESS) {
			strErrorOut = FormatWindowsFailure("Begin always-aborted route registration diagnostics", unCode);
			return false;
		}

		OwnedSession.m_bTransaction = true;

		// All real API builders run inside this invisible transaction. The
		// owning destructor aborts on success, failure, and exception alike.
		if (fnAddConnectionPolicy && !PinApplication(OwnedSession.m_hEngine, fnAddConnectionPolicy, DiagnosticExecutable, CapturedUplink, strErrorOut)) {
			return false;
		}

		GUID SublayerKey {};
		if (CoCreateGuid(&SublayerKey) != S_OK) {
			strErrorOut = "Cannot create the diagnostic policy identity";
			return false;
		}

		FWPM_SUBLAYER0 Sublayer {};
		Sublayer.subLayerKey = SublayerKey;
		Sublayer.displayData.name = const_cast<wchar_t*>(L"WireSteam filter API validation: transaction is never committed");
		Sublayer.weight = kSublayerWeight;
		unCode = FwpmSubLayerAdd0(OwnedSession.m_hEngine, &Sublayer, nullptr);
		if (unCode != ERROR_SUCCESS) {
			strErrorOut = FormatWindowsFailure("Stage the always-aborted diagnostic policy group", unCode);
			return false;
		}

		std::vector<std::filesystem::path> const vecApplications { DiagnosticExecutable };

		return GuardIPv4(OwnedSession.m_hEngine, SublayerKey, vecApplications, CapturedUplink, strErrorOut) &&
			PermitMappedTransport(OwnedSession.m_hEngine, SublayerKey, vecApplications, CapturedUplink, strErrorOut) &&
			BlockIPv6(OwnedSession.m_hEngine, SublayerKey, strErrorOut) &&
			LearnLegacyUDP(OwnedSession.m_hEngine, SublayerKey, vecApplications, strErrorOut) &&
			BlockLegacyTransportOnVPN(OwnedSession.m_hEngine, SublayerKey, vecApplications, CapturedUplink.m_LUID.Value, strErrorOut) &&
			LegacyInternetRoutes::ProbeEndpointPermits(OwnedSession.m_hEngine, &SublayerKey, DiagnosticExecutable, CapturedUplink.m_LUID.Value, CapturedUplink.m_unSource, strErrorOut) &&
			AddVPNPermit(OwnedSession.m_hEngine, SublayerKey, CapturedUplink.m_LUID.Value, htonl(kDiagnosticOverlayAddress), strErrorOut, kLegacyVPNPermitWeight) &&
			AddVPNPermit(OwnedSession.m_hEngine, SublayerKey, CapturedUplink.m_LUID.Value, htonl(kDiagnosticOverlayAddress), strErrorOut, kVPNPermitWeight);
	}

	bool InternetClientRouting::Start(std::filesystem::path const& CoreExecutable, std::string const& strOverlayIPv4, std::string& strErrorOut, const bool bDeferGuard) {
		constexpr UINT16 kSublayerWeight = 65535;

		if (m_pImplementation->m_hEngine) {
			strErrorOut = "Internet-client routing is already running";
			return false;
		}

		const fnAddPolicy fnAddConnectionPolicy = m_pImplementation->m_Module.GetAddPolicy();
		if (!fnAddConnectionPolicy && !LegacyInternetRoutes::IsSupported()) {
			strErrorOut = GetMissingRoutePolicyReason();
			return false;
		}

		if (!IsValidExecutable(CoreExecutable) || (_wcsicmp(CoreExecutable.filename().c_str(), L"WireSteam.exe") != 0)) {
			strErrorOut = "The Internet transport executable must be an existing absolute WireSteam.exe path";
			return false;
		}

		ULONG unOverlayAddress = 0;
		if (!ParseOverlayIPv4(strOverlayIPv4, unOverlayAddress)) {
			strErrorOut = "Internet-client routing requires one RFC1918 IPv4 tunnel address";
			return false;
		}

		Uplink CapturedUplink;
		if (!CaptureUplink(CapturedUplink, strErrorOut)) {
			return false;
		}

		std::vector<std::filesystem::path> vecSteamExecutables = FindSteamExecutables(strErrorOut);
		if (vecSteamExecutables.empty()) {
			return false;
		}

		vecSteamExecutables.push_back(CoreExecutable);
		try {
			FWPM_SESSION0 Session {};
			// WFP reads this immutable display text during session creation.
			Session.displayData.name = const_cast<wchar_t*>(L"WireSteam Internet client");
			Session.flags = FWPM_SESSION_FLAG_DYNAMIC;
			DWORD unCode = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &Session, &m_pImplementation->m_hEngine);
			if (unCode != ERROR_SUCCESS) {
				m_pImplementation->m_hEngine = nullptr;
				strErrorOut = FormatWindowsFailure("Open Internet-client WFP session", unCode);
				return false;
			}

			// Subscribe before committing the app-specific bootstrap filters: their
			// policy-change reauthorization also reveals already-open Steam UDP
			// sockets. Global telemetry options must be set outside a transaction.
			if (!fnAddConnectionPolicy) {
				m_pImplementation->m_pLegacyRoutes = std::make_unique<LegacyInternetRoutes>();
				if (!m_pImplementation->m_pLegacyRoutes->Start(m_pImplementation->m_hEngine, vecSteamExecutables, CapturedUplink.m_LUID.Value, CapturedUplink.m_unSource, CapturedUplink.m_unNextHop, unOverlayAddress, strErrorOut)) {
					Stop();
					return false;
				}
			}

			unCode = FwpmTransactionBegin0(m_pImplementation->m_hEngine, 0);
			if (unCode != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Begin Internet-client routing transaction", unCode);
				Stop();
				return false;
			}

			GUID SublayerKey {};
			FWPM_SUBLAYER0 Sublayer {};
			if (CoCreateGuid(&SublayerKey) != S_OK) {
				strErrorOut = "Cannot create the IPv6 policy identity";
				Stop();
				return false;
			}

			Sublayer.subLayerKey = SublayerKey;
			Sublayer.displayData.name = const_cast<wchar_t*>(L"WireSteam IPv4 Internet client");
			Sublayer.weight = kSublayerWeight;
			unCode = FwpmSubLayerAdd0(m_pImplementation->m_hEngine, &Sublayer, nullptr);
			if (unCode != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Create the Internet-client IPv6 policy group", unCode);
				Stop();
				return false;
			}

			for (auto const& Executable : vecSteamExecutables) {
				if (fnAddConnectionPolicy && !PinApplication(m_pImplementation->m_hEngine, fnAddConnectionPolicy, Executable, CapturedUplink, strErrorOut)) {
					Stop();
					return false;
				}
			}

			const UINT8 unTransportWeight = fnAddConnectionPolicy ? kVPNPermitWeight : kLegacyVPNPermitWeight;
			if (!PermitMappedTransport(m_pImplementation->m_hEngine, SublayerKey, vecSteamExecutables, CapturedUplink, strErrorOut, unTransportWeight)) {
				Stop();
				return false;
			}

			if (!fnAddConnectionPolicy && bDeferGuard && !PermitPhysicalTransport(m_pImplementation->m_hEngine, SublayerKey, vecSteamExecutables, CapturedUplink, strErrorOut, unTransportWeight)) {
				Stop();
				return false;
			}

			if (!bDeferGuard && !GuardIPv4(m_pImplementation->m_hEngine, SublayerKey, vecSteamExecutables, CapturedUplink, strErrorOut, unTransportWeight)) {
				Stop();
				return false;
			}

			if (!fnAddConnectionPolicy && !LearnLegacyUDP(m_pImplementation->m_hEngine, SublayerKey, vecSteamExecutables, strErrorOut)) {
				Stop();
				return false;
			}

			if (!BlockIPv6(m_pImplementation->m_hEngine, SublayerKey, strErrorOut)) {
				Stop();
				return false;
			}

			unCode = FwpmTransactionCommit0(m_pImplementation->m_hEngine);
			if (unCode != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Commit Internet-client routing", unCode);
				Stop();
				return false;
			}

			if (m_pImplementation->m_pLegacyRoutes && !m_pImplementation->m_pLegacyRoutes->ActivateBootstrap(&SublayerKey, m_pImplementation->m_FilterMutex, strErrorOut)) {
				Stop();
				return false;
			}

			if (fnAddConnectionPolicy) {
				if (!EnableActiveRoutePolicies(strErrorOut) || !EnableActiveRoutePolicies(strErrorOut, true)) {
					Stop();
					return false;
				}
			}

			m_pImplementation->m_strUplinkDescription = EncodeUTF8(CapturedUplink.m_wstrName) + (fnAddConnectionPolicy ? " | per-application routing" : " | Windows 10 guarded endpoint routing");
			m_pImplementation->m_Sublayer = SublayerKey;
			m_pImplementation->m_unOverlayAddress = unOverlayAddress;
			m_pImplementation->m_Underlay = CapturedUplink;
			m_pImplementation->m_vecApplications = std::move(vecSteamExecutables);
			m_pImplementation->m_bGuardEnabled = !bDeferGuard;
			strErrorOut.clear();
		} catch (...) {
			// The implementation owns every acquired session/worker during failed publication.
			Stop();
			throw;
		}

		return true;
	}

	bool InternetClientRouting::EnableGuard(std::string& strErrorOut) {
		strErrorOut.clear();

		if (!m_pImplementation->m_hEngine) {
			strErrorOut = "Internet-client routing is not running";
			return false;
		}

		if (m_pImplementation->m_bGuardEnabled) {
			return true;
		}

		if (m_pImplementation->m_pLegacyRoutes && !m_pImplementation->m_pLegacyRoutes->PrepareAdapter(strErrorOut)) {
			return false;
		}

		std::lock_guard<std::mutex> FilterLock = std::lock_guard<std::mutex>(m_pImplementation->m_FilterMutex);
		DWORD unCode = FwpmTransactionBegin0(m_pImplementation->m_hEngine, 0);
		if (unCode != ERROR_SUCCESS) {
			strErrorOut = FormatWindowsFailure("Begin the Internet-client IPv4 guard", unCode);
			return false;
		}

		PolicyTransaction Transaction(m_pImplementation->m_hEngine);
		if (!GuardIPv4(m_pImplementation->m_hEngine, m_pImplementation->m_Sublayer, m_pImplementation->m_vecApplications, m_pImplementation->m_Underlay, strErrorOut, m_pImplementation->m_pLegacyRoutes ? kLegacyVPNPermitWeight : kVPNPermitWeight)) {
			return false;
		}

		unCode = FwpmTransactionCommit0(m_pImplementation->m_hEngine);
		if (unCode != ERROR_SUCCESS) {
			strErrorOut = FormatWindowsFailure("Commit the Internet-client IPv4 guard", unCode);
			return false;
		}

		Transaction.Release();
		m_pImplementation->m_bGuardEnabled = true;

		return true;
	}

	bool InternetClientRouting::TryBindAdapter(std::wstring const& wstrTunnelName, bool& bBoundOut, std::string& strErrorOut) {
		constexpr std::size_t kMaximumTunnelNameCharacters = 30;

		bBoundOut = false;
		strErrorOut.clear();

		if (!m_pImplementation->m_hEngine) {
			strErrorOut = "Internet-client routing is not running";
			return false;
		}

		if (!Healthy(strErrorOut)) {
			return false;
		}

		if (!m_pImplementation->m_bGuardEnabled) {
			strErrorOut = "Enable the Internet-client egress guard before attaching its VPN adapter";
			return false;
		}

		if (m_pImplementation->m_unAdapterLUID) {
			bBoundOut = true;
			return true;
		}

		if ((wstrTunnelName.rfind(L"WireSteam-", 0) != 0) || (wstrTunnelName.size() > kMaximumTunnelNameCharacters)) {
			strErrorOut = "The Internet-client adapter must have a WireSteam-owned tunnel name";
			return false;
		}

		MIB_IF_TABLE2* pInterfaces = nullptr;
		DWORD unCode = GetIfTable2(&pInterfaces);
		std::unique_ptr<MIB_IF_TABLE2, InterfaceTableDeleter> pInterfaceTable(pInterfaces);
		if (unCode != NO_ERROR) {
			strErrorOut = FormatWindowsFailure("Find the Internet-client VPN adapter", unCode);
			return false;
		}

		if (!pInterfaceTable) {
			strErrorOut = FormatWindowsFailure("Find the Internet-client VPN adapter", ERROR_INVALID_DATA);
			return false;
		}

		NET_LUID LUID {};
		// The successful SDK allocation owns its full NumEntries/Table extent.
		for (std::size_t unInterfaceIndex = 0; unInterfaceIndex < pInterfaceTable->NumEntries; ++unInterfaceIndex) {
			MIB_IF_ROW2 const& Row = pInterfaceTable->Table[unInterfaceIndex];
			if (_wcsicmp(Row.Alias, wstrTunnelName.c_str()) != 0) {
				continue;
			}

			if (Row.InterfaceAndOperStatusFlags.HardwareInterface || (ToLowercase(Row.Description).find(L"wireguard") == std::wstring::npos)) {
				strErrorOut = "The expected Internet-client adapter is not an official WireGuard tunnel";
				return false;
			}

			LUID = Row.InterfaceLuid;
			break;
		}

		if (!LUID.Value) {
			return true;
		}

		MIB_UNICASTIPADDRESS_ROW Address {};
		InitializeUnicastIpAddressEntry(&Address);
		Address.InterfaceLuid = LUID;
		Address.Address.si_family = AF_INET;
		Address.Address.Ipv4.sin_addr.S_un.S_addr = m_pImplementation->m_unOverlayAddress;
		unCode = GetUnicastIpAddressEntry(&Address);
		if (unCode == ERROR_NOT_FOUND) {
			return true;
		}

		if (unCode != NO_ERROR) {
			strErrorOut = FormatWindowsFailure("Verify the Internet-client VPN address", unCode);
			return false;
		}

		if (Address.DadState != IpDadStatePreferred) {
			return true;
		}

		std::lock_guard<std::mutex> FilterLock = std::lock_guard<std::mutex>(m_pImplementation->m_FilterMutex);
		unCode = FwpmTransactionBegin0(m_pImplementation->m_hEngine, 0);
		if (unCode != ERROR_SUCCESS) {
			strErrorOut = FormatWindowsFailure("Begin the Internet-client VPN permit", unCode);
			return false;
		}

		PolicyTransaction Transaction(m_pImplementation->m_hEngine);
		if (m_pImplementation->m_pLegacyRoutes && !BlockLegacyTransportOnVPN(m_pImplementation->m_hEngine, m_pImplementation->m_Sublayer, m_pImplementation->m_vecApplications, LUID.Value, strErrorOut)) {
			return false;
		}

		if (!AddVPNPermit(m_pImplementation->m_hEngine, m_pImplementation->m_Sublayer, LUID.Value, m_pImplementation->m_unOverlayAddress, strErrorOut, m_pImplementation->m_pLegacyRoutes ? kLegacyVPNPermitWeight : kVPNPermitWeight)) {
			return false;
		}

		unCode = FwpmTransactionCommit0(m_pImplementation->m_hEngine);
		if (unCode != ERROR_SUCCESS) {
			strErrorOut = FormatWindowsFailure("Commit the Internet-client VPN permit", unCode);
			return false;
		}

		Transaction.Release();
		m_pImplementation->m_unAdapterLUID = LUID.Value;
		bBoundOut = true;

		return true;
	}

	bool InternetClientRouting::Healthy(std::string& strErrorOut) const {
		if (m_pImplementation->m_pLegacyRoutes) {
			return m_pImplementation->m_pLegacyRoutes->Healthy(strErrorOut);
		}

		strErrorOut.clear();

		return true;
	}

	void InternetClientRouting::Stop() noexcept {
		// A failed setup can still own the dynamic engine's write transaction.
		// Release it before restoring global telemetry on a separate engine.
		if (m_pImplementation->m_hEngine) {
			try {
				std::lock_guard<std::mutex> FilterLock = std::lock_guard<std::mutex>(m_pImplementation->m_FilterMutex);
				// Terminal best effort; the later session close is the final cleanup.
				FwpmTransactionAbort0(m_pImplementation->m_hEngine);
			} catch (...) {
				std::fputs("WireSteam: cannot serialize routing session cleanup.\n", stderr);
				std::terminate();
			}
		}

		// The worker may need the filter mutex; do not hold it through Stop/join.
		if (m_pImplementation->m_pLegacyRoutes) {
			m_pImplementation->m_pLegacyRoutes->Stop();
			m_pImplementation->m_pLegacyRoutes.reset();
		}

		if (m_pImplementation->m_hEngine) {
			// All borrowers are quiescent; this terminal handle close is best effort.
			FwpmEngineClose0(m_pImplementation->m_hEngine);
			m_pImplementation->m_hEngine = nullptr;
		}

		m_pImplementation->m_strUplinkDescription.clear();
		m_pImplementation->m_unAdapterLUID = 0;
		m_pImplementation->m_unOverlayAddress = 0;
		m_pImplementation->m_bGuardEnabled = false;
		m_pImplementation->m_vecApplications.clear();
		m_pImplementation->m_Underlay = {};
		// Enabling route-policy evaluation alone changes no route after our dynamic
		// session disappears. Leave the active-store switch enabled rather than
		// disabling policies another application may have installed in the meantime.
		// No persistent system setting is written; Windows resets it on reboot.
	}

	std::string InternetClientRouting::UplinkDescription() const {
		return m_pImplementation->m_strUplinkDescription;
	}

	void RunInternetRoutingTests() {
		constexpr UINT8 kMaximumFilterWeight = 15;
		constexpr UINT8 kMappedPrefixBits = 96;
		constexpr std::size_t kMappedPrefixBytes = 0x0C;
		constexpr ULONG kLoopbackAddress = 0x7F000001;
		constexpr ULONG kLinkLocalAddress = 0xA9FE0101;
		constexpr ULONG kMulticastAddress = 0xE0000001;
		constexpr ULONG kLANAddress = 0xC0A80164;
		constexpr ULONG kPublicAddress = 0x08080808;
		constexpr ULONG kOverlayAddress = 0x0A4D0002;
		constexpr char kEmbeddedNullAddress[] = "10.77.0.2\0ignored";

		RunLegacyInternetRoutesTests();

		auto Require = [](const bool bOkay, char const* pszText) -> void {
			if (!bOkay) {
				throw std::runtime_error(pszText);
			}
		};

		UINT64 unTestInterface = 42;
		BYTE arrTestIdentity[] { 0x01, 0x02, 0x03 };
		FWP_BYTE_BLOB TestApplication { sizeof(arrTestIdentity), arrTestIdentity };
		std::array<FWPM_FILTER_CONDITION0, 2> const arrRecursionConditions = BuildApplicationInterfaceConditions(&TestApplication, &unTestInterface);
		Require((kLegacyTransportBlockWeight > kLegacyVPNPermitWeight) && (kVPNPermitWeight <= kMaximumFilterWeight) && (kLegacyTransportBlockWeight <= kMaximumFilterWeight) && (arrRecursionConditions[0].fieldKey == FWPM_CONDITION_ALE_APP_ID) && (arrRecursionConditions[0].conditionValue.byteBlob == &TestApplication) && (arrRecursionConditions[1].fieldKey == FWPM_CONDITION_IP_LOCAL_INTERFACE) && (arrRecursionConditions[1].conditionValue.uint64 == &unTestInterface), "The Windows 10 Steam recursion guard must override the VPN permit only for its exact apps and VPN interface");

		std::string const strWindows10Reason = GetMissingRoutePolicyReason(19045, 6456);
		Require((strWindows10Reason.find("Windows 10 (10.0.19045.6456)") != std::string::npos) && (strWindows10Reason.find("Private-network profiles and Internet-server profiles remain available") != std::string::npos), "The Windows 10 capability error hides the supported tunnel modes");
		Require((GetMissingRoutePolicyReason(22631, 0).find("Windows 11 (10.0.22631.0)") != std::string::npos) && (GetMissingRoutePolicyReason(0, 0).find("this Windows version") != std::string::npos), "The missing-route-policy error misidentifies the current Windows version");
		Require(!IsUsableSource(0x00000000), "Internet uplink source accepted an unspecified address");
		Require(!IsUsableSource(htonl(kLoopbackAddress)), "Internet uplink source accepted loopback");
		Require(!IsUsableSource(htonl(kLinkLocalAddress)), "Internet uplink source accepted link-local");
		Require(!IsUsableSource(htonl(kMulticastAddress)), "Internet uplink source accepted multicast");
		Require(IsUsableSource(htonl(kLANAddress)), "Internet uplink source rejected a LAN uplink");
		Require(IsUsableSource(htonl(kPublicAddress)), "Internet uplink source rejected a public uplink");

		FWP_BYTE_ARRAY16 const MappedAddress = BuildMappedIPv4Address(htonl(kOverlayAddress));
		IN6_ADDR ExpectedMappedAddress {};
		IN6_ADDR RealIPv6Address {};
		Require((InetPtonA(AF_INET6, "::ffff:10.77.0.2", &ExpectedMappedAddress) == 1) && (std::memcmp(MappedAddress.byteArray16, &ExpectedMappedAddress, sizeof(ExpectedMappedAddress)) == 0), "IPv4-mapped VPN address lost its wire byte order");

		FWP_V6_ADDR_AND_MASK MappedPrefix = BuildMappedIPv4Prefix();
		Require((MappedPrefix.prefixLength == kMappedPrefixBits) && (std::memcmp(MappedPrefix.addr, MappedAddress.byteArray16, kMappedPrefixBytes) == 0) && (InetPtonA(AF_INET6, "2001:4860:4860::8888", &RealIPv6Address) == 1) && (std::memcmp(MappedPrefix.addr, &RealIPv6Address, kMappedPrefixBytes) != 0), "The mapped-IPv4 exception also matches genuine IPv6");
		FWPM_FILTER_CONDITION0 const MappedCondition = BuildMappedRemoteCondition(&MappedPrefix);
		Require((MappedCondition.fieldKey == FWPM_CONDITION_IP_REMOTE_ADDRESS) && (MappedCondition.matchType == FWP_MATCH_EQUAL) && (MappedCondition.conditionValue.type == FWP_V6_ADDR_MASK) && (MappedCondition.conditionValue.v6AddrMask == &MappedPrefix), "The dual-stack exception lacks a bounded mapped-IPv4 destination");

		ULONG unOverlayAddress = 0;
		Require(ParseOverlayIPv4("10.77.0.2/32", unOverlayAddress) && (unOverlayAddress == htonl(kOverlayAddress)), "Internet overlay parser lost the client address");
		Require(ParseOverlayIPv4("172.31.255.254", unOverlayAddress), "Internet overlay parser rejected private IPv4");

		const ULONG unAddressBeforeFailure = unOverlayAddress;
		std::string const strEmbeddedNullAddress = std::string(kEmbeddedNullAddress, sizeof(kEmbeddedNullAddress) - 1);
		Require(!ParseOverlayIPv4(strEmbeddedNullAddress, unOverlayAddress) && (unOverlayAddress == unAddressBeforeFailure), "Internet overlay parser accepted an embedded null or changed its output on failure");

		for (auto const& pszMalformedAddress : { "", "1.1.1.1", "127.0.0.1", "172.32.0.1", "192.167.0.1", "fd77::2", "10.77.0.2/33", "10.77.0.2/x", "10.77.0.2/32/0" }) {
			Require(!ParseOverlayIPv4(pszMalformedAddress, unOverlayAddress), "Internet overlay parser accepted an invalid or nonprivate address");
		}

		FWPM_FILTER_CONDITION0 const NonLoopbackCondition = BuildNonLoopbackCondition();
		Require((NonLoopbackCondition.fieldKey == FWPM_CONDITION_FLAGS) && (NonLoopbackCondition.matchType == FWP_MATCH_FLAGS_NONE_SET) && (NonLoopbackCondition.conditionValue.type == FWP_UINT32) && (NonLoopbackCondition.conditionValue.uint32 == FWP_CONDITION_FLAG_IS_LOOPBACK), "Internet routing failed to preserve loopback");

		InternetClientRouting UnusedRouting;
		UnusedRouting.Stop();
		UnusedRouting.Stop();
		Require(UnusedRouting.UplinkDescription().empty(), "Unused Internet routing has an uplink");
		bool bBound = true;
		std::string strBindingError;
		Require(!UnusedRouting.TryBindAdapter(L"WireSteam-test", bBound, strBindingError) && !bBound && !strBindingError.empty(), "Internet routing bound an adapter without an active guard");
		Require(!UnusedRouting.EnableGuard(strBindingError) && !strBindingError.empty(), "Internet routing enabled a guard without a WFP session");

		std::string strReason;
		const bool bSupported = InternetClientRouting::IsSupported(strReason);
		Require(bSupported == strReason.empty(), "Internet routing capability reason is inconsistent");

		if (bSupported) {
			Require(!InternetClientRouting::ProbeRegistration({}, 0, strReason) && !strReason.empty(), "The route registration probe accepted missing explicit diagnostics parameters");
		}
	}
} // namespace ws

#ifdef WIRESTEAM_ROUTING_OWNS_NTDDI_VERSION
#undef NTDDI_VERSION
#undef WIRESTEAM_ROUTING_OWNS_NTDDI_VERSION
#endif
