#include "preflight.hpp"

// General
#include <winsock2.h>
#include <windows.h>
// IP Helper exposes its IPv6 tables only after the Winsock IPv6 declarations.
#include <ws2tcpip.h>
#include <iphlpapi.h>

// C++
#include <cstddef>
#include <cstring>
#include <utility>

// STL
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

// Project
#include "transport/net.hpp"

namespace ws {
	namespace {
		constexpr std::size_t kIPv4AddressBytes = 0x04;
		constexpr std::size_t kIPv6AddressBytes = 0x10;
		constexpr DWORD kInitialTableBytes = 16384;
		constexpr DWORD kMaximumTableBytes = 16 * 1024 * 1024;
		constexpr std::size_t kMaximumTableAttempts = 5;

		struct WSAScope {
			WSAScope() {
				constexpr BYTE kWinsockMajorVersion = 2;
				constexpr BYTE kWinsockMinorVersion = 2;

				WSADATA StartupData {};
				m_bInitialized = WSAStartup(MAKEWORD(kWinsockMajorVersion, kWinsockMinorVersion), &StartupData) == 0;
			}

			~WSAScope() {
				if (m_bInitialized) {
					WSACleanup();
				}
			}

			WSAScope(WSAScope const&) = delete;
			WSAScope& operator=(WSAScope const&) = delete;

			bool m_bInitialized;
		};

		struct LocalAddress {
			LocalAddress() :
				m_arrBytes {}
			{
				m_nFamily = AF_UNSPEC;
				m_bActive = false;
			}

			int m_nFamily;
			std::array<unsigned char, kIPv6AddressBytes> m_arrBytes;
			std::string m_strInterfaceName;
			bool m_bActive;
		};

		struct LocalBinding {
			LocalBinding() {
				m_unPort = 0;
				m_unProcessID = 0;
			}

			LocalAddress m_Address;
			std::uint16_t m_unPort;
			DWORD m_unProcessID;
		};

		std::string EncodeInterfaceName(wchar_t const* pszName) {
			constexpr int kMaximumNameBytes = 1024;
			constexpr unsigned char kFirstPrintableCharacter = 0x20;

			if (!pszName || !*pszName) {
				return "unnamed interface";
			}

			const int nByteCount = WideCharToMultiByte(CP_UTF8, 0, pszName, -1, nullptr, 0, nullptr, nullptr);
			if ((nByteCount <= 1) || (nByteCount > kMaximumNameBytes)) {
				return "unnamed interface";
			}

			std::string strText = std::string(static_cast<std::size_t>(nByteCount), '\0');
			if (WideCharToMultiByte(CP_UTF8, 0, pszName, -1, strText.data(), nByteCount, nullptr, nullptr) != nByteCount) {
				return "unnamed interface";
			}

			strText.pop_back();
			for (auto& chCharacter : strText) {
				if ((static_cast<unsigned char>(chCharacter) < kFirstPrintableCharacter) || (chCharacter == '"')) {
					chCharacter = ' ';
				}
			}

			return strText;
		}

		bool ParseAddress(std::string const& strAddress, LocalAddress& AddressOut) {
			const std::size_t unSlashOffset = strAddress.find('/');
			std::string const strHost = strAddress.substr(0, unSlashOffset);
			AddressOut.m_nFamily = (strHost.find(':') == std::string::npos) ? AF_INET : AF_INET6;

			return InetPtonA(AddressOut.m_nFamily, strHost.c_str(), AddressOut.m_arrBytes.data()) == 1;
		}

		bool AreAddressesEqual(LocalAddress const& Left, LocalAddress const& Right) {
			return (Left.m_nFamily == Right.m_nFamily) && ((Left.m_nFamily == AF_INET) || (Left.m_nFamily == AF_INET6)) && std::equal(Left.m_arrBytes.begin(), Left.m_arrBytes.begin() + ((Left.m_nFamily == AF_INET) ? kIPv4AddressBytes : kIPv6AddressBytes), Right.m_arrBytes.begin());
		}

		bool IsWildcardAddress(LocalAddress const& AddressValue) {
			const std::size_t unLength = (AddressValue.m_nFamily == AF_INET) ? kIPv4AddressBytes : (AddressValue.m_nFamily == AF_INET6) ? kIPv6AddressBytes : 0;

			return unLength && std::all_of(AddressValue.m_arrBytes.begin(), AddressValue.m_arrBytes.begin() + unLength, [](unsigned char unByte) -> bool {
				return unByte == 0x00;
			});
		}

		bool ReadMappedIPv4Address(LocalAddress const& AddressValue, LocalAddress& AddressOut) {
			constexpr std::size_t kMarkerOffset = 0x0A;
			constexpr unsigned char kMarkerByte = 0xFF;
			constexpr std::size_t kPayloadOffset = kIPv6AddressBytes - kIPv4AddressBytes;

			if ((AddressValue.m_nFamily != AF_INET6) || (AddressValue.m_arrBytes[kMarkerOffset] != kMarkerByte) || (AddressValue.m_arrBytes[kMarkerOffset + 0x01] != kMarkerByte) || !std::all_of(AddressValue.m_arrBytes.begin(), AddressValue.m_arrBytes.begin() + kMarkerOffset, [](unsigned char unByte) -> bool {
				return unByte == 0x00;
			})) {
				return false;
			}

			AddressOut = {};
			AddressOut.m_nFamily = AF_INET;
			std::copy(AddressValue.m_arrBytes.begin() + kPayloadOffset, AddressValue.m_arrBytes.end(), AddressOut.m_arrBytes.begin());

			return true;
		}

