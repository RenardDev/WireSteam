#include "internet_server.hpp"

// General
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#define WIRESTEAM_SERVER_OWNS_LEAN_WINDOWS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#define WIRESTEAM_SERVER_OWNS_NOMINMAX
#endif
#include <winsock2.h>
// ws2tcpip enables the IP route records exposed by netioapi.
#include <ws2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <objbase.h>
#include <sddl.h>

// C++
#include <charconv>
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
#include <filesystem>
#include <ios>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <vector>

// Project
#include "platform.hpp"

namespace ws {
	namespace {
		enum class NATAction {
			Probe = 0,
			Create,
			Remove
		};

		constexpr wchar_t kRegistryParent[] = L"SOFTWARE\\WireSteam\\InternetGateways";
		constexpr DWORD kRecoveryVersion = 1;

		static_assert(std::is_standard_layout_v<SECURITY_DESCRIPTOR_RELATIVE> && std::is_trivially_copyable_v<SECURITY_DESCRIPTOR_RELATIVE>);
		static_assert(std::is_standard_layout_v<ACL> && std::is_trivially_copyable_v<ACL>);
		static_assert(std::is_standard_layout_v<ACE_HEADER> && std::is_trivially_copyable_v<ACE_HEADER>);
		static_assert(sizeof(SECURITY_DESCRIPTOR_RELATIVE) == 0x14);
		static_assert(sizeof(ACL) == 0x08);
		static_assert(sizeof(ACE_HEADER) == 0x04);
		static_assert(sizeof(DWORD) == 0x04);
		static_assert(offsetof(ACCESS_ALLOWED_ACE, SidStart) == 0x08);
		static_assert(offsetof(SID, SubAuthority) == 0x08);

		struct ForwardingState {
			ForwardingState() noexcept {
				m_LUID = {};
				m_GUID = {};
				m_bChanged = false;
			}

			NET_LUID m_LUID;
			GUID m_GUID;
			bool m_bChanged;
		};

		struct RegistryKey {
			RegistryKey() noexcept {
				m_hValue = nullptr;
			}

			explicit RegistryKey(HKEY const hKey) noexcept :
				m_hValue(hKey)
			{
			}

			~RegistryKey() {
				if (m_hValue) {
					// Terminal release of this registry reference is best effort.
					RegCloseKey(m_hValue);
				}
			}

			RegistryKey(RegistryKey const&) = delete;
			RegistryKey& operator=(RegistryKey const&) = delete;

			HKEY m_hValue;
		};

		struct LocalMemoryDeleter {
			void operator()(void* pMemory) const noexcept {
				if (pMemory) {
					// Each owner contains an exact LocalAlloc result.
					LocalFree(pMemory);
				}
			}
		};

		struct UnicastAddressTableDeleter {
			void operator()(MIB_UNICASTIPADDRESS_TABLE* pTable) const noexcept {
				if (pTable) {
					FreeMibTable(pTable);
				}
			}
		};

		std::string FormatWindowsFailure(char const* pszOperation, const DWORD unCode) {
			return std::string(pszOperation) + " failed (Windows error " + std::to_string(unCode) + ")";
		}

		bool IsValidAdapterName(std::string const& strName) {
			constexpr std::size_t kPrefixLength = 10;
			constexpr std::size_t kMaximumNameLength = 30;

			if (!strName.starts_with("WireSteam-") || (strName.size() <= kPrefixLength) || (strName.size() > kMaximumNameLength)) {
				return false;
			}

			return std::all_of(strName.begin() + kPrefixLength, strName.end(), [](const unsigned char unCharacter) -> bool {
				return ((unCharacter >= 'a') && (unCharacter <= 'z')) || ((unCharacter >= 'A') && (unCharacter <= 'Z')) ||
					((unCharacter >= '0') && (unCharacter <= '9')) || (unCharacter == '_') || (unCharacter == '-');
			});
		}

		// Optional outputs are committed only after a complete canonical private subnet is valid.
		bool IsValidSubnet(std::string const& strSubnet, std::uint32_t* punNetworkAddressOut = nullptr, std::uint32_t* punNetworkMaskOut = nullptr) {
			constexpr unsigned kAddressBits = 32;
			constexpr unsigned kMinimumPrefix = 8;
			constexpr unsigned kMaximumPrefix = 30;
			constexpr unsigned kMaximumOctet = 255;
			constexpr std::size_t kAddressOctets = 4;
			constexpr unsigned kPrivate172PrefixBits = 12;
			constexpr unsigned kPrivate192PrefixBits = 16;
			constexpr unsigned kFirstOctetShift = 0x18;
			constexpr unsigned kSecondOctetShift = 0x10;
			constexpr unsigned kThirdOctetShift = 0x08;

			static_assert(std::numeric_limits<std::uint32_t>::digits == kAddressBits);

			const std::size_t unSlash = strSubnet.find('/');
			if ((unSlash == std::string::npos) || (strSubnet.find('/', unSlash + 1) != std::string::npos)) {
				return false;
			}

			unsigned unPrefix = 0;
			std::from_chars_result const ParsedPrefix = std::from_chars(strSubnet.data() + unSlash + 1, strSubnet.data() + strSubnet.size(), unPrefix);
			if ((ParsedPrefix.ec != std::errc {}) || (ParsedPrefix.ptr != (strSubnet.data() + strSubnet.size())) || (unPrefix < kMinimumPrefix) || (unPrefix > kMaximumPrefix) ||
				(strSubnet.substr(unSlash + 1) != std::to_string(unPrefix))) {
				return false;
			}

			std::array<unsigned, kAddressOctets> arrBytes {};
			std::size_t unStart = 0;
			for (std::size_t unIndex = 0; unIndex < arrBytes.size(); ++unIndex) {
				const std::size_t unEnd = (unIndex == (arrBytes.size() - 1)) ? unSlash : strSubnet.find('.', unStart);
				if ((unEnd == std::string::npos) || (unEnd <= unStart) || (unEnd > unSlash)) {
					return false;
				}

				std::from_chars_result const ParsedByte = std::from_chars(strSubnet.data() + unStart, strSubnet.data() + unEnd, arrBytes[unIndex]);
				if ((ParsedByte.ec != std::errc {}) || (ParsedByte.ptr != (strSubnet.data() + unEnd)) || (arrBytes[unIndex] > kMaximumOctet) ||
					(strSubnet.substr(unStart, unEnd - unStart) != std::to_string(arrBytes[unIndex]))) {
					return false;
				}

				unStart = unEnd + 1;
			}

			const bool bPrivateNetwork = ((arrBytes[0] == 0x0A) && (unPrefix >= kMinimumPrefix)) ||
				((arrBytes[0] == 0xAC) && ((arrBytes[1] & 0xF0) == 0x10) && (unPrefix >= kPrivate172PrefixBits)) ||
				((arrBytes[0] == 0xC0) && (arrBytes[1] == 0xA8) && (unPrefix >= kPrivate192PrefixBits));
			const std::uint32_t unAddress = (std::uint32_t(arrBytes[0]) << kFirstOctetShift) | (std::uint32_t(arrBytes[1]) << kSecondOctetShift) | (std::uint32_t(arrBytes[2]) << kThirdOctetShift) | std::uint32_t(arrBytes[3]);
			const std::uint32_t unHostMask = (std::uint32_t(0x01) << (kAddressBits - unPrefix)) - 1;
			if (!bPrivateNetwork || ((unAddress & unHostMask) != 0x00)) {
				return false;
			}

			if (punNetworkAddressOut) {
				*punNetworkAddressOut = unAddress;
			}

			if (punNetworkMaskOut) {
				*punNetworkMaskOut = ~unHostMask;
			}

			return true;
		}

		// Widens caller-validated ASCII without performing Unicode decoding.
		std::wstring WidenASCII(std::string const& strValue) {
			return { strValue.begin(), strValue.end() };
		}

		bool IsBoundedRecoverySID(BYTE const* punData, const std::size_t unAvailable) noexcept {
			constexpr std::size_t kSIDHeaderBytes = offsetof(SID, SubAuthority);

			if (!punData || (unAvailable < kSIDHeaderBytes) || (punData[offsetof(SID, Revision)] != SID_REVISION)) {
				return false;
			}

			const std::size_t unSubAuthorities = punData[offsetof(SID, SubAuthorityCount)];

			return (unSubAuthorities <= SID_MAX_SUB_AUTHORITIES) && (unSubAuthorities <= ((unAvailable - kSIDHeaderBytes) / sizeof(DWORD)));
		}

		// Both callers own a self-relative descriptor and pass its actual returned byte extent.
		bool IsTrustedRecoveryDescriptor(PSECURITY_DESCRIPTOR pDescriptor, const std::size_t unDescriptorBytes, std::string& strErrorOut) {
			constexpr std::size_t kRequiredACEs = 2;
			constexpr std::size_t kACESIDOffset = offsetof(ACCESS_ALLOWED_ACE, SidStart);

			if (!pDescriptor || (unDescriptorBytes < sizeof(SECURITY_DESCRIPTOR_RELATIVE)) || ((reinterpret_cast<std::uintptr_t>(pDescriptor) % alignof(DWORD)) != 0)) {
				strErrorOut = "Internet gateway recovery records must be owned and writable only by Administrators and SYSTEM";
				return false;
			}

			// Copy the serialized header into aligned storage before following its offsets.
			SECURITY_DESCRIPTOR_RELATIVE RelativeDescriptor {};
			std::memcpy(&RelativeDescriptor, pDescriptor, sizeof(RelativeDescriptor));
			BYTE* punDescriptor = static_cast<BYTE*>(pDescriptor);
			if ((RelativeDescriptor.Revision != SECURITY_DESCRIPTOR_REVISION) || !(RelativeDescriptor.Control & SE_SELF_RELATIVE) || !(RelativeDescriptor.Control & SE_DACL_PROTECTED) || !(RelativeDescriptor.Control & SE_DACL_PRESENT) || (RelativeDescriptor.Owner < sizeof(RelativeDescriptor)) || (RelativeDescriptor.Owner > unDescriptorBytes) || ((RelativeDescriptor.Owner % alignof(DWORD)) != 0) || !IsBoundedRecoverySID(punDescriptor + RelativeDescriptor.Owner, unDescriptorBytes - RelativeDescriptor.Owner) || (RelativeDescriptor.Dacl < sizeof(RelativeDescriptor)) || (RelativeDescriptor.Dacl > unDescriptorBytes) || ((RelativeDescriptor.Dacl % alignof(ACL)) != 0) || ((unDescriptorBytes - RelativeDescriptor.Dacl) < sizeof(ACL))) {
				strErrorOut = "Internet gateway recovery records must be owned and writable only by Administrators and SYSTEM";
				return false;
			}

			ACL ACLHeader {};
			std::memcpy(&ACLHeader, punDescriptor + RelativeDescriptor.Dacl, sizeof(ACLHeader));
			if (((ACLHeader.AclRevision != ACL_REVISION) && (ACLHeader.AclRevision != ACL_REVISION_DS)) || (ACLHeader.AceCount != kRequiredACEs) || (ACLHeader.AclSize < sizeof(ACL)) || (ACLHeader.AclSize > (unDescriptorBytes - RelativeDescriptor.Dacl))) {
				strErrorOut = "Internet gateway recovery records must be owned and writable only by Administrators and SYSTEM";
				return false;
			}

			alignas(DWORD) std::array<BYTE, SECURITY_MAX_SID_SIZE> arrAdministrators {};
			alignas(DWORD) std::array<BYTE, SECURITY_MAX_SID_SIZE> arrSystem {};
			DWORD unAdministratorsBytes = static_cast<DWORD>(arrAdministrators.size());
			DWORD unSystemBytes = static_cast<DWORD>(arrSystem.size());
			if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, arrAdministrators.data(), &unAdministratorsBytes) ||
				!CreateWellKnownSid(WinLocalSystemSid, nullptr, arrSystem.data(), &unSystemBytes)) {
				const DWORD unError = GetLastError();
				strErrorOut = FormatWindowsFailure("Verify Internet gateway recovery identities", unError);
				return false;
			}

