#include "internet_legacy_routes.hpp"

// General
// Windows SDK
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#define WIRESTEAM_LEGACY_ROUTES_OWNS_NTDDI_VERSION
#endif
#include <winsock2.h>
// ws2tcpip enables the IP route records exposed by the later netioapi include.
#include <ws2tcpip.h>
#include <Windows.h>
#include <fwpmu.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <objbase.h>
#include <sddl.h>
#include <tlhelp32.h>

// C++
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <utility>

// STL
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace ws {
	namespace {
		struct FWPValueDeleter {
			void operator()(FWP_VALUE0* pValue) const noexcept {
				void* pMemory = pValue;
				FwpmFreeMemory0(&pMemory);
			}
		};

		struct Key {
			Key() {
				m_hKey = nullptr;
			}

			~Key() {
				// Final owned-handle cleanup is best effort; a destructor cannot report a close failure.
				Close();
			}

			Key(Key const&) = delete;
			Key& operator=(Key const&) = delete;

			LSTATUS Close() noexcept {
				HKEY hKey = m_hKey;
				// A close attempt invalidates this wrapper; an ambiguous failure must not cause a retry.
				m_hKey = nullptr;

				return hKey ? RegCloseKey(hKey) : ERROR_SUCCESS;
			}

			HKEY m_hKey;
		};

		// The registry schema is four independent DWORD values; preserve this aggregate carrier.
		struct TelemetrySettings {
			DWORD m_unOldCollection;
			DWORD m_unOldKeywords;
			DWORD m_unOwnCollection;
			DWORD m_unOwnKeywords;
		};

		struct LocalMemoryDeleter {
			void operator()(void* pMemory) const noexcept {
				// ConvertStringSecurityDescriptor allocates LocalAlloc storage owned by this scope.
				LocalFree(pMemory);
			}
		};

		struct HandleDeleter {
			void operator()(void* pHandle) const noexcept {
				if (pHandle) {
					// Only locally acquired handles reach this owner; closure cannot be retried safely.
					CloseHandle(pHandle);
				}
			}
		};

		struct FWPBlobDeleter {
			void operator()(FWP_BYTE_BLOB* pBlob) const noexcept {
				if (pBlob) {
					void* pMemory = pBlob;
					FwpmFreeMemory0(&pMemory);
				}
			}
		};

		struct MIBTableDeleter {
			void operator()(MIB_UNICASTIPADDRESS_TABLE* pTable) const noexcept {
				if (pTable) {
					FreeMibTable(pTable);
				}
			}
		};

		constexpr wchar_t kRecoveryParent[] = L"SOFTWARE\\WireSteam";
		constexpr wchar_t kRecoveryName[] = L"LegacyRouting";
		constexpr wchar_t kRecoveryPath[] = L"SOFTWARE\\WireSteam\\LegacyRouting";
		constexpr wchar_t kLeaseName[] = L"Global\\WireSteamInternetLegacyRoutes-v1";
		constexpr wchar_t kRecoveryDescriptorSDDL[] = L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)";
		constexpr DWORD kRecoveryVersion = 1;
		constexpr TelemetrySettings kDefaultTelemetrySettings { 0, 0, 1, 0 };

		static_assert(sizeof(DWORD) == 0x04);
		static_assert(sizeof(SECURITY_ATTRIBUTES) <= std::numeric_limits<DWORD>::max());

		constexpr ULONG kRouteLifetimeSeconds = 45;
		constexpr std::size_t kMaximumRoutes = 4096;
		constexpr std::size_t kMaximumEndpointFilters = 32768;
		constexpr UINT32 kOutboundEventDirection = 0x3901; // FWPM_NET_EVENT_CLASSIFY_DROP2 documentation.
		constexpr UINT8 kIPv4PrefixLength = 32;

		static_assert(sizeof(ULONG) == 0x04);

		std::string FormatWindowsFailure(char const* pszOperation, DWORD unError) {
			return std::string(pszOperation) + " (Windows error " + std::to_string(unError) + ")";
		}

		bool IsPublicDestination(ULONG unNetworkOrder) {
			constexpr unsigned kFirstOctetShift = 0x18;
			constexpr unsigned kCarrierGradeNATShift = 0x16;
			constexpr unsigned kTwoOctetShift = 0x10;
			constexpr unsigned kPrivate172Shift = 0x14;
			constexpr ULONG kPrivate10Prefix = 0x0A;
			constexpr ULONG kLoopbackPrefix = 0x7F;
			constexpr ULONG kCarrierGradeNATPrefix = 0x191;
			constexpr ULONG kLinkLocalPrefix = 0xA9FE;
			constexpr ULONG kPrivate172Prefix = 0xAC1;
			constexpr ULONG kPrivate192Prefix = 0xC0A8;
			constexpr ULONG kMulticastPrefix = 0xE0;
			constexpr ULONG kBroadcastAddress = 0xFFFFFFFF;

			const ULONG unAddress = ntohl(unNetworkOrder);

			return unAddress && ((unAddress >> kFirstOctetShift) != 0x00) && ((unAddress >> kFirstOctetShift) != kPrivate10Prefix) && ((unAddress >> kFirstOctetShift) != kLoopbackPrefix) && ((unAddress >> kCarrierGradeNATShift) != kCarrierGradeNATPrefix) && ((unAddress >> kTwoOctetShift) != kLinkLocalPrefix) && ((unAddress >> kPrivate172Shift) != kPrivate172Prefix) && ((unAddress >> kTwoOctetShift) != kPrivate192Prefix) && ((unAddress >> kFirstOctetShift) < kMulticastPrefix) && (unAddress != kBroadcastAddress);
		}

		bool ReadMappedIPv4Address(FWP_BYTE_ARRAY16 const& Address, ULONG& unNetworkOrderOut) {
			constexpr std::array<BYTE, 0x0C> kMappedPrefix { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF };

			static_assert((kMappedPrefix.size() + sizeof(ULONG)) == sizeof(Address.byteArray16));

			if (std::memcmp(Address.byteArray16, kMappedPrefix.data(), kMappedPrefix.size()) != 0) {
				return false;
			}

			std::memcpy(&unNetworkOrderOut, Address.byteArray16 + kMappedPrefix.size(), sizeof(unNetworkOrderOut));

			return true;
		}

		bool ReadEventAddress(FWPM_NET_EVENT2 const& Event, ULONG unSource, ULONG unOverlay, std::vector<std::vector<BYTE>> const& vecIdentities, ULONG& unAddressOut, bool& bUDPOut) {
			constexpr UINT32 kFields = FWPM_NET_EVENT_FLAG_IP_VERSION_SET | FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET | FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET | FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET | FWPM_NET_EVENT_FLAG_APP_ID_SET;
			constexpr UINT32 kMaximumIdentityBytes = 65536;
			constexpr UINT32 kUnknownDirection = std::numeric_limits<UINT32>::max();

			if (((Event.header.flags & kFields) != kFields) || ((Event.header.ipProtocol != IPPROTO_TCP) && (Event.header.ipProtocol != IPPROTO_UDP)) || !Event.header.appId.data || !Event.header.appId.size || (Event.header.appId.size > kMaximumIdentityBytes)) {
				return false;
			}

			UINT32 unDirection = kUnknownDirection;
			bool bLoopback = true;
			if ((Event.type == FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW) && Event.classifyAllow) {
				unDirection = Event.classifyAllow->msFwpDirection;
				bLoopback = Event.classifyAllow->isLoopback != FALSE;

			} else if ((Event.type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP) && Event.classifyDrop) {
				unDirection = Event.classifyDrop->msFwpDirection;
				bLoopback = Event.classifyDrop->isLoopback != FALSE;

			} else {
				return false;
			}

			// Microsoft's two event structures document both representations. Do not
			// accept inbound/forward events or guess direction from addresses/ports.
			if (bLoopback || ((unDirection != FWP_DIRECTION_OUTBOUND) && (unDirection != kOutboundEventDirection))) {
				return false;
			}

			if (std::none_of(vecIdentities.begin(), vecIdentities.end(), [&Event](std::vector<BYTE> const& vecIdentity) -> bool {
				return (vecIdentity.size() == Event.header.appId.size) && (std::memcmp(vecIdentity.data(), Event.header.appId.data, vecIdentity.size()) == 0);
			})) {
				return false;
			}

			ULONG unAddress = 0;
			if (Event.header.ipVersion == FWP_IP_VERSION_V4) {
				// Match the known physical/overlay source to distinguish the observed
				// byte-order representations before converting the remote route address.
				if ((Event.header.localAddrV4 == ntohl(unSource)) || (Event.header.localAddrV4 == ntohl(unOverlay))) {
					unAddress = htonl(Event.header.remoteAddrV4);

				} else if ((Event.header.localAddrV4 == unSource) || (Event.header.localAddrV4 == unOverlay)) {
					unAddress = Event.header.remoteAddrV4;

				} else {
					return false;
				}

			} else if (Event.header.ipVersion == FWP_IP_VERSION_V6) {
				ULONG unLocal = 0;
				if (!ReadMappedIPv4Address(Event.header.localAddrV6, unLocal) || ((unLocal != unSource) && (unLocal != unOverlay)) || !ReadMappedIPv4Address(Event.header.remoteAddrV6, unAddress)) {
					return false;
				}

			} else {
				return false;
			}

			if (!IsPublicDestination(unAddress)) {
				return false;
			}

			unAddressOut = unAddress;
			bUDPOut = Event.header.ipProtocol == IPPROTO_UDP;

			return true;
		}

		MIB_IPFORWARD_ROW2 BuildEndpointRoute(ULONG unDestination, NET_LUID LUID, ULONG unNextHop, ULONG unMetric) {
			MIB_IPFORWARD_ROW2 Row {};
			InitializeIpForwardEntry(&Row);
			Row.InterfaceLuid = LUID;
			Row.DestinationPrefix.Prefix.si_family = AF_INET;
			Row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr = unDestination;
			Row.DestinationPrefix.PrefixLength = kIPv4PrefixLength;
			Row.NextHop.si_family = AF_INET;
			Row.NextHop.Ipv4.sin_addr.S_un.S_addr = unNextHop;
			Row.SitePrefixLength = kIPv4PrefixLength;
			Row.ValidLifetime = Row.PreferredLifetime = kRouteLifetimeSeconds;
			Row.Metric = unMetric;
			Row.Protocol = MIB_IPPROTO_NETMGMT;
			Row.Loopback = Row.AutoconfigureAddress = Row.Publish = Row.Immortal = FALSE;

			return Row;
		}

		bool MatchesOwnedRoute(MIB_IPFORWARD_ROW2 const& Current, MIB_IPFORWARD_ROW2 const& Owned) {
			return (Current.InterfaceLuid.Value == Owned.InterfaceLuid.Value) && (Current.DestinationPrefix.Prefix.si_family == AF_INET) && (Current.DestinationPrefix.PrefixLength == kIPv4PrefixLength) && (Current.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr == Owned.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr) && (Current.NextHop.si_family == AF_INET) && (Current.NextHop.Ipv4.sin_addr.S_un.S_addr == Owned.NextHop.Ipv4.sin_addr.S_un.S_addr) && (Current.Protocol == MIB_IPPROTO_NETMGMT) && (Current.Metric == Owned.Metric) && (Current.ValidLifetime <= kRouteLifetimeSeconds) && (Current.PreferredLifetime <= kRouteLifetimeSeconds) && !Current.Loopback && !Current.AutoconfigureAddress && !Current.Publish && !Current.Immortal;
		}

		template <class TGetRoute, class TCreateRoute, class TSetRoute>
		bool RefreshOwnedRoute(MIB_IPFORWARD_ROW2 const& Owned, TGetRoute fnGetRoute, TCreateRoute fnCreateRoute, TSetRoute fnSetRoute, std::string& strErrorOut) {
			MIB_IPFORWARD_ROW2 Current = Owned;
			DWORD unError = fnGetRoute(&Current);
			if (unError == ERROR_NOT_FOUND) {
				unError = fnCreateRoute(&Owned);

			} else if (unError == NO_ERROR) {
				if (!MatchesOwnedRoute(Current, Owned)) {
					strErrorOut = "A Steam endpoint route was changed by another application; it was not replaced";
					return false;
				}

				Current.ValidLifetime = Current.PreferredLifetime = kRouteLifetimeSeconds;
				unError = fnSetRoute(&Current);
			}

			if (unError != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Refresh a short-lived Steam endpoint route", unError);
				return false;
			}

			return true;
		}

		bool ReadTelemetryOption(HANDLE hEngine, FWPM_ENGINE_OPTION Option, UINT32& unResultOut, std::string& strErrorOut) {
			FWP_VALUE0* pRawValue = nullptr;
			const DWORD unError = FwpmEngineGetOption0(hEngine, Option, &pRawValue);
			std::unique_ptr<FWP_VALUE0, FWPValueDeleter> pValue(pRawValue);
			if (unError != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Read Windows connection telemetry settings", unError);
				return false;
			}

			const bool bValid = pValue && (pValue->type == FWP_UINT32);
			if (bValid) {
				unResultOut = pValue->uint32;
			}

			pValue.reset();

			if (!bValid) {
				strErrorOut = "Windows returned an invalid connection telemetry setting";
			}

			return bValid;
		}

		DWORD WriteTelemetryOption(HANDLE hEngine, FWPM_ENGINE_OPTION Option, UINT32 unValue) {
			FWP_VALUE0 Setting {};
			Setting.type = FWP_UINT32;
			Setting.uint32 = unValue;

			return FwpmEngineSetOption0(hEngine, Option, &Setting);
		}

		bool ValidateSettings(DWORD unVersion, TelemetrySettings const& Settings) {
			return (unVersion == kRecoveryVersion) && (Settings.m_unOldCollection <= 1) && (Settings.m_unOwnCollection == 1) && ((Settings.m_unOwnKeywords == Settings.m_unOldKeywords) || (Settings.m_unOwnKeywords == (Settings.m_unOldKeywords | FWPM_NET_EVENT_KEYWORD_CLASSIFY_ALLOW)));
		}

		// Borrow a complete descriptor returned by the security APIs (including its nested SID/ACL storage).
		bool ValidateDescriptorPermissions(PSECURITY_DESCRIPTOR pDescriptor) {
			constexpr DWORD kExpectedACECount = 2;
			constexpr std::size_t kSIDOffset = offsetof(ACCESS_ALLOWED_ACE, SidStart);
			constexpr std::size_t kSIDHeaderBytes = 0x08;
			constexpr std::size_t kSubAuthorityCountOffset = 0x01;

			static_assert(offsetof(ACCESS_ALLOWED_ACE, Header) == 0x00);
			static_assert(offsetof(SID, SubAuthorityCount) == kSubAuthorityCountOffset);
			static_assert(offsetof(SID, SubAuthority) == kSIDHeaderBytes);
			static_assert(SID_MAX_SUB_AUTHORITIES <= ((std::numeric_limits<std::size_t>::max() - kSIDHeaderBytes) / sizeof(DWORD)));

			if (!pDescriptor || !IsValidSecurityDescriptor(pDescriptor)) {
				return false;
			}

			alignas(SID) std::array<BYTE, SECURITY_MAX_SID_SIZE> arrAdminSID {};
			alignas(SID) std::array<BYTE, SECURITY_MAX_SID_SIZE> arrSystemSID {};
			DWORD unAdminBytes = static_cast<DWORD>(arrAdminSID.size());
			DWORD unSystemBytes = static_cast<DWORD>(arrSystemSID.size());
			if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, arrAdminSID.data(), &unAdminBytes) || !CreateWellKnownSid(WinLocalSystemSid, nullptr, arrSystemSID.data(), &unSystemBytes)) {
				return false;
			}

			PSID pOwner = nullptr;
			BOOL bDefaulted = FALSE;
			BOOL bPresent = FALSE;
			PACL pACL = nullptr;
			SECURITY_DESCRIPTOR_CONTROL unControl = 0;
			DWORD unRevision = 0;
			if (!GetSecurityDescriptorOwner(pDescriptor, &pOwner, &bDefaulted) || !pOwner || (!EqualSid(pOwner, arrAdminSID.data()) && !EqualSid(pOwner, arrSystemSID.data())) || !GetSecurityDescriptorControl(pDescriptor, &unControl, &unRevision) || !(unControl & SE_DACL_PROTECTED) || !GetSecurityDescriptorDacl(pDescriptor, &bPresent, &pACL, &bDefaulted) || !bPresent || !pACL || !IsValidAcl(pACL) || (pACL->AceCount != kExpectedACECount)) {
				return false;
			}

			bool bHasAdmin = false;
			bool bHasSystem = false;
			for (DWORD unIndex = 0; unIndex < pACL->AceCount; ++unIndex) {
				void* pRaw = nullptr;
				if (!GetAce(pACL, unIndex, &pRaw) || !pRaw) {
					return false;
				}

				// GetAce returns an aligned native ACE; verify its entire SID extent before inspection.
				auto const* pHeader = static_cast<ACE_HEADER const*>(pRaw);
				if ((pHeader->AceType != ACCESS_ALLOWED_ACE_TYPE) || pHeader->AceFlags || (pHeader->AceSize < (kSIDOffset + kSIDHeaderBytes))) {
					return false;
				}

				auto const* pACE = static_cast<ACCESS_ALLOWED_ACE const*>(pRaw);
				if ((pACE->Mask & KEY_ALL_ACCESS) != KEY_ALL_ACCESS) {
					return false;
				}

				BYTE const* punSIDBytes = reinterpret_cast<BYTE const*>(pRaw) + kSIDOffset;
				const std::size_t unSubAuthorityCount = punSIDBytes[kSubAuthorityCountOffset];
				if (unSubAuthorityCount > SID_MAX_SUB_AUTHORITIES) {
					return false;
				}

				const std::size_t unSIDBytes = kSIDHeaderBytes + unSubAuthorityCount * sizeof(DWORD);
				// These read-only Windows APIs expose mutable PSID parameters.
				PSID pSID = const_cast<BYTE*>(punSIDBytes);
				if ((unSIDBytes > (pHeader->AceSize - kSIDOffset)) || !IsValidSid(pSID)) {
					return false;
				}

				if (EqualSid(pSID, arrAdminSID.data()) && !bHasAdmin) {
					bHasAdmin = true;

				} else if (EqualSid(pSID, arrSystemSID.data()) && !bHasSystem) {
					bHasSystem = true;

				} else {
					return false;
				}
			}

			return bHasAdmin && bHasSystem;
		}

		bool ValidateKeyPermissions(HKEY hKey, std::string& strErrorOut) {
			constexpr SECURITY_INFORMATION kInformation = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
			constexpr DWORD kMaximumDescriptorBytes = 65536;

			DWORD unBytes = 0;
			if ((RegGetKeySecurity(hKey, kInformation, nullptr, &unBytes) != ERROR_INSUFFICIENT_BUFFER) || !unBytes || (unBytes > kMaximumDescriptorBytes)) {
				strErrorOut = "Cannot read Windows 10 telemetry recovery permissions";
				return false;
			}

			std::vector<BYTE> vecBuffer(unBytes);
			PSECURITY_DESCRIPTOR pDescriptor = vecBuffer.data();
			if ((RegGetKeySecurity(hKey, kInformation, pDescriptor, &unBytes) != ERROR_SUCCESS) || !ValidateDescriptorPermissions(pDescriptor)) {
				strErrorOut = "Windows 10 telemetry recovery must be owned and writable only by Administrators and SYSTEM";
				return false;
			}

			return true;
		}

		bool OpenParent(Key& Parent, bool bCreate, bool& bMissingOut, std::string& strErrorOut) {
			bMissingOut = false;
			LSTATUS nError = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRecoveryParent, REG_OPTION_OPEN_LINK, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &Parent.m_hKey);
			if (((nError == ERROR_FILE_NOT_FOUND) || (nError == ERROR_PATH_NOT_FOUND)) && !bCreate) {
				bMissingOut = true;
				return true;
			}

			if (((nError == ERROR_FILE_NOT_FOUND) || (nError == ERROR_PATH_NOT_FOUND)) && bCreate) {
				PSECURITY_DESCRIPTOR pRawDescriptor = nullptr;
				const BOOL bConverted = ConvertStringSecurityDescriptorToSecurityDescriptorW(kRecoveryDescriptorSDDL, SDDL_REVISION_1, &pRawDescriptor, nullptr);
				const DWORD unConversionError = bConverted ? ERROR_SUCCESS : GetLastError();
				std::unique_ptr<void, LocalMemoryDeleter> pDescriptor(pRawDescriptor);
				if (!bConverted) {
					strErrorOut = FormatWindowsFailure("Protect telemetry recovery namespace", unConversionError);
					return false;
				}

				SECURITY_ATTRIBUTES Attributes { static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), pDescriptor.get(), FALSE };
				nError = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRecoveryParent, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &Attributes, &Parent.m_hKey, nullptr);
				pDescriptor.reset();
			}

			if (nError != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Open telemetry recovery namespace", static_cast<DWORD>(nError));
				return false;
			}

			return ValidateKeyPermissions(Parent.m_hKey, strErrorOut);
		}

		bool ReadRegistryDWORD(HKEY hKey, wchar_t const* pszName, DWORD& unValueOut) {
			DWORD unType = 0;
			DWORD unValue = 0;
			DWORD unBytes = sizeof(unValue);
			const LSTATUS nError = RegQueryValueExW(hKey, pszName, nullptr, &unType, reinterpret_cast<BYTE*>(&unValue), &unBytes);
			if ((nError != ERROR_SUCCESS) || (unType != REG_DWORD) || (unBytes != sizeof(unValue))) {
				return false;
			}

			unValueOut = unValue;

			return true;
		}

		// bExistsOut reports presence even when validation fails; SettingsOut commits only on full success.
		bool ReadSettings(TelemetrySettings& SettingsOut, bool& bExistsOut, std::string& strErrorOut) {
			bExistsOut = false;
			Key Parent;
			Key Record;
			bool bMissing = false;
			if (!OpenParent(Parent, false, bMissing, strErrorOut) || bMissing) {
				return bMissing;
			}

			const LSTATUS nError = RegOpenKeyExW(Parent.m_hKey, kRecoveryName, REG_OPTION_OPEN_LINK, KEY_READ | KEY_WOW64_64KEY, &Record.m_hKey);
			if (nError == ERROR_FILE_NOT_FOUND) {
				return true;
			}

			if (nError != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Open telemetry recovery record", static_cast<DWORD>(nError));
				return false;
			}

			bExistsOut = true;

			if (!ValidateKeyPermissions(Record.m_hKey, strErrorOut)) {
				return false;
			}

			TelemetrySettings Pending = kDefaultTelemetrySettings;
			DWORD unVersion = 0;
			if (!ReadRegistryDWORD(Record.m_hKey, L"Version", unVersion) || !ReadRegistryDWORD(Record.m_hKey, L"OldCollection", Pending.m_unOldCollection) || !ReadRegistryDWORD(Record.m_hKey, L"OldKeywords", Pending.m_unOldKeywords) || !ReadRegistryDWORD(Record.m_hKey, L"OwnCollection", Pending.m_unOwnCollection) || !ReadRegistryDWORD(Record.m_hKey, L"OwnKeywords", Pending.m_unOwnKeywords) || !ValidateSettings(unVersion, Pending)) {
				strErrorOut = "The protected Windows 10 telemetry recovery record is incomplete or malformed; recovery was refused";
				return false;
			}

			SettingsOut = Pending;

			return true;
		}

		bool DeleteSettings(std::string& strErrorOut) {
			Key Parent;
			bool bMissing = false;
			if (!OpenParent(Parent, false, bMissing, strErrorOut) || bMissing) {
				return bMissing;
			}

			Key Record;
			LSTATUS nError = RegOpenKeyExW(Parent.m_hKey, kRecoveryName, REG_OPTION_OPEN_LINK, KEY_READ | KEY_WOW64_64KEY, &Record.m_hKey);
			if (nError == ERROR_FILE_NOT_FOUND) {
				return true;
			}

			if ((nError != ERROR_SUCCESS) || !ValidateKeyPermissions(Record.m_hKey, strErrorOut)) {
				if (strErrorOut.empty()) {
					strErrorOut = FormatWindowsFailure("Verify completed telemetry recovery record", static_cast<DWORD>(nError));
				}

				return false;
			}

			nError = Record.Close();
			if (nError != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Close completed telemetry recovery record", static_cast<DWORD>(nError));
				return false;
			}

			nError = RegDeleteKeyExW(Parent.m_hKey, kRecoveryName, KEY_WOW64_64KEY, 0);
			if ((nError != ERROR_SUCCESS) && (nError != ERROR_FILE_NOT_FOUND)) {
				strErrorOut = FormatWindowsFailure("Remove completed telemetry recovery record", static_cast<DWORD>(nError));
				return false;
			}

			nError = RegFlushKey(Parent.m_hKey);
			if (nError != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Persist completed telemetry recovery cleanup", static_cast<DWORD>(nError));
				return false;
			}

			return true;
		}

		// bCreatedOut records ownership immediately, even if later validation/persistence fails.
		bool WriteSettings(TelemetrySettings const& Settings, bool& bCreatedOut, std::string& strErrorOut) {
			bCreatedOut = false;
			Key Parent;
			Key Record;
			bool bMissing = false;
			if (!OpenParent(Parent, true, bMissing, strErrorOut)) {
				return false;
			}

			PSECURITY_DESCRIPTOR pRawDescriptor = nullptr;
			const BOOL bConverted = ConvertStringSecurityDescriptorToSecurityDescriptorW(kRecoveryDescriptorSDDL, SDDL_REVISION_1, &pRawDescriptor, nullptr);
			const DWORD unConversionError = bConverted ? ERROR_SUCCESS : GetLastError();
			std::unique_ptr<void, LocalMemoryDeleter> pDescriptor(pRawDescriptor);
			if (!bConverted) {
				strErrorOut = FormatWindowsFailure("Protect telemetry recovery record", unConversionError);
				return false;
			}

			SECURITY_ATTRIBUTES Attributes { static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), pDescriptor.get(), FALSE };
			DWORD unDisposition = 0;
			LSTATUS nError = RegCreateKeyExW(Parent.m_hKey, kRecoveryName, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &Attributes, &Record.m_hKey, &unDisposition);
			pDescriptor.reset();

			if (nError != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Create telemetry recovery record", static_cast<DWORD>(nError));
				return false;
			}

			if (unDisposition != REG_CREATED_NEW_KEY) {
				strErrorOut = "A telemetry recovery record already exists; complete recovery before activation";
				return false;
			}

			bCreatedOut = true;

			if (!ValidateKeyPermissions(Record.m_hKey, strErrorOut)) {
				return false;
			}

			auto WriteValue = [&Record, &nError](wchar_t const* pszName, DWORD unValue) -> void {
				if (nError == ERROR_SUCCESS) {
					nError = RegSetValueExW(Record.m_hKey, pszName, 0, REG_DWORD, reinterpret_cast<BYTE const*>(&unValue), sizeof(unValue));
				}
			};
			WriteValue(L"OldCollection", Settings.m_unOldCollection);
			WriteValue(L"OldKeywords", Settings.m_unOldKeywords);
			WriteValue(L"OwnCollection", Settings.m_unOwnCollection);
			WriteValue(L"OwnKeywords", Settings.m_unOwnKeywords);
			WriteValue(L"Version", kRecoveryVersion); // Commit and flush BEFORE the first global setting mutation.
			if (nError == ERROR_SUCCESS) {
				nError = RegFlushKey(Record.m_hKey);
			}

			if (nError != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Persist telemetry recovery record", static_cast<DWORD>(nError));
				return false;
			}

			return true;
		}

		template <class TGetOption, class TSetOption>
		bool RestoreSettings(TelemetrySettings const& Settings, TGetOption fnGetOption, TSetOption fnSetOption, std::string& strErrorOut) {
			UINT32 unCollection = 0;
			UINT32 unKeywords = 0;
			if (!fnGetOption(FWPM_ENGINE_COLLECT_NET_EVENTS, unCollection, strErrorOut) || !fnGetOption(FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS, unKeywords, strErrorOut)) {
				return false;
			}

			// A different external value ends our ownership. Preserve it and do not
			// disable collection an external application might now depend on.
			if (((unCollection != Settings.m_unOwnCollection) && (unCollection != Settings.m_unOldCollection)) || ((unKeywords != Settings.m_unOwnKeywords) && (unKeywords != Settings.m_unOldKeywords))) {
				return true;
			}

			if (unKeywords != Settings.m_unOldKeywords) {
				const DWORD unError = fnSetOption(FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS, Settings.m_unOldKeywords);
				if (unError != ERROR_SUCCESS) {
					strErrorOut = FormatWindowsFailure("Restore Windows connection telemetry keywords", unError);
					return false;
				}
			}

			if (unCollection != Settings.m_unOldCollection) {
				const DWORD unError = fnSetOption(FWPM_ENGINE_COLLECT_NET_EVENTS, Settings.m_unOldCollection);
				if (unError != ERROR_SUCCESS) {
					strErrorOut = FormatWindowsFailure("Restore Windows connection telemetry collection", unError);
					return false;
				}
			}

			return true;
		}

		bool RestoreRecord(HANDLE hEngine, TelemetrySettings const& Settings, std::string& strErrorOut) {
			TelemetrySettings Recorded = kDefaultTelemetrySettings;
			bool bExists = false;
			if (!ReadSettings(Recorded, bExists, strErrorOut)) {
				return false;
			}

			if (!bExists || (Recorded.m_unOldCollection != Settings.m_unOldCollection) || (Recorded.m_unOldKeywords != Settings.m_unOldKeywords) || (Recorded.m_unOwnCollection != Settings.m_unOwnCollection) || (Recorded.m_unOwnKeywords != Settings.m_unOwnKeywords)) {
				strErrorOut = "The owned Windows 10 telemetry recovery snapshot changed; it was left untouched";
				return false;
			}

			// These synchronous callables borrow hEngine only for RestoreSettings below.
			auto GetOption = [hEngine](FWPM_ENGINE_OPTION Option, UINT32& unValueOut, std::string& strMessageOut) -> bool {
				return ReadTelemetryOption(hEngine, Option, unValueOut, strMessageOut);
			};
			auto SetOption = [hEngine](FWPM_ENGINE_OPTION Option, UINT32 unValue) -> DWORD {
				return WriteTelemetryOption(hEngine, Option, unValue);
			};

			return RestoreSettings(Settings, GetOption, SetOption, strErrorOut) && DeleteSettings(strErrorOut);
		}

		bool RecoverUnderLease(HANDLE hEngine, std::string& strErrorOut) {
			TelemetrySettings Settings = kDefaultTelemetrySettings;
			bool bExists = false;
			if (!ReadSettings(Settings, bExists, strErrorOut)) {
				return false;
			}

			return !bExists || RestoreRecord(hEngine, Settings, strErrorOut);
		}

		// hHandleOut is initially empty caller-owned cleanup storage and may become nonnull on failure.
		// bAcquiredOut records mutex ownership separately; both existing callers release/close that state.
		bool AcquireLease(HANDLE& hHandleOut, bool& bAcquiredOut, std::string& strErrorOut) {
			constexpr wchar_t kLeaseDescriptorSDDL[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";

			bAcquiredOut = false;
			PSECURITY_DESCRIPTOR pRawDescriptor = nullptr;
			const BOOL bConverted = ConvertStringSecurityDescriptorToSecurityDescriptorW(kLeaseDescriptorSDDL, SDDL_REVISION_1, &pRawDescriptor, nullptr);
			const DWORD unConversionError = bConverted ? ERROR_SUCCESS : GetLastError();
			std::unique_ptr<void, LocalMemoryDeleter> pDescriptor(pRawDescriptor);
			if (!bConverted) {
				strErrorOut = FormatWindowsFailure("Protect the Windows 10 Steam route lease", unConversionError);
				return false;
			}

			SECURITY_ATTRIBUTES Attributes { static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), pDescriptor.get(), FALSE };
			hHandleOut = CreateMutexW(&Attributes, FALSE, kLeaseName);
			const DWORD unCreateError = hHandleOut ? ERROR_SUCCESS : GetLastError();
			pDescriptor.reset();

			if (!hHandleOut) {
				strErrorOut = FormatWindowsFailure("Open the Windows 10 Steam route lease", unCreateError);
				return false;
			}

			const DWORD unResult = WaitForSingleObject(hHandleOut, 0);
			const DWORD unWaitError = ((unResult != WAIT_OBJECT_0) && (unResult != WAIT_ABANDONED) && (unResult != WAIT_TIMEOUT)) ? GetLastError() : ERROR_SUCCESS;
			bAcquiredOut = (unResult == WAIT_OBJECT_0) || (unResult == WAIT_ABANDONED);
			if (!bAcquiredOut && (unResult != WAIT_TIMEOUT)) {
				strErrorOut = FormatWindowsFailure("Acquire the Windows 10 Steam route lease", unWaitError);
				return false;
			}

			return true;
		}

		ULONGLONG GetRetryDeadline(const ULONGLONG unNow) {
			constexpr ULONGLONG kRetryDelayMilliseconds = 1000;

			if (unNow > (std::numeric_limits<ULONGLONG>::max() - kRetryDelayMilliseconds)) {
				throw std::overflow_error("Windows 10 Steam endpoint retry deadline overflow");
			}

			return unNow + kRetryDelayMilliseconds;
		}

		void IncrementEventCounter(std::atomic<unsigned>& unCounter) {
			unsigned unPrevious = unCounter.load();
			for (;;) {
				if (unPrevious == std::numeric_limits<unsigned>::max()) {
					throw std::overflow_error("Windows 10 Steam telemetry counter overflow");
				}

				if (unCounter.compare_exchange_weak(unPrevious, unPrevious + 1)) {
					return;
				}
			}
		}

		ULONGLONG GetWorkerDeadline(const ULONGLONG unNow, const ULONGLONG unInterval) {
			if (unInterval > (std::numeric_limits<ULONGLONG>::max() - unNow)) {
				throw std::overflow_error("Windows 10 Steam worker deadline overflow");
			}

			return unNow + unInterval;
		}

		bool ReadTCPTableCount(std::vector<BYTE> const& vecStorage, const DWORD unReturnedBytes, const std::size_t unRowOffset, const std::size_t unRowBytes, DWORD& unCountOut) {
			if ((unReturnedBytes > vecStorage.size()) || (unRowOffset > unReturnedBytes) || (unRowOffset < sizeof(DWORD)) || (unRowBytes == 0)) {
				return false;
			}

			DWORD unCount = 0;
			std::memcpy(&unCount, vecStorage.data(), sizeof(unCount));
			if (unCount > ((unReturnedBytes - unRowOffset) / unRowBytes)) {
				return false;
			}

			unCountOut = unCount;

			return true;
		}
	} // namespace

	struct LegacyInternetRoutes::Implementation {
		Implementation() {
			m_hFilterEngine = nullptr;
			m_hSettingsEngine = nullptr;
			m_hLease = nullptr;
			m_LUID = {};
			m_unSourceAddress = 0;
			m_unNextHop = 0;
			m_unOverlayAddress = 0;
			m_unMetric = 0;
			m_unOldCollection = 0;
			m_unOldKeywords = 0;
			m_unOwnKeywords = 0;
			m_bRecordCreated = false;
			m_bRecordPrepared = false;
			m_bOwnsLease = false;
			m_bObserveOnly = false;
			m_unLastDirection = std::numeric_limits<UINT32>::max();
			m_unMaximumRoutes = kMaximumRoutes;
			m_Sublayer = {};
			m_pFilterMutex = nullptr;
			m_bStopping = false;
			m_bBusy = false;
			m_bObservedUDP = false;
		}

		void Fail(std::string strError) {
			std::lock_guard<std::mutex> FailureLock = std::lock_guard<std::mutex>(m_Mutex);
			if (m_strFailure.empty()) {
				m_strFailure = std::move(strError);
			}

			m_Changed.notify_all();
		}

		void Queue(const ULONG unAddress, const bool bUDP = false, const bool bDropped = false) {
			std::lock_guard<std::mutex> QueueLock = std::lock_guard<std::mutex>(m_Mutex);
			if (m_bStopping || !IsPublicDestination(unAddress)) {
				return;
			}

			m_bObservedUDP = m_bObservedUDP || bUDP;

			if (m_setKnown.contains(unAddress) && bDropped) {
				const ULONGLONG unNow = GetTickCount64();
				if (unNow < m_mapRetryAfter[unAddress]) {
					return;
				}

				m_mapRetryAfter[unAddress] = GetRetryDeadline(unNow);
			}

			if (!m_setKnown.contains(unAddress) || bDropped) {
				if (!m_setKnown.contains(unAddress) && (m_setKnown.size() >= m_unMaximumRoutes)) {
					m_strFailure = "Windows 10 Steam transport exceeded its bounded endpoint route limit";

				} else {
					m_setKnown.insert(unAddress);
					m_mapRetryAfter[unAddress] = GetRetryDeadline(GetTickCount64());
					m_setPending.insert(unAddress);
				}
			}

			m_Changed.notify_all();
		}

		static void CALLBACK HandleEvent(void* pContext, FWPM_NET_EVENT2 const* pEvent) noexcept {
			if (!pContext || !pEvent) {
				return;
			}

			Implementation& State = *static_cast<Implementation*>(pContext);
			try {
				ULONG unAddress = 0;
				bool bUDP = false;
				IncrementEventCounter(State.m_unCallbacks);

				if (State.m_bObserveOnly) {
					State.m_unLastFlags = pEvent->header.flags;

					if ((pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW) && pEvent->classifyAllow) {
						State.m_unLastDirection = pEvent->classifyAllow->msFwpDirection;
					}

					if ((pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP) && pEvent->classifyDrop) {
						State.m_unLastDirection = pEvent->classifyDrop->msFwpDirection;
					}

					FWP_BYTE_BLOB const& Identity = pEvent->header.appId;
					if (Identity.data && std::any_of(State.m_vecIdentities.begin(), State.m_vecIdentities.end(), [&Identity](std::vector<BYTE> const& vecExpected) -> bool {
						return (vecExpected.size() == Identity.size) && (vecExpected.empty() || (std::memcmp(vecExpected.data(), Identity.data, vecExpected.size()) == 0));
					})) {
						IncrementEventCounter(State.m_unIdentityEvents);
					}

					if ((pEvent->header.ipVersion == FWP_IP_VERSION_V4) && std::any_of(State.m_vecProbeSources.begin(), State.m_vecProbeSources.end(), [pEvent](const ULONG unLocalAddress) -> bool {
						return (pEvent->header.localAddrV4 == unLocalAddress) || (pEvent->header.localAddrV4 == ntohl(unLocalAddress));
					})) {
						IncrementEventCounter(State.m_unSourceEvents);
					}
				}

				bool bValid = ReadEventAddress(*pEvent, State.m_unSourceAddress, State.m_unOverlayAddress, State.m_vecIdentities, unAddress, bUDP);
				if (!bValid && State.m_bObserveOnly) {
					for (auto const& unLocalAddress : State.m_vecProbeSources) {
						if (ReadEventAddress(*pEvent, unLocalAddress, State.m_unOverlayAddress, State.m_vecIdentities, unAddress, bUDP)) {
							bValid = true;
							break;
						}
					}
				}

				if (bValid) {
					if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW) {
						IncrementEventCounter(State.m_unAllowedEvents);

					} else {
						IncrementEventCounter(State.m_unDroppedEvents);
					}

					State.Queue(unAddress, bUDP, pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP);
				}
			} catch (...) {
				// The OS callback cannot propagate failures or allocate a fallback record.
				State.m_bCallbackFailed = true;
				State.m_Changed.notify_all();
			}
		}

		bool AddRoute(const ULONG unDestination, std::string& strErrorOut) {
			constexpr ULONG kMinimumRemainingLifetimeSeconds = 5;

			MIB_IPFORWARD_ROW2 PreviouslyOwned {};
			bool bRenew = false;
			{
				std::lock_guard<std::mutex> RouteLock = std::lock_guard<std::mutex>(m_Mutex);
				auto itExisting = m_mapOwned.find(unDestination);
				if (itExisting != m_mapOwned.end()) {
					PreviouslyOwned = itExisting->second;
					bRenew = true;
				}
			}

			if (bRenew) {
				MIB_IPFORWARD_ROW2 Current = PreviouslyOwned;
				if ((GetIpForwardEntry2(&Current) == NO_ERROR) && MatchesOwnedRoute(Current, PreviouslyOwned) && (Current.ValidLifetime > kMinimumRemainingLifetimeSeconds)) {
					return true;
				}

				return RefreshOwnedRoute(PreviouslyOwned, &GetIpForwardEntry2, &CreateIpForwardEntry2, &SetIpForwardEntry2, strErrorOut);
			}

			MIB_IPFORWARD_ROW2 Route = BuildEndpointRoute(unDestination, m_LUID, m_unNextHop, m_unMetric);
			const DWORD unCode = CreateIpForwardEntry2(&Route);
			if ((unCode == ERROR_OBJECT_ALREADY_EXISTS) || (unCode == ERROR_ALREADY_EXISTS)) {
				// Existing routes are never adopted, refreshed, or removed, even
				// if another application happened to use identical parameters.
				MIB_IPFORWARD_ROW2 Existing = Route;
				const NETIO_STATUS unFound = GetIpForwardEntry2(&Existing);
				if ((unFound != NO_ERROR) || (Existing.InterfaceLuid.Value != m_LUID.Value) || (Existing.NextHop.Ipv4.sin_addr.S_un.S_addr != m_unNextHop)) {
					strErrorOut = "An existing endpoint route conflicts with the Windows 10 Steam transport";
					return false;
				}

				return true;
			}

			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Create a short-lived Steam endpoint route", unCode);
				return false;
			}

			try {
				std::lock_guard<std::mutex> RouteLock = std::lock_guard<std::mutex>(m_Mutex);
				m_mapOwned.emplace(unDestination, Route);
			} catch (...) {
				// Allocation failure must not orphan a route created by this call.
				MIB_IPFORWARD_ROW2 Current = Route;
				if ((GetIpForwardEntry2(&Current) == NO_ERROR) && MatchesOwnedRoute(Current, Route)) {
					// Best effort: the finite route lifetime bounds a failed cleanup.
					DeleteIpForwardEntry2(&Current);
				}

				throw;
			}

			return true;
		}

		bool PermitEndpoint(const ULONG unDestination, std::string& strErrorOut, const bool bCallerOwnsTransaction = false) {
			struct Transaction {
				Transaction(HANDLE const hEngine, const bool bOwned) :
					m_hEngine(hEngine),
					m_bOwned(bOwned)
				{
				}

				Transaction(Transaction const&) = delete;
				Transaction& operator=(Transaction const&) = delete;

				~Transaction() {
					if (m_bOwned) {
						// A failed abort is bounded by closure of the owning WFP session.
						FwpmTransactionAbort0(m_hEngine);
					}
				}

				HANDLE m_hEngine;
				bool m_bOwned;
			};

			constexpr std::size_t kConditionCount = 4;
			constexpr UINT8 kPermitWeight = 15;
			constexpr std::size_t kMappedMarkerOffset = 0x0A;
			constexpr std::size_t kMappedAddressOffset = 0x0C;

			static_assert(kConditionCount <= std::numeric_limits<UINT32>::max());
			static_assert((kMappedAddressOffset + sizeof(ULONG)) == sizeof(FWP_BYTE_ARRAY16));

			if (m_setPermitted.contains(unDestination)) {
				return true;
			}

			if ((!m_pFilterMutex && !bCallerOwnsTransaction) || IsEqualGUID(m_Sublayer, GUID_NULL)) {
				strErrorOut = "The Steam endpoint policy group is not ready";
				return false;
			}

			std::unique_lock<std::mutex> FilterLock;
			if (!bCallerOwnsTransaction) {
				FilterLock = std::unique_lock<std::mutex>(*m_pFilterMutex);
			}

			DWORD unCode = bCallerOwnsTransaction ? ERROR_SUCCESS : FwpmTransactionBegin0(m_hFilterEngine, 0);
			if (unCode != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Begin a Steam endpoint permit", unCode);
				return false;
			}

			Transaction FilterTransaction = Transaction(m_hFilterEngine, !bCallerOwnsTransaction);
			UINT64 unInterfaceLUID = m_LUID.Value;
			for (auto const& vecIdentity : m_vecIdentities) {
				if (vecIdentity.size() > std::numeric_limits<UINT32>::max()) {
					strErrorOut = "The Steam endpoint application identity exceeds the Windows API limit";
					return false;
				}

				// WFP reads the blob synchronously; its SDK carrier lacks const byte storage.
				FWP_BYTE_BLOB Application { static_cast<UINT32>(vecIdentity.size()), const_cast<BYTE*>(vecIdentity.data()) };
				std::array<FWPM_FILTER_CONDITION0, kConditionCount> arrConditions {};
				arrConditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
				arrConditions[0].matchType = FWP_MATCH_EQUAL;
				arrConditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
				arrConditions[0].conditionValue.byteBlob = &Application;
				arrConditions[1].fieldKey = FWPM_CONDITION_IP_LOCAL_INTERFACE;
				arrConditions[1].matchType = FWP_MATCH_EQUAL;
				arrConditions[1].conditionValue.type = FWP_UINT64;
				arrConditions[1].conditionValue.uint64 = &unInterfaceLUID;
				arrConditions[2].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
				arrConditions[2].matchType = FWP_MATCH_EQUAL;
				arrConditions[2].conditionValue.type = FWP_UINT32;
				arrConditions[2].conditionValue.uint32 = ntohl(m_unSourceAddress);
				arrConditions[3].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
				arrConditions[3].matchType = FWP_MATCH_EQUAL;
				arrConditions[3].conditionValue.type = FWP_UINT32;
				arrConditions[3].conditionValue.uint32 = ntohl(unDestination);
				FWPM_FILTER0 Filter {};
				Filter.subLayerKey = m_Sublayer;
				Filter.weight.type = FWP_UINT8;
				Filter.weight.uint8 = kPermitWeight;
				Filter.action.type = FWP_ACTION_PERMIT;
				// FwpmFilterAdd0 copies display text and never writes through this SDK field.
				Filter.displayData.name = const_cast<wchar_t*>(L"WireSteam Windows 10: allow only this learned Steam endpoint on its original physical uplink");
				Filter.numFilterConditions = static_cast<UINT32>(arrConditions.size());
				Filter.filterCondition = arrConditions.data();
				for (auto const& Layer : { FWPM_LAYER_ALE_AUTH_CONNECT_V4, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4 }) {
					Filter.layerKey = Layer;
					unCode = FwpmFilterAdd0(m_hFilterEngine, &Filter, nullptr, nullptr);
					if (unCode != ERROR_SUCCESS) {
						break;
					}
				}

				if (unCode != ERROR_SUCCESS) {
					break;
				}

				FWP_BYTE_ARRAY16 Local {};
				FWP_BYTE_ARRAY16 Remote {};
				Local.byteArray16[kMappedMarkerOffset] = 0xFF;
				Local.byteArray16[kMappedMarkerOffset + 0x01] = 0xFF;
				Remote.byteArray16[kMappedMarkerOffset] = 0xFF;
				Remote.byteArray16[kMappedMarkerOffset + 0x01] = 0xFF;
				std::memcpy(Local.byteArray16 + kMappedAddressOffset, &m_unSourceAddress, sizeof(m_unSourceAddress));
				std::memcpy(Remote.byteArray16 + kMappedAddressOffset, &unDestination, sizeof(unDestination));
				arrConditions[2].conditionValue.type = FWP_BYTE_ARRAY16_TYPE;
				arrConditions[3].conditionValue.type = FWP_BYTE_ARRAY16_TYPE;
				arrConditions[2].conditionValue.byteArray16 = &Local;
				arrConditions[3].conditionValue.byteArray16 = &Remote;
				for (auto const& Layer : { FWPM_LAYER_ALE_AUTH_CONNECT_V6, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6 }) {
					Filter.layerKey = Layer;
					unCode = FwpmFilterAdd0(m_hFilterEngine, &Filter, nullptr, nullptr);
					if (unCode != ERROR_SUCCESS) {
						break;
					}
				}

				if (unCode != ERROR_SUCCESS) {
					break;
				}
			}

			if ((unCode == ERROR_SUCCESS) && !bCallerOwnsTransaction) {
				unCode = FwpmTransactionCommit0(m_hFilterEngine);
				if (unCode == ERROR_SUCCESS) {
					FilterTransaction.m_bOwned = false;
				}
			}

			if (unCode != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Install an exact Steam endpoint permit", unCode);
				return false;
			}

			// The WFP session owns every committed filter, even if this insertion throws.
			m_setPermitted.insert(unDestination);

			return true;
		}

		bool RefreshRoutes(std::string& strErrorOut) {
			MIB_IF_ROW2 Physical {};
			Physical.InterfaceLuid = m_LUID;
			if ((GetIfEntry2(&Physical) != NO_ERROR) || (Physical.OperStatus != IfOperStatusUp) || !Physical.InterfaceAndOperStatusFlags.HardwareInterface) {
				strErrorOut = "The original physical Steam uplink is no longer available; reconnect the tunnel";
				return false;
			}

			MIB_UNICASTIPADDRESS_ROW Address {};
			InitializeUnicastIpAddressEntry(&Address);
			Address.InterfaceLuid = m_LUID;
			Address.Address.si_family = AF_INET;
			Address.Address.Ipv4.sin_addr.S_un.S_addr = m_unSourceAddress;
			if ((GetUnicastIpAddressEntry(&Address) != NO_ERROR) || (Address.DadState != IpDadStatePreferred)) {
				strErrorOut = "The physical Steam uplink address changed; reconnect the tunnel";
				return false;
			}

			std::vector<MIB_IPFORWARD_ROW2> vecRoutes;
			{
				std::lock_guard<std::mutex> RouteLock = std::lock_guard<std::mutex>(m_Mutex);
				for (auto const& Entry : m_mapOwned) {
					vecRoutes.push_back(Entry.second);
				}
			}

			for (auto const& Route : vecRoutes) {
				if (!RefreshOwnedRoute(Route, &GetIpForwardEntry2, &CreateIpForwardEntry2, &SetIpForwardEntry2, strErrorOut)) {
					return false;
				}
			}

			return true;
		}

		void SeedTCP() {
			constexpr DWORD kMaximumPathCharacters = 32768;
			constexpr DWORD kMaximumTableBytes = 16 * 1024 * 1024;
			constexpr std::size_t kIPv4RowOffset = offsetof(MIB_TCPTABLE_OWNER_PID, table);
			constexpr std::size_t kIPv6RowOffset = offsetof(MIB_TCP6TABLE_OWNER_PID, table);

			static_assert(sizeof(PROCESSENTRY32W) <= std::numeric_limits<DWORD>::max());
			static_assert(kIPv4RowOffset >= sizeof(DWORD));
			static_assert(kIPv6RowOffset >= sizeof(DWORD));

			std::set<DWORD> setProcesses;
			const HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (hSnapshot == INVALID_HANDLE_VALUE) {
				return;
			}

			std::unique_ptr<void, HandleDeleter> pSnapshot(hSnapshot);
			PROCESSENTRY32W Process {};
			Process.dwSize = sizeof(Process);
			// Discovery is best effort; telemetry remains the authoritative learn path.
			if (Process32FirstW(pSnapshot.get(), &Process)) {
				do {
					if ((_wcsicmp(Process.szExeFile, L"steam.exe") != 0) && (_wcsicmp(Process.szExeFile, L"WireSteam.exe") != 0)) {
						continue;
					}

					std::unique_ptr<void, HandleDeleter> pProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Process.th32ProcessID));
					if (!pProcess) {
						continue;
					}

					std::wstring wstrPath = std::wstring(kMaximumPathCharacters, L'\0');
					DWORD unLength = static_cast<DWORD>(wstrPath.size());
					if (QueryFullProcessImageNameW(pProcess.get(), 0, wstrPath.data(), &unLength) && (unLength <= wstrPath.size())) {
						wstrPath.resize(unLength);
						if (std::any_of(m_vecApplications.begin(), m_vecApplications.end(), [&wstrPath](std::filesystem::path const& Expected) -> bool {
							return _wcsicmp(Expected.c_str(), wstrPath.c_str()) == 0;
						})) {
							setProcesses.insert(Process.th32ProcessID);
						}
					}
				} while (Process32NextW(pSnapshot.get(), &Process));
			}

			pSnapshot.reset();

			DWORD unBytes = 0;
			if ((GetExtendedTcpTable(nullptr, &unBytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER) || (unBytes > kMaximumTableBytes)) {
				return;
			}

			std::vector<BYTE> vecStorage(unBytes);
			if (GetExtendedTcpTable(vecStorage.data(), &unBytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
				return;
			}

			DWORD unCount = 0;
			if (!ReadTCPTableCount(vecStorage, unBytes, kIPv4RowOffset, sizeof(MIB_TCPROW_OWNER_PID), unCount)) {
				return;
			}

			for (std::size_t unIndex = 0; unIndex < unCount; ++unIndex) {
				MIB_TCPROW_OWNER_PID Row {};
				std::memcpy(&Row, vecStorage.data() + kIPv4RowOffset + (unIndex * sizeof(Row)), sizeof(Row));
				if (setProcesses.contains(Row.dwOwningPid) && (Row.dwLocalAddr == m_unSourceAddress) && (Row.dwState != MIB_TCP_STATE_LISTEN) && (Row.dwState != MIB_TCP_STATE_CLOSED)) {
					Queue(Row.dwRemoteAddr);
				}
			}

			unBytes = 0;
			if ((GetExtendedTcpTable(nullptr, &unBytes, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER) || (unBytes > kMaximumTableBytes)) {
				return;
			}

			vecStorage.resize(unBytes);
			if (GetExtendedTcpTable(vecStorage.data(), &unBytes, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
				return;
			}

			if (!ReadTCPTableCount(vecStorage, unBytes, kIPv6RowOffset, sizeof(MIB_TCP6ROW_OWNER_PID), unCount)) {
				return;
			}

			for (std::size_t unIndex = 0; unIndex < unCount; ++unIndex) {
				MIB_TCP6ROW_OWNER_PID Row {};
				std::memcpy(&Row, vecStorage.data() + kIPv6RowOffset + (unIndex * sizeof(Row)), sizeof(Row));
				if (!setProcesses.contains(Row.dwOwningPid) || (Row.dwState == MIB_TCP_STATE_LISTEN) || (Row.dwState == MIB_TCP_STATE_CLOSED)) {
					continue;
				}

				FWP_BYTE_ARRAY16 Local {};
				FWP_BYTE_ARRAY16 Remote {};
				ULONG unLocalIPv4 = 0;
				ULONG unRemoteIPv4 = 0;
				static_assert(sizeof(Row.ucLocalAddr) == sizeof(Local.byteArray16));
				static_assert(sizeof(Row.ucRemoteAddr) == sizeof(Remote.byteArray16));
				std::memcpy(Local.byteArray16, Row.ucLocalAddr, sizeof(Row.ucLocalAddr));
				std::memcpy(Remote.byteArray16, Row.ucRemoteAddr, sizeof(Row.ucRemoteAddr));
				if (ReadMappedIPv4Address(Local, unLocalIPv4) && (unLocalIPv4 == m_unSourceAddress) && ReadMappedIPv4Address(Remote, unRemoteIPv4)) {
					Queue(unRemoteIPv4);
				}
			}
		}

		void Run() noexcept {
			constexpr ULONGLONG kRefreshIntervalMilliseconds = 10000;
			constexpr ULONGLONG kSeedIntervalMilliseconds = 2000;
			constexpr std::chrono::milliseconds kPollInterval(100);

			try {
				ULONGLONG unRefreshAt = GetWorkerDeadline(GetTickCount64(), kRefreshIntervalMilliseconds);
				ULONGLONG unSeedAt = GetWorkerDeadline(GetTickCount64(), kSeedIntervalMilliseconds);
				for (;;) {
					std::set<ULONG> setWork;
					{
						std::unique_lock<std::mutex> WorkLock = std::unique_lock<std::mutex>(m_Mutex);
						m_Changed.wait_for(WorkLock, kPollInterval, [this]() -> bool {
							return m_bStopping || !m_setPending.empty() || m_bCallbackFailed;
						});
						if (m_bStopping) {
							return;
						}

						if (m_bCallbackFailed && m_strFailure.empty()) {
							m_strFailure = "Windows connection telemetry could not queue a Steam endpoint";
						}

						setWork.swap(m_setPending);
						m_bBusy = true;
					}

					std::string strError;
					for (auto const& unAddress : setWork) {
						if (!AddRoute(unAddress, strError) || !PermitEndpoint(unAddress, strError)) {
							break;
						}
					}

					if (strError.empty() && (GetTickCount64() >= unRefreshAt)) {
						RefreshRoutes(strError);
						unRefreshAt = GetWorkerDeadline(GetTickCount64(), kRefreshIntervalMilliseconds);
					}

					if (GetTickCount64() >= unSeedAt) {
						SeedTCP();
						unSeedAt = GetWorkerDeadline(GetTickCount64(), kSeedIntervalMilliseconds);
					}

					{
						std::lock_guard<std::mutex> WorkLock = std::lock_guard<std::mutex>(m_Mutex);
						if (!strError.empty() && m_strFailure.empty()) {
							m_strFailure = std::move(strError);
						}

						m_bBusy = false;
						m_Changed.notify_all();
					}
				}
			} catch (...) {
				try {
					Fail("The Windows 10 endpoint route worker failed");
				} catch (...) {
					// Health readers already consume this allocation-free failure signal.
					m_bCallbackFailed = true;
					m_Changed.notify_all();
				}
			}
		}

		HANDLE m_hFilterEngine;
		HANDLE m_hSettingsEngine;
		HANDLE m_hLease;
		std::vector<HANDLE> m_vecSubscriptions;
		// Published completely before subscribing; unchanged until all callbacks quiesce.
		std::vector<std::vector<BYTE>> m_vecIdentities;
		std::vector<std::filesystem::path> m_vecApplications;
		NET_LUID m_LUID;
		ULONG m_unSourceAddress;
		ULONG m_unNextHop;
		ULONG m_unOverlayAddress;
		ULONG m_unMetric;
		UINT32 m_unOldCollection;
		UINT32 m_unOldKeywords;
		UINT32 m_unOwnKeywords;
		bool m_bRecordCreated;
		bool m_bRecordPrepared;
		bool m_bOwnsLease;
		bool m_bObserveOnly;
		std::vector<ULONG> m_vecProbeSources;
		std::atomic<unsigned> m_unAllowedEvents;
		std::atomic<unsigned> m_unDroppedEvents;
		std::atomic<unsigned> m_unCallbacks;
		std::atomic<unsigned> m_unIdentityEvents;
		std::atomic<unsigned> m_unSourceEvents;
		std::atomic<UINT32> m_unLastFlags;
		std::atomic<UINT32> m_unLastDirection;
		// Never hold this state lock together with the borrowed WFP filter mutex.
		mutable std::mutex m_Mutex;
		std::condition_variable m_Changed;
		std::set<ULONG> m_setPending;
		std::set<ULONG> m_setKnown;
		std::set<ULONG> m_setPermitted;
		std::size_t m_unMaximumRoutes;
		std::map<ULONG, ULONGLONG> m_mapRetryAfter;
		std::map<ULONG, MIB_IPFORWARD_ROW2> m_mapOwned;
		GUID m_Sublayer;
		std::mutex* m_pFilterMutex;
		bool m_bStopping;
		bool m_bBusy;
		bool m_bObservedUDP;
		std::atomic<bool> m_bCallbackFailed;
		std::string m_strFailure;
		std::thread m_Worker;
	};

	LegacyInternetRoutes::LegacyInternetRoutes() {
		m_pImplementation = std::make_unique<Implementation>();
	}

	LegacyInternetRoutes::~LegacyInternetRoutes() {
		Stop();
	}

	bool LegacyInternetRoutes::IsSupported() {
		// GetModuleHandleW borrows the module already loaded by the WFP imports.
		const HMODULE hModule = GetModuleHandleW(L"fwpuclnt.dll");

		return hModule && GetProcAddress(hModule, "FwpmNetEventSubscribe1");
	}

	bool LegacyInternetRoutes::HasRecovery() {
		Key RecoveryKey;
		const LSTATUS nCode = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRecoveryPath, REG_OPTION_OPEN_LINK, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &RecoveryKey.m_hKey);

		return (nCode != ERROR_FILE_NOT_FOUND) && (nCode != ERROR_PATH_NOT_FOUND);
	}

	bool LegacyInternetRoutes::RecoverSettings(std::string& strErrorOut) {
		struct RecoveryOwner {
			RecoveryOwner() {
				m_hLease = nullptr;
				m_hEngine = nullptr;
				m_bAcquired = false;
			}

			RecoveryOwner(RecoveryOwner const&) = delete;
			RecoveryOwner& operator=(RecoveryOwner const&) = delete;

			~RecoveryOwner() {
				// Best-effort terminal cleanup cannot change the completed restore result.
				if (m_hEngine) {
					FwpmEngineClose0(m_hEngine);
				}

				if (m_hLease) {
					if (m_bAcquired) {
						ReleaseMutex(m_hLease);
					}

					CloseHandle(m_hLease);
				}
			}

			HANDLE m_hLease;
			HANDLE m_hEngine;
			bool m_bAcquired;
		};

		strErrorOut.clear();

		if (!HasRecovery()) {
			return true;
		}

		RecoveryOwner Owner;
		if (!AcquireLease(Owner.m_hLease, Owner.m_bAcquired, strErrorOut)) {
			return false;
		}

		// A different live Internet-client worker owns both its active routes and
		// its telemetry snapshot. An unrelated `down` must not disturb it.
		if (!Owner.m_bAcquired) {
			return true;
		}

		const DWORD unCode = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, nullptr, &Owner.m_hEngine);
		if (unCode != ERROR_SUCCESS) {
			strErrorOut = FormatWindowsFailure("Open orphaned telemetry recovery", unCode);
			return false;
		}

		return RecoverUnderLease(Owner.m_hEngine, strErrorOut);
	}

	bool LegacyInternetRoutes::ProbeEndpointPermits(void* pFilterEngine, void const* pSublayerKey, std::filesystem::path const& Application, const std::uint64_t unUplinkLUID, const std::uint32_t unSourceAddress, std::string& strErrorOut) {
		constexpr ULONG kDiagnosticDestination = 0x01010101;

		if (!pFilterEngine || !pSublayerKey || !unUplinkLUID || !unSourceAddress || !Application.is_absolute()) {
			strErrorOut = "Endpoint registration diagnostics require an owned transaction, app, and uplink";
			return false;
		}

		LegacyInternetRoutes Probe;
		Probe.m_pImplementation->m_hFilterEngine = pFilterEngine;
		Probe.m_pImplementation->m_unSourceAddress = unSourceAddress;
		Probe.m_pImplementation->m_LUID.Value = unUplinkLUID;
		std::memcpy(&Probe.m_pImplementation->m_Sublayer, pSublayerKey, sizeof(Probe.m_pImplementation->m_Sublayer));
		FWP_BYTE_BLOB* pApplicationID = nullptr;
		const DWORD unCode = FwpmGetAppIdFromFileName0(Application.c_str(), &pApplicationID);
		std::unique_ptr<FWP_BYTE_BLOB, FWPBlobDeleter> pOwnedApplicationID(pApplicationID);
		if ((unCode != ERROR_SUCCESS) || !pOwnedApplicationID || !pOwnedApplicationID->data || !pOwnedApplicationID->size) {
			pOwnedApplicationID.reset();
			strErrorOut = FormatWindowsFailure("Identify the diagnostic endpoint app", unCode);
			return false;
		}

		Probe.m_pImplementation->m_vecIdentities.emplace_back(pOwnedApplicationID->data, pOwnedApplicationID->data + pOwnedApplicationID->size);
		pOwnedApplicationID.reset();

		return Probe.m_pImplementation->PermitEndpoint(htonl(kDiagnosticDestination), strErrorOut, true);
	}

	bool LegacyInternetRoutes::ProbeTelemetry(std::vector<std::filesystem::path> const& vecApplications, const unsigned unSeconds, std::string& strReportOut, std::string& strErrorOut) {
		struct Session {
			Session() {
				m_hEngine = nullptr;
			}

			Session(Session const&) = delete;
			Session& operator=(Session const&) = delete;

			~Session() {
				if (m_hEngine) {
					// Terminal cleanup is best effort; this dynamic session owns all diagnostic filters.
					FwpmTransactionAbort0(m_hEngine);
					FwpmEngineClose0(m_hEngine);
				}
			}

			HANDLE m_hEngine;
		};

		struct Transaction {
			explicit Transaction(HANDLE const hEngine) :
				m_hEngine(hEngine)
			{
				m_bActive = true;
			}

			Transaction(Transaction const&) = delete;
			Transaction& operator=(Transaction const&) = delete;

			~Transaction() {
				if (m_bActive) {
					// Session closure is the final cleanup if this best-effort abort fails.
					FwpmTransactionAbort0(m_hEngine);
				}
			}

			HANDLE m_hEngine;
			bool m_bActive;
		};

		constexpr unsigned kMinimumObservationSeconds = 1;
		constexpr unsigned kMaximumObservationSeconds = 10;
		constexpr std::size_t kMaximumApplications = 16;
		constexpr ULONG kRouteProbeAddress = 0x08080808;
		constexpr ULONG kDiagnosticOverlayAddress = 0x0AFFFFFE;
		constexpr UINT16 kSublayerWeight = 65520;
		constexpr UINT8 kObserveWeight = 1;
		constexpr UINT8 kDropWeight = 15;
		constexpr std::size_t kConditionCount = 2;
		constexpr DWORD kMaximumPathCharacters = 32768;

		static_assert(kConditionCount <= std::numeric_limits<UINT32>::max());

		strReportOut.clear();
		strErrorOut.clear();

		if ((unSeconds < kMinimumObservationSeconds) || (unSeconds > kMaximumObservationSeconds) || vecApplications.empty() || (vecApplications.size() > kMaximumApplications)) {
			strErrorOut = "Connection-event diagnostics require 1..16 executable paths and a 1..10 second observation window";
			return false;
		}

		for (auto const& Application : vecApplications) {
			const DWORD unAttributes = GetFileAttributesW(Application.c_str());
			if (!Application.is_absolute() || (_wcsicmp(Application.extension().c_str(), L".exe") != 0) || (unAttributes == INVALID_FILE_ATTRIBUTES) ||
				(unAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
				strErrorOut = "Connection-event diagnostics require existing absolute executable paths";
				return false;
			}
		}

		SOCKADDR_INET Destination {};
		SOCKADDR_INET Selected {};
		Destination.si_family = AF_INET;
		Destination.Ipv4.sin_addr.S_un.S_addr = htonl(kRouteProbeAddress);
		MIB_IPFORWARD_ROW2 Route {};
		NETIO_STATUS unCode = GetBestRoute2(nullptr, 0, nullptr, &Destination, 0, &Route, &Selected);
		if ((unCode != NO_ERROR) || (Selected.si_family != AF_INET) || !Selected.Ipv4.sin_addr.S_un.S_addr) {
			strErrorOut = FormatWindowsFailure("Find a current source for connection-event diagnostics", unCode);
			return false;
		}

		Session DiagnosticSession;

		FWPM_SESSION0 Description {};
		Description.flags = FWPM_SESSION_FLAG_DYNAMIC;
		Description.displayData.name = const_cast<wchar_t*>(L"WireSteam connection-event diagnostic: logging permits only");
		unCode = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &Description, &DiagnosticSession.m_hEngine);
		if (unCode != ERROR_SUCCESS) {
			strErrorOut = FormatWindowsFailure("Open connection-event diagnostics", unCode);
			return false;
		}

		LegacyInternetRoutes Probe;
		Probe.m_pImplementation->m_bObserveOnly = true;
		MIB_UNICASTIPADDRESS_TABLE* pAddresses = nullptr;
		const NETIO_STATUS unAddressCode = GetUnicastIpAddressTable(AF_INET, &pAddresses);
		std::unique_ptr<MIB_UNICASTIPADDRESS_TABLE, MIBTableDeleter> pOwnedAddresses(pAddresses);
		if ((unAddressCode == NO_ERROR) && pOwnedAddresses) {
			// The successful SDK allocation owns its complete NumEntries/Table extent.
			for (std::size_t unIndex = 0; unIndex < pOwnedAddresses->NumEntries; ++unIndex) {
				Probe.m_pImplementation->m_vecProbeSources.push_back(pOwnedAddresses->Table[unIndex].Address.Ipv4.sin_addr.S_un.S_addr);
			}
		}

		pOwnedAddresses.reset();

		if (!Probe.Start(DiagnosticSession.m_hEngine, vecApplications, Route.InterfaceLuid.Value, Selected.Ipv4.sin_addr.S_un.S_addr, Route.NextHop.Ipv4.sin_addr.S_un.S_addr, htonl(kDiagnosticOverlayAddress), strErrorOut)) {
			return false;
		}

		// Scope logging permits to the requested apps. Committing a policy change
		// reauthorizes existing flows, while the ordinary firewall can still deny
		// them in another sublayer. No block or route action is installed here.
		unCode = FwpmTransactionBegin0(DiagnosticSession.m_hEngine, 0);
		if (unCode != ERROR_SUCCESS) {
			strErrorOut = FormatWindowsFailure("Begin connection-event logging permits", unCode);
			return false;
		}

		Transaction FilterTransaction(DiagnosticSession.m_hEngine);

		GUID Group {};
		if (FAILED(CoCreateGuid(&Group))) {
			strErrorOut = "Cannot identify diagnostic logging permits";
			return false;
		}

		FWPM_SUBLAYER0 Sublayer {};
		Sublayer.subLayerKey = Group;
		Sublayer.weight = kSublayerWeight;
		Sublayer.displayData.name = const_cast<wchar_t*>(L"WireSteam diagnostic logging permits");
		unCode = FwpmSubLayerAdd0(DiagnosticSession.m_hEngine, &Sublayer, nullptr);
		for (auto const& vecIdentity : Probe.m_pImplementation->m_vecIdentities) {
			if (unCode != ERROR_SUCCESS) {
				break;
			}

			if (vecIdentity.size() > std::numeric_limits<UINT32>::max()) {
				strErrorOut = "The diagnostic application identity exceeds the Windows API limit";
				return false;
			}

			// WFP copies this borrowed identity; its SDK carrier lacks const storage.
			FWP_BYTE_BLOB ApplicationID { static_cast<UINT32>(vecIdentity.size()), const_cast<BYTE*>(vecIdentity.data()) };
			std::array<FWPM_FILTER_CONDITION0, kConditionCount> arrConditions {};
			arrConditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
			arrConditions[0].matchType = FWP_MATCH_EQUAL;
			arrConditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
			arrConditions[0].conditionValue.byteBlob = &ApplicationID;
			arrConditions[1].fieldKey = FWPM_CONDITION_FLAGS;
			arrConditions[1].matchType = FWP_MATCH_FLAGS_NONE_SET;
			arrConditions[1].conditionValue.type = FWP_UINT32;
			arrConditions[1].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;
			FWPM_FILTER0 Filter {};
			Filter.subLayerKey = Group;
			Filter.action.type = FWP_ACTION_PERMIT;
			Filter.displayData.name = const_cast<wchar_t*>(L"WireSteam diagnostic: observe exact-app nonloopback outbound connections");
			Filter.weight.type = FWP_UINT8;
			Filter.weight.uint8 = kObserveWeight;
			Filter.numFilterConditions = static_cast<UINT32>(arrConditions.size());
			Filter.filterCondition = arrConditions.data();
			for (auto const& Layer : { FWPM_LAYER_ALE_AUTH_CONNECT_V4, FWPM_LAYER_ALE_AUTH_CONNECT_V6 }) {
				Filter.layerKey = Layer;
				unCode = FwpmFilterAdd0(DiagnosticSession.m_hEngine, &Filter, nullptr, nullptr);
				if (unCode != ERROR_SUCCESS) {
					break;
				}
			}
		}

		if (unCode == ERROR_SUCCESS) {
			// Force a known drop owned only by this isolated diagnostic process,
			// including loopback DNS redirection by an existing VPN. No Steam or
			// application process is blocked by this diagnostic filter.
			std::wstring wstrExecutable = std::wstring(kMaximumPathCharacters, L'\0');
			const DWORD unLength = GetModuleFileNameW(nullptr, wstrExecutable.data(), static_cast<DWORD>(wstrExecutable.size()));
			FWP_BYTE_BLOB* pOwnID = nullptr;
			if (!unLength || (unLength >= wstrExecutable.size())) {
				unCode = ERROR_INVALID_NAME;
			} else {
				wstrExecutable.resize(unLength);
				unCode = FwpmGetAppIdFromFileName0(wstrExecutable.c_str(), &pOwnID);
			}

			std::unique_ptr<FWP_BYTE_BLOB, FWPBlobDeleter> pOwnedID(pOwnID);
			if ((unCode == ERROR_SUCCESS) && pOwnedID) {
				std::array<FWPM_FILTER_CONDITION0, kConditionCount> arrConditions {};
				arrConditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
				arrConditions[0].matchType = FWP_MATCH_EQUAL;
				arrConditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
				arrConditions[0].conditionValue.byteBlob = pOwnedID.get();
				arrConditions[1].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
				arrConditions[1].matchType = FWP_MATCH_EQUAL;
				arrConditions[1].conditionValue.type = FWP_UINT8;
				arrConditions[1].conditionValue.uint8 = IPPROTO_UDP;
				FWPM_FILTER0 Filter {};
				Filter.subLayerKey = Group;
				Filter.action.type = FWP_ACTION_BLOCK;
				Filter.displayData.name = const_cast<wchar_t*>(L"WireSteam diagnostic: known UDP drop for only this diagnostic executable");
				Filter.weight.type = FWP_UINT8;
				Filter.weight.uint8 = kDropWeight;
				Filter.numFilterConditions = static_cast<UINT32>(arrConditions.size());
				Filter.filterCondition = arrConditions.data();
				for (auto const& Layer : { FWPM_LAYER_ALE_AUTH_CONNECT_V4, FWPM_LAYER_ALE_AUTH_CONNECT_V6 }) {
					Filter.layerKey = Layer;
					unCode = FwpmFilterAdd0(DiagnosticSession.m_hEngine, &Filter, nullptr, nullptr);
					if (unCode != ERROR_SUCCESS) {
						break;
					}
				}
			}

			pOwnedID.reset();
		}

		if (unCode != ERROR_SUCCESS) {
			strErrorOut = FormatWindowsFailure("Stage exact-app diagnostic logging permits", unCode);
			return false;
		}

		unCode = FwpmTransactionCommit0(DiagnosticSession.m_hEngine);
		if (unCode != ERROR_SUCCESS) {
			strErrorOut = FormatWindowsFailure("Commit diagnostic logging permits", unCode);
			return false;
		}

		FilterTransaction.m_bActive = false;

		std::size_t unEndpoints = 0;
		bool bUDP = false;
		UINT32 unActualCollection = 0;
		UINT32 unActualKeywords = 0;
		if (!ReadTelemetryOption(Probe.m_pImplementation->m_hSettingsEngine, FWPM_ENGINE_COLLECT_NET_EVENTS, unActualCollection, strErrorOut) ||
			!ReadTelemetryOption(Probe.m_pImplementation->m_hSettingsEngine, FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS, unActualKeywords, strErrorOut)) {
			return false;
		}

		{
			std::unique_lock<std::mutex> ObservationLock = std::unique_lock<std::mutex>(Probe.m_pImplementation->m_Mutex);
			Probe.m_pImplementation->m_Changed.wait_for(ObservationLock, std::chrono::seconds(unSeconds), [&Probe]() -> bool {
				return Probe.m_pImplementation->m_bCallbackFailed.load();
			});
			unEndpoints = Probe.m_pImplementation->m_setPending.size();
			bUDP = Probe.m_pImplementation->m_bObservedUDP;
		}

		strReportOut = "Windows 10 connection-event diagnostic: callbacks=" + std::to_string(Probe.m_pImplementation->m_unCallbacks.load()) + " collection=" + std::to_string(unActualCollection) + " keywords=" + std::to_string(unActualKeywords) + " identity matches=" + std::to_string(Probe.m_pImplementation->m_unIdentityEvents.load()) + " local-source matches=" + std::to_string(Probe.m_pImplementation->m_unSourceEvents.load()) + " last flags=" + std::to_string(Probe.m_pImplementation->m_unLastFlags.load()) + " last direction=" + std::to_string(Probe.m_pImplementation->m_unLastDirection.load()) + " allowed=" + std::to_string(Probe.m_pImplementation->m_unAllowedEvents.load()) + " dropped=" + std::to_string(Probe.m_pImplementation->m_unDroppedEvents.load()) + " public IPv4 endpoints=" + std::to_string(unEndpoints) + " UDP observed=" + (bUDP ? "yes" : "no") + "; endpoint routes were not created";

		const bool bHealthy = Probe.Healthy(strErrorOut);
		Probe.Stop();

		if (HasRecovery()) {
			strErrorOut = "Connection-event diagnostics left protected telemetry recovery pending; run down to restore it";
			return false;
		}

		if (!bHealthy) {
			return false;
		}

		if (!bUDP) {
			strErrorOut = "No exact-application outbound IPv4 UDP event was observed during connection-event diagnostics";
			return false;
		}

		return true;
	}

	bool LegacyInternetRoutes::Start(void* pFilterEngine, std::vector<std::filesystem::path> const& vecApplications, const std::uint64_t unUplinkLUID, const std::uint32_t unSourceAddress, const std::uint32_t unNextHop, const std::uint32_t unOverlayAddress, std::string& strErrorOut) {
		constexpr std::size_t kMaximumApplications = 16;
		constexpr std::size_t kFiltersPerApplication = 4;
		constexpr ULONG kMinimumMetric = 10000;
		constexpr ULONG kMetricRange = 50001;
		constexpr UINT32 kCollectionEnabled = 1;
		constexpr DWORD kEndTimeLow = 0xFFFFFFFF;
		constexpr DWORD kEndTimeHigh = 0x7FFFFFFF;

		static_assert(kMinimumMetric <= (std::numeric_limits<ULONG>::max() - (kMetricRange - 1)));

		strErrorOut.clear();

		if (m_pImplementation->m_hFilterEngine || !pFilterEngine || vecApplications.empty() || (vecApplications.size() > kMaximumApplications) || !unUplinkLUID || !unSourceAddress || !unOverlayAddress) {
			strErrorOut = "Invalid Windows 10 Steam route setup";
			return false;
		}

		try {
			m_pImplementation->m_hFilterEngine = pFilterEngine;
			m_pImplementation->m_vecApplications = vecApplications;
			m_pImplementation->m_LUID.Value = unUplinkLUID;
			m_pImplementation->m_unSourceAddress = unSourceAddress;
			m_pImplementation->m_unNextHop = unNextHop;
			m_pImplementation->m_unOverlayAddress = unOverlayAddress;
			GUID Identity {};
			if (FAILED(CoCreateGuid(&Identity))) {
				strErrorOut = "Cannot create the Steam endpoint route ownership identity";
				Stop();
				return false;
			}

			m_pImplementation->m_unMetric = kMinimumMetric + (Identity.Data1 % kMetricRange);

			if (!AcquireLease(m_pImplementation->m_hLease, m_pImplementation->m_bOwnsLease, strErrorOut)) {
				Stop();
				return false;
			}

			if (!m_pImplementation->m_bOwnsLease) {
				strErrorOut = "Another Windows 10 Internet-client session is already running";
				Stop();
				return false;
			}

			DWORD unCode = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, nullptr, &m_pImplementation->m_hSettingsEngine);
			if (unCode != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Open Windows connection telemetry settings", unCode);
				Stop();
				return false;
			}

			if (!RecoverUnderLease(m_pImplementation->m_hSettingsEngine, strErrorOut)) {
				Stop();
				return false;
			}

			if (!ReadTelemetryOption(m_pImplementation->m_hSettingsEngine, FWPM_ENGINE_COLLECT_NET_EVENTS, m_pImplementation->m_unOldCollection, strErrorOut) || !ReadTelemetryOption(m_pImplementation->m_hSettingsEngine, FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS, m_pImplementation->m_unOldKeywords, strErrorOut)) {
				Stop();
				return false;
			}

			// Production learns from its own explicit DROP policy and therefore works
			// with disabled Windows Firewall profiles. ALLOW collection is diagnostic.
			m_pImplementation->m_unOwnKeywords = m_pImplementation->m_unOldKeywords | (m_pImplementation->m_bObserveOnly ? FWPM_NET_EVENT_KEYWORD_CLASSIFY_ALLOW : 0);
			TelemetrySettings const Settings { m_pImplementation->m_unOldCollection, m_pImplementation->m_unOldKeywords, kCollectionEnabled, m_pImplementation->m_unOwnKeywords };
			if (!WriteSettings(Settings, m_pImplementation->m_bRecordCreated, strErrorOut)) {
				Stop();
				return false;
			}

			m_pImplementation->m_bRecordPrepared = true;

			if (m_pImplementation->m_unOldCollection != kCollectionEnabled) {
				unCode = WriteTelemetryOption(m_pImplementation->m_hSettingsEngine, FWPM_ENGINE_COLLECT_NET_EVENTS, kCollectionEnabled);
				if (unCode != ERROR_SUCCESS) {
					strErrorOut = FormatWindowsFailure("Enable Windows connection telemetry", unCode);
					Stop();
					return false;
				}
			}

			if (m_pImplementation->m_unOwnKeywords != m_pImplementation->m_unOldKeywords) {
				unCode = WriteTelemetryOption(m_pImplementation->m_hSettingsEngine, FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS, m_pImplementation->m_unOwnKeywords);
				if (unCode != ERROR_SUCCESS) {
					strErrorOut = FormatWindowsFailure("Observe allowed Steam connection destinations", unCode);
					Stop();
					return false;
				}
			}

			for (auto const& Application : vecApplications) {
				FWP_BYTE_BLOB* pApplicationID = nullptr;
				unCode = FwpmGetAppIdFromFileName0(Application.c_str(), &pApplicationID);
				std::unique_ptr<FWP_BYTE_BLOB, FWPBlobDeleter> pOwnedID(pApplicationID);
				if ((unCode != ERROR_SUCCESS) || !pOwnedID || !pOwnedID->data || !pOwnedID->size) {
					pOwnedID.reset();
					strErrorOut = FormatWindowsFailure("Identify a Windows 10 Steam executable", unCode);
					Stop();
					return false;
				}

				m_pImplementation->m_vecIdentities.emplace_back(pOwnedID->data, pOwnedID->data + pOwnedID->size);
				pOwnedID.reset();
			}

			const std::size_t unIdentityCount = m_pImplementation->m_vecIdentities.size();
			if ((unIdentityCount == 0) || (unIdentityCount > (std::numeric_limits<std::size_t>::max() / kFiltersPerApplication))) {
				strErrorOut = "Windows 10 Steam endpoint application count exceeds its bounded filter capacity";
				Stop();
				return false;
			}

			m_pImplementation->m_unMaximumRoutes = std::min(kMaximumRoutes, kMaximumEndpointFilters / (unIdentityCount * kFiltersPerApplication));

			if (unIdentityCount > (m_pImplementation->m_vecSubscriptions.max_size() - m_pImplementation->m_vecSubscriptions.size())) {
				strErrorOut = "Windows 10 Steam subscription count exceeds its bounded capacity";
				Stop();
				return false;
			}

			// Reserve before publishing callbacks: appending a HANDLE then cannot allocate.
			m_pImplementation->m_vecSubscriptions.reserve(m_pImplementation->m_vecSubscriptions.size() + unIdentityCount);
			// Every callback sees the complete immutable identity list, including a
			// notification racing the creation of a later per-application subscription.
			for (auto const& vecIdentityBytes : m_pImplementation->m_vecIdentities) {
				if (vecIdentityBytes.size() > std::numeric_limits<UINT32>::max()) {
					strErrorOut = "Windows 10 Steam application identity exceeds the Windows API limit";
					Stop();
					return false;
				}

				// WFP reads this blob synchronously; its SDK carrier lacks const storage.
				FWP_BYTE_BLOB ApplicationID { static_cast<UINT32>(vecIdentityBytes.size()), const_cast<BYTE*>(vecIdentityBytes.data()) };
				FWPM_FILTER_CONDITION0 Condition {};
				Condition.fieldKey = FWPM_CONDITION_ALE_APP_ID;
				Condition.matchType = FWP_MATCH_EQUAL;
				Condition.conditionValue.type = FWP_BYTE_BLOB_TYPE;
				Condition.conditionValue.byteBlob = &ApplicationID;
				FWPM_NET_EVENT_ENUM_TEMPLATE0 Selection {};
				Selection.numFilterConditions = 1;
				Selection.filterCondition = &Condition;
				// An all-zero end time describes a range ending in 1601, not a
				// perpetual subscription. Match current and future connection events.
				Selection.endTime.dwLowDateTime = kEndTimeLow;
				Selection.endTime.dwHighDateTime = kEndTimeHigh;
				FWPM_NET_EVENT_SUBSCRIPTION0 Subscription {};
				Subscription.enumTemplate = &Selection;
				HANDLE hSubscription = nullptr;
				unCode = FwpmNetEventSubscribe1(m_pImplementation->m_hSettingsEngine, &Subscription, &Implementation::HandleEvent, m_pImplementation.get(), &hSubscription);
				if (unCode != ERROR_SUCCESS) {
					strErrorOut = FormatWindowsFailure("Subscribe to Windows 10 Steam connection destinations", unCode);
					Stop();
					return false;
				}

				m_pImplementation->m_vecSubscriptions.push_back(hSubscription);
			}

			if (!m_pImplementation->m_bObserveOnly) {
				m_pImplementation->SeedTCP();
			}
		} catch (...) {
			Stop();
			throw;
		}

		return true;
	}

	bool LegacyInternetRoutes::ActivateBootstrap(void const* pSublayerKey, std::mutex& FilterMutex, std::string& strErrorOut) {
		if (!m_pImplementation->m_hFilterEngine || !pSublayerKey || m_pImplementation->m_Worker.joinable() || m_pImplementation->m_bObserveOnly) {
			strErrorOut = "Invalid Steam endpoint policy activation";
			return false;
		}

		std::memcpy(&m_pImplementation->m_Sublayer, pSublayerKey, sizeof(m_pImplementation->m_Sublayer));
		if (IsEqualGUID(m_pImplementation->m_Sublayer, GUID_NULL)) {
			strErrorOut = "The Steam endpoint policy group is missing";
			return false;
		}

		m_pImplementation->m_pFilterMutex = &FilterMutex;
		m_pImplementation->m_Worker = std::thread([pState = m_pImplementation.get()]() -> void {
			pState->Run();
		});

		return true;
	}

	bool LegacyInternetRoutes::PrepareAdapter(std::string& strErrorOut) {
		constexpr std::chrono::seconds kActivationTimeout(5);

		std::unique_lock<std::mutex> ReadinessLock = std::unique_lock<std::mutex>(m_pImplementation->m_Mutex);
		const bool bReady = m_pImplementation->m_Changed.wait_for(ReadinessLock, kActivationTimeout, [this]() -> bool {
			return !m_pImplementation->m_strFailure.empty() || m_pImplementation->m_bCallbackFailed || (m_pImplementation->m_setPending.empty() && !m_pImplementation->m_bBusy && m_pImplementation->m_bObservedUDP);
		});

		if (!m_pImplementation->m_strFailure.empty()) {
			strErrorOut = m_pImplementation->m_strFailure;
			return false;
		}

		if (m_pImplementation->m_bCallbackFailed) {
			strErrorOut = "Windows connection telemetry could not queue a Steam relay endpoint";
			return false;
		}

		if (!bReady) {
			strErrorOut = "No usable Steam relay UDP connection was observed before Internet activation. Windows connection telemetry is required for Windows 10 routing.";
			return false;
		}

		strErrorOut.clear();

		return true;
	}

	bool LegacyInternetRoutes::Healthy(std::string& strErrorOut) const {
		std::lock_guard<std::mutex> HealthLock = std::lock_guard<std::mutex>(m_pImplementation->m_Mutex);
		strErrorOut = m_pImplementation->m_strFailure;
		if (m_pImplementation->m_bCallbackFailed && strErrorOut.empty()) {
			strErrorOut = "Windows connection telemetry stopped queuing Steam endpoint routes";
		}

		return strErrorOut.empty();
	}

	void LegacyInternetRoutes::Stop() noexcept {
		if (!m_pImplementation) {
			return;
		}

		// Successful unsubscribe waits for any in-flight callback. Hold no callback lock.
		// https://learn.microsoft.com/windows/win32/api/fwpmu/nf-fwpmu-fwpmneteventunsubscribe0
		for (auto const& hSubscription : m_pImplementation->m_vecSubscriptions) {
			if (!hSubscription) {
				continue;
			}

			if (!m_pImplementation->m_hSettingsEngine) {
				std::fputs("WireSteam: cannot quiesce a telemetry callback without its session.\n", stderr);
				std::terminate();
			}

			const DWORD unCode = FwpmNetEventUnsubscribe0(m_pImplementation->m_hSettingsEngine, hSubscription);
			if (unCode != ERROR_SUCCESS) {
				// A failed unsubscribe does not prove context lifetime ended. Returning
				// could destroy this callback's borrowed storage; session close is not
				// documented to provide the same in-flight completion boundary.
				std::fputs("WireSteam: telemetry unsubscribe failed; refusing to free live callback state.\n", stderr);
				std::terminate();
			}
		}

		m_pImplementation->m_vecSubscriptions.clear();

		try {
			{
				std::lock_guard<std::mutex> StopLock = std::lock_guard<std::mutex>(m_pImplementation->m_Mutex);
				m_pImplementation->m_bStopping = true;
				m_pImplementation->m_Changed.notify_all();
			}

			if (m_pImplementation->m_Worker.joinable()) {
				m_pImplementation->m_Worker.join();
			}
		} catch (...) {
			// The worker borrows this state; neither destruction nor detach is safe.
			std::fputs("WireSteam: cannot quiesce the endpoint route worker.\n", stderr);
			std::terminate();
		}

		for (auto const& Entry : m_pImplementation->m_mapOwned) {
			MIB_IPFORWARD_ROW2 Current = Entry.second;
			if ((GetIpForwardEntry2(&Current) == NO_ERROR) && MatchesOwnedRoute(Current, Entry.second)) {
				// Best effort: the existing finite lifetime bounds a failed route delete.
				DeleteIpForwardEntry2(&Current);
			}
		}

		m_pImplementation->m_mapOwned.clear();

		if (m_pImplementation->m_hSettingsEngine) {
			try {
				std::string strRestoreError;
				if (m_pImplementation->m_bRecordPrepared) {
					TelemetrySettings const Settings { m_pImplementation->m_unOldCollection, m_pImplementation->m_unOldKeywords, 1, m_pImplementation->m_unOwnKeywords };
					if (RestoreRecord(m_pImplementation->m_hSettingsEngine, Settings, strRestoreError)) {
						m_pImplementation->m_bRecordCreated = false;
						m_pImplementation->m_bRecordPrepared = false;
					}

				} else if (m_pImplementation->m_bRecordCreated && DeleteSettings(strRestoreError)) {
					m_pImplementation->m_bRecordCreated = false;
				}
			} catch (...) {
				// Keep the protected recovery journal/flags for the next explicit recovery.
				std::fputs("WireSteam: telemetry restoration failed; protected recovery remains pending.\n", stderr);
			}

			// The callbacks are already quiescent; terminal handle cleanup is best effort.
			FwpmEngineClose0(m_pImplementation->m_hSettingsEngine);
			m_pImplementation->m_hSettingsEngine = nullptr;
		}

		if (m_pImplementation->m_hLease) {
			if (m_pImplementation->m_bOwnsLease) {
				// The caller-confined owner releases its lease before terminal closure.
				ReleaseMutex(m_pImplementation->m_hLease);
			}

			CloseHandle(m_pImplementation->m_hLease);
			m_pImplementation->m_hLease = nullptr;
		}

		m_pImplementation->m_bOwnsLease = false;
		m_pImplementation->m_hFilterEngine = nullptr;
	}

	void RunLegacyInternetRoutesTests() {
		constexpr std::array<ULONG, 9> kRejectedAddresses { 0x00000000, 0x0A000001, 0x7F000001, 0xA9FE0101, 0xAC100001, 0xC0A80101, 0x64400001, 0xE0000001, 0xFFFFFFFF };
		constexpr ULONG kRelayHostAddress = 0xA2FEC629;
		constexpr ULONG kSourceHostAddress = 0xC0A80164;
		constexpr ULONG kOverlayHostAddress = 0x0A4D0002;
		constexpr std::size_t kMappedMarkerOffset = 0x0A;
		constexpr std::size_t kMappedAddressOffset = 0x0C;
		constexpr std::size_t kOwnershipChanges = 5;
		constexpr ULONGLONG kMaximumDeadline = std::numeric_limits<ULONGLONG>::max();
		constexpr ULONGLONG kRetryDelayMilliseconds = 1000;
		constexpr ULONGLONG kWorkerIntervalMilliseconds = 100;
		constexpr std::size_t kTableRowOffset = sizeof(DWORD);
		constexpr std::size_t kTableRowBytes = sizeof(MIB_TCPROW_OWNER_PID);
		constexpr std::array<std::pair<wchar_t const*, bool>, 5> kDescriptors { { { L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)", true },
			{ L"O:SYG:SYD:P(A;;KA;;;BA)(A;;KA;;;SY)", true },
			{ L"O:BUG:BUD:P(A;;KA;;;SY)(A;;KA;;;BA)", false },
			{ L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)(A;;KR;;;BU)", false },
			{ L"O:BAG:BAD:P(A;;KR;;;SY)(A;;KA;;;BA)", false } } };

		static_assert((kTableRowOffset + kTableRowBytes + 1) <= std::numeric_limits<DWORD>::max());

		auto const Require = [](const bool bOkay, char const* pszMessage) -> void {
			if (!bOkay) {
				throw std::runtime_error(pszMessage);
			}
		};

		Require(GetRetryDeadline(kMaximumDeadline - kRetryDelayMilliseconds) == kMaximumDeadline, "A representable endpoint retry deadline was rejected");
		bool bRetryOverflow = false;
		try {
			GetRetryDeadline(kMaximumDeadline - kRetryDelayMilliseconds + 1);
		} catch (std::overflow_error const&) {
			bRetryOverflow = true;
		}

		Require(bRetryOverflow, "An endpoint retry deadline wrapped its monotonic counter");

		Require(GetWorkerDeadline(kMaximumDeadline - kWorkerIntervalMilliseconds, kWorkerIntervalMilliseconds) == kMaximumDeadline, "A representable worker deadline was rejected");
		bool bWorkerOverflow = false;
		try {
			GetWorkerDeadline(kMaximumDeadline, 1);
		} catch (std::overflow_error const&) {
			bWorkerOverflow = true;
		}

		Require(bWorkerOverflow, "A worker deadline wrapped its monotonic counter");

		std::atomic<unsigned> unEvents;
		IncrementEventCounter(unEvents);
		Require(unEvents.load() == 1, "A telemetry event counter did not increment its defined default state");

		unEvents = std::numeric_limits<unsigned>::max();
		bool bCounterOverflow = false;
		try {
			IncrementEventCounter(unEvents);
		} catch (std::overflow_error const&) {
			bCounterOverflow = true;
		}

		Require(bCounterOverflow && (unEvents.load() == std::numeric_limits<unsigned>::max()), "A telemetry counter overflow changed its saturated value");

		std::vector<BYTE> vecTable(kTableRowOffset + kTableRowBytes);
		DWORD unStoredCount = 1;
		std::memcpy(vecTable.data(), &unStoredCount, sizeof(unStoredCount));
		DWORD unReadCount = 0;
		Require(ReadTCPTableCount(vecTable, static_cast<DWORD>(vecTable.size()), kTableRowOffset, kTableRowBytes, unReadCount) && (unReadCount == 1), "A bounded TCP table rejected its complete row");
		Require(!ReadTCPTableCount(vecTable, static_cast<DWORD>(vecTable.size() + 1), kTableRowOffset, kTableRowBytes, unReadCount), "A TCP table accepted more returned bytes than its storage");
		Require(!ReadTCPTableCount(vecTable, static_cast<DWORD>(kTableRowOffset - 1), kTableRowOffset, kTableRowBytes, unReadCount), "A TCP table read a truncated row-count header");
		Require(!ReadTCPTableCount(vecTable, static_cast<DWORD>(vecTable.size()), kTableRowOffset - 1, kTableRowBytes, unReadCount), "A TCP table overlapped its count and first row");
		Require(!ReadTCPTableCount(vecTable, static_cast<DWORD>(vecTable.size()), kTableRowOffset, 0, unReadCount), "A TCP table accepted a zero-sized row");
		unStoredCount = 2;
		std::memcpy(vecTable.data(), &unStoredCount, sizeof(unStoredCount));
		Require(!ReadTCPTableCount(vecTable, static_cast<DWORD>(vecTable.size()), kTableRowOffset, kTableRowBytes, unReadCount) && (unReadCount == 1), "A truncated TCP table committed an unbounded row count");

		unStoredCount = 0;
		std::memcpy(vecTable.data(), &unStoredCount, sizeof(unStoredCount));
		Require(ReadTCPTableCount(vecTable, static_cast<DWORD>(kTableRowOffset), kTableRowOffset, kTableRowBytes, unReadCount) && (unReadCount == 0), "A complete empty TCP table was rejected");

		for (auto const& unAddress : kRejectedAddresses) {
			Require(!IsPublicDestination(htonl(unAddress)), "A Steam endpoint route accepted a private, multicast, or loopback destination");
		}

		Require(IsPublicDestination(htonl(kRelayHostAddress)), "A Steam endpoint route rejected a public relay");
		std::vector<std::vector<BYTE>> const vecIdentities { { 0x01, 0x02, 0x03, 0x04 } };
		FWPM_NET_EVENT2 Event {};
		Event.header.flags = FWPM_NET_EVENT_FLAG_IP_VERSION_SET | FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET |
			FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET | FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET | FWPM_NET_EVENT_FLAG_APP_ID_SET;
		Event.header.ipVersion = FWP_IP_VERSION_V4;
		Event.header.ipProtocol = IPPROTO_UDP;
		Event.header.localAddrV4 = kSourceHostAddress;
		Event.header.remoteAddrV4 = kRelayHostAddress;
		Event.header.appId = { static_cast<UINT32>(vecIdentities[0].size()), const_cast<BYTE*>(vecIdentities[0].data()) };
		FWPM_NET_EVENT_CLASSIFY_ALLOW0 Allow {};
		Allow.msFwpDirection = FWP_DIRECTION_OUTBOUND;
		Event.type = FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW;
		Event.classifyAllow = &Allow;
		const ULONG unSource = htonl(kSourceHostAddress);
		const ULONG unOverlay = htonl(kOverlayHostAddress);
		ULONG unAddressOut = 0;
		bool bUDP = false;
		Require(ReadEventAddress(Event, unSource, unOverlay, vecIdentities, unAddressOut, bUDP) && (unAddressOut == htonl(kRelayHostAddress)) && bUDP,
			"Windows 10 Steam routing failed to identify a permitted UDP relay with correct byte order");
		Allow.msFwpDirection = FWP_DIRECTION_INBOUND;
		Require(!ReadEventAddress(Event, unSource, unOverlay, vecIdentities, unAddressOut, bUDP), "An inbound event created an outbound endpoint route");
		Allow.msFwpDirection = FWP_DIRECTION_OUTBOUND;
		Allow.isLoopback = TRUE;
		Require(!ReadEventAddress(Event, unSource, unOverlay, vecIdentities, unAddressOut, bUDP), "A loopback event created a physical endpoint route");
		Allow.isLoopback = FALSE;
		Require(!ReadEventAddress(Event, unSource, unOverlay, { { 0x05, 0x06 } }, unAddressOut, bUDP), "A foreign executable inherited a Steam endpoint route");
		FWPM_NET_EVENT_CLASSIFY_DROP2 Drop {};
		Drop.msFwpDirection = kOutboundEventDirection;
		Event.type = FWPM_NET_EVENT_TYPE_CLASSIFY_DROP;
		Event.classifyDrop = &Drop;
		Event.header.localAddrV4 = ntohl(unOverlay);
		Require(ReadEventAddress(Event, unSource, unOverlay, vecIdentities, unAddressOut, bUDP), "A newly blocked Steam relay was not learned through the guard");
		Event.header.ipVersion = FWP_IP_VERSION_V6;
		Event.header.localAddrV6 = {};
		Event.header.remoteAddrV6 = {};
		Event.header.localAddrV6.byteArray16[kMappedMarkerOffset] = Event.header.localAddrV6.byteArray16[kMappedMarkerOffset + 0x01] = 0xFF;
		Event.header.remoteAddrV6.byteArray16[kMappedMarkerOffset] = Event.header.remoteAddrV6.byteArray16[kMappedMarkerOffset + 0x01] = 0xFF;
		const ULONG unRelay = htonl(kRelayHostAddress);
		std::memcpy(Event.header.localAddrV6.byteArray16 + kMappedAddressOffset, &unSource, sizeof(unSource));
		std::memcpy(Event.header.remoteAddrV6.byteArray16 + kMappedAddressOffset, &unRelay, sizeof(unRelay));
		Require(ReadEventAddress(Event, unSource, unOverlay, vecIdentities, unAddressOut, bUDP) && (unAddressOut == unRelay),
			"A dual-stack IPv4-mapped Steam relay did not receive a Windows 10 endpoint route");
		Event.header.remoteAddrV6.byteArray16[0x00] = 0x20;
		Require(!ReadEventAddress(Event, unSource, unOverlay, vecIdentities, unAddressOut, bUDP), "A real IPv6 address escaped the IPv4-only routing guard");
		NET_LUID LUID {};
		LUID.Value = 123;
		MIB_IPFORWARD_ROW2 const Route = BuildEndpointRoute(unRelay, LUID, unSource, 123456);
		Require((Route.ValidLifetime == kRouteLifetimeSeconds) && (Route.PreferredLifetime == kRouteLifetimeSeconds) && !Route.Immortal && (Route.DestinationPrefix.PrefixLength == kIPv4PrefixLength) &&
			MatchesOwnedRoute(Route, Route),
			"A Windows 10 endpoint route is persistent or has an incorrect owner identity");
		for (std::size_t unChange = 0; unChange < kOwnershipChanges; ++unChange) {
			MIB_IPFORWARD_ROW2 Foreign = Route;
			if (unChange == 0) {
				++Foreign.Metric;
			}

			if (unChange == 1) {
				++Foreign.InterfaceLuid.Value;
			}

			if (unChange == 2) {
				Foreign.Immortal = TRUE;
			}

			if (unChange == 3) {
				Foreign.ValidLifetime = std::numeric_limits<ULONG>::max();
			}

			if (unChange == 4) {
				++Foreign.NextHop.Ipv4.sin_addr.S_un.S_addr;
			}

			Require(!MatchesOwnedRoute(Foreign, Route), "Windows 10 endpoint cleanup adopted a foreign route");
		}

		// Exercise the real refresh state machine through fake IP Helper calls:
		// an idle session must renew all routes; an expired route may be recreated,
		// while a changed or pre-existing route can never be adopted for renewal.
		MIB_IPFORWARD_ROW2 KernelRow = Route;
		KernelRow.ValidLifetime = KernelRow.PreferredLifetime = 20;
		unsigned unCreates = 0;
		unsigned unSets = 0;
		std::string strRefreshError;
		auto GetRoute = [&KernelRow](MIB_IPFORWARD_ROW2* pValueOut) -> DWORD {
			*pValueOut = KernelRow;

			return NO_ERROR;
		};
		auto CreateRoute = [&unCreates, &KernelRow](MIB_IPFORWARD_ROW2 const* pValue) -> DWORD {
			++unCreates;
			KernelRow = *pValue;

			return NO_ERROR;
		};
		auto SetRoute = [&unSets, &KernelRow](MIB_IPFORWARD_ROW2 const* pValue) -> DWORD {
			++unSets;
			KernelRow = *pValue;

			return NO_ERROR;
		};
		Require(RefreshOwnedRoute(Route, GetRoute, CreateRoute, SetRoute, strRefreshError) && (unSets == 1) && (unCreates == 0) &&
			(KernelRow.ValidLifetime == kRouteLifetimeSeconds) && (KernelRow.PreferredLifetime == kRouteLifetimeSeconds),
			"An idle Windows 10 session did not renew its exact owned endpoint route");
		auto MissingRoute = [](MIB_IPFORWARD_ROW2*) -> DWORD {
			return ERROR_NOT_FOUND;
		};
		Require(RefreshOwnedRoute(Route, MissingRoute, CreateRoute, SetRoute, strRefreshError) && (unCreates == 1) && (unSets == 1),
			"An expired owned endpoint route was not recreated with a fresh finite lifetime");
		++KernelRow.Metric;
		Require(!RefreshOwnedRoute(Route, GetRoute, CreateRoute, SetRoute, strRefreshError) && (unCreates == 1) && (unSets == 1),
			"The endpoint refresh worker modified a route that lost its ownership identity");
		auto RacedRoute = [&unCreates](MIB_IPFORWARD_ROW2 const*) -> DWORD {
			++unCreates;

			return ERROR_OBJECT_ALREADY_EXISTS;
		};
		Require(!RefreshOwnedRoute(Route, MissingRoute, RacedRoute, SetRoute, strRefreshError) && (unSets == 1),
			"The endpoint refresh worker adopted an existing route after a creation race");
		TelemetrySettings const Saved { 0, 0x02, 1, 0x02 | FWPM_NET_EVENT_KEYWORD_CLASSIFY_ALLOW };
		Require(ValidateSettings(1, Saved) && !ValidateSettings(2, Saved), "Telemetry recovery accepted an unknown schema version");
		TelemetrySettings Malformed = Saved;
		Malformed.m_unOldCollection = 2;
		Require(!ValidateSettings(1, Malformed), "Telemetry recovery accepted a non-Boolean collection state");
		Malformed = Saved;
		++Malformed.m_unOwnKeywords;
		Require(!ValidateSettings(1, Malformed), "Telemetry recovery accepted unrelated global keyword mutations");

		for (auto const& Case : kDescriptors) {
			PSECURITY_DESCRIPTOR pDescriptor = nullptr;
			const BOOL bConverted = ConvertStringSecurityDescriptorToSecurityDescriptorW(Case.first, SDDL_REVISION_1, &pDescriptor, nullptr);
			std::unique_ptr<void, LocalMemoryDeleter> pOwnedDescriptor(pDescriptor);
			Require(bConverted != FALSE, "The telemetry recovery ACL fixture is invalid");
			const bool bValid = ValidateDescriptorPermissions(pOwnedDescriptor.get());
			pOwnedDescriptor.reset();
			Require(bValid == Case.second, "Telemetry recovery trusted an unprotected owner or permissions");
		}

		UINT32 unCollection = Saved.m_unOwnCollection;
		UINT32 unKeywords = Saved.m_unOwnKeywords;
		std::vector<FWPM_ENGINE_OPTION> vecMutations;
		auto const GetSetting = [&unCollection, &unKeywords](const FWPM_ENGINE_OPTION Option, UINT32& unValueOut, std::string&) -> bool {
			unValueOut = (Option == FWPM_ENGINE_COLLECT_NET_EVENTS) ? unCollection : unKeywords;

			return true;
		};
		auto const SetSetting = [&vecMutations, &unCollection, &unKeywords](const FWPM_ENGINE_OPTION Option, const UINT32 unValue) -> DWORD {
			vecMutations.push_back(Option);
			((Option == FWPM_ENGINE_COLLECT_NET_EVENTS) ? unCollection : unKeywords) = unValue;

			return ERROR_SUCCESS;
		};
		std::string strSettingsError;
		Require(RestoreSettings(Saved, GetSetting, SetSetting, strSettingsError) && (unCollection == Saved.m_unOldCollection) && (unKeywords == Saved.m_unOldKeywords) &&
			(vecMutations == std::vector<FWPM_ENGINE_OPTION> { FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS, FWPM_ENGINE_COLLECT_NET_EVENTS }),
			"Telemetry crash recovery did not restore the exact saved settings in a safe order");
		vecMutations.clear();
		unCollection = Saved.m_unOwnCollection;
		unKeywords = 0x3F;
		Require(RestoreSettings(Saved, GetSetting, SetSetting, strSettingsError) && vecMutations.empty() && (unCollection == Saved.m_unOwnCollection) && (unKeywords == 0x3F),
			"Telemetry recovery overwrote settings changed by another application");
		unCollection = Saved.m_unOwnCollection;
		unKeywords = Saved.m_unOldKeywords;
		Require(RestoreSettings(Saved, GetSetting, SetSetting, strSettingsError) && (vecMutations.size() == 1) && (unCollection == Saved.m_unOldCollection),
			"Interrupted telemetry recovery could not complete its remaining setting");
		vecMutations.clear();
		unCollection = Saved.m_unOwnCollection;
		unKeywords = Saved.m_unOwnKeywords;
		auto const RefuseSetting = [&vecMutations](const FWPM_ENGINE_OPTION Option, UINT32) -> DWORD {
			vecMutations.push_back(Option);

			return ERROR_ACCESS_DENIED;
		};
		Require(!RestoreSettings(Saved, GetSetting, RefuseSetting, strSettingsError) && (vecMutations.size() == 1) && (unCollection == Saved.m_unOwnCollection),
			"Failed telemetry recovery disabled collection after its keyword restoration was refused");
		LegacyInternetRoutes Unused;
		std::string strError;
		Require(Unused.Healthy(strError) && strError.empty(), "An unused Windows 10 route worker reports a false failure");
		Require(!Unused.Start(nullptr, {}, 0, 0, 0, 0, strError) && !strError.empty(), "The Windows 10 route worker accepted missing setup");
		std::string strReport;
		Require(!LegacyInternetRoutes::ProbeTelemetry({}, 0, strReport, strError) && !strError.empty() && strReport.empty(),
			"Connection-event diagnostics accepted an unbounded or unscoped invocation");
		Unused.Stop();
		Unused.Stop();
	}
} // namespace ws

#ifdef WIRESTEAM_LEGACY_ROUTES_OWNS_NTDDI_VERSION
#undef NTDDI_VERSION
#undef WIRESTEAM_LEGACY_ROUTES_OWNS_NTDDI_VERSION
#endif