		bool DoBindingAddressesOverlap(LocalAddress const& Desired, LocalAddress const& Actual) {
			if (Desired.m_nFamily == Actual.m_nFamily) {
				return IsWildcardAddress(Actual) || AreAddressesEqual(Desired, Actual);
			}

			// Endpoint tables cannot reveal IPV6_V6ONLY. Conservatively treat an IPv6
			// wildcard as dual stack, while reporting this ambiguity in the diagnostic.
			// The forwarder itself creates IPv6 listeners with IPV6_V6ONLY enabled.
			if ((Desired.m_nFamily == AF_INET) && (Actual.m_nFamily == AF_INET6)) {
				if (IsWildcardAddress(Actual)) {
					return true;
				}

				LocalAddress Mapped;
				return ReadMappedIPv4Address(Actual, Mapped) && AreAddressesEqual(Desired, Mapped);
			}

			return false;
		}

		std::string FormatAddress(LocalAddress const& AddressValue) {
			std::array<char, INET6_ADDRSTRLEN> szText {};
			if (!InetNtopA(AddressValue.m_nFamily, AddressValue.m_arrBytes.data(), szText.data(), szText.size())) {
				return "unknown address";
			}

			return (AddressValue.m_nFamily == AF_INET6) ? "[" + std::string(szText.data()) + "]" : std::string(szText.data());
		}

		bool CheckForwardBindings(Config const& Configuration, PortForwardProtocol Protocol, std::vector<LocalBinding> const& vecBindings, std::string& strErrorOut) {
			std::string const strProtocolLabel = (Protocol == PortForwardProtocol::Tcp) ? "TCP" : "UDP";
			for (auto const& Rule : Configuration.m_vecPortForwards) {
				if (Rule.m_Protocol != Protocol) {
					continue;
				}

				LocalAddress Desired;
				if (!ParseAddress(Rule.m_strListenAddress, Desired)) {
					strErrorOut = "Cannot check the configured " + strProtocolLabel + " forwarding address";
					return false;
				}

				for (auto const& Actual : vecBindings) {
					if ((Actual.m_unPort != Rule.m_unListenPort) || !DoBindingAddressesOverlap(Desired, Actual.m_Address)) {
						continue;
					}

					strErrorOut = strProtocolLabel + " forwarding listener " + FormatAddress(Desired) + ":" + std::to_string(Rule.m_unListenPort) +
						" conflicts with an existing " + strProtocolLabel + " endpoint on " + FormatAddress(Actual.m_Address) + ":" + std::to_string(Actual.m_unPort);

					if (Actual.m_unProcessID) {
						strErrorOut += " (process ID " + std::to_string(Actual.m_unProcessID) + ")";
					}

					if ((Desired.m_nFamily == AF_INET) && (Actual.m_Address.m_nFamily == AF_INET6) && IsWildcardAddress(Actual.m_Address)) {
						strErrorOut += "; an IPv6 wildcard listener may also own this IPv4 port";
					}

					strErrorOut += ". Stop the conflicting listener or choose another forwarding Listen port. A service bound only to the loopback Target address can keep using its own port.";
					return false;
				}
			}

			return true;
		}

		bool CheckTCPListeners(Config const& Configuration, std::vector<LocalBinding> const& vecListeners, std::string& strErrorOut) {
			return CheckForwardBindings(Configuration, PortForwardProtocol::Tcp, vecListeners, strErrorOut);
		}

		bool CheckUDPBindings(Config const& Configuration, std::vector<LocalBinding> const& vecBindings, std::string& strErrorOut) {
			return CheckForwardBindings(Configuration, PortForwardProtocol::Udp, vecBindings, strErrorOut);
		}