			PSID pOwner = punDescriptor + RelativeDescriptor.Owner;
			if (!EqualSid(pOwner, arrAdministrators.data()) && !EqualSid(pOwner, arrSystem.data())) {
				strErrorOut = "Internet gateway recovery records must be owned and writable only by Administrators and SYSTEM";
				return false;
			}

			bool bAdministratorsAllowed = false;
			bool bSystemAllowed = false;
			std::size_t unACEOffset = sizeof(ACL);
			BYTE* punACL = punDescriptor + RelativeDescriptor.Dacl;
			for (std::size_t unIndex = 0; unIndex < kRequiredACEs; ++unIndex) {
				if ((unACEOffset > ACLHeader.AclSize) || ((ACLHeader.AclSize - unACEOffset) < sizeof(ACE_HEADER))) {
					strErrorOut = "Cannot verify Internet gateway recovery record permissions";
					return false;
				}

				ACE_HEADER ACEHeader {};
				std::memcpy(&ACEHeader, punACL + unACEOffset, sizeof(ACEHeader));
				if ((ACEHeader.AceSize < kACESIDOffset) || (ACEHeader.AceSize > (ACLHeader.AclSize - unACEOffset)) || (((RelativeDescriptor.Dacl + unACEOffset + kACESIDOffset) % alignof(DWORD)) != 0) || !IsBoundedRecoverySID(punACL + unACEOffset + kACESIDOffset, ACEHeader.AceSize - kACESIDOffset)) {
					strErrorOut = "Cannot verify Internet gateway recovery record permissions";
					return false;
				}

				ACCESS_MASK unMask = 0;
				std::memcpy(&unMask, punACL + unACEOffset + offsetof(ACCESS_ALLOWED_ACE, Mask), sizeof(unMask));
				if ((ACEHeader.AceType != ACCESS_ALLOWED_ACE_TYPE) || ((unMask & KEY_ALL_ACCESS) != KEY_ALL_ACCESS)) {
					strErrorOut = "Internet gateway recovery record has unexpected permissions";
					return false;
				}

				PSID pSID = punACL + unACEOffset + kACESIDOffset;
				if (EqualSid(pSID, arrAdministrators.data()) && !bAdministratorsAllowed) {
					bAdministratorsAllowed = true;
				} else if (EqualSid(pSID, arrSystem.data()) && !bSystemAllowed) {
					bSystemAllowed = true;
				} else {
					strErrorOut = "Internet gateway recovery record permits an unexpected identity";
					return false;
				}

				unACEOffset += ACEHeader.AceSize;
			}

