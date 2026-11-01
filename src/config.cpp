#include "config.hpp"

// General
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

// C++
#include <charconv>
#include <cstring>
#include <utility>

// STL
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace ws {
	namespace {
		constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		constexpr std::size_t kMaximumConfigurationBytes = 1024 * 1024;
		constexpr std::size_t kMaximumPeers = 128;
		constexpr std::size_t kMaximumPortForwards = 32;
		constexpr std::size_t kIPv6AddressBytes = 0x10;

		struct Field {
			Field(std::string const& strValue, std::size_t unLine) :
				m_strValue(strValue),
				m_unLine(unLine)
			{
			}

			std::string m_strValue;
			std::size_t m_unLine;
		};

		struct Section {
			Section(std::string const& strName, std::size_t unLine) :
				m_strName(strName),
				m_unLine(unLine)
			{
			}

			std::string m_strName;
			std::size_t m_unLine;
			std::map<std::string, Field> m_mapFields;
		};

		void WipeSensitive(std::string& strValue) noexcept {
			if (!strValue.empty()) {
				SecureZeroMemory(strValue.data(), strValue.size());
			}
		}

		void WipeSensitive(Key& KeyValue) noexcept {
			SecureZeroMemory(KeyValue.data(), KeyValue.size());
		}

		void WipeSensitive(std::stringbuf& Buffer) noexcept {
			// These guards borrow non-const owned streams and query their current buffer only at cleanup.
			// This wipes live logical contents, not allocations already released by earlier STL growth.
			std::string_view const strvContents = Buffer.view();
			if (!strvContents.empty()) {
				SecureZeroMemory(const_cast<char*>(strvContents.data()), strvContents.size());
			}
		}

		void WipeSensitive(Config& Configuration) noexcept {
			WipeSensitive(Configuration.m_strPrivateKey);
			for (auto& Peer : Configuration.m_vecPeers) {
				WipeSensitive(Peer.m_strPresharedKey);
			}
		}

		void WipeSensitive(std::vector<Section>& vecSections) noexcept {
			for (auto& SectionValue : vecSections) {
				for (auto& FieldEntry : SectionValue.m_mapFields) {
					WipeSensitive(FieldEntry.second.m_strValue);
				}
			}
		}

		template <class TValue>
		class SensitiveCleanup {
		public:
			explicit SensitiveCleanup(TValue& Value) noexcept :
				m_Value(Value)
			{
			}

			~SensitiveCleanup() noexcept {
				WipeSensitive(m_Value);
			}

			SensitiveCleanup(SensitiveCleanup const&) = delete;
			SensitiveCleanup& operator=(SensitiveCleanup const&) = delete;
			SensitiveCleanup(SensitiveCleanup&&) = delete;
			SensitiveCleanup& operator=(SensitiveCleanup&&) = delete;

		private:
			TValue& m_Value;
		};

		std::string Trim(std::string_view strvValue) {
			const std::size_t unFirst = strvValue.find_first_not_of(" \t\r\n");
			if (unFirst == std::string_view::npos) {
				return {};
			}

			return std::string(strvValue.substr(unFirst, strvValue.find_last_not_of(" \t\r\n") - unFirst + 1));
		}

		bool Fail(std::string& strErrorOut, std::size_t unLine, std::string const& strMessage) {
			strErrorOut = (unLine ? "Line " + std::to_string(unLine) + ": " : "") + strMessage;

			return false;
		}

		bool ReadSections(std::string const& strText, bool bCompiled, std::vector<Section>& vecSectionsOut, std::string& strErrorOut) {
			constexpr std::size_t kMaximumLineBytes = 4096;
			constexpr std::size_t kUTF8BOMBytes = 0x03;

			if (strText.empty()) {
				return Fail(strErrorOut, 0, "The configuration is empty.");
			}

			if (strText.size() > kMaximumConfigurationBytes) {
				return Fail(strErrorOut, 0, "The configuration exceeds 1 MiB.");
			}

			if (strText.find('\0') != std::string::npos) {
				return Fail(strErrorOut, 0, "NUL bytes are not allowed in configuration files.");
			}

			std::istringstream InputStream(strText);
			SensitiveCleanup<std::stringbuf> InputCleanup(*InputStream.rdbuf());

			std::string strLine;
			SensitiveCleanup<std::string> LineCleanup(strLine);
			// The validated input size bounds every line, so secret text never grows this buffer.
			strLine.reserve(strText.size());
			std::size_t unLineNumber = 0;
			bool bInterfaceSeen = false;
			bool bSteamSeen = false;
			std::size_t unPeerCount = 0;
			std::size_t unForwardCount = 0;
			for (;;) {
				WipeSensitive(strLine);

				if (!std::getline(InputStream, strLine)) {
					break;
				}

				++unLineNumber;

				if (strLine.size() > kMaximumLineBytes) {
					return Fail(strErrorOut, unLineNumber, "A configuration line exceeds 4096 bytes.");
				}

				std::string_view strvLine = strLine;
				if ((unLineNumber == 1) && strvLine.starts_with("\xEF\xBB\xBF")) {
					strvLine.remove_prefix(kUTF8BOMBytes);
				}

				std::string strPreparedLine = Trim(strvLine.substr(0, strvLine.find_first_of("#;")));
				SensitiveCleanup<std::string> PreparedLineCleanup(strPreparedLine);
				if (strPreparedLine.empty()) {
					continue;
				}

				if (strPreparedLine.front() == '[') {
					if ((strPreparedLine.back() != ']') || (strPreparedLine.find(']', 1) != (strPreparedLine.size() - 1))) {
						return Fail(strErrorOut, unLineNumber, "Malformed section header.");
					}

					std::string strName = Trim(std::string_view(strPreparedLine).substr(1, strPreparedLine.size() - 2));
					SensitiveCleanup<std::string> NameCleanup(strName);
					if (strName == "Interface") {
						if (bInterfaceSeen) {
							return Fail(strErrorOut, unLineNumber, "Only one [Interface] section is allowed.");
						}

						bInterfaceSeen = true;
					} else if ((strName == "WireSteam") && !bCompiled) {
						if (bSteamSeen) {
							return Fail(strErrorOut, unLineNumber, "Only one [WireSteam] section is allowed.");
						}

						bSteamSeen = true;
					} else if (strName == "Peer") {
						if (++unPeerCount > kMaximumPeers) {
							return Fail(strErrorOut, unLineNumber, "At most 128 peers are supported.");
						}
					} else if ((strName == "PortForward") && !bCompiled) {
						if (++unForwardCount > kMaximumPortForwards) {
							return Fail(strErrorOut, unLineNumber, "At most 32 [PortForward] rules are supported.");
						}
					} else {
						return Fail(strErrorOut, unLineNumber, "Unknown or unsupported section [" + strName + "].");
					}

					vecSectionsOut.push_back(Section(strName, unLineNumber));
					continue;
				}

				if (vecSectionsOut.empty()) {
					return Fail(strErrorOut, unLineNumber, "A setting must appear inside a section.");
				}

				const std::size_t unEqualsOffset = strPreparedLine.find('=');
				if (unEqualsOffset == std::string::npos) {
					return Fail(strErrorOut, unLineNumber, "Expected Key = Value.");
				}

				std::string strKey = Trim(std::string_view(strPreparedLine).substr(0, unEqualsOffset));
				SensitiveCleanup<std::string> KeyCleanup(strKey);
				std::string strValue = Trim(std::string_view(strPreparedLine).substr(unEqualsOffset + 1));
				SensitiveCleanup<std::string> ValueCleanup(strValue);
				if (strKey.empty() || strValue.empty()) {
					return Fail(strErrorOut, unLineNumber, "A setting name and value must both be present.");
				}

				Section& SectionValue = vecSectionsOut.back();
				std::set<std::string> const setInterfaceKeys { "PrivateKey", "Address", "ListenPort", "DNS", "MTU" };
				std::set<std::string> const setSteamKeys { "AppID", "VirtualPort", "RelayOnly", "ReconnectSeconds", "SendRate", "StatsSeconds", "InternetMode", "InternetSubnet" };
				std::set<std::string> const setForwardKeys { "Protocol", "Listen", "Target", "MaxConnections", "ConnectTimeoutSeconds", "IdleTimeoutSeconds" };
				std::set<std::string> const setPeerKeys = bCompiled
					? std::set<std::string> { "PublicKey", "PresharedKey", "AllowedIPs", "Endpoint", "PersistentKeepalive" }
					: std::set<std::string> { "PublicKey", "PresharedKey", "AllowedIPs", "SteamID", "SteamPort", "BridgePort", "PersistentKeepalive", "Dial" };
				std::set<std::string> const& setAllowed = (SectionValue.m_strName == "Interface") ? setInterfaceKeys : (SectionValue.m_strName == "WireSteam") ? setSteamKeys : (SectionValue.m_strName == "PortForward") ? setForwardKeys : setPeerKeys;
				if (!setAllowed.contains(strKey)) {
					if (!bCompiled && (SectionValue.m_strName == "Peer") && (strKey == "Endpoint")) {
						return Fail(strErrorOut, unLineNumber, "Use SteamID instead of Endpoint in a WireSteam configuration.");
					}

					return Fail(strErrorOut, unLineNumber, "Unsupported setting '" + strKey + "' in [" + SectionValue.m_strName + "].");
				}

				if (!SectionValue.m_mapFields.try_emplace(strKey, strValue, unLineNumber).second) {
					return Fail(strErrorOut, unLineNumber, "Duplicate setting '" + strKey + "'.");
				}
			}

			if (!bInterfaceSeen) {
				return Fail(strErrorOut, 0, "An [Interface] section is required.");
			}

			if (!unPeerCount) {
				return Fail(strErrorOut, 0, "At least one [Peer] section is required.");
			}

			return true;
		}

		template <class T>
		bool ReadUnsignedNumber(std::string const& strValue, T& unResultOut) {
			static_assert(std::is_unsigned_v<T>);

			if (strValue.empty()) {
				return false;
			}

			T unNumber = 0;
			std::from_chars_result const Parsed = std::from_chars(strValue.data(), strValue.data() + strValue.size(), unNumber);
			if ((Parsed.ec != std::errc {}) || (Parsed.ptr != (strValue.data() + strValue.size()))) {
				return false;
			}

			unResultOut = unNumber;

			return true;
		}

		bool ReadNumber(Section const& SectionValue, char const* pszKey, std::uint64_t unMinimum, std::uint64_t unMaximum, std::uint64_t& unResultOut, std::string& strErrorOut, bool bRequired = false) {
			auto itField = SectionValue.m_mapFields.find(pszKey);
			if (itField == SectionValue.m_mapFields.end()) {
				return !bRequired || Fail(strErrorOut, SectionValue.m_unLine, std::string(pszKey) + " is required.");
			}

			std::uint64_t unNumber = 0;
			if (!ReadUnsignedNumber(itField->second.m_strValue, unNumber) || (unNumber < unMinimum) || (unNumber > unMaximum)) {
				return Fail(strErrorOut, itField->second.m_unLine, std::string(pszKey) + " must be an integer from " + std::to_string(unMinimum) + " to " + std::to_string(unMaximum) + ".");
			}

			unResultOut = unNumber;

			return true;
		}

		bool ReadBoolean(Section const& SectionValue, char const* pszKey, bool& bResultOut, std::string& strErrorOut) {
			auto itField = SectionValue.m_mapFields.find(pszKey);
			if (itField == SectionValue.m_mapFields.end()) {
				return true;
			}

			if ((itField->second.m_strValue == "true") || (itField->second.m_strValue == "1")) {
				bResultOut = true;
			} else if ((itField->second.m_strValue == "false") || (itField->second.m_strValue == "0")) {
				bResultOut = false;
			} else {
				return Fail(strErrorOut, itField->second.m_unLine, std::string(pszKey) + " must be true or false.");
			}

			return true;
		}

		bool ReadKeyField(Section const& SectionValue, char const* pszName, std::string& strResultOut, std::string& strErrorOut, bool bRequired) {
			auto itField = SectionValue.m_mapFields.find(pszName);
			if (itField == SectionValue.m_mapFields.end()) {
				return !bRequired || Fail(strErrorOut, SectionValue.m_unLine, std::string(pszName) + " is required.");
			}

			Key KeyValue {};
			SensitiveCleanup<Key> KeyCleanup(KeyValue);
			if (!DecodeKey(itField->second.m_strValue, KeyValue) || std::all_of(KeyValue.begin(), KeyValue.end(), [](std::uint8_t unByte) -> bool {
				return unByte == 0x00;
			})) {
				return Fail(strErrorOut, itField->second.m_unLine, std::string(pszName) + " must be a nonzero 32-byte key in canonical Base64.");
			}

			strResultOut = itField->second.m_strValue;

			return true;
		}

		struct Network {
			Network() :
				m_arrBytes {}
			{
				m_nFamily = AF_INET;
				m_unPrefix = 0;
			}

			int m_nFamily;
			std::array<std::uint8_t, kIPv6AddressBytes> m_arrBytes;
			unsigned m_unPrefix;
		};

		bool IsPrivateNetwork(Network const& NetworkAddress) {
			constexpr std::uint8_t kPrivateIPv4ClassBMask = 0xF0;
			constexpr std::uint8_t kPrivateIPv6Mask = 0xFE;
			constexpr std::uint8_t kPrivateIPv6Prefix = 0xFC;
			constexpr unsigned kPrivateIPv4ClassAPrefixBits = 8;
			constexpr unsigned kPrivateIPv4ClassBPrefixBits = 12;
			constexpr unsigned kPrivateIPv4ClassCPrefixBits = 16;
			constexpr unsigned kPrivateIPv6PrefixBits = 7;

			if (NetworkAddress.m_nFamily == AF_INET) {
				return ((NetworkAddress.m_arrBytes[0x00] == 0x0A) && (NetworkAddress.m_unPrefix >= kPrivateIPv4ClassAPrefixBits)) || ((NetworkAddress.m_arrBytes[0x00] == 0xAC) && ((NetworkAddress.m_arrBytes[0x01] & kPrivateIPv4ClassBMask) == 0x10) && (NetworkAddress.m_unPrefix >= kPrivateIPv4ClassBPrefixBits)) || ((NetworkAddress.m_arrBytes[0x00] == 0xC0) && (NetworkAddress.m_arrBytes[0x01] == 0xA8) && (NetworkAddress.m_unPrefix >= kPrivateIPv4ClassCPrefixBits));
			}

			return ((NetworkAddress.m_arrBytes[0x00] & kPrivateIPv6Mask) == kPrivateIPv6Prefix) && (NetworkAddress.m_unPrefix >= kPrivateIPv6PrefixBits);
		}

		void MaskNetwork(Network& NetworkValue) {
			constexpr std::size_t kIPv4AddressBytes = 0x04;
			constexpr std::size_t kBitsPerByte = 8;
			constexpr int kByteMask = 0xFF;

			const std::size_t unByteCount = (NetworkValue.m_nFamily == AF_INET) ? kIPv4AddressBytes : kIPv6AddressBytes;
			for (std::size_t unByteIndex = 0; unByteIndex < unByteCount; ++unByteIndex) {
				const std::size_t unFirstBit = unByteIndex * kBitsPerByte;
				if (NetworkValue.m_unPrefix <= unFirstBit) {
					NetworkValue.m_arrBytes[unByteIndex] = 0x00;
				} else if (NetworkValue.m_unPrefix < (unFirstBit + kBitsPerByte)) {
					NetworkValue.m_arrBytes[unByteIndex] &= static_cast<std::uint8_t>(kByteMask << (kBitsPerByte - (NetworkValue.m_unPrefix - unFirstBit)));
				}
			}
		}

		std::string FormatNetworkAddress(Network const& NetworkValue, bool bWithPrefix) {
			char szAddress[INET6_ADDRSTRLEN] {};
			if (!InetNtopA(NetworkValue.m_nFamily, NetworkValue.m_arrBytes.data(), szAddress, sizeof(szAddress))) {
				throw std::runtime_error("Unable to format an IP address.");
			}

			return std::string(szAddress) + (bWithPrefix ? "/" + std::to_string(NetworkValue.m_unPrefix) : "");
		}

		bool ParseNetwork(std::string const& strText, bool bWithPrefix, bool bNormalize, Network& NetworkOut, bool bPrivateOnly = true) {
			constexpr unsigned kIPv4AddressBits = 32;
			constexpr unsigned kIPv6AddressBits = 128;

			const std::size_t unSlashOffset = strText.find('/');
			if ((unSlashOffset != std::string::npos) != bWithPrefix) {
				return false;
			}

			std::string const strAddress = strText.substr(0, unSlashOffset);
			Network NetworkValue;
			if (InetPtonA(AF_INET, strAddress.c_str(), NetworkValue.m_arrBytes.data()) == 1) {
				NetworkValue.m_nFamily = AF_INET;
			} else if (InetPtonA(AF_INET6, strAddress.c_str(), NetworkValue.m_arrBytes.data()) == 1) {
				NetworkValue.m_nFamily = AF_INET6;
			} else {
				return false;
			}

			const unsigned unAddressBits = (NetworkValue.m_nFamily == AF_INET) ? kIPv4AddressBits : kIPv6AddressBits;
			NetworkValue.m_unPrefix = unAddressBits;
			if (bWithPrefix && (!ReadUnsignedNumber(strText.substr(unSlashOffset + 1), NetworkValue.m_unPrefix) || (NetworkValue.m_unPrefix > unAddressBits))) {
				return false;
			}

			if (bPrivateOnly && !IsPrivateNetwork(NetworkValue)) {
				return false;
			}

			if (bNormalize) {
				MaskNetwork(NetworkValue);
			}

			NetworkOut = NetworkValue;

			return true;
		}

		bool ReadForwardEndpoint(Section const& SectionValue, char const* pszKey, bool bLoopback, std::string& strAddressOut, std::uint16_t& unPortOut, std::string& strErrorOut) {
			constexpr unsigned kMaximumPort = 65535;
			constexpr std::size_t kIPv6LoopbackLastByte = kIPv6AddressBytes - 1;

			auto itField = SectionValue.m_mapFields.find(pszKey);
			if (itField == SectionValue.m_mapFields.end()) {
				return Fail(strErrorOut, SectionValue.m_unLine, std::string(pszKey) + " is required in [PortForward].");
			}

			std::string const& strValue = itField->second.m_strValue;
			std::string strHost;
			std::string strService;
			if (!strValue.empty() && (strValue.front() == '[')) {
				const std::size_t unBracketOffset = strValue.find(']');
				if ((unBracketOffset == std::string::npos) || ((unBracketOffset + 1) >= strValue.size()) || (strValue[unBracketOffset + 1] != ':')) {
					return Fail(strErrorOut, itField->second.m_unLine, std::string(pszKey) + " must be IPv4:port or [IPv6]:port.");
				}

				strHost = strValue.substr(1, unBracketOffset - 1);
				strService = strValue.substr(unBracketOffset + 2);

				if (strHost.find(':') == std::string::npos) {
					return Fail(strErrorOut, itField->second.m_unLine, std::string(pszKey) + " uses brackets only for IPv6 addresses.");
				}
			} else {
				const std::size_t unSeparatorOffset = strValue.find(':');
				if ((unSeparatorOffset == std::string::npos) || (strValue.find(':', unSeparatorOffset + 1) != std::string::npos)) {
					return Fail(strErrorOut, itField->second.m_unLine, std::string(pszKey) + " must be IPv4:port or [IPv6]:port.");
				}

				strHost = strValue.substr(0, unSeparatorOffset);
				strService = strValue.substr(unSeparatorOffset + 1);
			}

			Network NetworkValue;
			unsigned unPortNumber = 0;
			if (!ParseNetwork(strHost, false, false, NetworkValue, false) ||
				!ReadUnsignedNumber(strService, unPortNumber) || !unPortNumber || (unPortNumber > kMaximumPort)) {
				return Fail(strErrorOut, itField->second.m_unLine, std::string(pszKey) + " requires a numeric IP address and port from 1 to 65535.");
			}

			if (bLoopback) {
				const bool bLocal = (NetworkValue.m_nFamily == AF_INET) ? NetworkValue.m_arrBytes[0x00] == 0x7F : (std::all_of(NetworkValue.m_arrBytes.begin(), NetworkValue.m_arrBytes.begin() + kIPv6LoopbackLastByte, [](std::uint8_t unByte) -> bool {
					return unByte == 0x00;
				}) && (NetworkValue.m_arrBytes[kIPv6LoopbackLastByte] == 0x01));
				if (!bLocal) {
					return Fail(strErrorOut, itField->second.m_unLine, "PortForward Target must be a local loopback address (127.x.x.x or [::1]).");
				}
			} else if (!IsPrivateNetwork(NetworkValue)) {
				return Fail(strErrorOut, itField->second.m_unLine, "PortForward Listen must use an exact private VPN Interface Address, not a wildcard or external address.");
			}

			strAddressOut = FormatNetworkAddress(NetworkValue, false);
			unPortOut = static_cast<std::uint16_t>(unPortNumber);

			return true;
		}

		bool ParsePortForward(Section const& SectionValue, PortForwardConfig& Rule, std::string& strErrorOut) {
			constexpr std::uint64_t kMaximumConnections = 256;
			constexpr std::uint64_t kMaximumConnectTimeoutSeconds = 300;
			constexpr std::uint64_t kMaximumIdleTimeoutSeconds = 86400;

			if (auto itProtocol = SectionValue.m_mapFields.find("Protocol"); itProtocol != SectionValue.m_mapFields.end()) {
				std::string strName = itProtocol->second.m_strValue;
				std::transform(strName.begin(), strName.end(), strName.begin(), [](char chCharacter) -> char {
					return ((chCharacter >= 'a') && (chCharacter <= 'z')) ? static_cast<char>(chCharacter - 'a' + 'A') : chCharacter;
				});

				if (strName == "TCP") {
					Rule.m_Protocol = PortForwardProtocol::Tcp;
				} else if (strName == "UDP") {
					Rule.m_Protocol = PortForwardProtocol::Udp;
				} else {
					return Fail(strErrorOut, itProtocol->second.m_unLine, "PortForward Protocol must be TCP or UDP.");
				}
			}

			std::uint64_t unMaximumConnections = Rule.m_unMaximumConnections;
			std::uint64_t unConnectTimeoutSeconds = Rule.m_unConnectTimeoutSeconds;
			std::uint64_t unIdleTimeoutSeconds = Rule.m_unIdleTimeoutSeconds;
			if (!ReadForwardEndpoint(SectionValue, "Listen", false, Rule.m_strListenAddress, Rule.m_unListenPort, strErrorOut) ||
				!ReadForwardEndpoint(SectionValue, "Target", true, Rule.m_strTargetAddress, Rule.m_unTargetPort, strErrorOut) ||
				!ReadNumber(SectionValue, "MaxConnections", 1, kMaximumConnections, unMaximumConnections, strErrorOut) ||
				!ReadNumber(SectionValue, "ConnectTimeoutSeconds", 1, kMaximumConnectTimeoutSeconds, unConnectTimeoutSeconds, strErrorOut) ||
				!ReadNumber(SectionValue, "IdleTimeoutSeconds", 0, kMaximumIdleTimeoutSeconds, unIdleTimeoutSeconds, strErrorOut)) {
				return false;
			}

			Rule.m_unMaximumConnections = static_cast<unsigned>(unMaximumConnections);
			Rule.m_unConnectTimeoutSeconds = static_cast<unsigned>(unConnectTimeoutSeconds);
			Rule.m_unIdleTimeoutSeconds = static_cast<unsigned>(unIdleTimeoutSeconds);

			return true;
		}

		bool ValidatePortForwards(Config const& Configuration, std::string& strErrorOut) {
			if (Configuration.m_vecPortForwards.size() > kMaximumPortForwards) {
				return Fail(strErrorOut, 0, "At most 32 [PortForward] rules are supported.");
			}

			if (!Configuration.m_vecPortForwards.empty() && (Configuration.m_InternetMode == InternetMode::Client)) {
				return Fail(strErrorOut, 0, "Port forwarding requires a private-network or Internet-gateway profile. Internet-client profiles reserve this process for the physical Steam transport.");
			}

			if (Configuration.m_unListenPort && PortForwardConflictsWithWireGuardPort(Configuration, Configuration.m_unListenPort)) {
				return Fail(strErrorOut, 0, "UDP PortForward Listen and Target ports must differ from the WireGuard ListenPort, which reserves that port on all addresses. Choose another port or set Interface ListenPort = 0.");
			}

			for (auto const& Peer : Configuration.m_vecPeers) {
				if (Peer.m_unBridgePort && PortForwardConflictsWithBridgePort(Configuration, Peer.m_unBridgePort)) {
					return Fail(strErrorOut, 0, "A UDP PortForward Target at 127.0.0.1 must not use a peer BridgePort. Choose another port or omit BridgePort for automatic allocation.");
				}
			}

			std::set<std::tuple<PortForwardProtocol, std::string, std::uint16_t>> setListeners;
			for (auto const& Rule : Configuration.m_vecPortForwards) {
				if ((Rule.m_Protocol != PortForwardProtocol::Tcp) && (Rule.m_Protocol != PortForwardProtocol::Udp)) {
					return Fail(strErrorOut, 0, "PortForward Protocol must be TCP or UDP.");
				}

				bool bLocal = false;
				Network Listener;
				if (!ParseNetwork(Rule.m_strListenAddress, false, false, Listener)) {
					return Fail(strErrorOut, 0, "Invalid PortForward Listen address.");
				}

				for (auto const& strAddress : Configuration.m_vecAddresses) {
					Network Configured;
					if (ParseNetwork(strAddress, true, false, Configured) && (Configured.m_nFamily == Listener.m_nFamily) && (Configured.m_arrBytes == Listener.m_arrBytes)) {
						bLocal = true;
						break;
					}
				}

				if (!bLocal) {
					return Fail(strErrorOut, 0, "PortForward Listen must exactly match a local Interface Address in this profile.");
				}

				if (!setListeners.emplace(Rule.m_Protocol, Rule.m_strListenAddress, Rule.m_unListenPort).second) {
					return Fail(strErrorOut, 0, "PortForward rules cannot share the same protocol, Listen address and port.");
				}
			}

			return true;
		}

		bool DoNetworksOverlap(Network Left, Network Right) {
			if (Left.m_nFamily != Right.m_nFamily) {
				return false;
			}

			Left.m_unPrefix = Right.m_unPrefix = std::min(Left.m_unPrefix, Right.m_unPrefix);
			MaskNetwork(Left);
			MaskNetwork(Right);

			return Left.m_arrBytes == Right.m_arrBytes;
		}

		bool DoesNetworkContain(Network Parent, Network Child) {
			if ((Parent.m_nFamily != Child.m_nFamily) || (Parent.m_unPrefix > Child.m_unPrefix)) {
				return false;
			}

			Child.m_unPrefix = Parent.m_unPrefix;
			MaskNetwork(Parent);
			MaskNetwork(Child);

			return Parent.m_arrBytes == Child.m_arrBytes;
		}

		bool IsUnicastIPv4DNSAddress(Network const& NetworkValue) {
			// IPv4 DNS addresses are numeric unicast destinations. Link-local and
			// loopback nameservers cannot be reached through the remote exit node.
			return (NetworkValue.m_nFamily == AF_INET) && (NetworkValue.m_arrBytes[0x00] != 0x00) && (NetworkValue.m_arrBytes[0x00] != 0x7F) &&
				(NetworkValue.m_arrBytes[0x00] < 0xE0) && !((NetworkValue.m_arrBytes[0x00] == 0xA9) && (NetworkValue.m_arrBytes[0x01] == 0xFE));
		}

		bool ReadAddressList(Section const& SectionValue, char const* pszKey, bool bWithPrefix, bool bNormalize, std::vector<std::string>& vecAddressesOut, std::vector<Network>* pNetworksOut, std::string& strErrorOut, bool bRequired, bool bInternetClient = false) {
			constexpr std::size_t kMaximumAddressEntries = 256;

			auto itField = SectionValue.m_mapFields.find(pszKey);
			if (itField == SectionValue.m_mapFields.end()) {
				return !bRequired || Fail(strErrorOut, SectionValue.m_unLine, std::string(pszKey) + " is required.");
			}

			std::set<std::string> setSeen;
			std::size_t unBegin = 0;
			while (unBegin <= itField->second.m_strValue.size()) {
				const std::size_t unCommaOffset = itField->second.m_strValue.find(',', unBegin);
				std::string const strItem = Trim(std::string_view(itField->second.m_strValue).substr(unBegin, (unCommaOffset == std::string::npos) ? std::string::npos : (unCommaOffset - unBegin)));
				Network NetworkValue;
				if (strItem.empty() || !ParseNetwork(strItem, bWithPrefix, bNormalize, NetworkValue, !bInternetClient)) {
					return Fail(strErrorOut, itField->second.m_unLine, std::string(pszKey) + " must contain private RFC1918 IPv4 or ULA IPv6 " + (bWithPrefix ? "CIDRs wholly inside their private ranges. Public and default routes are unsupported." : "IP addresses."));
				}

				if (bInternetClient && ((NetworkValue.m_nFamily != AF_INET) || (!bWithPrefix && !IsUnicastIPv4DNSAddress(NetworkValue)))) {
					return Fail(strErrorOut, itField->second.m_unLine, std::string(pszKey) + " in Internet client mode must contain numeric IPv4 " + (bWithPrefix ? "CIDRs." : "unicast nameserver addresses (excluding loopback and link-local addresses)."));
				}

				std::string const strFormatted = FormatNetworkAddress(NetworkValue, bWithPrefix);
				if (!setSeen.insert(strFormatted).second) {
					return Fail(strErrorOut, itField->second.m_unLine, std::string(pszKey) + " contains a duplicate address or route.");
				}

				vecAddressesOut.push_back(strFormatted);

				if (pNetworksOut) {
					pNetworksOut->push_back(NetworkValue);
				}

				if (vecAddressesOut.size() > kMaximumAddressEntries) {
					return Fail(strErrorOut, itField->second.m_unLine, std::string(pszKey) + " contains too many entries (maximum 256).");
				}

				if (unCommaOffset == std::string::npos) {
					break;
				}

				unBegin = unCommaOffset + 1;
			}

			return true;
		}

		bool ReadInternetSettings(std::vector<Section> const& vecSections, Config& Configuration, std::string& strErrorOut) {
			constexpr unsigned kMaximumSubnetPrefixBits = 30;

			for (auto const& SectionValue : vecSections) {
				if (SectionValue.m_strName != "WireSteam") {
					continue;
				}

				if (auto itMode = SectionValue.m_mapFields.find("InternetMode"); itMode != SectionValue.m_mapFields.end()) {
					if (itMode->second.m_strValue == "off") {
						Configuration.m_InternetMode = InternetMode::Off;
					} else if (itMode->second.m_strValue == "client") {
						Configuration.m_InternetMode = InternetMode::Client;
					} else if (itMode->second.m_strValue == "server") {
						Configuration.m_InternetMode = InternetMode::Server;
					} else {
						return Fail(strErrorOut, itMode->second.m_unLine, "InternetMode must be off, client, or server.");
					}
				}

				if (auto itSubnet = SectionValue.m_mapFields.find("InternetSubnet"); itSubnet != SectionValue.m_mapFields.end()) {
					Network NetworkValue;
					if (!ParseNetwork(itSubnet->second.m_strValue, true, true, NetworkValue) || (NetworkValue.m_nFamily != AF_INET) || (NetworkValue.m_unPrefix > kMaximumSubnetPrefixBits) ||
						(FormatNetworkAddress(NetworkValue, true) != itSubnet->second.m_strValue)) {
						return Fail(strErrorOut, itSubnet->second.m_unLine, "InternetSubnet must be a canonical private RFC1918 IPv4 network CIDR with prefix length at most 30 (for example 10.77.0.0/24).");
					}

					Configuration.m_strInternetSubnet = itSubnet->second.m_strValue;
				}
			}

			return true;
		}

		bool ValidateInternetSettings(Config const& Configuration, bool bCompiled, std::string& strErrorOut) {
			constexpr unsigned kMaximumSubnetPrefixBits = 30;
			constexpr unsigned kIPv4AddressBits = 32;
			constexpr unsigned kBitsPerByte = 8;
			constexpr unsigned kLastBitInByte = 0x07;

			if (Configuration.m_InternetMode == InternetMode::Off) {
				return Configuration.m_strInternetSubnet.empty() || Fail(strErrorOut, 0, "InternetSubnet requires InternetMode = server.");
			}

			if (Configuration.m_vecAddresses.size() != 1) {
				return Fail(strErrorOut, 0, "Internet mode requires exactly one private IPv4 Interface Address.");
			}

			Network Local;
			if (!ParseNetwork(Configuration.m_vecAddresses[0], true, false, Local) || (Local.m_nFamily != AF_INET)) {
				return Fail(strErrorOut, 0, "Internet mode requires one private IPv4 Interface Address. IPv6 exit traffic is not supported.");
			}

			if (Configuration.m_InternetMode == InternetMode::Client) {
				if (!Configuration.m_strInternetSubnet.empty()) {
					return Fail(strErrorOut, 0, "InternetSubnet is only valid with InternetMode = server.");
				}

				if (Configuration.m_vecPeers.size() != 1) {
					return Fail(strErrorOut, 0, "Internet client mode requires exactly one gateway peer.");
				}

				if (!Configuration.m_bRelayOnly) {
					return Fail(strErrorOut, 0, "Internet client mode requires RelayOnly = true.");
				}

				if (Configuration.m_vecDNS.empty()) {
					return Fail(strErrorOut, 0, "Internet client mode requires at least one numeric IPv4 DNS server in [Interface].");
				}

				std::vector<std::string> const& vecRoutes = Configuration.m_vecPeers.front().m_vecAllowedIPs;
				std::set<std::string> const setExpected = bCompiled ? std::set<std::string> { "0.0.0.0/1", "128.0.0.0/1" } : std::set<std::string> { "0.0.0.0/0" };
				if (std::set<std::string>(vecRoutes.begin(), vecRoutes.end()) != setExpected) {
					return Fail(strErrorOut, 0, bCompiled ? "Generated Internet client AllowedIPs must be exactly 0.0.0.0/1, 128.0.0.0/1." : "Internet client AllowedIPs must be exactly 0.0.0.0/0 on its single gateway peer.");
				}

				return true;
			}

			if (Configuration.m_strInternetSubnet.empty()) {
				return Fail(strErrorOut, 0, "Internet server mode requires InternetSubnet (for example 10.77.0.0/24).");
			}

			Network Subnet;
			if (!ParseNetwork(Configuration.m_strInternetSubnet, true, true, Subnet) || (Subnet.m_nFamily != AF_INET) || (Subnet.m_unPrefix > kMaximumSubnetPrefixBits) ||
				(FormatNetworkAddress(Subnet, true) != Configuration.m_strInternetSubnet)) {
				return Fail(strErrorOut, 0, "InternetSubnet must be a canonical private RFC1918 IPv4 network CIDR with prefix length at most 30.");
			}

			Network LocalHost = Local;
			LocalHost.m_unPrefix = kIPv4AddressBits;
			if (!DoesNetworkContain(Subnet, LocalHost)) {
				return Fail(strErrorOut, 0, "The server Interface Address must belong to InternetSubnet.");
			}

			Network NetworkAddress = Subnet;
			MaskNetwork(NetworkAddress);
			Network BroadcastAddress = NetworkAddress;
			for (std::size_t unBit = Subnet.m_unPrefix; unBit < kIPv4AddressBits; ++unBit) {
				BroadcastAddress.m_arrBytes[unBit / kBitsPerByte] |= static_cast<std::uint8_t>(0x01 << (kLastBitInByte - unBit % kBitsPerByte));
			}

			if ((Local.m_arrBytes == NetworkAddress.m_arrBytes) || (Local.m_arrBytes == BroadcastAddress.m_arrBytes)) {
				return Fail(strErrorOut, 0, "The server Interface Address cannot be the InternetSubnet network or broadcast address.");
			}

			for (auto const& Peer : Configuration.m_vecPeers) {
				for (auto const& strRoute : Peer.m_vecAllowedIPs) {
					Network Address;
					if (!ParseNetwork(strRoute, true, true, Address) || (Address.m_nFamily != AF_INET) || (Address.m_unPrefix != kIPv4AddressBits) ||
						!DoesNetworkContain(Subnet, Address)) {
						return Fail(strErrorOut, 0, "Internet server peer AllowedIPs must be private IPv4 /32 client addresses inside InternetSubnet.");
					}

					if ((Address.m_arrBytes == Local.m_arrBytes) || (Address.m_arrBytes == NetworkAddress.m_arrBytes) || (Address.m_arrBytes == BroadcastAddress.m_arrBytes)) {
						return Fail(strErrorOut, 0, "Internet server peer addresses cannot equal the server, subnet network, or broadcast address.");
					}
				}
			}

			return true;
		}

		bool ParseConfigurationInternal(std::string const& strText, Config& ConfigurationOut, std::string& strErrorOut, bool bCompiled, InternetMode Mode = InternetMode::Off, std::string const& strInternetSubnet = {}) {
			constexpr std::uint64_t kMaximumPort = 65535;
			constexpr std::uint64_t kMinimumMTU = 1280;
			constexpr std::uint64_t kMaximumMTU = 1420;
			constexpr std::uint64_t kMaximumVirtualPort = 999;
			constexpr std::uint64_t kMaximumReconnectSeconds = 300;
			constexpr std::uint64_t kMinimumSendRate = 65536;
			constexpr std::uint64_t kMaximumSendRate = 134217728;
			constexpr std::uint64_t kMaximumStatisticsSeconds = 3600;
			constexpr unsigned kSteamUniverseShift = 0x38;
			constexpr unsigned kSteamAccountTypeShift = 0x34;
			constexpr unsigned kSteamInstanceShift = 0x20;
			constexpr std::uint64_t kSteamAccountTypeMask = 0xF;
			constexpr std::uint64_t kSteamInstanceMask = 0xFFFFF;
			constexpr std::uint64_t kSteamAccountIDMask = 0xFFFFFFFF;
			constexpr std::uint64_t kPublicSteamUniverse = 1;
			constexpr std::uint64_t kIndividualSteamAccount = 1;
			constexpr std::uint64_t kDesktopSteamInstance = 1;
			constexpr std::string_view kEndpointPrefix = "127.0.0.1:";

			std::vector<Section> vecSections;
			SensitiveCleanup<std::vector<Section>> SectionsCleanup(vecSections);
			if (!ReadSections(strText, bCompiled, vecSections, strErrorOut)) {
				return false;
			}

			Config Configuration;
			SensitiveCleanup<Config> ConfigurationCleanup(Configuration);
			Configuration.m_InternetMode = Mode;
			Configuration.m_strInternetSubnet = strInternetSubnet;
			if (!bCompiled && !ReadInternetSettings(vecSections, Configuration, strErrorOut)) {
				return false;
			}

			std::set<std::uint64_t> setSteamIDs;
			std::set<std::string> setPublicKeys;
			std::set<std::uint16_t> setFixedBridgePorts;
			std::vector<Network> vecPreviousRoutes;
			for (auto const& SectionValue : vecSections) {
				if (SectionValue.m_strName == "Interface") {
					if (!ReadKeyField(SectionValue, "PrivateKey", Configuration.m_strPrivateKey, strErrorOut, true) ||
						!ReadAddressList(SectionValue, "Address", true, false, Configuration.m_vecAddresses, nullptr, strErrorOut, true) ||
						!ReadAddressList(SectionValue, "DNS", false, false, Configuration.m_vecDNS, nullptr, strErrorOut, false, Configuration.m_InternetMode == InternetMode::Client)) {
						return false;
					}

					std::uint64_t unListenPort = Configuration.m_unListenPort;
					std::uint64_t unMTU = Configuration.m_nMTU;
					if (!ReadNumber(SectionValue, "ListenPort", bCompiled ? 1 : 0, kMaximumPort, unListenPort, strErrorOut, bCompiled) ||
						!ReadNumber(SectionValue, "MTU", kMinimumMTU, kMaximumMTU, unMTU, strErrorOut)) {
						return false;
					}

					Configuration.m_unListenPort = static_cast<std::uint16_t>(unListenPort);
					Configuration.m_nMTU = static_cast<int>(unMTU);
				} else if (SectionValue.m_strName == "WireSteam") {
					std::uint64_t unAppID = Configuration.m_unAppID;
					std::uint64_t unVirtualPort = Configuration.m_nVirtualPort;
					std::uint64_t unReconnectSeconds = Configuration.m_nReconnectSeconds;
					std::uint64_t unSendRate = Configuration.m_nSendRate;
					std::uint64_t unStatisticsSeconds = Configuration.m_nStatisticsSeconds;
					if (!ReadNumber(SectionValue, "AppID", 1, std::numeric_limits<std::uint32_t>::max(), unAppID, strErrorOut) ||
						!ReadNumber(SectionValue, "VirtualPort", 0, kMaximumVirtualPort, unVirtualPort, strErrorOut) ||
						!ReadNumber(SectionValue, "ReconnectSeconds", 1, kMaximumReconnectSeconds, unReconnectSeconds, strErrorOut) ||
						!ReadNumber(SectionValue, "SendRate", kMinimumSendRate, kMaximumSendRate, unSendRate, strErrorOut) ||
						!ReadNumber(SectionValue, "StatsSeconds", 0, kMaximumStatisticsSeconds, unStatisticsSeconds, strErrorOut) ||
						!ReadBoolean(SectionValue, "RelayOnly", Configuration.m_bRelayOnly, strErrorOut)) {
						return false;
					}

					Configuration.m_unAppID = static_cast<std::uint32_t>(unAppID);
					Configuration.m_nVirtualPort = static_cast<int>(unVirtualPort);
					Configuration.m_nReconnectSeconds = static_cast<int>(unReconnectSeconds);
					Configuration.m_nSendRate = static_cast<int>(unSendRate);
					Configuration.m_nStatisticsSeconds = static_cast<int>(unStatisticsSeconds);
				} else if (SectionValue.m_strName == "PortForward") {
					PortForwardConfig Rule;
					if (!ParsePortForward(SectionValue, Rule, strErrorOut)) {
						return false;
					}

					Configuration.m_vecPortForwards.push_back(std::move(Rule));
				} else {
					PeerConfig Peer;
					SensitiveCleanup<std::string> PresharedKeyCleanup(Peer.m_strPresharedKey);
					std::vector<Network> vecRoutes;
					if (!ReadKeyField(SectionValue, "PublicKey", Peer.m_strPublicKey, strErrorOut, true) ||
						!ReadKeyField(SectionValue, "PresharedKey", Peer.m_strPresharedKey, strErrorOut, false) ||
						!ReadAddressList(SectionValue, "AllowedIPs", true, true, Peer.m_vecAllowedIPs, &vecRoutes, strErrorOut, true, Configuration.m_InternetMode == InternetMode::Client)) {
						return false;
					}

					if (!setPublicKeys.insert(Peer.m_strPublicKey).second) {
						return Fail(strErrorOut, SectionValue.m_unLine, "The same PublicKey is configured for multiple peers.");
					}

					for (auto const& Route : vecRoutes) {
						for (auto const& Earlier : vecPreviousRoutes) {
							if (DoNetworksOverlap(Route, Earlier)) {
								return Fail(strErrorOut, SectionValue.m_unLine, "AllowedIPs overlap a route assigned to another peer.");
							}
						}
					}

					vecPreviousRoutes.insert(vecPreviousRoutes.end(), vecRoutes.begin(), vecRoutes.end());

					std::uint64_t unKeepalive = Peer.m_nKeepalive;
					if (!ReadNumber(SectionValue, "PersistentKeepalive", 0, kMaximumPort, unKeepalive, strErrorOut)) {
						return false;
					}

					Peer.m_nKeepalive = static_cast<int>(unKeepalive);

					if (bCompiled) {
						auto itEndpoint = SectionValue.m_mapFields.find("Endpoint");
						if (itEndpoint == SectionValue.m_mapFields.end()) {
							return Fail(strErrorOut, SectionValue.m_unLine, "Endpoint is required in the generated WireGuard configuration.");
						}

						unsigned unEndpointPort = 0;
						if (!itEndpoint->second.m_strValue.starts_with(kEndpointPrefix) ||
							!ReadUnsignedNumber(itEndpoint->second.m_strValue.substr(kEndpointPrefix.size()), unEndpointPort) || !unEndpointPort || (unEndpointPort > kMaximumPort)) {
							return Fail(strErrorOut, itEndpoint->second.m_unLine, "The generated Endpoint must be 127.0.0.1 followed by a nonzero UDP port.");
						}

						Peer.m_unBridgePort = static_cast<std::uint16_t>(unEndpointPort);
						if (!setFixedBridgePorts.insert(Peer.m_unBridgePort).second) {
							return Fail(strErrorOut, itEndpoint->second.m_unLine, "Each peer must use a distinct loopback Endpoint port.");
						}
					} else {
						std::uint64_t unSteamID = 0;
						std::uint64_t unSteamPort = 0;
						std::uint64_t unBridgePort = 0;
						if (!ReadNumber(SectionValue, "SteamID", 1, std::numeric_limits<std::uint64_t>::max(), unSteamID, strErrorOut, true)) {
							return false;
						}

						if (((unSteamID >> kSteamUniverseShift) != kPublicSteamUniverse) || (((unSteamID >> kSteamAccountTypeShift) & kSteamAccountTypeMask) != kIndividualSteamAccount) || (((unSteamID >> kSteamInstanceShift) & kSteamInstanceMask) != kDesktopSteamInstance) || !(unSteamID & kSteamAccountIDMask)) {
							return Fail(strErrorOut, SectionValue.m_mapFields.at("SteamID").m_unLine, "SteamID must be a public-universe individual desktop SteamID64.");
						}

						if (!setSteamIDs.insert(unSteamID).second) {
							return Fail(strErrorOut, SectionValue.m_unLine, "The same SteamID is configured for multiple peers.");
						}

						Peer.m_unSteamID = unSteamID;

						if (SectionValue.m_mapFields.contains("SteamPort")) {
							if (!ReadNumber(SectionValue, "SteamPort", 0, kMaximumVirtualPort, unSteamPort, strErrorOut)) {
								return false;
							}

							Peer.m_nSteamPort = static_cast<int>(unSteamPort);
						}

						if (!ReadNumber(SectionValue, "BridgePort", 0, kMaximumPort, unBridgePort, strErrorOut) ||
							!ReadBoolean(SectionValue, "Dial", Peer.m_bDial, strErrorOut)) {
							return false;
						}

						Peer.m_unBridgePort = static_cast<std::uint16_t>(unBridgePort);
						if (Peer.m_unBridgePort && !setFixedBridgePorts.insert(Peer.m_unBridgePort).second) {
							return Fail(strErrorOut, SectionValue.m_unLine, "Each explicit BridgePort must be distinct.");
						}
					}

					Configuration.m_vecPeers.push_back(std::move(Peer));
				}
			}

			if (bCompiled) {
				for (auto const& Peer : Configuration.m_vecPeers) {
					if (Peer.m_unBridgePort == Configuration.m_unListenPort) {
						return Fail(strErrorOut, 0, "A loopback Endpoint cannot equal the WireGuard ListenPort.");
					}
				}
			} else if (Configuration.m_unListenPort) {
				for (auto const& Peer : Configuration.m_vecPeers) {
					if (Peer.m_unBridgePort == Configuration.m_unListenPort) {
						return Fail(strErrorOut, 0, "BridgePort cannot equal the WireGuard ListenPort.");
					}
				}
			}

			if (!ValidateInternetSettings(Configuration, bCompiled, strErrorOut) || !ValidatePortForwards(Configuration, strErrorOut)) {
				return false;
			}

			WipeSensitive(ConfigurationOut);
			ConfigurationOut = std::move(Configuration);
			strErrorOut.clear();

			return true;
		}

		std::string Join(std::vector<std::string> const& vecItems) {
			std::string strOutput;
			for (auto const& strItem : vecItems) {
				if (!strOutput.empty()) {
					strOutput += ", ";
				}

				strOutput += strItem;
			}

			return strOutput;
		}

		void Expect(bool bCondition, std::string const& strMessage) {
			if (!bCondition) {
				throw std::runtime_error("Configuration test failed: " + strMessage);
			}
		}

		void TestSensitiveCleanup(bool bThrow) {
			constexpr char kInputFixture[] = "input fixture";
			constexpr std::size_t kOutputFixtureBytes = 4096;

			std::string strSmall = "fixture";
			std::string strLarge = std::string(128, 'K');
			Key KeyValue {};
			KeyValue.fill(0xA5);
			Config Configuration;
			Configuration.m_strPrivateKey = "private fixture";
			Configuration.m_vecPeers.emplace_back();
			Configuration.m_vecPeers.back().m_strPresharedKey = "preshared fixture";
			std::vector<Section> vecSections;
			vecSections.emplace_back("Interface", 1);
			vecSections.back().m_mapFields.try_emplace("PrivateKey", "field fixture", 2);
			bool bCaught = false;

			std::istringstream InputStream(kInputFixture);
			std::ostringstream OutputStream;

			try {
				SensitiveCleanup<std::string> SmallCleanup(strSmall);
				SensitiveCleanup<std::string> LargeCleanup(strLarge);
				SensitiveCleanup<Key> KeyCleanup(KeyValue);
				SensitiveCleanup<Config> ConfigurationCleanup(Configuration);
				SensitiveCleanup<std::vector<Section>> SectionsCleanup(vecSections);
				SensitiveCleanup<std::stringbuf> InputCleanup(*InputStream.rdbuf());
				SensitiveCleanup<std::stringbuf> OutputCleanup(*OutputStream.rdbuf());

				std::string strRead;
				SensitiveCleanup<std::string> ReadCleanup(strRead);
				while (std::getline(InputStream, strRead)) {
				}

				OutputStream << std::string(kOutputFixtureBytes, 'O');

				if (bThrow) {
					throw std::runtime_error("sensitive cleanup fixture");
				}
			} catch (std::runtime_error const&) {
				bCaught = true;
			}

			auto IsWiped = [](std::string_view strvValue) -> bool {
				return std::all_of(strvValue.begin(), strvValue.end(), [](char chValue) -> bool {
					return chValue == 0x00;
				});
			};

			const bool bKeyWiped = std::all_of(KeyValue.begin(), KeyValue.end(), [](std::uint8_t unByte) -> bool {
				return unByte == 0x00;
			});
			Expect(bCaught == bThrow, "sensitive cleanup exception path");
			Expect(IsWiped(strSmall) && IsWiped(strLarge), "small and heap string cleanup");
			Expect(bKeyWiped, "decoded key cleanup");
			Expect(IsWiped(Configuration.m_strPrivateKey) && IsWiped(Configuration.m_vecPeers.front().m_strPresharedKey), "temporary configuration cleanup");
			Expect(IsWiped(vecSections.front().m_mapFields.at("PrivateKey").m_strValue), "parsed field cleanup");
			Expect((InputStream.view().size() == (sizeof(kInputFixture) - 1)) && IsWiped(InputStream.view()), "input buffer cleanup after EOF");
			Expect((OutputStream.view().size() == kOutputFixtureBytes) && IsWiped(OutputStream.view()), "current output buffer cleanup after growth");
		}
	} // namespace

	bool DecodeKey(std::string const& strText, Key& KeyOut) {
		constexpr std::size_t kEncodedKeyCharacters = 44;
		constexpr std::size_t kKeyDataCharacters = 43;
		constexpr unsigned kBitsPerSymbol = 6;
		constexpr unsigned kBitsPerByte = 8;
		constexpr unsigned kPaddingMask = 0x3;

		if ((strText.size() != kEncodedKeyCharacters) || (strText[kKeyDataCharacters] != '=')) {
			return false;
		}

		Key Decoded {};
		SensitiveCleanup<Key> DecodedCleanup(Decoded);
		unsigned unBits = 0;
		unsigned unBitCount = 0;
		std::size_t unOffset = 0;
		for (std::size_t unByteIndex = 0; unByteIndex < kKeyDataCharacters; ++unByteIndex) {
			char const* pszMatch = std::strchr(kBase64, strText[unByteIndex]);
			if (!strText[unByteIndex] || !pszMatch) {
				return false;
			}

			unBits = (unBits << kBitsPerSymbol) | static_cast<unsigned>(pszMatch - kBase64);
			unBitCount += kBitsPerSymbol;
			if (unBitCount >= kBitsPerByte) {
				unBitCount -= kBitsPerByte;
				if (unOffset >= Decoded.size()) {
					return false;
				}

				Decoded[unOffset++] = static_cast<std::uint8_t>(unBits >> unBitCount);
			}
		}

		if ((unOffset != Decoded.size()) || ((unBits & kPaddingMask) != 0x00)) {
			return false;
		}

		std::string strCanonical = EncodeKey(Decoded);
		SensitiveCleanup<std::string> CanonicalCleanup(strCanonical);
		if (strCanonical != strText) {
			return false;
		}

		KeyOut = Decoded;

		return true;
	}

	std::string EncodeKey(Key const& KeyValue) {
		constexpr std::size_t kEncodedKeyCharacters = 44;
		constexpr unsigned kBitsPerSymbol = 6;
		constexpr unsigned kBitsPerByte = 8;
		constexpr unsigned kSymbolMask = 0x3F;
		constexpr std::size_t kSymbolsPerBlock = 4;

		std::string strOutput;
		strOutput.reserve(kEncodedKeyCharacters);
		unsigned unBits = 0;
		unsigned unBitCount = 0;
		for (auto const& unByte : KeyValue) {
			unBits = (unBits << kBitsPerByte) | unByte;
			unBitCount += kBitsPerByte;
			while (unBitCount >= kBitsPerSymbol) {
				unBitCount -= kBitsPerSymbol;
				strOutput += kBase64[(unBits >> unBitCount) & kSymbolMask];
			}
		}

		if (unBitCount) {
			strOutput += kBase64[(unBits << (kBitsPerSymbol - unBitCount)) & kSymbolMask];
		}

		while (strOutput.size() % kSymbolsPerBlock) {
			strOutput += '=';
		}

		return strOutput;
	}

	bool ParseConfig(std::string const& strText, Config& ConfigurationOut, std::string& strErrorOut) {
		return ParseConfigurationInternal(strText, ConfigurationOut, strErrorOut, false);
	}

	std::string CompileConfig(Config const& Configuration, std::uint16_t unWireGuardPort, std::vector<std::uint16_t> const& vecPeerPorts) {
		if (!unWireGuardPort || (vecPeerPorts.size() != Configuration.m_vecPeers.size()) ||
			std::any_of(vecPeerPorts.begin(), vecPeerPorts.end(), [](std::uint16_t unPort) -> bool {
			return unPort == 0;
		})) {
			throw std::invalid_argument("WireGuard and per-peer loopback ports must be assigned before compiling the configuration.");
		}

		if (PortForwardConflictsWithWireGuardPort(Configuration, unWireGuardPort)) {
			throw std::invalid_argument("WireGuard ListenPort must differ from every UDP PortForward Listen and Target port.");
		}

		for (auto const& unPort : vecPeerPorts) {
			if (PortForwardConflictsWithBridgePort(Configuration, unPort)) {
				throw std::invalid_argument("Peer BridgePort must not occupy a UDP PortForward Target at 127.0.0.1.");
			}
		}

		std::string strErrorOut;
		if (!ValidateInternetSettings(Configuration, false, strErrorOut) || !ValidatePortForwards(Configuration, strErrorOut)) {
			throw std::invalid_argument("Unable to compile configuration: " + strErrorOut);
		}

		std::ostringstream OutputStream;
		SensitiveCleanup<std::stringbuf> OutputCleanup(*OutputStream.rdbuf());

		OutputStream
			<< "# Generated by WireSteam. Contains private keys; do not share.\n"
			<< "[Interface]\n"
			<< "PrivateKey = " << Configuration.m_strPrivateKey << '\n'
			<< "Address = " << Join(Configuration.m_vecAddresses) << '\n'
			<< "ListenPort = " << unWireGuardPort << '\n'
			<< "MTU = " << Configuration.m_nMTU << '\n';

		if (!Configuration.m_vecDNS.empty()) {
			OutputStream << "DNS = " << Join(Configuration.m_vecDNS) << '\n';
		}

		for (std::size_t unPeerIndex = 0; unPeerIndex < Configuration.m_vecPeers.size(); ++unPeerIndex) {
			PeerConfig const& Peer = Configuration.m_vecPeers[unPeerIndex];

			OutputStream
				<< "\n"
				<< "[Peer]\n"
				<< "PublicKey = " << Peer.m_strPublicKey << '\n';

			if (!Peer.m_strPresharedKey.empty()) {
				OutputStream << "PresharedKey = " << Peer.m_strPresharedKey << '\n';
			}

			OutputStream
				<< "AllowedIPs = " << ((Configuration.m_InternetMode == InternetMode::Client) ? "0.0.0.0/1, 128.0.0.0/1" : Join(Peer.m_vecAllowedIPs)) << '\n'
				<< "Endpoint = 127.0.0.1:" << vecPeerPorts[unPeerIndex] << '\n'
				<< "PersistentKeepalive = " << Peer.m_nKeepalive << '\n';
		}

		std::string strGenerated = std::string(OutputStream.view());
		SensitiveCleanup<std::string> GeneratedCleanup(strGenerated);
		Config Validated;
		SensitiveCleanup<Config> ValidatedCleanup(Validated);
		if (!ParseConfigurationInternal(strGenerated, Validated, strErrorOut, true, Configuration.m_InternetMode, Configuration.m_strInternetSubnet)) {
			throw std::invalid_argument("Unable to compile configuration: " + strErrorOut);
		}

		return std::string(OutputStream.view());
	}

	std::string RedactedSummary(Config const& Configuration) {
		std::ostringstream OutputStream;

		OutputStream
			<< "Interface addresses: " << Join(Configuration.m_vecAddresses) << '\n'
			<< "WireGuard listen port: " << Configuration.m_unListenPort << '\n'
			<< "MTU: " << Configuration.m_nMTU << '\n'
			<< "Steam AppID: " << Configuration.m_unAppID << '\n'
			<< "Steam virtual port: " << Configuration.m_nVirtualPort << '\n'
			<< "Steam transport: " << (Configuration.m_bRelayOnly ? "SDR relay only" : "direct P2P with SDR fallback") << '\n'
			<< "Internet mode: " << InternetModeName(Configuration.m_InternetMode)
			<< ((Configuration.m_InternetMode == InternetMode::Client) ? " (IPv4 exit; non-loopback IPv6 blocked)" : "") << '\n';

		if (!Configuration.m_strInternetSubnet.empty()) {
			OutputStream << "Internet NAT subnet: " << Configuration.m_strInternetSubnet << '\n';
		}

		OutputStream << "Peers: " << Configuration.m_vecPeers.size() << '\n';
		for (std::size_t unPeerIndex = 0; unPeerIndex < Configuration.m_vecPeers.size(); ++unPeerIndex) {
			PeerConfig const& Peer = Configuration.m_vecPeers[unPeerIndex];

			OutputStream << "  Peer " << unPeerIndex + 1 << ": SteamID " << Peer.m_unSteamID << ", routes " << Join(Peer.m_vecAllowedIPs) << ", Steam port " << ((Peer.m_nSteamPort < 0) ? Configuration.m_nVirtualPort : Peer.m_nSteamPort) << ", " << (Peer.m_bDial ? "dial and listen" : "listen only") << '\n';
		}

		OutputStream << "Port forwards: " << Configuration.m_vecPortForwards.size() << '\n';
		for (auto const& Rule : Configuration.m_vecPortForwards) {
			auto FormatEndpoint = [](std::string const& strAddress, std::uint16_t unPort) -> std::string {
				return ((strAddress.find(':') == std::string::npos) ? strAddress : "[" + strAddress + "]") + ":" + std::to_string(unPort);
			};

			OutputStream << "  " << PortForwardProtocolName(Rule.m_Protocol) << " " << FormatEndpoint(Rule.m_strListenAddress, Rule.m_unListenPort) << " -> " << FormatEndpoint(Rule.m_strTargetAddress, Rule.m_unTargetPort) << " (max " << Rule.m_unMaximumConnections << ((Rule.m_Protocol == PortForwardProtocol::Tcp) ? " connections)\n" : " client mappings)\n");
		}

		return OutputStream.str();
	}

	char const* PortForwardProtocolName(PortForwardProtocol Protocol) {
		return (Protocol == PortForwardProtocol::Udp) ? "UDP" : "TCP";
	}

	bool PortForwardConflictsWithWireGuardPort(Config const& Configuration, std::uint16_t unPort) {
		return std::any_of(Configuration.m_vecPortForwards.begin(), Configuration.m_vecPortForwards.end(), [unPort](PortForwardConfig const& Rule) -> bool {
			return (Rule.m_Protocol == PortForwardProtocol::Udp) && ((Rule.m_unListenPort == unPort) || (Rule.m_unTargetPort == unPort));
		});
	}

	bool PortForwardConflictsWithBridgePort(Config const& Configuration, std::uint16_t unPort) {
		return std::any_of(Configuration.m_vecPortForwards.begin(), Configuration.m_vecPortForwards.end(), [unPort](PortForwardConfig const& Rule) -> bool {
			return (Rule.m_Protocol == PortForwardProtocol::Udp) && (Rule.m_strTargetAddress == "127.0.0.1") && (Rule.m_unTargetPort == unPort);
		});
	}

	char const* InternetModeName(InternetMode Mode) {
		switch (Mode) {
			case InternetMode::Off:
				return "off";

			case InternetMode::Client:
				return "client";

			case InternetMode::Server:
				return "server";
		}

		return "off";
	}

	bool ValidateCompiledConfig(std::string const& strText, std::string& strErrorOut, InternetMode Mode, std::string const& strInternetSubnet) {
		Config Configuration;
		SensitiveCleanup<Config> ConfigurationCleanup(Configuration);

		return ParseConfigurationInternal(strText, Configuration, strErrorOut, true, Mode, strInternetSubnet);
	}

	void RunConfigTests() {
		constexpr std::size_t kMalformedPaddingIndex = 0x2A;
		constexpr std::size_t kExpectedMaximumPortForwards = 32;
		constexpr std::uint16_t kFirstTestForwardPort = 25565;

		TestSensitiveCleanup(false);
		TestSensitiveCleanup(true);

		Key KeyValue {};
		for (std::size_t unByteIndex = 0; unByteIndex < KeyValue.size(); ++unByteIndex) {
			KeyValue[unByteIndex] = static_cast<std::uint8_t>(unByteIndex + 1);
		}

		Key PeerKey = KeyValue;
		PeerKey[0] = 0x64;
		std::string const strPrivateKey = EncodeKey(KeyValue);
		std::string const strPublicKey = EncodeKey(PeerKey);
		Key Decoded {};
		Expect(DecodeKey(strPrivateKey, Decoded) && (Decoded == KeyValue), "32-byte Base64 round trip");
		Expect(!DecodeKey(strPrivateKey.substr(1), Decoded), "short key rejection");

		std::string strBadPadding = strPrivateKey;
		strBadPadding[kMalformedPaddingIndex] = 'B';
		Expect(!DecodeKey(strBadPadding, Decoded), "noncanonical padding rejection");

		std::string const strValid = "[Interface]\nPrivateKey = " + strPrivateKey +
			"\nAddress = 10.77.0.1/24, fd77::1/64\nDNS = 10.77.0.53, fd77::53\nListenPort = 51820\nMTU = 1280\n"
			"[WireSteam]\nAppID = 480\nVirtualPort = 7\nRelayOnly = true\nReconnectSeconds = 3\nSendRate = 4194304\nStatsSeconds = 0\n"
			"[Peer]\nPublicKey = " +
			strPublicKey + "\nPresharedKey = " + strPrivateKey +
			"\nAllowedIPs = 10.78.0.55/24, fd78::123/64\nSteamID = 76561198000000000\nSteamPort = 8\nPersistentKeepalive = 25\nDial = false\n";
		Config Parsed;
		std::string strError;
		const bool bValidConfigurationParsed = ParseConfig(strValid, Parsed, strError);
		Expect(bValidConfigurationParsed, "valid config: " + strError);
		Expect((Parsed.m_vecPeers.size() == 1) && (Parsed.m_vecPeers[0].m_vecAllowedIPs[0] == "10.78.0.0/24") && (Parsed.m_vecPeers[0].m_vecAllowedIPs[1] == "fd78::/64"), "route normalization");
		Expect((Parsed.m_vecAddresses[0] == "10.77.0.1/24") && (Parsed.m_vecPeers[0].m_nSteamPort == 8) && !Parsed.m_vecPeers[0].m_bDial, "address and peer settings");

		std::string const strSummary = RedactedSummary(Parsed);
		Expect((strSummary.find(strPrivateKey) == std::string::npos) && (strSummary.find(strPublicKey) == std::string::npos), "summary redaction");

		std::string const strCompiled = CompileConfig(Parsed, 51820, { 51999 });
		const bool bCompiledConfigurationValid = ValidateCompiledConfig(strCompiled, strError);
		Expect(bCompiledConfigurationValid, "compiled config validation: " + strError);
		Expect((strCompiled.find("Endpoint = 127.0.0.1:51999") != std::string::npos) && (strCompiled.find("SteamID") == std::string::npos), "SteamID compilation");

		auto ReplaceText = [](std::string strInput, std::string const& strOriginal, std::string const& strReplacement) -> std::string {
			const std::size_t unOffset = strInput.find(strOriginal);
			if (unOffset == std::string::npos) {
				throw std::runtime_error("Invalid configuration test fixture.");
			}

			return strInput.replace(unOffset, strOriginal.size(), strReplacement);
		};

		std::vector<std::pair<std::string, std::string>> const vecInvalidRoutes {
			{ "0.0.0.0/0", "default route" }, { "0.0.0.0/1", "half default route" }, { "8.8.8.0/24", "public route" },
			{ "10.0.0.0/7", "escaping private range" }, { "172.16.0.0/11", "escaping 172 range" },
			{ "192.168.0.0/15", "escaping 192 range" }, { "127.0.0.0/8", "loopback route" }, { "169.254.0.0/16", "link-local route" },
			{ "::/0", "IPv6 default route" }, { "fc00::/6", "escaping ULA range" }, { "fe80::/64", "IPv6 link-local route" }
		};
		for (auto const& InvalidRoute : vecInvalidRoutes) {
			std::string const& strRoute = InvalidRoute.first;
			std::string const& strLabel = InvalidRoute.second;
			Expect(!ParseConfig(ReplaceText(strValid, "10.78.0.55/24, fd78::123/64", strRoute), Parsed, strError), strLabel + " rejection");
		}

		for (auto const& strInjection : std::vector<std::string> { "PostUp = calc.exe\n", "PrivateKey = " + strPrivateKey + "\n" }) {
			Expect(!ParseConfig(ReplaceText(strValid, "Address = ", strInjection + "Address = "), Parsed, strError), "unsafe or duplicate interface setting rejection");
		}

		Expect(!ParseConfig(strValid + "[Interface]\n", Parsed, strError), "duplicate Interface rejection");
		Expect(!ParseConfig(ReplaceText(strValid, "SteamID = 76561198000000000", "SteamID = 480"), Parsed, strError), "invalid SteamID rejection");
		Expect(!ParseConfig(ReplaceText(strValid, "Dial = false", "Endpoint = 1.2.3.4:1234"), Parsed, strError), "Endpoint setting rejection");
		Expect(!ParseConfig(ReplaceText(strValid, "Dial = false", "Dial = perhaps"), Parsed, strError), "invalid boolean rejection");
		Expect(!ParseConfig(ReplaceText(strValid, "MTU = 1280", "MTU = 99999"), Parsed, strError), "invalid MTU rejection");
		Expect(!ParseConfig(ReplaceText(strValid, "AppID = 480", "AppID = 4294967296"), Parsed, strError), "numeric overflow rejection");
		Expect(!ParseConfig(strValid + std::string("\0", 1), Parsed, strError), "NUL rejection");
		Expect(!ParseConfig(std::string(kMaximumConfigurationBytes + 1, 'x'), Parsed, strError), "file size bound");

		Key SecondKey = PeerKey;
		SecondKey[1] = 0x63;
		std::string const strSecondPeer = "[Peer]\nPublicKey = " + EncodeKey(SecondKey) + "\nAllowedIPs = 10.78.0.5/32\nSteamID = 76561198000000001\n";
		Expect(!ParseConfig(strValid + strSecondPeer, Parsed, strError), "overlapping routes across peers rejection");
		Expect(!ValidateCompiledConfig(ReplaceText(strCompiled, "127.0.0.1:51999", "8.8.8.8:51999"), strError), "nonloopback endpoint rejection");
		Expect(!ValidateCompiledConfig(ReplaceText(strCompiled, "MTU = 1280", "MTU = 1280\nPostUp = calc.exe"), strError), "compiled hook rejection");
		Expect(!ValidateCompiledConfig(ReplaceText(strCompiled, "127.0.0.1:51999", "127.0.0.1:51820"), strError), "endpoint self-loop rejection");
		Expect(ParseConfig("\xEF\xBB\xBF" + strValid, Parsed, strError), "UTF-8 BOM support");

		std::string const strForward = "[PortForward]\nProtocol = TCP\nListen = 10.77.0.1:25565\nTarget = 127.0.0.1:25565\n";
		std::string const strIPv6Forward = "[PortForward]\nListen = [fd77:0:0::1]:25565\nTarget = [::1]:25565\nMaxConnections = 8\nConnectTimeoutSeconds = 3\nIdleTimeoutSeconds = 0\n";
		Config Forwarded;
		const bool bForwardRulesParsed = ParseConfig(strForward + strValid + strIPv6Forward, Forwarded, strError);
		Expect(bForwardRulesParsed, "forward rules may precede Interface and follow peers: " + strError);
		Expect((Forwarded.m_vecPortForwards.size() == 2) && (Forwarded.m_vecPortForwards[0].m_strListenAddress == "10.77.0.1") && (Forwarded.m_vecPortForwards[0].m_Protocol == PortForwardProtocol::Tcp) && (Forwarded.m_vecPortForwards[1].m_Protocol == PortForwardProtocol::Tcp) && (Forwarded.m_vecPortForwards[0].m_unTargetPort == 25565) && (Forwarded.m_vecPortForwards[0].m_unMaximumConnections == 64) && (Forwarded.m_vecPortForwards[1].m_strListenAddress == "fd77::1") && (Forwarded.m_vecPortForwards[1].m_strTargetAddress == "::1") && (Forwarded.m_vecPortForwards[1].m_unIdleTimeoutSeconds == 0), "TCP forwarding defaults, dual-family addresses and bounds");

		std::string const strForwardedCompiled = CompileConfig(Forwarded, 51820, { 51999 });
		Expect((strForwardedCompiled.find("PortForward") == std::string::npos) && (strForwardedCompiled.find("25565") == std::string::npos) && ValidateCompiledConfig(strForwardedCompiled, strError), "forwarding directives never reach official WireGuard configuration");

		std::string const strForwardSummary = RedactedSummary(Forwarded);
		Expect((strForwardSummary.find("TCP 10.77.0.1:25565 -> 127.0.0.1:25565") != std::string::npos) && (strForwardSummary.find("[fd77::1]:25565 -> [::1]:25565") != std::string::npos) && (strForwardSummary.find(strPrivateKey) == std::string::npos), "forwarding summary endpoints without private keys");
		Expect(!ValidateCompiledConfig(strCompiled + strForward, strError), "compiled configs reject application forwarding sections");
		Expect(!ParseConfig(strValid + strForward + strForward, Forwarded, strError), "duplicate TCP listeners rejected");

		std::string const strUDPForward = ReplaceText(strForward, "Protocol = TCP", "Protocol = UDP");
		Expect(ParseConfig(strValid + strForward + strUDPForward, Forwarded, strError) && (Forwarded.m_vecPortForwards.size() == 2) && (Forwarded.m_vecPortForwards[0].m_Protocol == PortForwardProtocol::Tcp) && (Forwarded.m_vecPortForwards[1].m_Protocol == PortForwardProtocol::Udp), "TCP and UDP may publish the same VPN address and port independently");
		Expect((RedactedSummary(Forwarded).find("UDP 10.77.0.1:25565 -> 127.0.0.1:25565") != std::string::npos) && (CompileConfig(Forwarded, 51820, { 51999 }).find("PortForward") == std::string::npos), "UDP summary and compiled stripping");
		Expect(!ParseConfig(strValid + strUDPForward + strUDPForward, Forwarded, strError), "duplicate UDP listeners rejected");
		Expect(!ParseConfig(strValid + ReplaceText(strUDPForward, "Listen = 10.77.0.1:25565", "Listen = 10.77.0.1:51820"), Forwarded, strError) && (strError.find("WireGuard ListenPort") != std::string::npos), "UDP forwarding cannot collide with WireGuard wildcard port");
		Expect(ParseConfig(strValid + ReplaceText(strForward, "Listen = 10.77.0.1:25565", "Listen = 10.77.0.1:51820"), Forwarded, strError), "TCP forwarding may use the same numeric port as WireGuard UDP");
		for (auto const& pszTarget : { "127.0.0.1:51820", "127.0.0.2:51820", "[::1]:51820" }) {
			Expect(!ParseConfig(strValid + ReplaceText(strUDPForward, "Target = 127.0.0.1:25565", "Target = " + std::string(pszTarget)), Forwarded, strError), "UDP targets cannot occupy WireGuard wildcard port");
		}

		std::string const strFixedBridgeBase = ReplaceText(strValid, "Dial = false", "Dial = false\nBridgePort = 51999");
		std::string const strBridgeTarget = ReplaceText(strUDPForward, "Target = 127.0.0.1:25565", "Target = 127.0.0.1:51999");
		Expect(!ParseConfig(strFixedBridgeBase + strBridgeTarget, Forwarded, strError) && (strError.find("BridgePort") != std::string::npos), "UDP target cannot occupy fixed peer bridge endpoint");
		Expect(ParseConfig(strFixedBridgeBase + ReplaceText(strBridgeTarget, "Target = 127.0.0.1", "Target = 127.0.0.2"), Forwarded, strError), "different loopback address can use the same numeric port as peer bridge");
		Expect(ParseConfig(strValid + strBridgeTarget, Forwarded, strError), "automatic peer bridge with a UDP target reservation");
		bool bActualBridgeRejected = false;
		try {
			CompileConfig(Forwarded, 51820, { 51999 });
		} catch (std::invalid_argument const&) {
			bActualBridgeRejected = true;
		}

		Expect(bActualBridgeRejected && PortForwardConflictsWithBridgePort(Forwarded, 51999) && !PortForwardConflictsWithBridgePort(Forwarded, 51998), "allocated bridge endpoint must avoid local UDP targets");
		Expect(ParseConfig(ReplaceText(strValid, "ListenPort = 51820", "ListenPort = 0") + strUDPForward, Forwarded, strError), "automatic WireGuard port with UDP forwarding");
		bool bActualPortRejected = false;
		try {
			CompileConfig(Forwarded, 25565, { 51999 });
		} catch (std::invalid_argument const&) {
			bActualPortRejected = true;
		}

		Expect(bActualPortRejected && PortForwardConflictsWithWireGuardPort(Forwarded, 25565) && !PortForwardConflictsWithWireGuardPort(Forwarded, 51820), "actual allocated WireGuard port must avoid UDP forwarding ports");
		Expect(!ParseConfig(strValid + strIPv6Forward + ReplaceText(strIPv6Forward, "fd77:0:0::1", "fd77::1"), Forwarded, strError), "equivalent IPv6 listeners rejected");
		for (auto const& pszListen : { "0.0.0.0:25565", "[::]:25565", "127.0.0.1:25565", "10.77.0.2:25565", "8.8.8.8:25565", "localhost:25565",
			"10.77.0.1:0", "10.77.0.1:65536", "10.77.0.1:-1", "[10.77.0.1]:25565", "fd77::1:25565", "[fd77::1]:", "[fd77::1]extra:25565" }) {
			Expect(!ParseConfig(strValid + ReplaceText(strForward, "10.77.0.1:25565", pszListen), Forwarded, strError), "invalid forwarding Listen rejected: " + std::string(pszListen));
		}

		for (auto const& pszTarget : { "10.77.0.2:25565", "0.0.0.0:25565", "localhost:25565", "[::]:25565", "[fd77::1]:25565", "[::ffff:127.0.0.1]:25565", "127.0.0.1:0" }) {
			Expect(!ParseConfig(strValid + ReplaceText(strForward, "127.0.0.1:25565", pszTarget), Forwarded, strError), "non-loopback or invalid Target rejected");
		}

		for (auto const& pszSetting : { "Protocol = SCTP", "Protocol = TCP+UDP", "Protocol = TCP + UDP", "Protocol = TCP\nProtocol = UDP", "MaxConnections = 0", "MaxConnections = 257",
			"ConnectTimeoutSeconds = 0", "ConnectTimeoutSeconds = 301", "IdleTimeoutSeconds = 86401", "Extra = unsupported" }) {
			std::string const strBase = std::string(pszSetting).starts_with("Protocol") ? ReplaceText(strForward, "Protocol = TCP", pszSetting) : strForward + pszSetting + "\n";
			Expect(!ParseConfig(strValid + strBase, Forwarded, strError), "invalid forwarding protocol/duplicate/limit setting rejected");
		}

		Expect(!ParseConfig(strValid + "[PortForward]\nListen = 10.77.0.1:25565\n", Forwarded, strError), "forwarding Target required");
		Expect(!ParseConfig(strValid + "[PortForward]\nTarget = 127.0.0.1:25565\n", Forwarded, strError), "forwarding Listen required");
		Expect(ParseConfig(strValid + ReplaceText(strForward, "Protocol = TCP", "Protocol = tcp"), Forwarded, strError), "TCP protocol is case insensitive");
		Expect(ParseConfig(strValid + ReplaceText(strUDPForward, "Protocol = UDP", "Protocol = udp"), Forwarded, strError) && (Forwarded.m_vecPortForwards[0].m_Protocol == PortForwardProtocol::Udp), "UDP protocol is case insensitive");
		Expect(ParseConfig(strValid + ReplaceText(strIPv6Forward, "[PortForward]", "[PortForward]\nProtocol = UDP"), Forwarded, strError) && (Forwarded.m_vecPortForwards[0].m_strListenAddress == "fd77::1") && (Forwarded.m_vecPortForwards[0].m_Protocol == PortForwardProtocol::Udp), "UDP IPv6 and loopback target are supported");

		std::string strManyForwards = strValid;
		for (std::size_t unForwardIndex = 0; unForwardIndex < kExpectedMaximumPortForwards; ++unForwardIndex) {
			strManyForwards += ReplaceText((unForwardIndex % 2) ? strUDPForward : strForward, "Listen = 10.77.0.1:25565", "Listen = 10.77.0.1:" + std::to_string(kFirstTestForwardPort + unForwardIndex));
		}

		Expect(ParseConfig(strManyForwards, Forwarded, strError) && (Forwarded.m_vecPortForwards.size() == kExpectedMaximumPortForwards), "32 separate TCP/UDP forwarding rules accepted");
		Expect(ValidateCompiledConfig(CompileConfig(Forwarded, 51820, { 51999 }), strError), "32 forwarding rules compile without entering WireGuard configuration");
		Expect(!ParseConfig(strManyForwards + ReplaceText(strForward, "Listen = 10.77.0.1:25565", "Listen = 10.77.0.1:25599"), Forwarded, strError) && (strError.find("At most 32 [PortForward]") != std::string::npos), "33rd forwarding rule rejected during parsing");

		Config TooManyForwards = Forwarded;
		PortForwardConfig ExtraForward = Forwarded.m_vecPortForwards.front();
		ExtraForward.m_unListenPort = static_cast<std::uint16_t>(kFirstTestForwardPort + kExpectedMaximumPortForwards);
		TooManyForwards.m_vecPortForwards.push_back(ExtraForward);
		bool bExtraForwardRejected = false;
		try {
			CompileConfig(TooManyForwards, 51820, { 51999 });
		} catch (std::invalid_argument const& Exception) {
			bExtraForwardRejected = std::string(Exception.what()).find("At most 32 [PortForward]") != std::string::npos;
		}

		Expect(bExtraForwardRejected, "33rd programmatic forwarding rule rejected during compilation");

		std::string const strInternetBase = "[Interface]\nPrivateKey = " + strPrivateKey +
			"\nAddress = 10.77.0.2/32\nDNS = 1.1.1.1, 8.8.8.8\nListenPort = 0\nMTU = 1280\n"
			"[WireSteam]\nAppID = 480\nVirtualPort = 40\nRelayOnly = true\nInternetMode = client\n"
			"[Peer]\nPublicKey = " +
			strPublicKey + "\nAllowedIPs = 0.0.0.0/0\nSteamID = 76561198000000000\n";
		Config Client;
		const bool bInternetClientParsed = ParseConfig(strInternetBase, Client, strError);
		Expect(bInternetClientParsed, "explicit IPv4 Internet client: " + strError);
		Expect((Client.m_InternetMode == InternetMode::Client) && (Client.m_vecDNS[0] == "1.1.1.1"), "Internet client metadata");
		Expect(!ParseConfig(strInternetBase + ReplaceText(strForward, "Listen = 10.77.0.1", "Listen = 10.77.0.2"), Forwarded, strError) && (strError.find("Internet-client") != std::string::npos), "Internet-client per-application transport policy rejects proxy listeners explicitly");

		std::string const strClientCompiled = CompileConfig(Client, 51820, { 51999 });
		Expect((strClientCompiled.find("AllowedIPs = 0.0.0.0/1, 128.0.0.0/1") != std::string::npos) && (strClientCompiled.find("0.0.0.0/0") == std::string::npos), "client compiles split default routes without the WireGuard /0 firewall mode");
		Expect(!ValidateCompiledConfig(strClientCompiled, strError), "compiled Internet routes require explicit client authorization");
		const bool bInternetClientCompiledValid = ValidateCompiledConfig(strClientCompiled, strError, InternetMode::Client);
		Expect(bInternetClientCompiledValid, "authorized compiled Internet client: " + strError);
		Expect(!ValidateCompiledConfig(ReplaceText(strClientCompiled, "0.0.0.0/1, 128.0.0.0/1", "0.0.0.0/0"), strError, InternetMode::Client), "compiled true default rejected");
		Expect(!ValidateCompiledConfig(ReplaceText(strClientCompiled, "0.0.0.0/1, 128.0.0.0/1", "0.0.0.0/1"), strError, InternetMode::Client), "incomplete compiled split default rejected");
		Expect(!ValidateCompiledConfig(strCompiled, strError, InternetMode::Client), "private-only compiled config is not an Internet client");
		Expect(!ParseConfig(ReplaceText(strInternetBase, "InternetMode = client", "InternetMode = off"), Parsed, strError), "public DNS/default routes remain off by default");
		Expect(!ParseConfig(ReplaceText(strInternetBase, "InternetMode = client", "InternetMode = unknown"), Parsed, strError), "unknown Internet mode rejection");
		Expect(!ParseConfig(ReplaceText(strInternetBase, "RelayOnly = true", "RelayOnly = false"), Parsed, strError), "full Internet client requires SDR relay mode");
		Expect(!ParseConfig(ReplaceText(strInternetBase, "DNS = 1.1.1.1, 8.8.8.8\n", ""), Parsed, strError), "Internet client requires explicit DNS");
		for (auto const& pszDNS : { "127.0.0.1", "169.254.1.1", "0.0.0.0", "224.0.0.1", "255.255.255.255", "::1", "2606:4700:4700::1111", "resolver.example" }) {
			Expect(!ParseConfig(ReplaceText(strInternetBase, "1.1.1.1, 8.8.8.8", pszDNS), Parsed, strError), "invalid IPv4 Internet DNS rejection: " + std::string(pszDNS));
		}

		for (auto const& pszRoutes : { "::/0", "0.0.0.0/0, ::/0", "0.0.0.0/1, 128.0.0.0/1", "1.1.1.1/32", "10.77.0.0/24", "0.0.0.0/0, 10.77.0.0/24" }) {
			Expect(!ParseConfig(ReplaceText(strInternetBase, "0.0.0.0/0", pszRoutes), Parsed, strError), "Internet client requires exactly the explicit default: " + std::string(pszRoutes));
		}

		Expect(!ParseConfig(ReplaceText(strInternetBase, "10.77.0.2/32", "fd77::2/128"), Parsed, strError), "IPv6-only Internet interface rejection");
		Expect(!ParseConfig(ReplaceText(strInternetBase, "10.77.0.2/32", "10.77.0.2/32, fd77::2/128"), Parsed, strError), "multi-address Internet interface rejection");
		Expect(!ParseConfig(ReplaceText(strInternetBase, "InternetMode = client", "InternetMode = client\nInternetSubnet = 10.77.0.0/24"), Parsed, strError), "Internet client subnet field rejection");

		std::string const strInternetWithLateSteam = ReplaceText(ReplaceText(strInternetBase, "[WireSteam]\nAppID = 480\nVirtualPort = 40\nRelayOnly = true\nInternetMode = client\n", ""), "SteamID = 76561198000000000\n", "SteamID = 76561198000000000\n[WireSteam]\nInternetMode = client\n");
		Expect(ParseConfig(strInternetWithLateSteam, Parsed, strError), "Internet role parsing is independent of section order");

		std::string const strServerBase = "[Interface]\nPrivateKey = " + strPrivateKey +
			"\nAddress = 10.77.0.1/24\nListenPort = 51820\nMTU = 1280\n[WireSteam]\nInternetMode = server\nInternetSubnet = 10.77.0.0/24\n"
			"[Peer]\nPublicKey = " +
			strPublicKey + "\nAllowedIPs = 10.77.0.2/32\nSteamID = 76561198000000000\nDial = false\n";
		Config Server;
		const bool bInternetServerParsed = ParseConfig(strServerBase, Server, strError);
		Expect(bInternetServerParsed, "explicit IPv4 Internet server: " + strError);
		Expect((Server.m_InternetMode == InternetMode::Server) && (Server.m_strInternetSubnet == "10.77.0.0/24"), "Internet server metadata");
		Expect(ParseConfig(strServerBase + strForward, Forwarded, strError) && (Forwarded.m_vecPortForwards.size() == 1), "Internet gateways support local-service forwarding");

		std::string const strServerCompiled = CompileConfig(Server, 51820, { 51999 });
		const bool bInternetServerCompiledValid = ValidateCompiledConfig(strServerCompiled, strError, InternetMode::Server, Server.m_strInternetSubnet);
		Expect(bInternetServerCompiledValid, "server compiled validation: " + strError);
		Expect(!ValidateCompiledConfig(strServerCompiled, strError, InternetMode::Server), "server compiled authorization requires NAT subnet");
		Expect(!ValidateCompiledConfig(strServerCompiled, strError, InternetMode::Server, "10.78.0.0/24"), "server compiled validation checks supplied NAT subnet");
		Expect(!ParseConfig(ReplaceText(strServerBase, "InternetSubnet = 10.77.0.0/24\n", ""), Parsed, strError), "server requires explicit NAT subnet");
		Expect(!ParseConfig(ReplaceText(strServerBase, "InternetMode = server", "InternetMode = off"), Parsed, strError), "NAT subnet requires server mode");
		for (auto const& pszSubnet : { "0.0.0.0/0", "8.8.8.0/24", "10.77.0.1/24", "10.77.0.0/31", "10.77.0.1/32", "fd77::/64" }) {
			Expect(!ParseConfig(ReplaceText(strServerBase, "InternetSubnet = 10.77.0.0/24", "InternetSubnet = " + std::string(pszSubnet)), Parsed, strError), "invalid Internet NAT subnet rejection: " + std::string(pszSubnet));
		}

		for (auto const& pszRoutes : { "0.0.0.0/0", "10.78.0.2/32", "10.77.0.0/24", "10.77.0.1/32", "10.77.0.0/32", "10.77.0.255/32", "fd77::2/128" }) {
			Expect(!ParseConfig(ReplaceText(strServerBase, "AllowedIPs = 10.77.0.2/32", "AllowedIPs = " + std::string(pszRoutes)), Parsed, strError), "Internet server source address rejection: " + std::string(pszRoutes));
		}

		for (auto const& pszAddress : { "10.78.0.1/24", "10.77.0.0/24", "10.77.0.255/24", "fd77::1/64" }) {
			Expect(!ParseConfig(ReplaceText(strServerBase, "Address = 10.77.0.1/24", "Address = " + std::string(pszAddress)), Parsed, strError), "Internet server interface address rejection: " + std::string(pszAddress));
		}

		Expect((RedactedSummary(Client).find("Internet mode: client") != std::string::npos) && (RedactedSummary(Server).find("Internet NAT subnet: 10.77.0.0/24") != std::string::npos), "Internet summary mode/subnet metadata");

		bool bThrew = false;
		try {
			CompileConfig(Parsed, 51820, {});
		} catch (std::invalid_argument const&) {
			bThrew = true;
		}

		Expect(bThrew, "unassigned endpoint rejection");
	}
} // namespace ws