		bool ReadTCPListeners(int nFamily, std::vector<LocalBinding>& vecListenersOut, std::string& strErrorOut) {
			// OWNER_PID is supported for both families; BASIC tables are not supported
			// by GetExtendedTcpTable for IPv6. These snapshots require no elevation.
			DWORD unSize = kInitialTableBytes;
			std::vector<unsigned char> vecBuffer;
			DWORD unStatus = ERROR_INSUFFICIENT_BUFFER;
			for (std::size_t unAttempt = 0; unAttempt != kMaximumTableAttempts; ++unAttempt) {
				if ((unSize < sizeof(DWORD)) || (unSize > kMaximumTableBytes)) {
					strErrorOut = "Cannot inspect TCP forwarding listeners: response is too large";
					return false;
				}

				vecBuffer.resize(unSize);
				unStatus = GetExtendedTcpTable(vecBuffer.data(), &unSize, FALSE, nFamily, TCP_TABLE_OWNER_PID_LISTENER, 0);
				if (unStatus != ERROR_INSUFFICIENT_BUFFER) {
					break;
				}
			}

			if (unStatus == ERROR_NO_DATA) {
				return true;
			}

			if (unStatus != NO_ERROR) {
				strErrorOut = "Cannot inspect existing " + std::string((nFamily == AF_INET) ? "IPv4" : "IPv6") +
					" TCP forwarding listeners (Windows error " + std::to_string(unStatus) + ")";
				return false;
			}

			DWORD unCount = 0;
			std::memcpy(&unCount, vecBuffer.data(), sizeof(unCount));
			const std::size_t unOffset = (nFamily == AF_INET) ? offsetof(MIB_TCPTABLE_OWNER_PID, table) : offsetof(MIB_TCP6TABLE_OWNER_PID, table);
			const std::size_t unRowSize = (nFamily == AF_INET) ? sizeof(MIB_TCPROW_OWNER_PID) : sizeof(MIB_TCP6ROW_OWNER_PID);
			if ((vecBuffer.size() < unOffset) || (unCount > ((vecBuffer.size() - unOffset) / unRowSize))) {
				strErrorOut = "Cannot inspect TCP forwarding listeners: the returned table is incomplete";
				return false;
			}

			for (std::size_t unRowIndex = 0; unRowIndex < unCount; ++unRowIndex) {
				LocalBinding Entry;
				Entry.m_Address.m_nFamily = nFamily;
				unsigned char const* punRowBytes = vecBuffer.data() + unOffset + unRowIndex * unRowSize;
				if (nFamily == AF_INET) {
					MIB_TCPROW_OWNER_PID Row {};
					std::memcpy(&Row, punRowBytes, sizeof(Row));
					if (Row.dwState != MIB_TCP_STATE_LISTEN) {
						continue;
					}

					std::memcpy(Entry.m_Address.m_arrBytes.data(), &Row.dwLocalAddr, sizeof(Row.dwLocalAddr));
					Entry.m_unPort = ntohs(static_cast<u_short>(Row.dwLocalPort));
					Entry.m_unProcessID = Row.dwOwningPid;
				} else {
					MIB_TCP6ROW_OWNER_PID Row {};
					std::memcpy(&Row, punRowBytes, sizeof(Row));
					if (Row.dwState != MIB_TCP_STATE_LISTEN) {
						continue;
					}

					std::memcpy(Entry.m_Address.m_arrBytes.data(), Row.ucLocalAddr, sizeof(Row.ucLocalAddr));
					Entry.m_unPort = ntohs(static_cast<u_short>(Row.dwLocalPort));
					Entry.m_unProcessID = Row.dwOwningPid;
				}

				vecListenersOut.push_back(std::move(Entry));
			}

			return true;
		}

		bool ReadUDPBindings(int nFamily, std::vector<LocalBinding>& vecBindingsOut, std::string& strErrorOut) {
			DWORD unSize = kInitialTableBytes;
			std::vector<unsigned char> vecBuffer;
			DWORD unStatus = ERROR_INSUFFICIENT_BUFFER;
			for (std::size_t unAttempt = 0; unAttempt != kMaximumTableAttempts; ++unAttempt) {
				if ((unSize < sizeof(DWORD)) || (unSize > kMaximumTableBytes)) {
					strErrorOut = "Cannot inspect UDP forwarding endpoints: response is too large";
					return false;
				}

				vecBuffer.resize(unSize);
				unStatus = GetExtendedUdpTable(vecBuffer.data(), &unSize, FALSE, nFamily, UDP_TABLE_OWNER_PID, 0);
				if (unStatus != ERROR_INSUFFICIENT_BUFFER) {
					break;
				}
			}

			if (unStatus == ERROR_NO_DATA) {
				return true;
			}

			if (unStatus != NO_ERROR) {
				strErrorOut = "Cannot inspect existing " + std::string((nFamily == AF_INET) ? "IPv4" : "IPv6") +
					" UDP forwarding endpoints (Windows error " + std::to_string(unStatus) + ")";
				return false;
			}

			DWORD unCount = 0;
			std::memcpy(&unCount, vecBuffer.data(), sizeof(unCount));
			const std::size_t unOffset = (nFamily == AF_INET) ? offsetof(MIB_UDPTABLE_OWNER_PID, table) : offsetof(MIB_UDP6TABLE_OWNER_PID, table);
			const std::size_t unRowSize = (nFamily == AF_INET) ? sizeof(MIB_UDPROW_OWNER_PID) : sizeof(MIB_UDP6ROW_OWNER_PID);
			if ((vecBuffer.size() < unOffset) || (unCount > ((vecBuffer.size() - unOffset) / unRowSize))) {
				strErrorOut = "Cannot inspect UDP forwarding endpoints: the returned table is incomplete";
				return false;
			}

			for (std::size_t unRowIndex = 0; unRowIndex < unCount; ++unRowIndex) {
				LocalBinding Entry;
				Entry.m_Address.m_nFamily = nFamily;
				unsigned char const* punRowBytes = vecBuffer.data() + unOffset + unRowIndex * unRowSize;
				if (nFamily == AF_INET) {
					MIB_UDPROW_OWNER_PID Row {};
					std::memcpy(&Row, punRowBytes, sizeof(Row));
					std::memcpy(Entry.m_Address.m_arrBytes.data(), &Row.dwLocalAddr, sizeof(Row.dwLocalAddr));
					Entry.m_unPort = ntohs(static_cast<u_short>(Row.dwLocalPort));
					Entry.m_unProcessID = Row.dwOwningPid;
				} else {
					MIB_UDP6ROW_OWNER_PID Row {};
					std::memcpy(&Row, punRowBytes, sizeof(Row));
					std::memcpy(Entry.m_Address.m_arrBytes.data(), Row.ucLocalAddr, sizeof(Row.ucLocalAddr));
					Entry.m_unPort = ntohs(static_cast<u_short>(Row.dwLocalPort));
					Entry.m_unProcessID = Row.dwOwningPid;
				}

				vecBindingsOut.push_back(std::move(Entry));
			}

			return true;
		}