			return bAdministratorsAllowed && bSystemAllowed;
		}

		bool IsTrustedRecordSecurity(HKEY const hKey, std::string& strErrorOut) {
			constexpr SECURITY_INFORMATION kInformation = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
			constexpr DWORD kMaximumDescriptorBytes = 65536;

			DWORD unSize = 0;
			LSTATUS nStatus = RegGetKeySecurity(hKey, kInformation, nullptr, &unSize);
			if ((nStatus != ERROR_INSUFFICIENT_BUFFER) || !unSize || (unSize > kMaximumDescriptorBytes)) {
				strErrorOut = "Cannot verify Internet gateway recovery record security";
				return false;
			}

			std::vector<BYTE> vecBuffer(unSize);
			PSECURITY_DESCRIPTOR pDescriptor = vecBuffer.data();
			nStatus = RegGetKeySecurity(hKey, kInformation, pDescriptor, &unSize);
			if (nStatus != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Read Internet gateway recovery record security", nStatus);
				return false;
			}

			if (unSize > vecBuffer.size()) {
				strErrorOut = "Cannot verify Internet gateway recovery record security";
				return false;
			}

			return IsTrustedRecoveryDescriptor(pDescriptor, unSize, strErrorOut);
		}

		bool IsValidRecoveryPayload(const DWORD unVersion, std::string const& strExpectedName, std::string const& strRecordedName, std::string const& strSubnet, std::array<ForwardingState, 2> const& arrStates) {
			return (unVersion == kRecoveryVersion) && IsValidAdapterName(strExpectedName) && (strRecordedName == strExpectedName) && IsValidSubnet(strSubnet) &&
				arrStates[0].m_LUID.Value && arrStates[1].m_LUID.Value && (arrStates[0].m_LUID.Value != arrStates[1].m_LUID.Value) &&
				!IsEqualGUID(arrStates[0].m_GUID, GUID_NULL) && !IsEqualGUID(arrStates[1].m_GUID, GUID_NULL) &&
				!IsEqualGUID(arrStates[0].m_GUID, arrStates[1].m_GUID);
		}

		bool CanHandOffRecovery(std::string const& strLiveName, std::string const& strLiveSubnet, std::array<ForwardingState, 2> const& arrLiveStates, std::string const& strRecordedName, std::string const& strRecordedSubnet, std::array<ForwardingState, 2> const& arrRecordedStates) {
			if ((strLiveSubnet != strRecordedSubnet) || !IsValidRecoveryPayload(kRecoveryVersion, strLiveName, strRecordedName, strRecordedSubnet, arrRecordedStates)) {
				return false;
			}

			for (std::size_t unIndex = 0; unIndex < arrLiveStates.size(); ++unIndex) {
				if ((arrLiveStates[unIndex].m_LUID.Value != arrRecordedStates[unIndex].m_LUID.Value) ||
					!IsEqualGUID(arrLiveStates[unIndex].m_GUID, arrRecordedStates[unIndex].m_GUID) ||
					(arrLiveStates[unIndex].m_bChanged && !arrRecordedStates[unIndex].m_bChanged)) {
					return false;
				}
			}

			return true;
		}

		// RootKey is a fresh empty owner; missing is a successful no-record result.
		bool OpenRecoveryParent(RegistryKey& RootKey, const bool bCreate, bool& bMissingOut, std::string& strErrorOut, const bool bWritable = false) {
			bMissingOut = false;

			if (!bCreate) {
				const LSTATUS nStatus = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryParent, REG_OPTION_OPEN_LINK, KEY_READ | (bWritable ? KEY_WRITE : 0) | KEY_WOW64_64KEY, &RootKey.m_hValue);
				if ((nStatus == ERROR_FILE_NOT_FOUND) || (nStatus == ERROR_PATH_NOT_FOUND)) {
					bMissingOut = true;
					return true;
				}

				if (nStatus != ERROR_SUCCESS) {
					strErrorOut = FormatWindowsFailure("Open Internet gateway recovery records", nStatus);
					return false;
				}

				return IsTrustedRecordSecurity(RootKey.m_hValue, strErrorOut);
			}

			PSECURITY_DESCRIPTOR pDescriptor = nullptr;
			const BOOL nConverted = ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)", SDDL_REVISION_1, &pDescriptor, nullptr);
			const DWORD unConversionError = nConverted ? ERROR_SUCCESS : GetLastError();
			std::unique_ptr<void, LocalMemoryDeleter> pDescriptorOwner(pDescriptor);
			if (!nConverted) {
				strErrorOut = FormatWindowsFailure("Protect Internet gateway recovery records", unConversionError);
				return false;
			}

			SECURITY_ATTRIBUTES Attributes { sizeof(SECURITY_ATTRIBUTES), pDescriptor, FALSE };
			RegistryKey SoftwareKey;
			RegistryKey ProjectKey;
			LSTATUS nStatus = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_READ | KEY_CREATE_SUB_KEY | KEY_WOW64_64KEY, &SoftwareKey.m_hValue);
			if (nStatus == ERROR_SUCCESS) {
				nStatus = RegCreateKeyExW(SoftwareKey.m_hValue, L"WireSteam", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_CREATE_SUB_KEY | KEY_WOW64_64KEY, &Attributes, &ProjectKey.m_hValue, nullptr);
			}

			if (nStatus != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Create Internet gateway recovery namespace", nStatus);
				return false;
			}

			if (!IsTrustedRecordSecurity(ProjectKey.m_hValue, strErrorOut)) {
				return false;
			}

			nStatus = RegCreateKeyExW(ProjectKey.m_hValue, L"InternetGateways", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &Attributes, &RootKey.m_hValue, nullptr);
			if (nStatus != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Create Internet gateway recovery records", nStatus);
				return false;
			}

			return IsTrustedRecordSecurity(RootKey.m_hValue, strErrorOut);
		}

		// Raw registry boundary: a failed query can partially write bytes; callers validate a full record.
		bool ReadRegistryValue(HKEY const hKey, wchar_t const* pszName, const DWORD unType, void* pData, const DWORD unLength) {
			if (!hKey || !pszName || !pData || (unLength == 0)) {
				return false;
			}

			DWORD unActualType = 0;
			DWORD unActualLength = unLength;

			return (RegQueryValueExW(hKey, pszName, nullptr, &unActualType, static_cast<BYTE*>(pData), &unActualLength) == ERROR_SUCCESS) &&
				(unActualType == unType) && (unActualLength == unLength);
		}

		bool ReadRegistryASCIIString(HKEY const hKey, wchar_t const* pszName, std::string& strValueOut) {
			constexpr std::size_t kMaximumCharacters = 129;
			constexpr wchar_t kMaximumASCIICharacter = 0x7F;

			static_assert((kMaximumCharacters * sizeof(wchar_t)) <= std::numeric_limits<DWORD>::max());

			if (!hKey || !pszName) {
				return false;
			}

			wchar_t szBuffer[kMaximumCharacters] {};
			DWORD unType = 0;
			DWORD unBytes = sizeof(szBuffer);
			if ((RegQueryValueExW(hKey, pszName, nullptr, &unType, reinterpret_cast<BYTE*>(szBuffer), &unBytes) != ERROR_SUCCESS) ||
				(unType != REG_SZ) || (unBytes < sizeof(wchar_t)) || (unBytes % sizeof(wchar_t)) || (unBytes > sizeof(szBuffer))) {
				return false;
			}

			const std::size_t unCount = unBytes / sizeof(wchar_t);
			if ((szBuffer[unCount - 1] != 0) || (std::wcslen(szBuffer) != (unCount - 1))) {
				return false;
			}

			std::string strValue;
			for (std::size_t unIndex = 0; (unIndex + 1) < unCount; ++unIndex) {
				if (szBuffer[unIndex] > kMaximumASCIICharacter) {
					return false;
				}

				strValue += static_cast<char>(szBuffer[unIndex]);
			}

			strValueOut = std::move(strValue);

			return true;
		}

		// Existence is published even for an invalid record; payload outputs commit only on success.
		bool ReadRecoveryRecord(std::string const& strName, std::string& strSubnetOut, std::array<ForwardingState, 2>& arrStatesOut, bool& bExistsOut, std::string& strErrorOut) {
			bExistsOut = false;

			RegistryKey ParentKey;
			RegistryKey RecordKey;
			bool bMissing = false;
			if (!OpenRecoveryParent(ParentKey, false, bMissing, strErrorOut) || bMissing) {
				return bMissing;
			}

			const LSTATUS nStatus = RegOpenKeyExW(ParentKey.m_hValue, WidenASCII(strName).c_str(), REG_OPTION_OPEN_LINK, KEY_READ | KEY_WOW64_64KEY, &RecordKey.m_hValue);
			if (nStatus == ERROR_FILE_NOT_FOUND) {
				return true;
			}

			if (nStatus != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Open owned Internet gateway recovery record", nStatus);
				return false;
			}

			bExistsOut = true;

			if (!IsTrustedRecordSecurity(RecordKey.m_hValue, strErrorOut)) {
				return false;
			}

			DWORD unVersion = 0;
			std::string strRecordedName;
			std::string strSubnet;
			std::array<ForwardingState, 2> arrStates {};

			if (!ReadRegistryValue(RecordKey.m_hValue, L"Version", REG_DWORD, &unVersion, sizeof(unVersion)) || (unVersion != kRecoveryVersion) ||
				!ReadRegistryASCIIString(RecordKey.m_hValue, L"Name", strRecordedName) || (strRecordedName != strName) ||
				!ReadRegistryASCIIString(RecordKey.m_hValue, L"Subnet", strSubnet) || !IsValidSubnet(strSubnet)) {
				strErrorOut = "The Internet gateway recovery record is incomplete or malformed; cleanup was refused";
				return false;
			}

			for (std::size_t unIndex = 0; unIndex < arrStates.size(); ++unIndex) {
				std::wstring const wstrPrefix = L"Forwarding" + std::to_wstring(unIndex);
				DWORD unChanged = 0;
				if (!ReadRegistryValue(RecordKey.m_hValue, (wstrPrefix + L"Luid").c_str(), REG_QWORD, &arrStates[unIndex].m_LUID.Value, sizeof(arrStates[unIndex].m_LUID.Value)) ||
					!ReadRegistryValue(RecordKey.m_hValue, (wstrPrefix + L"Guid").c_str(), REG_BINARY, &arrStates[unIndex].m_GUID, sizeof(arrStates[unIndex].m_GUID)) ||
					!ReadRegistryValue(RecordKey.m_hValue, (wstrPrefix + L"Changed").c_str(), REG_DWORD, &unChanged, sizeof(unChanged)) ||
					(unChanged > 1) || !arrStates[unIndex].m_LUID.Value || IsEqualGUID(arrStates[unIndex].m_GUID, GUID_NULL)) {
					strErrorOut = "The Internet gateway forwarding recovery record is malformed; cleanup was refused";
					return false;
				}

				arrStates[unIndex].m_bChanged = unChanged != 0;
			}

			if (!IsValidRecoveryPayload(unVersion, strName, strRecordedName, strSubnet, arrStates)) {
				strErrorOut = "The Internet gateway recovery payload is invalid; cleanup was refused";
				return false;
			}

			strSubnetOut = std::move(strSubnet);
			arrStatesOut = arrStates;

			return true;
		}

		// A newly created record stays owned on partial failure; the live caller performs rollback.
		bool WriteRecoveryRecord(std::string const& strName, std::string const& strSubnet, std::array<ForwardingState, 2> const& arrStates, bool& bRecordOwnedOut, std::string& strErrorOut) {
			constexpr std::size_t kMaximumWideCharacters = std::numeric_limits<DWORD>::max() / sizeof(wchar_t);

			RegistryKey ParentKey;
			RegistryKey RecordKey;
			bool bMissing = false;
			if (!OpenRecoveryParent(ParentKey, true, bMissing, strErrorOut)) {
				return false;
			}

			PSECURITY_DESCRIPTOR pDescriptor = nullptr;
			const BOOL nConverted = ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)", SDDL_REVISION_1, &pDescriptor, nullptr);
			const DWORD unConversionError = nConverted ? ERROR_SUCCESS : GetLastError();
			std::unique_ptr<void, LocalMemoryDeleter> pDescriptorOwner(pDescriptor);
			if (!nConverted) {
				strErrorOut = FormatWindowsFailure("Protect owned Internet gateway recovery record", unConversionError);
				return false;
			}

			SECURITY_ATTRIBUTES Attributes { sizeof(SECURITY_ATTRIBUTES), pDescriptor, FALSE };
			DWORD unDisposition = 0;
			LSTATUS nStatus = RegCreateKeyExW(ParentKey.m_hValue, WidenASCII(strName).c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &Attributes, &RecordKey.m_hValue, &unDisposition);
			pDescriptorOwner.reset();

			if (nStatus != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Create owned Internet gateway recovery record", nStatus);
				return false;
			}

			if (unDisposition != REG_CREATED_NEW_KEY) {
				strErrorOut = "An Internet gateway recovery record already exists; run explicit tunnel cleanup before starting";
				return false;
			}

			bRecordOwnedOut = true;

			if (!IsTrustedRecordSecurity(RecordKey.m_hValue, strErrorOut)) {
				return false;
			}

			auto WriteValue = [&nStatus, &RecordKey](wchar_t const* pszName, const DWORD unType, void const* pData, const DWORD unLength) -> void {
				if (nStatus == ERROR_SUCCESS) {
					nStatus = RegSetValueExW(RecordKey.m_hValue, pszName, 0, unType, static_cast<BYTE const*>(pData), unLength);
				}
			};

			std::wstring const wstrName = WidenASCII(strName);
			std::wstring const wstrSubnet = WidenASCII(strSubnet);
			if ((wstrName.size() >= kMaximumWideCharacters) || (wstrSubnet.size() >= kMaximumWideCharacters)) {
				strErrorOut = "Internet gateway recovery text exceeds the Windows registry value size limit";
				return false;
			}

			WriteValue(L"Name", REG_SZ, wstrName.c_str(), static_cast<DWORD>((wstrName.size() + 1) * sizeof(wchar_t)));
			WriteValue(L"Subnet", REG_SZ, wstrSubnet.c_str(), static_cast<DWORD>((wstrSubnet.size() + 1) * sizeof(wchar_t)));

			for (std::size_t unIndex = 0; unIndex < arrStates.size(); ++unIndex) {
				std::wstring const wstrPrefix = L"Forwarding" + std::to_wstring(unIndex);
				const DWORD unChanged = arrStates[unIndex].m_bChanged ? 1 : 0;
				WriteValue((wstrPrefix + L"Luid").c_str(), REG_QWORD, &arrStates[unIndex].m_LUID.Value, sizeof(arrStates[unIndex].m_LUID.Value));
				WriteValue((wstrPrefix + L"Guid").c_str(), REG_BINARY, &arrStates[unIndex].m_GUID, sizeof(arrStates[unIndex].m_GUID));
				WriteValue((wstrPrefix + L"Changed").c_str(), REG_DWORD, &unChanged, sizeof(unChanged));
			}

			// Version is the commit marker. A partial write is never accepted as an
			// authorized crash-recovery record, and is removed by this live owner.
			WriteValue(L"Version", REG_DWORD, &kRecoveryVersion, sizeof(kRecoveryVersion));

			if (nStatus == ERROR_SUCCESS) {
				nStatus = RegFlushKey(RecordKey.m_hValue);
			}

			if (nStatus != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Persist owned Internet gateway recovery state", nStatus);
				return false;
			}

			return true;
		}

		bool RemoveRecoveryRecord(std::string const& strName, std::string const& strSubnet, std::string& strErrorOut) {
			RegistryKey ParentKey;
			RegistryKey RecordKey;
			bool bMissing = false;
			if (!OpenRecoveryParent(ParentKey, false, bMissing, strErrorOut, true) || bMissing) {
				return bMissing;
			}

			std::wstring const wstrName = WidenASCII(strName);
			LSTATUS nStatus = RegOpenKeyExW(ParentKey.m_hValue, wstrName.c_str(), REG_OPTION_OPEN_LINK, KEY_READ | KEY_WOW64_64KEY, &RecordKey.m_hValue);
			if (nStatus == ERROR_FILE_NOT_FOUND) {
				return true;
			}

			if (nStatus != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Open completed Internet gateway recovery record", nStatus);
				return false;
			}

			if (!IsTrustedRecordSecurity(RecordKey.m_hValue, strErrorOut)) {
				return false;
			}

			std::string strRecordedSubnet;
			// A missing prefix is possible only after a failed, partial write by this
			// live owner. A present different prefix must never be removed.
			DWORD unType = 0;
			DWORD unBytes = 0;
			nStatus = RegQueryValueExW(RecordKey.m_hValue, L"Subnet", nullptr, &unType, nullptr, &unBytes);
			if ((nStatus != ERROR_FILE_NOT_FOUND) && (!ReadRegistryASCIIString(RecordKey.m_hValue, L"Subnet", strRecordedSubnet) || (strRecordedSubnet != strSubnet))) {
				strErrorOut = "The owned Internet gateway recovery prefix changed; its record was left untouched";
				return false;
			}

			nStatus = RegCloseKey(RecordKey.m_hValue);
			RecordKey.m_hValue = nullptr;

			if (nStatus != ERROR_SUCCESS) {
				strErrorOut = FormatWindowsFailure("Close completed Internet gateway recovery record", nStatus);
				return false;
			}

			nStatus = RegDeleteKeyExW(ParentKey.m_hValue, wstrName.c_str(), KEY_WOW64_64KEY, 0);
			if ((nStatus != ERROR_SUCCESS) && (nStatus != ERROR_FILE_NOT_FOUND)) {
				strErrorOut = FormatWindowsFailure("Remove completed Internet gateway recovery record", nStatus);
				return false;
			}

			return true;
		}

		std::string EncodeUTF16LEBase64(std::wstring const& wstrText) {
			constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			constexpr unsigned kByteBits = 8;
			constexpr unsigned kEncodedBits = 6;
			constexpr unsigned kByteMask = 0xFF;
			constexpr unsigned kEncodedMask = 0x3F;
			constexpr std::size_t kBytesPerGroup = 3;
			constexpr std::size_t kCharactersPerGroup = 4;

			static_assert(sizeof(wchar_t) == 0x02);
			static_assert(sizeof(kAlphabet) == 65);

			if (wstrText.size() > ((std::numeric_limits<std::size_t>::max() - (kBytesPerGroup - 1)) / sizeof(wchar_t))) {
				throw std::length_error("Internet gateway PowerShell text exceeds the base64 input size limit");
			}

			const std::size_t unGroups = ((wstrText.size() * sizeof(wchar_t)) + (kBytesPerGroup - 1)) / kBytesPerGroup;
			if (unGroups > (std::numeric_limits<std::size_t>::max() / kCharactersPerGroup)) {
				throw std::length_error("Internet gateway PowerShell text exceeds the base64 output size limit");
			}

			std::string strEncoded;
			strEncoded.reserve(unGroups * kCharactersPerGroup);
			unsigned unPending = 0;
			unsigned unBits = 0;
			for (auto const& wchCharacter : wstrText) {
				for (auto const& unByte : { static_cast<unsigned>(wchCharacter & kByteMask), static_cast<unsigned>((wchCharacter >> kByteBits) & kByteMask) }) {
					unPending = (unPending << kByteBits) | unByte;
					unBits += kByteBits;
					while (unBits >= kEncodedBits) {
						unBits -= kEncodedBits;
						strEncoded += kAlphabet[(unPending >> unBits) & kEncodedMask];
					}
				}
			}

			if (unBits) {
				strEncoded += kAlphabet[(unPending << (kEncodedBits - unBits)) & kEncodedMask];
			}

			while (strEncoded.size() % kCharactersPerGroup) {
				strEncoded += '=';
			}

			return strEncoded;
		}

		std::filesystem::path FindSystemPowerShell(std::string& strErrorOut) {
			constexpr UINT kDirectoryCharacters = 32768;

			std::wstring wstrDirectory = std::wstring(kDirectoryCharacters, L'\0');
			const UINT unCount = GetSystemDirectoryW(wstrDirectory.data(), static_cast<UINT>(wstrDirectory.size()));
			if (!unCount) {
				const DWORD unError = GetLastError();
				strErrorOut = FormatWindowsFailure("Locate Windows PowerShell", unError);
				return {};
			}

			if (unCount >= wstrDirectory.size()) {
				strErrorOut = "The Windows system directory exceeds the supported path buffer";
				return {};
			}

			wstrDirectory.resize(unCount);
			std::filesystem::path const Executable = std::filesystem::path(wstrDirectory) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
			const DWORD unAttributes = GetFileAttributesW(Executable.c_str());
			if ((unAttributes == INVALID_FILE_ATTRIBUTES) || (unAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
				strErrorOut = "Windows PowerShell is unavailable in the protected Windows system directory";
				return {};
			}

			return Executable;
		}

		std::wstring BuildScriptPreamble(std::filesystem::path const& Executable) {
			// Only the OS-derived, protected module path enters this script. Avoid
			// PSModulePath search and profile loading in the elevated child process.
			std::wstring const wstrModule = (Executable.parent_path() / L"Modules" / L"NetNat" / L"NetNat.psd1").wstring();
			std::wstring wstrQuoted;
			for (auto const& wchCharacter : wstrModule) {
				wstrQuoted += wchCharacter;

				if (wchCharacter == L'\'') {
					wstrQuoted += wchCharacter;
				}
			}

			std::wostringstream ScriptStream;
			// Preserve the former string builder's exception propagation on output failure.
			ScriptStream.exceptions(std::ios::badbit | std::ios::failbit);

			ScriptStream
				<< L"$ErrorActionPreference = 'Stop'\n"
				<< L"$ProgressPreference = 'SilentlyContinue'\n"
				<< L"$PSModuleAutoLoadingPreference = 'None'\n"
				<< L"[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)\n"
				<< L"\n"
				<< L"try {\n"
				<< L"\tMicrosoft.PowerShell.Core\\Import-Module -Name '" << wstrQuoted << L"' -Force -ErrorAction Stop\n";

			return ScriptStream.str();
		}

		std::wstring BuildNATScript(const NATAction Action, std::string const& strName, std::string const& strSubnet, std::filesystem::path const& Executable) {
			if (!IsValidAdapterName(strName) || !IsValidSubnet(strSubnet)) {
				throw std::invalid_argument("Invalid Internet server NAT name or subnet");
			}

			std::wstring const wstrPreamble = BuildScriptPreamble(Executable);
			std::wstring const wstrName = WidenASCII(strName);
			std::wstring const wstrSubnet = WidenASCII(strSubnet);

			std::wostringstream ScriptStream;
			ScriptStream.exceptions(std::ios::badbit | std::ios::failbit);

			ScriptStream
				<< wstrPreamble
				<< L"\t$natName = '" << wstrName << L"'\n"
				<< L"\t$subnetPrefix = '" << wstrSubnet << L"'\n"
				<< L"\n";

			if ((Action == NATAction::Probe) || (Action == NATAction::Create)) {
				ScriptStream
					<< L"\t$allNATs = @(NetNat\\Get-NetNat -ErrorAction Stop)\n"
					<< L"\tif ($allNATs.Count -ne 0) {\n"
					<< L"\t\tthrow 'An existing Windows NAT is configured. WireSteam will not replace or modify it. Stop its owner or use a separate exit host.'\n"
					<< L"\t}\n"
					<< L"\n";

				if (Action == NATAction::Create) {
					ScriptStream
						<< L"\t$createdNAT = NetNat\\New-NetNat -Name $natName -InternalIPInterfaceAddressPrefix $subnetPrefix -ErrorAction Stop\n"
						<< L"\tif (($null -eq $createdNAT) -or ($createdNAT.Name -cne $natName) -or ($createdNAT.InternalIPInterfaceAddressPrefix -cne $subnetPrefix)) {\n"
						<< L"\t\tthrow 'Windows returned an unexpected NAT object; no foreign NAT was removed.'\n"
						<< L"\t}\n"
						<< L"\n"
						<< L"\t[Console]::Out.WriteLine('WIRESTEAM_NAT_CREATED')\n";

				} else {
					ScriptStream << L"\t[Console]::Out.WriteLine('WIRESTEAM_NAT_AVAILABLE')\n";
				}

			} else {
				// Filter the complete list instead of wildcard-capable name lookup,
				// and remove only the exact CIM object whose creation we acknowledged.
				ScriptStream
					<< L"\t$allNATs = @(NetNat\\Get-NetNat -ErrorAction Stop)\n"
					<< L"\t$ownedNATs = @($allNATs | Where-Object {\n"
					<< L"\t\t$_.Name -ceq $natName\n"
					<< L"\t})\n"
					<< L"\tif ($ownedNATs.Count -gt 1) {\n"
					<< L"\t\tthrow 'Multiple NAT objects matched the owned name; cleanup refused.'\n"
					<< L"\t}\n"
					<< L"\n"
					<< L"\tif ($ownedNATs.Count -eq 1) {\n"
					<< L"\t\tif ($ownedNATs[0].InternalIPInterfaceAddressPrefix -cne $subnetPrefix) {\n"
					<< L"\t\t\tthrow 'The owned NAT prefix changed; cleanup refused to remove a different network.'\n"
					<< L"\t\t}\n"
					<< L"\n"
					<< L"\t\tNetNat\\Remove-NetNat -InputObject $ownedNATs[0] -Confirm:$false -ErrorAction Stop\n"
					<< L"\t}\n"
					<< L"\n"
					<< L"\t[Console]::Out.WriteLine('WIRESTEAM_NAT_REMOVED')\n";
			}

			ScriptStream
				<< L"\texit 0\n"
				<< L"} catch {\n"
				<< L"\t[Console]::Error.WriteLine($_.Exception.Message)\n"
				<< L"\texit 1\n"
				<< L"}\n";

			return ScriptStream.str();
		}

		// Callers retain the global NetNat lease through this blocking child command;
		// serializing the single NAT prefix is intentional, and no callback runs here.
		bool RunNAT(const NATAction Action, std::string const& strName, std::string const& strSubnet, std::string& strErrorOut) {
			constexpr unsigned kProcessTimeoutMilliseconds = 30000;
			constexpr std::size_t kMaximumProcessOutputBytes = 65536;
			constexpr std::size_t kMaximumErrorBytes = 4096;

			std::filesystem::path const Executable = FindSystemPowerShell(strErrorOut);
			if (Executable.empty()) {
				return false;
			}

			std::string const strEncoded = EncodeUTF16LEBase64(BuildNATScript(Action, strName, strSubnet, Executable));
			std::wstring const wstrArguments = L"-NoLogo -NoProfile -NonInteractive -EncodedCommand " + std::wstring(strEncoded.begin(), strEncoded.end());
			ProcessResult const Result = RunProcess(Executable, wstrArguments, {}, kProcessTimeoutMilliseconds, kMaximumProcessOutputBytes);
			std::string const strMarker = (Action == NATAction::Probe) ? "WIRESTEAM_NAT_AVAILABLE" : (Action == NATAction::Create) ? "WIRESTEAM_NAT_CREATED" : "WIRESTEAM_NAT_REMOVED";
			if (!Result.m_strError.empty() || (Result.m_nExitCode != 0) || (Result.m_strOutput.find(strMarker) == std::string::npos)) {
				strErrorOut = "Windows NAT " + std::string((Action == NATAction::Probe) ? "availability check" : (Action == NATAction::Create) ? "creation" : "cleanup") +
					" failed. " + (!Result.m_strError.empty() ? Result.m_strError : Result.m_strOutput.empty() ? "The NetNat provider is unavailable or rejected the request." : Result.m_strOutput);
				if (strErrorOut.size() > kMaximumErrorBytes) {
					strErrorOut.resize(kMaximumErrorBytes);
				}

				if (Action == NATAction::Create) {
					strErrorOut += " NAT creation was not acknowledged; inspect Get-NetNat for a leftover WireSteam NAT before retrying. No existing NAT was removed.";
				}

				return false;
			}

			return true;
		}

		bool CaptureForwardingInterface(NET_LUID const LUID, ForwardingState& StateOut, std::string& strErrorOut) {
			MIB_IF_ROW2 InterfaceRow {};
			InterfaceRow.InterfaceLuid = LUID;
			const DWORD unCode = GetIfEntry2(&InterfaceRow);
			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Read the Internet gateway interface", unCode);
				return false;
			}

			if ((InterfaceRow.OperStatus != IfOperStatusUp) || (InterfaceRow.Type == IF_TYPE_SOFTWARE_LOOPBACK)) {
				strErrorOut = "The Internet gateway requires active IPv4 tunnel and uplink interfaces";
				return false;
			}

			StateOut.m_LUID = LUID;
			StateOut.m_GUID = InterfaceRow.InterfaceGuid;

			return true;
		}

		bool IsOwnedForwardingInterface(std::string const& strName, ForwardingState const& State, MIB_IF_ROW2 const& Row) {
			// The default uplink can belong to another VPN. Only the exact adapter
			// created for this session is authorized for an IP-interface update.
			return IsValidAdapterName(strName) && State.m_LUID.Value && (Row.InterfaceLuid.Value == State.m_LUID.Value) &&
				!IsEqualGUID(State.m_GUID, GUID_NULL) && IsEqualGUID(Row.InterfaceGuid, State.m_GUID) &&
				(_wcsicmp(Row.Alias, WidenASCII(strName).c_str()) == 0) &&
				(Row.OperStatus == IfOperStatusUp) && (Row.Type != IF_TYPE_SOFTWARE_LOOPBACK);
		}

		bool EnableTunnelForwarding(std::string const& strName, ForwardingState& State, std::string& strErrorOut) {
			MIB_IF_ROW2 InterfaceRow {};
			InterfaceRow.InterfaceLuid = State.m_LUID;
			DWORD unCode = GetIfEntry2(&InterfaceRow);
			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Verify the owned IPv4 forwarding interface", unCode);
				return false;
			}

			if (!IsOwnedForwardingInterface(strName, State, InterfaceRow)) {
				strErrorOut = "IPv4 forwarding can be changed only on this session's owned WireSteam adapter";
				return false;
			}

			MIB_IPINTERFACE_ROW Row {};
			InitializeIpInterfaceEntry(&Row);
			Row.Family = AF_INET;
			Row.InterfaceLuid = State.m_LUID;
			unCode = GetIpInterfaceEntry(&Row);
			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Read IPv4 forwarding state", unCode);
				return false;
			}

			if (Row.ForwardingEnabled) {
				return true;
			}

			Row.ForwardingEnabled = TRUE;
			Row.SitePrefixLength = 0;
			unCode = SetIpInterfaceEntry(&Row);
			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Enable session IPv4 forwarding", unCode);
				return false;
			}

			State.m_bChanged = true;

			return true;
		}

		bool RestoreForwarding(ForwardingState& State, std::string& strErrorOut) {
			if (!State.m_bChanged) {
				return true;
			}

			MIB_IF_ROW2 InterfaceRow {};
			InterfaceRow.InterfaceLuid = State.m_LUID;
			DWORD unCode = GetIfEntry2(&InterfaceRow);
			// Removing an adapter removes its interface-scoped forwarding setting.
			if ((unCode == ERROR_FILE_NOT_FOUND) || (unCode == ERROR_NOT_FOUND)) {
				State.m_bChanged = false;
				return true;
			}

			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Read owned IPv4 forwarding interface", unCode);
				return false;
			}

			if ((InterfaceRow.InterfaceLuid.Value != State.m_LUID.Value) || !IsEqualGUID(InterfaceRow.InterfaceGuid, State.m_GUID)) {
				strErrorOut = "The owned forwarding interface identity changed; its current settings were left untouched";
				return false;
			}

			MIB_IPINTERFACE_ROW Row {};
			InitializeIpInterfaceEntry(&Row);
			Row.Family = AF_INET;
			Row.InterfaceLuid = State.m_LUID;
			unCode = GetIpInterfaceEntry(&Row);
			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Read owned IPv4 forwarding state", unCode);
				return false;
			}

			if (Row.ForwardingEnabled) {
				Row.ForwardingEnabled = FALSE;
				Row.SitePrefixLength = 0;
				unCode = SetIpInterfaceEntry(&Row);
				if (unCode != NO_ERROR) {
					strErrorOut = FormatWindowsFailure("Restore session IPv4 forwarding", unCode);
					return false;
				}
			}

			State.m_bChanged = false;

			return true;
		}

		bool IsUsableUplinkSource(const ULONG unNetworkOrder) {
			constexpr ULONG kUnspecifiedOctet = 0x00;
			constexpr ULONG kLoopbackOctet = 0x7F;
			constexpr ULONG kLinkLocalPrefix = 0xA9FE;
			constexpr ULONG kFirstMulticastClass = 0x0E;
			constexpr unsigned kFirstOctetShift = 0x18;
			constexpr unsigned kTwoOctetShift = 0x10;
			constexpr unsigned kHighNibbleShift = 0x1C;

			const ULONG unAddress = ntohl(unNetworkOrder);

			return ((unAddress >> kFirstOctetShift) != kUnspecifiedOctet) && ((unAddress >> kFirstOctetShift) != kLoopbackOctet) &&
				((unAddress >> kTwoOctetShift) != kLinkLocalPrefix) && ((unAddress >> kHighNibbleShift) < kFirstMulticastClass);
		}

		bool CaptureGatewayInterfaces(std::string const& strName, std::string const& strSubnet, std::array<ForwardingState, 2>& arrStatesOut, std::string& strErrorOut) {
			constexpr ULONG kRouteProbeAddress = 0x08080808;
			constexpr UINT8 kMaximumDefaultPrefixBits = 1;
			constexpr std::size_t kAdapterPrefixLength = 10;

			std::wstring const wstrAlias = WidenASCII(strName);
			NET_LUID TunnelLUID {};
			DWORD unCode = ConvertInterfaceAliasToLuid(wstrAlias.c_str(), &TunnelLUID);
			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Find the owned WireGuard adapter", unCode);
				return false;
			}

			SOCKADDR_INET Target {};
			SOCKADDR_INET Source {};
			Target.si_family = AF_INET;
			Target.Ipv4.sin_addr.s_addr = htonl(kRouteProbeAddress);
			MIB_IPFORWARD_ROW2 Route {};
			unCode = GetBestRoute2(nullptr, 0, nullptr, &Target, 0, &Route, &Source);
			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Find the current IPv4 Internet uplink", unCode);
				return false;
			}

			if (!Route.InterfaceLuid.Value || (Route.InterfaceLuid.Value == TunnelLUID.Value) || (Source.si_family != AF_INET) ||
				(Route.NextHop.si_family != AF_INET) || !IsUsableUplinkSource(Source.Ipv4.sin_addr.s_addr) || (Route.DestinationPrefix.PrefixLength > kMaximumDefaultPrefixBits)) {
				strErrorOut = "The Internet server requires a usable IPv4 default uplink outside its WireGuard adapter";
				return false;
			}

			MIB_IF_ROW2 Uplink {};
			Uplink.InterfaceLuid = Route.InterfaceLuid;
			unCode = GetIfEntry2(&Uplink);
			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Read the current IPv4 Internet uplink", unCode);
				return false;
			}

			if (_wcsnicmp(Uplink.Alias, L"WireSteam-", kAdapterPrefixLength) == 0) {
				strErrorOut = "An Internet server uplink cannot be another WireSteam adapter";
				return false;
			}

			PMIB_UNICASTIPADDRESS_TABLE pAddresses = nullptr;
			unCode = GetUnicastIpAddressTable(AF_INET, &pAddresses);
			std::unique_ptr<MIB_UNICASTIPADDRESS_TABLE, UnicastAddressTableDeleter> pAddressTable(pAddresses);
			if (unCode != NO_ERROR) {
				strErrorOut = FormatWindowsFailure("Check the Internet NAT subnet against local adapters", unCode);
				return false;
			}

			if (!pAddressTable) {
				strErrorOut = "Windows returned no local adapter address table";
				return false;
			}

			std::uint32_t unNetwork = 0;
			std::uint32_t unMask = 0;
			if (!IsValidSubnet(strSubnet, &unNetwork, &unMask)) {
				strErrorOut = "The Internet NAT subnet is invalid";
				return false;
			}

			// Successful SDK ownership supplies the complete NumEntries/Table allocation.
			for (std::size_t unIndex = 0; unIndex < pAddressTable->NumEntries; ++unIndex) {
				MIB_UNICASTIPADDRESS_ROW const& Row = pAddressTable->Table[unIndex];
				if ((Row.InterfaceLuid.Value != TunnelLUID.Value) && (Row.Address.si_family == AF_INET) &&
					((ntohl(Row.Address.Ipv4.sin_addr.s_addr) & unMask) == unNetwork)) {
					strErrorOut = "InternetSubnet overlaps an IPv4 address assigned to another local adapter. Choose a dedicated VPN subnet before enabling Internet server mode.";
					return false;
				}
			}

			// Retain the uplink identity in the v1 recovery schema for backwards
			// compatibility, but new sessions never enable forwarding on that adapter.
			// WinNAT uses the host routing table; an existing VPN remains its egress
			// without changing that VPN's interface configuration or route metrics.
			std::array<ForwardingState, 2> arrCapturedStates = arrStatesOut;
			if (!CaptureForwardingInterface(TunnelLUID, arrCapturedStates[0], strErrorOut) || !CaptureForwardingInterface(Route.InterfaceLuid, arrCapturedStates[1], strErrorOut)) {
				return false;
			}

			arrStatesOut = arrCapturedStates;

			return true;
		}

		void ExpectCondition(const bool bCondition, char const* pszMessage) {
			if (!bCondition) {
				throw std::runtime_error(std::string("Internet server test failed: ") + pszMessage);
			}
		}
	} // namespace

	struct InternetServerGateway::Implementation {
		Implementation() {
			m_hMutex = nullptr;
			m_bMutexOwned = false;
			m_bNATOwned = false;
			m_bRecordOwned = false;
			m_bActive = false;
			m_bCleanupFailed = false;
		}

		Implementation(Implementation const&) = delete;
		Implementation& operator=(Implementation const&) = delete;

		~Implementation() {
			CloseMutex();
		}

		void ReleaseMutexOwnership() noexcept {
			// Helper and recovery instances acquire/release on the same stack-owning thread.
			// A confirmed acquisition must have exactly one confirmed release before handoff.
			if (!::ReleaseMutex(m_hMutex)) {
				std::fputs("WireSteam: cannot release the owned Internet gateway mutex.\n", stderr);
				std::terminate();
			}

			m_bMutexOwned = false;
		}

		void CloseMutex() noexcept {
			if (m_bMutexOwned) {
				ReleaseMutexOwnership();
			}

			if (m_hMutex) {
				// The owned mutex has been released; its terminal handle close is best effort.
				CloseHandle(m_hMutex);
			}

			m_hMutex = nullptr;
		}

		bool AcquireMutex(std::string& strErrorOut) {
			if (m_bMutexOwned) {
				return true;
			}

			PSECURITY_DESCRIPTOR pDescriptor = nullptr;
			const BOOL nConverted = ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &pDescriptor, nullptr);
			const DWORD unConversionError = nConverted ? ERROR_SUCCESS : GetLastError();
			std::unique_ptr<void, LocalMemoryDeleter> pDescriptorOwner(pDescriptor);
			if (!nConverted) {
				strErrorOut = FormatWindowsFailure("Protect the Internet gateway lock", unConversionError);
				return false;
			}

			SECURITY_ATTRIBUTES Attributes { sizeof(SECURITY_ATTRIBUTES), pDescriptor, FALSE };
			m_hMutex = CreateMutexW(&Attributes, FALSE, L"Global\\WireSteamInternetServerNetNatV1");
			const DWORD unCode = GetLastError();
			pDescriptorOwner.reset();

			if (!m_hMutex) {
				strErrorOut = FormatWindowsFailure("Acquire the Internet gateway lock", unCode);
				return false;
			}

			const DWORD unWaitResult = WaitForSingleObject(m_hMutex, 0);
			const DWORD unWaitError = (unWaitResult == WAIT_FAILED) ? GetLastError() : ERROR_SUCCESS;
			if ((unWaitResult != WAIT_OBJECT_0) && (unWaitResult != WAIT_ABANDONED)) {
				// No ownership was acquired; close before any diagnostic allocation can throw.
				CloseHandle(m_hMutex);
				m_hMutex = nullptr;
				strErrorOut = (unWaitResult == WAIT_FAILED) ? FormatWindowsFailure("Wait for the Internet gateway lock", unWaitError) : "Another WireSteam Internet server is already running; Windows supports one internal NAT prefix";
				return false;
			}

			m_bMutexOwned = true;

			return true;
		}

		std::string m_strName;
		std::string m_strSubnet;
		std::string m_strCleanupError;
		std::array<ForwardingState, 2> m_arrForwarding;
		HANDLE m_hMutex;
		bool m_bMutexOwned;
		bool m_bNATOwned;
		bool m_bRecordOwned;
		bool m_bActive;
		// Retained until all cleanup obligations and the lease are confirmed released.
		bool m_bCleanupFailed;
	};

	InternetServerGateway::InternetServerGateway() {
		m_pImplementation = std::make_unique<Implementation>();
	}

	InternetServerGateway::~InternetServerGateway() {
		Stop();
		m_pImplementation->CloseMutex();
	}

	bool InternetServerGateway::Start(std::string const& strAdapterName, std::string const& strSubnet, std::string& strErrorOut) {
		strErrorOut.clear();

		if (m_pImplementation->m_bActive || m_pImplementation->m_bNATOwned || m_pImplementation->m_bRecordOwned || m_pImplementation->m_arrForwarding[0].m_bChanged || m_pImplementation->m_arrForwarding[1].m_bChanged || m_pImplementation->m_bCleanupFailed) {
			strErrorOut = "The owned Internet gateway must be stopped before starting another session";
			return false;
		}

		if (!IsValidAdapterName(strAdapterName) || !IsValidSubnet(strSubnet)) {
			strErrorOut = "Internet server requires a WireSteam adapter name and canonical private IPv4 subnet";
			return false;
		}

		if (!IsElevated()) {
			strErrorOut = "Internet server setup requires administrator approval";
			return false;
		}

		m_pImplementation->m_strName = strAdapterName;
		m_pImplementation->m_strSubnet = strSubnet;
		m_pImplementation->m_strCleanupError.clear();

		if (!m_pImplementation->AcquireMutex(strErrorOut)) {
			return false;
		}

		try {
			bool bRecorded = false;
			std::string strOldSubnet;
			std::array<ForwardingState, 2> arrOldForwarding {};
			if (!ReadRecoveryRecord(strAdapterName, strOldSubnet, arrOldForwarding, bRecorded, strErrorOut) || bRecorded) {
				if (strErrorOut.empty()) {
					strErrorOut = "An owned Internet gateway recovery record already exists; run down with this tunnel name before starting";
				}

				Stop();
				return false;
			}

			if (!RunNAT(NATAction::Probe, strAdapterName, strSubnet, strErrorOut) ||
				!CaptureGatewayInterfaces(strAdapterName, strSubnet, m_pImplementation->m_arrForwarding, strErrorOut) ||
				!EnableTunnelForwarding(strAdapterName, m_pImplementation->m_arrForwarding[0], strErrorOut)) {
				Stop();

				if (!m_pImplementation->m_strCleanupError.empty()) {
					strErrorOut += " Cleanup: " + m_pImplementation->m_strCleanupError;
				}

				return false;
			}

			if (!RunNAT(NATAction::Create, strAdapterName, strSubnet, strErrorOut)) {
				Stop();

				if (!m_pImplementation->m_strCleanupError.empty()) {
					strErrorOut += " Cleanup: " + m_pImplementation->m_strCleanupError;
				}

				return false;
			}

			m_pImplementation->m_bNATOwned = true;

			if (!WriteRecoveryRecord(strAdapterName, strSubnet, m_pImplementation->m_arrForwarding, m_pImplementation->m_bRecordOwned, strErrorOut)) {
				Stop();

				if (!m_pImplementation->m_strCleanupError.empty()) {
					strErrorOut += " Cleanup: " + m_pImplementation->m_strCleanupError;
				}

				return false;
			}

			m_pImplementation->m_bActive = true;

			return true;
		} catch (...) {
			Stop();
			throw;
		}
	}

	void InternetServerGateway::Stop() noexcept {
		try {
			m_pImplementation->m_strCleanupError.clear();
			std::string strError;

			if (m_pImplementation->m_bNATOwned) {
				if (RunNAT(NATAction::Remove, m_pImplementation->m_strName, m_pImplementation->m_strSubnet, strError)) {
					m_pImplementation->m_bNATOwned = false;
				} else {
					m_pImplementation->m_strCleanupError = strError;
				}
			}

			for (auto itForwarding = m_pImplementation->m_arrForwarding.rbegin(); itForwarding != m_pImplementation->m_arrForwarding.rend(); ++itForwarding) {
				strError.clear();
				if (!RestoreForwarding(*itForwarding, strError)) {
					if (!m_pImplementation->m_strCleanupError.empty()) {
						m_pImplementation->m_strCleanupError += "; ";
					}

					m_pImplementation->m_strCleanupError += strError;
				}
			}

			m_pImplementation->m_bActive = false;

			if (!m_pImplementation->m_bNATOwned && !m_pImplementation->m_arrForwarding[0].m_bChanged && !m_pImplementation->m_arrForwarding[1].m_bChanged && m_pImplementation->m_bRecordOwned) {
				strError.clear();
				if (RemoveRecoveryRecord(m_pImplementation->m_strName, m_pImplementation->m_strSubnet, strError)) {
					m_pImplementation->m_bRecordOwned = false;
				} else {
					if (!m_pImplementation->m_strCleanupError.empty()) {
						m_pImplementation->m_strCleanupError += "; ";
					}

					m_pImplementation->m_strCleanupError += strError;
				}
			}

			if (!m_pImplementation->m_bNATOwned && !m_pImplementation->m_bRecordOwned && !m_pImplementation->m_arrForwarding[0].m_bChanged && !m_pImplementation->m_arrForwarding[1].m_bChanged) {
				m_pImplementation->CloseMutex();
				m_pImplementation->m_bCleanupFailed = false;
			}
		} catch (...) {
			m_pImplementation->m_bCleanupFailed = true;

			try {
				m_pImplementation->m_strCleanupError = "Unexpected failure during owned Internet gateway cleanup";
			} catch (...) {
				// The scalar failure marker still prevents an empty diagnostic from meaning success.
			}
		}
	}

	std::string InternetServerGateway::CleanupError() const {
		if (m_pImplementation->m_bCleanupFailed && m_pImplementation->m_strCleanupError.empty()) {
			// The return type owns its copy and may allocate; failure must never look like success.
			return "Unexpected failure during owned Internet gateway cleanup";
		}

		return m_pImplementation->m_strCleanupError;
	}

	bool InternetServerGateway::HandOffRecovery(std::string& strErrorOut) {
		strErrorOut.clear();

		if (!IsElevated()) {
			strErrorOut = "Internet gateway recovery handoff requires administrator privileges";
			return false;
		}

		if (m_pImplementation->m_bActive || (m_pImplementation->m_strCleanupError.empty() && !m_pImplementation->m_bCleanupFailed) || !m_pImplementation->m_bRecordOwned || !m_pImplementation->m_bMutexOwned) {
			strErrorOut = "Internet gateway recovery handoff requires a failed cleanup and its owned durable record";
			return false;
		}

		bool bExists = false;
		std::string strSubnet;
		std::array<ForwardingState, 2> arrStates {};
		if (!ReadRecoveryRecord(m_pImplementation->m_strName, strSubnet, arrStates, bExists, strErrorOut)) {
			return false;
		}

		if (!bExists || !CanHandOffRecovery(m_pImplementation->m_strName, m_pImplementation->m_strSubnet, m_pImplementation->m_arrForwarding, m_pImplementation->m_strName, strSubnet, arrStates)) {
			strErrorOut = "The complete owned Internet gateway recovery record is unavailable or does not cover every remaining change";
			return false;
		}

		// A fresh elevated worker can now take the lock and retry this exact
		// record. Leave its NAT, record, adapter, and forwarding state untouched.
		m_pImplementation->m_bNATOwned = false;
		m_pImplementation->m_bRecordOwned = false;
		for (auto& State : m_pImplementation->m_arrForwarding) {
			State.m_bChanged = false;
		}

		m_pImplementation->CloseMutex();

		return true;
	}

	bool InternetServerGateway::HasRecovery(std::string const& strAdapterName) {
		if (!IsValidAdapterName(strAdapterName)) {
			return false;
		}

		std::wstring const wstrPath = L"SOFTWARE\\WireSteam\\InternetGateways\\" + WidenASCII(strAdapterName);
		HKEY hKey = nullptr;
		const LSTATUS nResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, wstrPath.c_str(), 0, KEY_QUERY_VALUE, &hKey);
		RegistryKey KeyOwner(hKey);

		return (nResult != ERROR_FILE_NOT_FOUND) && (nResult != ERROR_PATH_NOT_FOUND);
	}

	bool InternetServerGateway::Recover(std::string const& strAdapterName, std::string& strErrorOut) {
		strErrorOut.clear();

		if (!IsValidAdapterName(strAdapterName)) {
			strErrorOut = "Internet gateway recovery requires a valid WireSteam adapter name";
			return false;
		}

		if (!IsElevated()) {
			strErrorOut = "Internet gateway recovery requires administrator approval";
			return false;
		}

		bool bExists = false;
		std::string strSubnet;
		std::array<ForwardingState, 2> arrForwarding {};
		if (!ReadRecoveryRecord(strAdapterName, strSubnet, arrForwarding, bExists, strErrorOut)) {
			return false;
		}

		if (!bExists) {
			return true;
		}

		InternetServerGateway Recovery;
		if (!Recovery.m_pImplementation->AcquireMutex(strErrorOut)) {
			return false;
		}

		// Re-read after the global lock: the previous live owner may have finished
		// cleanup between our first inspection and lock acquisition. Never load a
		// partial/malformed record into the RAII owner's mutable state.
		if (!ReadRecoveryRecord(strAdapterName, strSubnet, arrForwarding, bExists, strErrorOut)) {
			return false;
		}

		if (!bExists) {
			return true;
		}

		Recovery.m_pImplementation->m_strName = strAdapterName;
		Recovery.m_pImplementation->m_strSubnet = strSubnet;
		Recovery.m_pImplementation->m_arrForwarding = arrForwarding;
		Recovery.m_pImplementation->m_bRecordOwned = true;
		Recovery.m_pImplementation->m_bNATOwned = true;

		Recovery.Stop();
		strErrorOut = Recovery.CleanupError();

		return strErrorOut.empty();
	}

	void RunInternetServerTests() {
		constexpr std::uint32_t kNetworkAddress = 0x0A4D0000;
		constexpr std::uint32_t kNetworkMask = 0xFFFFFF00;
		constexpr ULONG kLANAddress = 0xC0A80164;
		constexpr ULONG kLoopbackAddress = 0x7F000001;
		constexpr ULONG kLinkLocalAddress = 0xA9FE0101;
		constexpr ULONG kMulticastAddress = 0xE0000001;
		constexpr WORD kOversizedACLBytes = std::numeric_limits<WORD>::max();
		constexpr WORD kTruncatedACEBytes = 0;
		constexpr BYTE kInvalidSIDSubAuthorities = SID_MAX_SUB_AUTHORITIES + 1;

		for (auto const& pszName : { "WireSteam-host", "WireSteam-ui-30c76bbda69e", "WireSteam-A_1-" }) {
			ExpectCondition(IsValidAdapterName(pszName), "valid adapter/NAT name rejected");
		}

		for (auto const& pszName : { "", "host", "WireSteam-", "WireSteam-a'b", "WireSteam-a;b", "WireSteam-a*b", "WireSteam-a/b", "WireSteam-a b", "WireSteam-abcdefghijklmnopqrstuvwxyz" }) {
			ExpectCondition(!IsValidAdapterName(pszName), "unsafe adapter/NAT name accepted");
		}

		for (auto const& pszSubnet : { "10.77.0.0/24", "10.0.0.0/8", "172.16.0.0/12", "192.168.1.0/30" }) {
			ExpectCondition(IsValidSubnet(pszSubnet), "valid private NAT subnet rejected");
		}

		for (auto const& pszSubnet : { "", "0.0.0.0/0", "8.8.8.0/24", "10.0.0.0/7", "10.77.0.1/24", "10.77.0.0/31", "10.77.0.0/032", "010.77.0.0/24", "172.16.0.0/11", "192.168.0.0/15", "fd77::/64", "10.77.0.0/24;calc", "10.77.0.0/24'" }) {
			ExpectCondition(!IsValidSubnet(pszSubnet), "unsafe or noncanonical NAT subnet accepted");
		}

		std::uint32_t unNetwork = 0;
		std::uint32_t unMask = 0;
		ExpectCondition(IsValidSubnet("10.77.0.0/24", &unNetwork, &unMask) && (unNetwork == kNetworkAddress) && (unMask == kNetworkMask), "NAT subnet host-order address and mask parsing");
		ExpectCondition(!IsValidSubnet("10.77.0.1/24", &unNetwork, &unMask) && (unNetwork == kNetworkAddress) && (unMask == kNetworkMask), "invalid subnet modified committed address or mask outputs");
		ExpectCondition(IsUsableUplinkSource(htonl(kLANAddress)) && !IsUsableUplinkSource(0x00000000) && !IsUsableUplinkSource(htonl(kLoopbackAddress)) && !IsUsableUplinkSource(htonl(kLinkLocalAddress)) && !IsUsableUplinkSource(htonl(kMulticastAddress)), "uplink source usability validation");
		ExpectCondition((EncodeUTF16LEBase64(L"A") == "QQA=") && (EncodeUTF16LEBase64(L"ABC") == "QQBCAEMA"), "PowerShell UTF-16LE encoding");
		ExpectCondition(EncodeUTF16LEBase64(L"").empty() && (EncodeUTF16LEBase64(L"\U0001F600") == "PdgA3g=="), "PowerShell UTF-16LE empty and surrogate-pair encoding");

		std::filesystem::path const Executable = std::filesystem::path(L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
		std::wstring const wstrProbe = BuildNATScript(NATAction::Probe, "WireSteam-host", "10.77.0.0/24", Executable);
		std::wstring const wstrCreate = BuildNATScript(NATAction::Create, "WireSteam-host", "10.77.0.0/24", Executable);
		std::wstring const wstrRemove = BuildNATScript(NATAction::Remove, "WireSteam-host", "10.77.0.0/24", Executable);
		ExpectCondition((wstrProbe.find(L"New-NetNat") == std::wstring::npos) && (wstrProbe.find(L"Remove-NetNat") == std::wstring::npos), "NAT capability probe contains a mutation");
		ExpectCondition((wstrCreate.find(L"$allNATs.Count -ne 0") < wstrCreate.find(L"NetNat\\New-NetNat")) && (wstrCreate.find(L"Remove-NetNat") == std::wstring::npos), "NAT create must refuse every pre-existing NAT without removing any");
		ExpectCondition((wstrRemove.find(L"$_.Name -ceq $natName") != std::wstring::npos) && (wstrRemove.find(L"InternalIPInterfaceAddressPrefix -cne $subnetPrefix") < wstrRemove.find(L"NetNat\\Remove-NetNat -InputObject $ownedNATs[0]")), "NAT cleanup lacks exact name, prefix, and input-object ownership restrictions");
		ExpectCondition((wstrCreate.find(L"$PSModuleAutoLoadingPreference = 'None'") != std::wstring::npos) && (wstrCreate.find(L"\\Modules\\NetNat\\NetNat.psd1'") != std::wstring::npos), "elevated NAT script must load only the protected OS module");

		bool bRejected = false;
		try {
			BuildNATScript(NATAction::Create, "WireSteam-host';calc", "10.77.0.0/24", Executable);
		} catch (std::invalid_argument const&) {
			bRejected = true;
		}

		ExpectCondition(bRejected, "command builder accepted unsafe input");

		auto DescriptorTrusted = [](wchar_t const* pszSDDL) -> bool {
			PSECURITY_DESCRIPTOR pDescriptor = nullptr;
			ULONG unDescriptorBytes = 0;
			const BOOL nConverted = ConvertStringSecurityDescriptorToSecurityDescriptorW(pszSDDL, SDDL_REVISION_1, &pDescriptor, &unDescriptorBytes);
			std::unique_ptr<void, LocalMemoryDeleter> pDescriptorOwner(pDescriptor);
			ExpectCondition(nConverted != FALSE, "test security descriptor parsing failed");
			std::string strError;

			return IsTrustedRecoveryDescriptor(pDescriptor, unDescriptorBytes, strError);
		};
		ExpectCondition(DescriptorTrusted(L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)"), "protected admin/SYSTEM recovery descriptor rejected");
		ExpectCondition(!DescriptorTrusted(L"O:BAG:BAD:(A;;KA;;;SY)(A;;KA;;;BA)"), "unprotected recovery descriptor accepted");
		ExpectCondition(!DescriptorTrusted(L"O:BUG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)"), "user-owned recovery descriptor accepted");
		ExpectCondition(!DescriptorTrusted(L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;WD)"), "world-writable recovery descriptor accepted");

		PSECURITY_DESCRIPTOR pRecoveryDescriptor = nullptr;
		ULONG unRecoveryDescriptorBytes = 0;
		const BOOL nRecoveryDescriptorConverted = ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)", SDDL_REVISION_1, &pRecoveryDescriptor, &unRecoveryDescriptorBytes);
		std::unique_ptr<void, LocalMemoryDeleter> pRecoveryDescriptorOwner(pRecoveryDescriptor);
		ExpectCondition(nRecoveryDescriptorConverted != FALSE, "bounded test security descriptor parsing failed");
		std::string strDescriptorError;
		ExpectCondition(!IsTrustedRecoveryDescriptor(nullptr, 0, strDescriptorError) && !IsTrustedRecoveryDescriptor(pRecoveryDescriptor, sizeof(SECURITY_DESCRIPTOR_RELATIVE) - 1, strDescriptorError), "missing or truncated recovery descriptor accepted");

		SECURITY_DESCRIPTOR_RELATIVE DescriptorHeader {};
		ExpectCondition(unRecoveryDescriptorBytes >= sizeof(DescriptorHeader), "test descriptor header exceeds its allocation");
		std::memcpy(&DescriptorHeader, pRecoveryDescriptor, sizeof(DescriptorHeader));
		std::vector<BYTE> vecMalformedDescriptor(unRecoveryDescriptorBytes);
		const std::size_t unOwnerOffset = DescriptorHeader.Owner;
		const std::size_t unDACLOffset = DescriptorHeader.Dacl;
		ExpectCondition((unOwnerOffset <= vecMalformedDescriptor.size()) && (offsetof(SID, SubAuthorityCount) < (vecMalformedDescriptor.size() - unOwnerOffset)) && (unDACLOffset <= vecMalformedDescriptor.size()) && ((sizeof(ACL) + sizeof(ACE_HEADER)) <= (vecMalformedDescriptor.size() - unDACLOffset)), "test descriptor offsets exceed their allocation");
		auto RejectMalformedDescriptor = [&strDescriptorError](std::vector<BYTE>& vecDescriptor) -> void {
			ExpectCondition(!IsTrustedRecoveryDescriptor(vecDescriptor.data(), vecDescriptor.size(), strDescriptorError), "out-of-bounds recovery descriptor component accepted");
		};

		std::memcpy(vecMalformedDescriptor.data(), pRecoveryDescriptor, unRecoveryDescriptorBytes);
		SECURITY_DESCRIPTOR_RELATIVE InvalidHeader = DescriptorHeader;
		InvalidHeader.Owner = std::numeric_limits<DWORD>::max();
		std::memcpy(vecMalformedDescriptor.data(), &InvalidHeader, sizeof(InvalidHeader));
		RejectMalformedDescriptor(vecMalformedDescriptor);

		InvalidHeader = DescriptorHeader;
		InvalidHeader.Dacl = std::numeric_limits<DWORD>::max();
		std::memcpy(vecMalformedDescriptor.data(), &InvalidHeader, sizeof(InvalidHeader));
		RejectMalformedDescriptor(vecMalformedDescriptor);

		std::memcpy(vecMalformedDescriptor.data(), pRecoveryDescriptor, unRecoveryDescriptorBytes);
		vecMalformedDescriptor[unOwnerOffset + offsetof(SID, SubAuthorityCount)] = kInvalidSIDSubAuthorities;
		RejectMalformedDescriptor(vecMalformedDescriptor);

		std::memcpy(vecMalformedDescriptor.data(), pRecoveryDescriptor, unRecoveryDescriptorBytes);
		std::memcpy(vecMalformedDescriptor.data() + unDACLOffset + offsetof(ACL, AclSize), &kOversizedACLBytes, sizeof(kOversizedACLBytes));
		RejectMalformedDescriptor(vecMalformedDescriptor);

		std::memcpy(vecMalformedDescriptor.data(), pRecoveryDescriptor, unRecoveryDescriptorBytes);
		std::memcpy(vecMalformedDescriptor.data() + unDACLOffset + sizeof(ACL) + offsetof(ACE_HEADER, AceSize), &kTruncatedACEBytes, sizeof(kTruncatedACEBytes));
		RejectMalformedDescriptor(vecMalformedDescriptor);

		std::array<ForwardingState, 2> arrRecoveryStates {};
		ExpectCondition(!arrRecoveryStates[0].m_LUID.Value && !arrRecoveryStates[1].m_LUID.Value && !arrRecoveryStates[0].m_bChanged && !arrRecoveryStates[1].m_bChanged, "empty forwarding state owns recovery changes");
		arrRecoveryStates[0].m_LUID.Value = 1;
		arrRecoveryStates[0].m_GUID.Data1 = 1;
		arrRecoveryStates[1].m_LUID.Value = 2;
		arrRecoveryStates[1].m_GUID.Data1 = 2;
		MIB_IF_ROW2 OwnedInterface {};
		OwnedInterface.InterfaceLuid = arrRecoveryStates[0].m_LUID;
		OwnedInterface.InterfaceGuid = arrRecoveryStates[0].m_GUID;
		OwnedInterface.OperStatus = IfOperStatusUp;
		OwnedInterface.Type = IF_TYPE_PROP_VIRTUAL;
		wcscpy_s(OwnedInterface.Alias, L"WireSteam-host");
		ExpectCondition(IsOwnedForwardingInterface("WireSteam-host", arrRecoveryStates[0], OwnedInterface), "owned tunnel forwarding interface rejected");

		MIB_IF_ROW2 ForeignInterface = OwnedInterface;
		wcscpy_s(ForeignInterface.Alias, L"warp-teams");
		ExpectCondition(!IsOwnedForwardingInterface("WireSteam-host", arrRecoveryStates[0], ForeignInterface), "an existing VPN uplink was authorized for a forwarding update");
		wcscpy_s(ForeignInterface.Alias, L"WireSteam-other");
		ExpectCondition(!IsOwnedForwardingInterface("WireSteam-host", arrRecoveryStates[0], ForeignInterface), "another WireSteam session was authorized for a forwarding update");
		ForeignInterface = OwnedInterface;
		ForeignInterface.InterfaceGuid.Data1 = 99;
		ExpectCondition(!IsOwnedForwardingInterface("WireSteam-host", arrRecoveryStates[0], ForeignInterface), "a reused interface LUID was authorized for a forwarding update");
		ForeignInterface = OwnedInterface;
		ForeignInterface.InterfaceLuid = arrRecoveryStates[1].m_LUID;
		ExpectCondition(!IsOwnedForwardingInterface("WireSteam-host", arrRecoveryStates[0], ForeignInterface) && !IsOwnedForwardingInterface("WireSteam-host", arrRecoveryStates[1], OwnedInterface), "the selected uplink was authorized as the owned forwarding target");
		ForeignInterface = OwnedInterface;
		ForeignInterface.OperStatus = IfOperStatusDown;
		ExpectCondition(!IsOwnedForwardingInterface("WireSteam-host", arrRecoveryStates[0], ForeignInterface), "a stopped tunnel was authorized for a forwarding update");
		ExpectCondition(IsValidRecoveryPayload(1, "WireSteam-host", "WireSteam-host", "10.77.0.0/24", arrRecoveryStates), "valid recovery schema rejected");
		ExpectCondition(!IsValidRecoveryPayload(2, "WireSteam-host", "WireSteam-host", "10.77.0.0/24", arrRecoveryStates) && !IsValidRecoveryPayload(1, "WireSteam-host", "WireSteam-other", "10.77.0.0/24", arrRecoveryStates) && !IsValidRecoveryPayload(1, "WireSteam-host", "WireSteam-host", "0.0.0.0/0", arrRecoveryStates), "untrusted recovery metadata accepted");
		arrRecoveryStates[1].m_GUID = arrRecoveryStates[0].m_GUID;
		ExpectCondition(!IsValidRecoveryPayload(1, "WireSteam-host", "WireSteam-host", "10.77.0.0/24", arrRecoveryStates), "duplicate recovery interface identity accepted");
		arrRecoveryStates[1].m_GUID.Data1 = 2;
		arrRecoveryStates[0].m_bChanged = true;
		std::array<ForwardingState, 2> const arrRecordedStates = arrRecoveryStates;
		ExpectCondition(CanHandOffRecovery("WireSteam-host", "10.77.0.0/24", arrRecoveryStates, "WireSteam-host", "10.77.0.0/24", arrRecordedStates), "complete durable recovery handoff rejected");
		std::array<ForwardingState, 2> arrIncompleteStates = arrRecordedStates;
		arrIncompleteStates[0].m_bChanged = false;
		ExpectCondition(!CanHandOffRecovery("WireSteam-host", "10.77.0.0/24", arrRecoveryStates, "WireSteam-host", "10.77.0.0/24", arrIncompleteStates), "handoff accepted an uncovered forwarding change");
		arrIncompleteStates = arrRecordedStates;
		arrIncompleteStates[0].m_LUID.Value = 99;
		ExpectCondition(!CanHandOffRecovery("WireSteam-host", "10.77.0.0/24", arrRecoveryStates, "WireSteam-host", "10.77.0.0/24", arrIncompleteStates) && !CanHandOffRecovery("WireSteam-host", "10.77.0.0/24", arrRecoveryStates, "WireSteam-host", "10.78.0.0/24", arrRecordedStates), "handoff accepted a different interface identity or prefix");

		InternetServerGateway UnusedGateway;
		UnusedGateway.Stop();
		UnusedGateway.Stop();
		ExpectCondition(UnusedGateway.CleanupError().empty(), "stopping an unused gateway failed");
		std::string strStartError;
		ExpectCondition(!UnusedGateway.Start("unsafe", "10.77.0.0/24", strStartError) && !strStartError.empty(), "gateway setup accepted an unsafe adapter name");
		UnusedGateway.Stop();
		ExpectCondition(UnusedGateway.CleanupError().empty(), "invalid gateway setup created a cleanup obligation");
	}
} // namespace ws

#ifdef WIRESTEAM_SERVER_OWNS_NOMINMAX
#undef NOMINMAX
#undef WIRESTEAM_SERVER_OWNS_NOMINMAX
#endif
#ifdef WIRESTEAM_SERVER_OWNS_LEAN_WINDOWS
#undef WIN32_LEAN_AND_MEAN
#undef WIRESTEAM_SERVER_OWNS_LEAN_WINDOWS
#endif