		bool CheckAddresses(Config const& Configuration, std::vector<LocalAddress> const& vecLocalAddresses, std::string& strErrorOut) {
			for (auto const& strAddress : Configuration.m_vecAddresses) {
				LocalAddress Desired;
				if (!ParseAddress(strAddress, Desired)) {
					strErrorOut = "Cannot check the configured VPN address";
					return false;
				}

				auto itMatch = std::find_if(vecLocalAddresses.begin(), vecLocalAddresses.end(), [&](LocalAddress const& Actual) -> bool {
					return Actual.m_bActive && AreAddressesEqual(Desired, Actual);
				});
				if (itMatch != vecLocalAddresses.end()) {
					strErrorOut = "VPN address " + strAddress.substr(0, strAddress.find('/')) + " is already assigned to active interface \"" +
						itMatch->m_strInterfaceName + "\". Stop the existing tunnel in WireSteam or WireGuard before activating this profile.";
					return false;
				}
			}

			return true;
		}

		bool ReadLocalAddresses(std::vector<LocalAddress>& vecLocalAddressesOut, std::string& strErrorOut) {
			constexpr std::size_t kMaximumAddressAttempts = 4;
			constexpr ULONG kAdapterFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

			ULONG unSize = kInitialTableBytes;
			std::vector<unsigned char> vecBuffer(unSize);
			ULONG unStatus = ERROR_BUFFER_OVERFLOW;
			for (std::size_t unAttempt = 0; unAttempt < kMaximumAddressAttempts; ++unAttempt) {
				vecBuffer.resize(unSize);
				unStatus = GetAdaptersAddresses(AF_UNSPEC, kAdapterFlags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(vecBuffer.data()), &unSize);
				if (unStatus != ERROR_BUFFER_OVERFLOW) {
					break;
				}

				if (!unSize || (unSize > kMaximumTableBytes)) {
					strErrorOut = "Cannot inspect active adapter addresses: response is too large";
					return false;
				}
			}

			if (unStatus == ERROR_NO_DATA) {
				return true;
			}

			if (unStatus != NO_ERROR) {
				strErrorOut = "Cannot inspect active adapter addresses (Windows error " + std::to_string(unStatus) + ")";
				return false;
			}

			for (PIP_ADAPTER_ADDRESSES pAdapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(vecBuffer.data()); pAdapter; pAdapter = pAdapter->Next) {
				if (pAdapter->OperStatus != IfOperStatusUp) {
					continue;
				}

				std::string const strName = EncodeInterfaceName(pAdapter->FriendlyName);
				for (PIP_ADAPTER_UNICAST_ADDRESS pAddress = pAdapter->FirstUnicastAddress; pAddress; pAddress = pAddress->Next) {
					if (!pAddress->Address.lpSockaddr) {
						continue;
					}

					LocalAddress Entry;
					Entry.m_nFamily = pAddress->Address.lpSockaddr->sa_family;
					Entry.m_bActive = true;
					Entry.m_strInterfaceName = strName;
					if ((Entry.m_nFamily == AF_INET) && (pAddress->Address.iSockaddrLength >= sizeof(sockaddr_in))) {
						sockaddr_in const* pIPv4Address = reinterpret_cast<sockaddr_in const*>(pAddress->Address.lpSockaddr);
						std::memcpy(Entry.m_arrBytes.data(), &pIPv4Address->sin_addr, sizeof(pIPv4Address->sin_addr));
					} else if ((Entry.m_nFamily == AF_INET6) && (pAddress->Address.iSockaddrLength >= sizeof(sockaddr_in6))) {
						sockaddr_in6 const* pIPv6Address = reinterpret_cast<sockaddr_in6 const*>(pAddress->Address.lpSockaddr);
						std::memcpy(Entry.m_arrBytes.data(), &pIPv6Address->sin6_addr, sizeof(pIPv6Address->sin6_addr));
					} else {
						continue;
					}

					vecLocalAddressesOut.push_back(std::move(Entry));
				}
			}

			return true;
		}

		bool CheckUDPPort(std::uint16_t unPort, int nFamily, std::string& strErrorOut) {
			st::net::Socket SocketValue(::socket(nFamily, SOCK_DGRAM, IPPROTO_UDP));
			if (!SocketValue) {
				const int nErrorCode = WSAGetLastError();
				if ((nFamily == AF_INET6) && ((nErrorCode == WSAEAFNOSUPPORT) || (nErrorCode == WSAEPROTONOSUPPORT))) {
					return true;
				}

				strErrorOut = "Cannot inspect WireGuard ListenPort " + std::to_string(unPort) + " (Winsock error " + std::to_string(nErrorCode) + ")";
				return false;
			}

			const BOOL nEnabled = TRUE;
			if (setsockopt(SocketValue.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&nEnabled), sizeof(nEnabled)) ||
				((nFamily == AF_INET6) && setsockopt(SocketValue.get(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char const*>(&nEnabled), sizeof(nEnabled)))) {
				const int nOptionErrorCode = WSAGetLastError();
				strErrorOut = "Cannot inspect WireGuard ListenPort " + std::to_string(unPort) + " (Winsock error " + std::to_string(nOptionErrorCode) + ")";
				return false;
			}

			sockaddr_storage Storage {};
			int nAddressLength = 0;
			if (nFamily == AF_INET) {
				PSOCKADDR_IN pIPv4Address = reinterpret_cast<PSOCKADDR_IN>(&Storage);
				pIPv4Address->sin_family = AF_INET;
				pIPv4Address->sin_port = htons(unPort);
				pIPv4Address->sin_addr.s_addr = htonl(INADDR_ANY);
				nAddressLength = sizeof(*pIPv4Address);
			} else {
				PSOCKADDR_IN6 pIPv6Address = reinterpret_cast<PSOCKADDR_IN6>(&Storage);
				pIPv6Address->sin6_family = AF_INET6;
				pIPv6Address->sin6_port = htons(unPort);
				nAddressLength = sizeof(*pIPv6Address);
			}

			if (::bind(SocketValue.get(), reinterpret_cast<sockaddr const*>(&Storage), nAddressLength) == 0) {
				return true;
			}

			const int nErrorCode = WSAGetLastError();
			strErrorOut = "WireGuard ListenPort " + std::to_string(unPort) + "/UDP is already in use or unavailable (Winsock error " + std::to_string(nErrorCode) + "). Stop the existing tunnel or the application using this port before activating this profile. "
				"Alternatively, choose a free ListenPort; 0 selects an automatic port.";

			return false;
		}

		void Expect(bool bCondition, std::string const& strMessage) {
			if (!bCondition) {
				throw std::runtime_error("Preflight regression: " + strMessage);
			}
		}
	} // namespace

	bool CheckAdapterConflicts(Config const& Configuration, std::string& strErrorOut) {
		strErrorOut.clear();
		WSAScope Winsock;
		if (!Winsock.m_bInitialized) {
			strErrorOut = "Cannot initialize Windows networking for local adapter checks";
			return false;
		}

		std::vector<LocalAddress> vecAddresses;
		if (!ReadLocalAddresses(vecAddresses, strErrorOut) || !CheckAddresses(Configuration, vecAddresses, strErrorOut)) {
			return false;
		}

		if (Configuration.m_unListenPort && (!CheckUDPPort(Configuration.m_unListenPort, AF_INET, strErrorOut) || !CheckUDPPort(Configuration.m_unListenPort, AF_INET6, strErrorOut))) {
			return false;
		}

		auto HasProtocol = [&](PortForwardProtocol Protocol) -> bool {
			return std::any_of(Configuration.m_vecPortForwards.begin(), Configuration.m_vecPortForwards.end(), [&](PortForwardConfig const& Rule) -> bool {
				return Rule.m_Protocol == Protocol;
			});
		};

		if (HasProtocol(PortForwardProtocol::Tcp)) {
			std::vector<LocalBinding> vecListeners;
			if (!ReadTCPListeners(AF_INET, vecListeners, strErrorOut) || !ReadTCPListeners(AF_INET6, vecListeners, strErrorOut) || !CheckTCPListeners(Configuration, vecListeners, strErrorOut)) {
				return false;
			}
		}

		if (HasProtocol(PortForwardProtocol::Udp)) {
			std::vector<LocalBinding> vecBindings;
			if (!ReadUDPBindings(AF_INET, vecBindings, strErrorOut) || !ReadUDPBindings(AF_INET6, vecBindings, strErrorOut) || !CheckUDPBindings(Configuration, vecBindings, strErrorOut)) {
				return false;
			}
		}

		return true;
	}

	void RunPreflightTests() {
		constexpr std::uint16_t kForwardTestPort = 25565;
		constexpr std::uint16_t kAlternateTestPort = 25566;
		constexpr std::size_t kMaximumNameBytes = 1024;
		constexpr std::size_t kMaximumASCIINameCharacters = kMaximumNameBytes - 1;

		WSAScope Winsock;
		Expect(Winsock.m_bInitialized, "Winsock initialization failed");

		Expect(EncodeInterfaceName(nullptr) == "unnamed interface", "null interface name did not use its fallback");
		Expect(EncodeInterfaceName(L"") == "unnamed interface", "empty interface name did not use its fallback");
		Expect(EncodeInterfaceName(L"Local\t\"VPN\"") == "Local  VPN ", "interface name controls or quotes were not sanitized");
		Expect(EncodeInterfaceName(L"Interface \u03A9") == "Interface \xCE\xA9", "interface name UTF8 bytes changed");
		std::wstring const wstrMaximumName = std::wstring(kMaximumASCIINameCharacters, L'A');
		Expect(EncodeInterfaceName(wstrMaximumName.c_str()) == std::string(kMaximumASCIINameCharacters, 'A'), "maximum interface name byte count was rejected");
		std::wstring const wstrOversizedName = std::wstring(kMaximumNameBytes, L'A');
		Expect(EncodeInterfaceName(wstrOversizedName.c_str()) == "unnamed interface", "oversized interface name bypassed the byte limit");

		Config Configuration;
		Configuration.m_unListenPort = 0;
		Configuration.m_vecAddresses = { "10.253.242.1/32", "fd77:abcd::1/128" };
		LocalAddress IPv4Address;
		LocalAddress IPv6Address;
		Expect(ParseAddress("10.253.242.1", IPv4Address) && ParseAddress("fd77:abcd:0:0::1", IPv6Address), "test addresses are invalid");
		IPv4Address.m_strInterfaceName = "Existing IPv4 tunnel";
		IPv4Address.m_bActive = true;
		IPv6Address.m_strInterfaceName = "Existing IPv6 tunnel";
		IPv6Address.m_bActive = true;

		std::string strError;
		Expect(!CheckAddresses(Configuration, { IPv4Address }, strError) && (strError.find("10.253.242.1") != std::string::npos) && (strError.find(IPv4Address.m_strInterfaceName) != std::string::npos), "active IPv4 conflict was not explained");
		Expect(!CheckAddresses(Configuration, { IPv6Address }, strError) && (strError.find(IPv6Address.m_strInterfaceName) != std::string::npos), "equivalent IPv6 notation bypassed conflict detection");
		IPv4Address.m_bActive = false;
		IPv6Address.m_bActive = false;
		Expect(CheckAddresses(Configuration, { IPv4Address, IPv6Address }, strError), "inactive adapters incorrectly blocked activation");
		Expect(ParseAddress("10.253.242.2", IPv4Address), "unrelated test address is invalid");
		IPv4Address.m_bActive = true;
		Expect(CheckAddresses(Configuration, { IPv4Address }, strError), "unrelated active address incorrectly blocked activation");

		Configuration.m_vecAddresses.clear();
		st::net::Socket Occupied = st::net::Bind({ "127.0.0.1", 0 }, SOCK_DGRAM, strError);
		Expect(static_cast<bool>(Occupied), "cannot bind an owned loopback test socket");
		Configuration.m_unListenPort = st::net::Bound(Occupied.get()).m_unPort;
		const std::uint16_t unFixedPort = Configuration.m_unListenPort;
		Expect((unFixedPort != 0) && !CheckAdapterConflicts(Configuration, strError) && (strError.find(std::to_string(unFixedPort)) != std::string::npos) && (strError.find("Stop the existing tunnel") != std::string::npos), "occupied fixed UDP port was not rejected with recovery advice");
		Expect(Configuration.m_unListenPort == unFixedPort, "preflight silently changed the configured port");
		Configuration.m_unListenPort = 0;
		Expect(CheckAdapterConflicts(Configuration, strError) && strError.empty(), "automatic ListenPort was rejected or retained an old diagnostic");
		st::net::Socket OccupiedIPv6 = st::net::Bind({ "::1", 0 }, SOCK_DGRAM, strError);
		if (OccupiedIPv6) {
			const std::uint16_t unIPv6Port = st::net::Bound(OccupiedIPv6.get()).m_unPort;
			Expect((unIPv6Port != 0) && !CheckUDPPort(unIPv6Port, AF_INET6, strError) && (strError.find(std::to_string(unIPv6Port)) != std::string::npos), "occupied IPv6 UDP port was not rejected");
		}

		auto CreateBinding = [](std::string const& strAddress, std::uint16_t unPort, DWORD unProcessID = 1234) -> LocalBinding {
			LocalBinding Binding;
			Expect(ParseAddress(strAddress, Binding.m_Address), "invalid TCP fixture address");
			Binding.m_unPort = unPort;
			Binding.m_unProcessID = unProcessID;

			return Binding;
		};

		PortForwardConfig ForwardRule;
		ForwardRule.m_strListenAddress = "10.253.242.1";
		ForwardRule.m_unListenPort = kForwardTestPort;
		ForwardRule.m_strTargetAddress = "127.0.0.1";
		ForwardRule.m_unTargetPort = kForwardTestPort;
		Configuration.m_vecPortForwards = { ForwardRule };
		Expect(CheckTCPListeners(Configuration, { CreateBinding("127.0.0.1", kForwardTestPort) }, strError), "loopback-only target using the same port incorrectly blocked VPN publication");
		Expect(CheckTCPListeners(Configuration, { CreateBinding("10.253.242.2", kForwardTestPort), CreateBinding("10.253.242.1", kAlternateTestPort) }, strError), "a different TCP address or port incorrectly blocked VPN publication");
		Expect(!CheckTCPListeners(Configuration, { CreateBinding("10.253.242.1", kForwardTestPort) }, strError) && (strError.find("25565") != std::string::npos) && (strError.find("process ID 1234") != std::string::npos), "exact TCP listener collision was not explained");
		Expect(!CheckTCPListeners(Configuration, { CreateBinding("0.0.0.0", kForwardTestPort) }, strError), "IPv4 wildcard collision was not rejected");
		Expect(!CheckTCPListeners(Configuration, { CreateBinding("::", kForwardTestPort) }, strError) && (strError.find("may also own this IPv4 port") != std::string::npos), "ambiguous IPv6 wildcard was not conservatively explained for IPv4 publication");
		Expect(!CheckTCPListeners(Configuration, { CreateBinding("::ffff:10.253.242.1", kForwardTestPort) }, strError), "IPv4-mapped listener collision was not rejected");
		Configuration.m_vecPortForwards[0].m_strListenAddress = "fd77:abcd::1";
		Expect(CheckTCPListeners(Configuration, { CreateBinding("0.0.0.0", kForwardTestPort), CreateBinding("::1", kForwardTestPort) }, strError), "IPv4 wildcard or IPv6 loopback incorrectly blocked a distinct IPv6-only VPN address");
		Expect(!CheckTCPListeners(Configuration, { CreateBinding("fd77:abcd:0:0::1", kForwardTestPort) }, strError), "equivalent IPv6 notation bypassed TCP listener conflict detection");
		Expect(!CheckTCPListeners(Configuration, { CreateBinding("::", kForwardTestPort) }, strError), "IPv6 wildcard collision was not rejected");

		st::net::Socket OwnedTCP = st::net::Bind({ "127.0.0.1", 0 }, SOCK_STREAM, strError);
		Expect(static_cast<bool>(OwnedTCP) && (listen(OwnedTCP.get(), 1) == 0), "cannot create an owned loopback TCP listener");
		const std::uint16_t unOwnedTCPPort = st::net::Bound(OwnedTCP.get()).m_unPort;
		std::vector<LocalBinding> vecActualListeners;
		const bool bTCPListenersRead = ReadTCPListeners(AF_INET, vecActualListeners, strError) && ReadTCPListeners(AF_INET6, vecActualListeners, strError);
		Expect(bTCPListenersRead, "cannot inspect owned TCP listener: " + strError);
		Configuration.m_vecPortForwards[0].m_strListenAddress = "127.0.0.1";
		Configuration.m_vecPortForwards[0].m_unListenPort = unOwnedTCPPort;
		Expect((unOwnedTCPPort != 0) && !CheckTCPListeners(Configuration, vecActualListeners, strError) && (strError.find(std::to_string(GetCurrentProcessId())) != std::string::npos), "TCP table omitted an owned IPv4 loopback listener or decoded its port incorrectly");
		Expect(!CheckAdapterConflicts(Configuration, strError) && (strError.find("TCP forwarding listener") != std::string::npos), "public preflight did not check configured TCP listener conflicts");
		Configuration.m_vecPortForwards[0].m_strListenAddress = "10.253.242.1";
		Configuration.m_vecPortForwards[0].m_unTargetPort = unOwnedTCPPort;
		Expect(CheckTCPListeners(Configuration, vecActualListeners, strError), "an owned loopback target incorrectly blocked publishing the same TCP port on a future VPN address");
		st::net::Socket OwnedTCPIPv6 = st::net::Bind({ "::1", 0 }, SOCK_STREAM, strError);
		if (OwnedTCPIPv6) {
			Expect(listen(OwnedTCPIPv6.get(), 1) == 0, "cannot listen on an owned IPv6 loopback socket");
			vecActualListeners.clear();
			Expect(ReadTCPListeners(AF_INET6, vecActualListeners, strError), "cannot inspect owned IPv6 TCP listener");
			Configuration.m_vecPortForwards[0].m_strListenAddress = "::1";
			Configuration.m_vecPortForwards[0].m_unListenPort = st::net::Bound(OwnedTCPIPv6.get()).m_unPort;
			Expect(!CheckTCPListeners(Configuration, vecActualListeners, strError), "TCP table omitted an owned IPv6 loopback listener");
		}

		PortForwardConfig UDPForwardRule = ForwardRule;
		UDPForwardRule.m_Protocol = PortForwardProtocol::Udp;
		Configuration.m_vecPortForwards = { UDPForwardRule };
		Expect(CheckUDPBindings(Configuration, { CreateBinding("127.0.0.1", kForwardTestPort) }, strError), "UDP loopback-only target using the same port incorrectly blocked VPN publication");
		Expect(CheckUDPBindings(Configuration, { CreateBinding("10.253.242.2", kForwardTestPort), CreateBinding("10.253.242.1", kAlternateTestPort) }, strError), "a different UDP address or port incorrectly blocked VPN publication");
		Expect(!CheckUDPBindings(Configuration, { CreateBinding("10.253.242.1", kForwardTestPort) }, strError) && (strError.find("UDP forwarding listener") != std::string::npos) && (strError.find("process ID 1234") != std::string::npos), "exact UDP binding collision was not explained");
		Expect(!CheckUDPBindings(Configuration, { CreateBinding("0.0.0.0", kForwardTestPort) }, strError), "IPv4 UDP wildcard collision was not rejected");
		Expect(!CheckUDPBindings(Configuration, { CreateBinding("::", kForwardTestPort) }, strError) && (strError.find("may also own this IPv4 port") != std::string::npos), "ambiguous IPv6 UDP wildcard was not conservatively explained for IPv4 publication");
		Expect(!CheckUDPBindings(Configuration, { CreateBinding("::ffff:10.253.242.1", kForwardTestPort) }, strError), "IPv4-mapped UDP binding collision was not rejected");
		Expect(CheckTCPListeners(Configuration, { CreateBinding("10.253.242.1", kForwardTestPort) }, strError), "a TCP binding incorrectly blocked a UDP rule on the same endpoint");
		Configuration.m_vecPortForwards = { ForwardRule };
		Expect(CheckUDPBindings(Configuration, { CreateBinding("10.253.242.1", kForwardTestPort) }, strError), "a UDP binding incorrectly blocked a TCP rule on the same endpoint");
		Configuration.m_vecPortForwards = { ForwardRule, UDPForwardRule };
		Expect(!CheckTCPListeners(Configuration, { CreateBinding("10.253.242.1", kForwardTestPort) }, strError) && !CheckUDPBindings(Configuration, { CreateBinding("10.253.242.1", kForwardTestPort) }, strError), "mixed TCP/UDP rules on the same endpoint did not retain independent conflict checks");
		Configuration.m_vecPortForwards = { UDPForwardRule };
		Configuration.m_vecPortForwards[0].m_strListenAddress = "fd77:abcd::1";
		Expect(CheckUDPBindings(Configuration, { CreateBinding("0.0.0.0", kForwardTestPort), CreateBinding("::1", kForwardTestPort) }, strError), "IPv4 UDP wildcard or IPv6 loopback incorrectly blocked a distinct IPv6-only VPN address");
		Expect(!CheckUDPBindings(Configuration, { CreateBinding("fd77:abcd:0:0::1", kForwardTestPort) }, strError), "equivalent IPv6 notation bypassed UDP binding conflict detection");
		Expect(!CheckUDPBindings(Configuration, { CreateBinding("::", kForwardTestPort) }, strError), "IPv6 UDP wildcard collision was not rejected");

		const std::uint16_t unOwnedUDPPort = st::net::Bound(Occupied.get()).m_unPort;
		std::vector<LocalBinding> vecActualUDP;
		const bool bUDPBindingsRead = ReadUDPBindings(AF_INET, vecActualUDP, strError) && ReadUDPBindings(AF_INET6, vecActualUDP, strError);
		Expect(bUDPBindingsRead, "cannot inspect owned UDP binding: " + strError);
		Configuration.m_vecPortForwards[0].m_strListenAddress = "127.0.0.1";
		Configuration.m_vecPortForwards[0].m_unListenPort = unOwnedUDPPort;
		Expect((unOwnedUDPPort != 0) && !CheckUDPBindings(Configuration, vecActualUDP, strError) && (strError.find(std::to_string(GetCurrentProcessId())) != std::string::npos), "UDP table omitted an owned IPv4 loopback binding or decoded its port incorrectly");
		Expect(!CheckAdapterConflicts(Configuration, strError) && (strError.find("UDP forwarding listener") != std::string::npos), "public preflight did not check configured UDP binding conflicts");
		Configuration.m_vecPortForwards[0].m_strListenAddress = "10.253.242.1";
		Configuration.m_vecPortForwards[0].m_unTargetPort = unOwnedUDPPort;
		Expect(CheckUDPBindings(Configuration, vecActualUDP, strError), "an owned loopback target incorrectly blocked publishing the same UDP port on a future VPN address");

		if (OccupiedIPv6) {
			vecActualUDP.clear();
			Expect(ReadUDPBindings(AF_INET6, vecActualUDP, strError), "cannot inspect owned IPv6 UDP binding");
			Configuration.m_vecPortForwards[0].m_strListenAddress = "::1";
			Configuration.m_vecPortForwards[0].m_unListenPort = st::net::Bound(OccupiedIPv6.get()).m_unPort;
			Expect(!CheckUDPBindings(Configuration, vecActualUDP, strError), "UDP table omitted an owned IPv6 loopback binding");
		}

		std::cout << "PASS local adapter preflight: IPv4/IPv6 address and WireGuard UDP conflicts, independent TCP/UDP forwarding bindings, exact/wildcard overlaps, loopback target coexistence and owned IPv4/IPv6 TCP/UDP tables\n";
	}
} // namespace ws
