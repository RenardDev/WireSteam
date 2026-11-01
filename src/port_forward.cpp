#include "port_forward.hpp"

// General
#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>

// C++
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <utility>

// STL
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iterator>
#include <mutex>
#include <thread>

namespace ws {
	namespace {
		using Clock = std::chrono::steady_clock;
		constexpr std::size_t kBufferBytes = 32 * 1024;
		constexpr std::size_t kMaximumSessions = 256;
		constexpr std::size_t kMaximumRules = 32;
		constexpr std::size_t kMaximumDatagramBytes = 65507;
		constexpr int kPollMilliseconds = 50;

		class Socket {
		public:
			Socket() {
				m_hSocket = INVALID_SOCKET;
			}

			explicit Socket(SOCKET hSocket) :
				m_hSocket(hSocket)
			{
			}

			Socket(Socket&& Other) noexcept :
				m_hSocket(std::exchange(Other.m_hSocket, INVALID_SOCKET))
			{
			}

			Socket(Socket const&) = delete;

			~Socket() {
				Reset();
			}

			Socket& operator=(Socket&& Other) noexcept {
				if (this != &Other) {
					Reset();
					m_hSocket = std::exchange(Other.m_hSocket, INVALID_SOCKET);
				}

				return *this;
			}

			Socket& operator=(Socket const&) = delete;

			void Reset() noexcept {
				if (m_hSocket != INVALID_SOCKET) {
					closesocket(m_hSocket);
					m_hSocket = INVALID_SOCKET;
				}
			}

			SOCKET Get() const {
				return m_hSocket;
			}

			explicit operator bool() const {
				return m_hSocket != INVALID_SOCKET;
			}

		private:
			SOCKET m_hSocket;
		};

		struct Endpoint {
			Endpoint() {
				m_Address = {};
				m_nAddressBytes = 0;
				m_nFamily = AF_UNSPEC;
			}

			sockaddr_storage m_Address;
			int m_nAddressBytes;
			int m_nFamily;
		};

		bool ParseNumericEndpoint(std::string const& strAddress, std::uint16_t unPort, Endpoint& EndpointOut) {
			PSOCKADDR_IN pIPv4Address = reinterpret_cast<PSOCKADDR_IN>(&EndpointOut.m_Address);
			if (InetPtonA(AF_INET, strAddress.c_str(), &pIPv4Address->sin_addr) == 1) {
				pIPv4Address->sin_family = AF_INET;
				pIPv4Address->sin_port = htons(unPort);
				EndpointOut.m_nFamily = AF_INET;
				EndpointOut.m_nAddressBytes = sizeof(sockaddr_in);
				return true;
			}

			EndpointOut.m_Address = {};
			PSOCKADDR_IN6 pIPv6Address = reinterpret_cast<PSOCKADDR_IN6>(&EndpointOut.m_Address);
			if (InetPtonA(AF_INET6, strAddress.c_str(), &pIPv6Address->sin6_addr) == 1) {
				pIPv6Address->sin6_family = AF_INET6;
				pIPv6Address->sin6_port = htons(unPort);
				EndpointOut.m_nFamily = AF_INET6;
				EndpointOut.m_nAddressBytes = sizeof(sockaddr_in6);
				return true;
			}

			return false;
		}

		bool IsUnspecifiedEndpoint(Endpoint const& EndpointValue) {
			if (EndpointValue.m_nFamily == AF_INET) {
				return reinterpret_cast<sockaddr_in const*>(&EndpointValue.m_Address)->sin_addr.s_addr == htonl(INADDR_ANY);
			}

			return IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<sockaddr_in6 const*>(&EndpointValue.m_Address)->sin6_addr) != 0;
		}

		bool IsLoopbackEndpoint(Endpoint const& EndpointValue) {
			constexpr int kIPv4FirstOctetShift = 0x18;
			constexpr std::uint32_t kIPv4LoopbackPrefix = 0x7F;

			if (EndpointValue.m_nFamily == AF_INET) {
				return (ntohl(reinterpret_cast<sockaddr_in const*>(&EndpointValue.m_Address)->sin_addr.s_addr) >> kIPv4FirstOctetShift) == kIPv4LoopbackPrefix;
			}

			return IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<sockaddr_in6 const*>(&EndpointValue.m_Address)->sin6_addr) != 0;
		}

		std::string FormatEndpoint(std::string const& strAddress, std::uint16_t unPort) {
			return ((strAddress.find(':') == std::string::npos) ? strAddress : "[" + strAddress + "]") + ":" + std::to_string(unPort);
		}

		bool WouldBlock(int nErrorCode) {
			return (nErrorCode == WSAEWOULDBLOCK) || (nErrorCode == WSAEINTR);
		}

		bool SetNonblocking(SOCKET hSocket) {
			u_long unEnabled = 1;

			return ioctlsocket(hSocket, FIONBIO, &unEnabled) == 0;
		}

		bool ConfigureUDPBuffers(SOCKET hSocket) {
			// Fixed kernel limits allow several maximum-sized datagrams in a burst,
			// while keeping the shared 256-mapping cap's memory use bounded.
			constexpr int kReceiveBufferBytes = 256 * 1024;
			constexpr int kSendBufferBytes = 64 * 1024;

			return (setsockopt(hSocket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&kReceiveBufferBytes), sizeof(kReceiveBufferBytes)) == 0) && (setsockopt(hSocket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char const*>(&kSendBufferBytes), sizeof(kSendBufferBytes)) == 0);
		}

		Socket CreateSocket(int nFamily, bool bUDP = false) {
			return Socket(WSASocketW(nFamily, bUDP ? SOCK_DGRAM : SOCK_STREAM, bUDP ? IPPROTO_UDP : IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
		}

		bool AreRemoteAddressesEqual(sockaddr_storage const& LeftAddress, sockaddr_storage const& RightAddress) {
			if (LeftAddress.ss_family != RightAddress.ss_family) {
				return false;
			}

			if (LeftAddress.ss_family == AF_INET) {
				sockaddr_in const& LeftIPv4Address = reinterpret_cast<sockaddr_in const&>(LeftAddress);
				sockaddr_in const& RightIPv4Address = reinterpret_cast<sockaddr_in const&>(RightAddress);

				return (LeftIPv4Address.sin_port == RightIPv4Address.sin_port) && (LeftIPv4Address.sin_addr.s_addr == RightIPv4Address.sin_addr.s_addr);
			}

			sockaddr_in6 const& LeftIPv6Address = reinterpret_cast<sockaddr_in6 const&>(LeftAddress);
			sockaddr_in6 const& RightIPv6Address = reinterpret_cast<sockaddr_in6 const&>(RightAddress);

			return (LeftIPv6Address.sin6_port == RightIPv6Address.sin6_port) && (LeftIPv6Address.sin6_scope_id == RightIPv6Address.sin6_scope_id) && (std::memcmp(&LeftIPv6Address.sin6_addr, &RightIPv6Address.sin6_addr, sizeof(LeftIPv6Address.sin6_addr)) == 0);
		}

		void EnableTCPNoDelay(SOCKET hSocket) {
			const BOOL nEnabled = TRUE;
			// This optional latency hint does not affect forwarding if the socket provider rejects it.
			setsockopt(hSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&nEnabled), sizeof(nEnabled));
		}

		struct Buffer {
			Buffer() :
				m_arrBytes {}
			{
				m_unBegin = 0;
				m_unEnd = 0;
			}

			void Compact() {
				if (m_unBegin == 0) {
					return;
				}

				if (m_unBegin != m_unEnd) {
					std::memmove(m_arrBytes.data(), m_arrBytes.data() + m_unBegin, m_unEnd - m_unBegin);
				}

				m_unEnd -= m_unBegin;
				m_unBegin = 0;
			}

			bool IsEmpty() const {
				return m_unBegin == m_unEnd;
			}

			bool IsFull() const {
				return (m_unEnd - m_unBegin) == m_arrBytes.size();
			}

			std::array<char, kBufferBytes> m_arrBytes;
			std::size_t m_unBegin;
			std::size_t m_unEnd;
		};
	} // namespace

	struct PortForwarder::Implementation {
		struct Listener {
			Listener(PortForwardConfig const& Configuration, Endpoint const& Target, Socket&& SocketValue, std::size_t unActiveSessions) :
				m_Configuration(Configuration),
				m_Target(Target),
				m_Socket(std::move(SocketValue)),
				m_unActiveSessions(unActiveSessions)
			{
			}

			PortForwardConfig m_Configuration;
			Endpoint m_Target;
			Socket m_Socket;
			std::size_t m_unActiveSessions;
		};

		struct Session {
			Session() {
				m_unRuleIndex = 0;
				m_bConnecting = false;
				m_bClientEOF = false;
				m_bTargetEOF = false;
				m_bClientShutdown = false;
				m_bTargetShutdown = false;
				m_bFailed = false;
			}

			std::size_t m_unRuleIndex;
			Socket m_Client;
			Socket m_Target;
			Buffer m_ToTarget;
			Buffer m_ToClient;
			Clock::time_point m_ConnectedAt;
			Clock::time_point m_LastActivity;
			bool m_bConnecting;
			bool m_bClientEOF;
			bool m_bTargetEOF;
			bool m_bClientShutdown;
			bool m_bTargetShutdown;
			bool m_bFailed;
		};

		struct UDPSession {
			UDPSession(std::size_t unRuleIndex, Socket&& Target, sockaddr_storage const& RemoteAddress, int nRemoteAddressBytes, Clock::time_point LastActivity, bool bFailed) :
				m_unRuleIndex(unRuleIndex),
				m_Target(std::move(Target)),
				m_RemoteAddress(RemoteAddress),
				m_nRemoteAddressBytes(nRemoteAddressBytes),
				m_LastActivity(LastActivity),
				m_bFailed(bFailed)
			{
			}

			std::size_t m_unRuleIndex;
			Socket m_Target;
			sockaddr_storage m_RemoteAddress;
			int m_nRemoteAddressBytes;
			Clock::time_point m_LastActivity;
			bool m_bFailed;
		};

		explicit Implementation(Logger fnLog) :
			m_Log(std::move(fnLog)),
			m_arrDatagram {}
		{
			m_bWSAStarted = false;
			m_bRunning = false;
		}

		void Log(std::string const& strMessage) noexcept {
			try {
				if (m_Log) {
					m_Log(strMessage);
				}
			} catch (...) {
				// A logger cannot terminate transport cleanup.
			}
		}

		void Clear() noexcept {
			m_vecSessions.clear();
			m_vecUDPSessions.clear();
			m_vecListeners.clear();

			if (m_bWSAStarted) {
				WSACleanup();
				m_bWSAStarted = false;
			}

			m_bRunning = false;
			m_bHealthy.store(false);
		}

		void RemoveClosed() {
			std::erase_if(m_vecSessions, [this](std::unique_ptr<Session> const& pSession) -> bool {
				if (!pSession->m_bFailed) {
					return false;
				}

				--m_vecListeners[pSession->m_unRuleIndex].m_unActiveSessions;

				return true;
			});

			Clock::time_point const CurrentTime = Clock::now();
			std::erase_if(m_vecUDPSessions, [this, CurrentTime](UDPSession const& SessionState) -> bool {
				const unsigned unTimeoutSeconds = m_vecListeners[SessionState.m_unRuleIndex].m_Configuration.m_unIdleTimeoutSeconds;
				if (!SessionState.m_bFailed && ((unTimeoutSeconds == 0) || ((CurrentTime - SessionState.m_LastActivity) < std::chrono::seconds(unTimeoutSeconds)))) {
					return false;
				}

				--m_vecListeners[SessionState.m_unRuleIndex].m_unActiveSessions;

				return true;
			});
		}

		void AcceptTCP(std::size_t unRuleIndex) {
			constexpr std::size_t kMaximumConnectionsPerTick = 32;

			Listener& CurrentListener = m_vecListeners[unRuleIndex];
			// Bounded per tick so even a saturated listener cannot delay Stop or peers.
			for (std::size_t unAttempt = 0; (unAttempt != kMaximumConnectionsPerTick) && !m_bStopping.load(); ++unAttempt) {
				Socket Client(WSAAccept(CurrentListener.m_Socket.Get(), nullptr, nullptr, nullptr, 0));
				if (!Client) {
					break;
				}

				// Accepted sockets inherit the listener's non-inheritable handle flag.
				// Explicitly clear inheritance as well for older socket providers.
				if (!SetHandleInformation(reinterpret_cast<HANDLE>(Client.Get()), HANDLE_FLAG_INHERIT, 0)) {
					continue;
				}

				if ((CurrentListener.m_unActiveSessions >= CurrentListener.m_Configuration.m_unMaximumConnections) || ((m_vecSessions.size() + m_vecUDPSessions.size()) >= kMaximumSessions)) {
					continue;
				}

				if (!SetNonblocking(Client.Get())) {
					continue;
				}

				Socket Target = CreateSocket(CurrentListener.m_Target.m_nFamily);
				if (!Target || !SetNonblocking(Target.Get())) {
					continue;
				}

				EnableTCPNoDelay(Client.Get());
				EnableTCPNoDelay(Target.Get());
				const int nResult = connect(Target.Get(), reinterpret_cast<sockaddr const*>(&CurrentListener.m_Target.m_Address), CurrentListener.m_Target.m_nAddressBytes);
				const int nErrorCode = (nResult == SOCKET_ERROR) ? WSAGetLastError() : 0;
				if ((nResult == SOCKET_ERROR) && (nErrorCode != WSAEWOULDBLOCK) && (nErrorCode != WSAEINPROGRESS) && (nErrorCode != WSAEALREADY)) {
					continue;
				}

				std::unique_ptr<Session> pSession = std::make_unique<Session>();
				pSession->m_unRuleIndex = unRuleIndex;
				pSession->m_Client = std::move(Client);
				pSession->m_Target = std::move(Target);
				pSession->m_bConnecting = nResult == SOCKET_ERROR;
				pSession->m_LastActivity = Clock::now();
				pSession->m_ConnectedAt = pSession->m_LastActivity;
				m_vecSessions.push_back(std::move(pSession));
				++CurrentListener.m_unActiveSessions;
			}
		}

		void AcceptUDP(std::size_t unRuleIndex) {
			constexpr std::size_t kMaximumDatagramsPerTick = 32;

			Listener& CurrentListener = m_vecListeners[unRuleIndex];
			for (std::size_t unAttempt = 0; (unAttempt != kMaximumDatagramsPerTick) && !m_bStopping.load(); ++unAttempt) {
				sockaddr_storage RemoteAddress {};
				int nRemoteAddressBytes = sizeof(RemoteAddress);
				const int nReceivedBytes = recvfrom(CurrentListener.m_Socket.Get(), m_arrDatagram.data(), static_cast<int>(m_arrDatagram.size()), 0, reinterpret_cast<sockaddr*>(&RemoteAddress), &nRemoteAddressBytes);
				if (nReceivedBytes == SOCKET_ERROR) {
					const int nErrorCode = WSAGetLastError();
					if (WouldBlock(nErrorCode)) {
						break;
					}

					// WSAEMSGSIZE consumes an oversized packet without forwarding a
					// truncated prefix. A failed peer must never close the listener.
					if ((nErrorCode == WSAEMSGSIZE) || (nErrorCode == WSAECONNRESET)) {
						continue;
					}

					break;
				}

				auto itSession = std::find_if(m_vecUDPSessions.begin(), m_vecUDPSessions.end(), [&](UDPSession const& SessionState) -> bool {
					return !SessionState.m_bFailed && (SessionState.m_unRuleIndex == unRuleIndex) && AreRemoteAddressesEqual(SessionState.m_RemoteAddress, RemoteAddress);
				});
				if (itSession == m_vecUDPSessions.end()) {
					if ((CurrentListener.m_unActiveSessions >= CurrentListener.m_Configuration.m_unMaximumConnections) || ((m_vecSessions.size() + m_vecUDPSessions.size()) >= kMaximumSessions)) {
						continue;
					}

					Socket Target = CreateSocket(CurrentListener.m_Target.m_nFamily, true);
					if (!Target || !SetNonblocking(Target.Get()) || !ConfigureUDPBuffers(Target.Get()) ||
						(connect(Target.Get(), reinterpret_cast<sockaddr const*>(&CurrentListener.m_Target.m_Address), CurrentListener.m_Target.m_nAddressBytes) != 0)) {
						continue;
					}

					// A connected target socket accepts replies only from this exact
					// local service, and its source port identifies this remote peer.
					m_vecUDPSessions.push_back(UDPSession(unRuleIndex, std::move(Target), RemoteAddress, nRemoteAddressBytes, Clock::now(), false));
					++CurrentListener.m_unActiveSessions;
					itSession = std::prev(m_vecUDPSessions.end());
				}

				const int nSentBytes = send(itSession->m_Target.Get(), m_arrDatagram.data(), nReceivedBytes, 0);
				if (nSentBytes == nReceivedBytes) {
					itSession->m_LastActivity = Clock::now(); // Includes a zero-length datagram.
				} else if ((nSentBytes == SOCKET_ERROR) && !WouldBlock(WSAGetLastError())) {
					itSession->m_bFailed = true;
				}
			}
		}

		void ProcessUDP(UDPSession& SessionState, short nEvents) {
			constexpr std::size_t kMaximumRepliesPerTick = 8;

			if (nEvents & POLLNVAL) {
				SessionState.m_bFailed = true;
				return;
			}

			if (!(nEvents & (POLLRDNORM | POLLRDBAND | POLLHUP | POLLERR))) {
				return;
			}

			for (std::size_t unAttempt = 0; (unAttempt != kMaximumRepliesPerTick) && !m_bStopping.load(); ++unAttempt) {
				const int nReceivedBytes = recv(SessionState.m_Target.Get(), m_arrDatagram.data(), static_cast<int>(m_arrDatagram.size()), 0);
				if (nReceivedBytes == SOCKET_ERROR) {
					const int nErrorCode = WSAGetLastError();
					if (WouldBlock(nErrorCode)) {
						break;
					}

					if (nErrorCode == WSAEMSGSIZE) {
						continue;
					}

					// ICMP refusal invalidates only this mapping. The next incoming
					// datagram can create a fresh mapping after its removal.
					SessionState.m_bFailed = true;
					break;
				}

				Listener const& CurrentListener = m_vecListeners[SessionState.m_unRuleIndex];
				const int nSentBytes = sendto(CurrentListener.m_Socket.Get(), m_arrDatagram.data(), nReceivedBytes, 0, reinterpret_cast<sockaddr const*>(&SessionState.m_RemoteAddress), SessionState.m_nRemoteAddressBytes);
				if (nSentBytes == nReceivedBytes) {
					SessionState.m_LastActivity = Clock::now();
				}
				// A remote peer's ICMP or full send buffer cannot affect other peers.
			}
		}

		static void Receive(Session& SessionState, SOCKET hSocket, Buffer& BufferValue, bool& bEndOfFile, short nEvents) {
			if (bEndOfFile || BufferValue.IsFull() || !(nEvents & (POLLRDNORM | POLLRDBAND | POLLHUP))) {
				return;
			}

			BufferValue.Compact();
			const int nResult = recv(hSocket, BufferValue.m_arrBytes.data() + BufferValue.m_unEnd, static_cast<int>(BufferValue.m_arrBytes.size() - BufferValue.m_unEnd), 0);
			if (nResult > 0) {
				BufferValue.m_unEnd += static_cast<std::size_t>(nResult);
				SessionState.m_LastActivity = Clock::now();
			} else if (nResult == 0) {
				bEndOfFile = true;
			} else if (!WouldBlock(WSAGetLastError())) {
				SessionState.m_bFailed = true;
			}
		}

		static void Send(Session& SessionState, SOCKET hSocket, Buffer& BufferValue, short nEvents) {
			if (BufferValue.IsEmpty() || !(nEvents & POLLWRNORM)) {
				return;
			}

			const int nResult = send(hSocket, BufferValue.m_arrBytes.data() + BufferValue.m_unBegin, static_cast<int>(BufferValue.m_unEnd - BufferValue.m_unBegin), 0);
			if (nResult > 0) {
				BufferValue.m_unBegin += static_cast<std::size_t>(nResult);
				if (BufferValue.IsEmpty()) {
					BufferValue.m_unEnd = 0;
					BufferValue.m_unBegin = 0;
				}

				SessionState.m_LastActivity = Clock::now();
			} else if ((nResult == 0) || !WouldBlock(WSAGetLastError())) {
				SessionState.m_bFailed = true;
			}
		}

		static bool HasSocketError(SOCKET hSocket, short nEvents) {
			if (nEvents & POLLNVAL) {
				return true;
			}

			if (!(nEvents & POLLERR)) {
				return false;
			}

			int nErrorCode = 0;
			int nErrorBytes = sizeof(nErrorCode);

			return (getsockopt(hSocket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&nErrorCode), &nErrorBytes) != 0) || (nErrorCode != 0);
		}

		void Process(Session& SessionState, short nClientEvents, short nTargetEvents) {
			if (HasSocketError(SessionState.m_Client.Get(), nClientEvents)) {
				SessionState.m_bFailed = true;
				return;
			}

			if (SessionState.m_bConnecting && (nTargetEvents & (POLLWRNORM | POLLRDNORM | POLLERR | POLLHUP | POLLNVAL))) {
				int nErrorCode = 0;
				int nErrorBytes = sizeof(nErrorCode);
				if ((nTargetEvents & POLLNVAL) || (getsockopt(SessionState.m_Target.Get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&nErrorCode), &nErrorBytes) != 0) || (nErrorCode != 0)) {
					SessionState.m_bFailed = true;
					return;
				}

				SessionState.m_bConnecting = false;
				SessionState.m_LastActivity = Clock::now();
			} else if (!SessionState.m_bConnecting && HasSocketError(SessionState.m_Target.Get(), nTargetEvents)) {
				SessionState.m_bFailed = true;
				return;
			}

			Receive(SessionState, SessionState.m_Client.Get(), SessionState.m_ToTarget, SessionState.m_bClientEOF, nClientEvents);

			if (!SessionState.m_bConnecting) {
				Receive(SessionState, SessionState.m_Target.Get(), SessionState.m_ToClient, SessionState.m_bTargetEOF, nTargetEvents);
				Send(SessionState, SessionState.m_Target.Get(), SessionState.m_ToTarget, nTargetEvents);
				Send(SessionState, SessionState.m_Client.Get(), SessionState.m_ToClient, nClientEvents);

				// Forward FIN only after that direction has drained; the reverse stream
				// stays open for a late response after an upload's shutdown(SD_SEND).
				// FIN propagation is best effort; EOF/socket errors, configured timeouts,
				// or Stop release the owned sockets without discarding the reverse stream.
				if (SessionState.m_bClientEOF && SessionState.m_ToTarget.IsEmpty() && !SessionState.m_bTargetShutdown) {
					shutdown(SessionState.m_Target.Get(), SD_SEND);
					SessionState.m_bTargetShutdown = true;
				}

				if (SessionState.m_bTargetEOF && SessionState.m_ToClient.IsEmpty() && !SessionState.m_bClientShutdown) {
					shutdown(SessionState.m_Client.Get(), SD_SEND);
					SessionState.m_bClientShutdown = true;
				}

				if (SessionState.m_bClientEOF && SessionState.m_bTargetEOF && SessionState.m_ToTarget.IsEmpty() && SessionState.m_ToClient.IsEmpty()) {
					SessionState.m_bFailed = true;
				}
			}

			Clock::time_point const CurrentTime = Clock::now();
			PortForwardConfig const& Configuration = m_vecListeners[SessionState.m_unRuleIndex].m_Configuration;
			if (SessionState.m_bConnecting && ((CurrentTime - SessionState.m_ConnectedAt) >= std::chrono::seconds(Configuration.m_unConnectTimeoutSeconds))) {
				SessionState.m_bFailed = true;
			}

			if ((Configuration.m_unIdleTimeoutSeconds != 0) && ((CurrentTime - SessionState.m_LastActivity) >= std::chrono::seconds(Configuration.m_unIdleTimeoutSeconds))) {
				SessionState.m_bFailed = true;
			}
		}

		void Run() noexcept {
			constexpr std::size_t kAbsentSlot = static_cast<std::size_t>(-1);
			constexpr std::size_t kSocketDirectionsPerSession = 2;

			try {
				struct Slots {
					std::size_t m_unClientIndex;
					std::size_t m_unTargetIndex;
				};

				std::vector<WSAPOLLFD> vecPollEntries;
				std::vector<Slots> vecSlots;
				std::vector<std::size_t> vecUDPSlots;
				vecPollEntries.reserve(kMaximumRules + kMaximumSessions * kSocketDirectionsPerSession);
				vecSlots.reserve(kMaximumSessions);
				vecUDPSlots.reserve(kMaximumSessions);

				while (!m_bStopping.load()) {
					vecPollEntries.clear();
					vecSlots.clear();
					vecUDPSlots.clear();
					for (auto const& CurrentListener : m_vecListeners) {
						vecPollEntries.push_back({ CurrentListener.m_Socket.Get(), POLLRDNORM, 0 });
					}

					for (auto const& pSession : m_vecSessions) {
						short nClientEvents = 0;
						short nTargetEvents = 0;
						if (!pSession->m_bClientEOF && !pSession->m_ToTarget.IsFull()) {
							nClientEvents |= POLLRDNORM;
						}

						if (!pSession->m_ToClient.IsEmpty()) {
							nClientEvents |= POLLWRNORM;
						}

						if (pSession->m_bConnecting) {
							nTargetEvents |= POLLWRNORM;
						} else {
							if (!pSession->m_bTargetEOF && !pSession->m_ToClient.IsFull()) {
								nTargetEvents |= POLLRDNORM;
							}

							if (!pSession->m_ToTarget.IsEmpty()) {
								nTargetEvents |= POLLWRNORM;
							}
						}

						Slots Entry { kAbsentSlot, kAbsentSlot };
						// Do not poll inactive directions: unsolicited HUP from an
						// EOF socket otherwise spins while a late reply is pending.
						if (nClientEvents != 0) {
							Entry.m_unClientIndex = vecPollEntries.size();
							vecPollEntries.push_back({ pSession->m_Client.Get(), nClientEvents, 0 });
						}

						if (nTargetEvents != 0) {
							Entry.m_unTargetIndex = vecPollEntries.size();
							vecPollEntries.push_back({ pSession->m_Target.Get(), nTargetEvents, 0 });
						}

						vecSlots.push_back(Entry);
					}

					for (auto const& SessionState : m_vecUDPSessions) {
						vecUDPSlots.push_back(vecPollEntries.size());
						vecPollEntries.push_back({ SessionState.m_Target.Get(), POLLRDNORM, 0 });
					}

					const int nPollResult = WSAPoll(vecPollEntries.data(), static_cast<ULONG>(vecPollEntries.size()), kPollMilliseconds);
					const int nPollErrorCode = (nPollResult == SOCKET_ERROR) ? WSAGetLastError() : 0;
					if (m_bStopping.load()) {
						break;
					}

					if (nPollResult == SOCKET_ERROR) {
						if (nPollErrorCode == WSAEINTR) {
							continue;
						}

						Log("Port forwarding stopped: socket polling failed (" + std::to_string(nPollErrorCode) + ").");
						break;
					}

					// Process only the snapshot before accepting additional sessions.
					for (std::size_t unSessionIndex = 0; unSessionIndex < m_vecSessions.size(); ++unSessionIndex) {
						Slots const Entry = vecSlots[unSessionIndex];
						Process(*m_vecSessions[unSessionIndex], (Entry.m_unClientIndex == kAbsentSlot) ? 0 : vecPollEntries[Entry.m_unClientIndex].revents, (Entry.m_unTargetIndex == kAbsentSlot) ? 0 : vecPollEntries[Entry.m_unTargetIndex].revents);
					}

					for (std::size_t unUDPSessionIndex = 0; unUDPSessionIndex < m_vecUDPSessions.size(); ++unUDPSessionIndex) {
						ProcessUDP(m_vecUDPSessions[unUDPSessionIndex], vecPollEntries[vecUDPSlots[unUDPSessionIndex]].revents);
					}

					RemoveClosed();

					for (std::size_t unListenerIndex = 0; (unListenerIndex < m_vecListeners.size()) && !m_bStopping.load(); ++unListenerIndex) {
						if (vecPollEntries[unListenerIndex].revents & POLLRDNORM) {
							if (m_vecListeners[unListenerIndex].m_Configuration.m_Protocol == PortForwardProtocol::Udp) {
								AcceptUDP(unListenerIndex);
							} else {
								AcceptTCP(unListenerIndex);
							}
						}
					}
				}
			} catch (std::exception const& Exception) {
				try {
					Log(std::string("Port forwarding stopped: ") + Exception.what());
				} catch (...) {
					// Formatting a failure must not unwind through this noexcept worker entry point.
					OutputDebugStringA("WireSteam: Port forwarding failure reporting failed.\n");
				}
			} catch (...) {
				try {
					Log("Port forwarding stopped unexpectedly.");
				} catch (...) {
					OutputDebugStringA("WireSteam: Port forwarding stopped unexpectedly.\n");
				}
			}

			m_vecSessions.clear();
			m_vecUDPSessions.clear();
			for (auto& CurrentListener : m_vecListeners) {
				CurrentListener.m_Socket.Reset();
				CurrentListener.m_unActiveSessions = 0;
			}

			m_bHealthy.store(false);
		}

		Logger m_Log;
		std::mutex m_LifecycleMutex;
		std::atomic<bool> m_bStopping;
		std::atomic<bool> m_bHealthy;
		std::thread m_Worker;
		std::vector<Listener> m_vecListeners;
		std::vector<std::unique_ptr<Session>> m_vecSessions;
		std::vector<UDPSession> m_vecUDPSessions;
		// UDP is best effort. A single shared scratch buffer preserves each complete
		// datagram; kernel backpressure drops packets instead of growing a queue.
		std::array<char, kMaximumDatagramBytes> m_arrDatagram;
		bool m_bWSAStarted;
		bool m_bRunning;
	};

	PortForwarder::PortForwarder(Logger fnLog) :
		m_pImplementation(std::make_unique<Implementation>(std::move(fnLog)))
	{
	}

	PortForwarder::~PortForwarder() noexcept {
		Stop();
	}

	bool PortForwarder::Start(std::vector<PortForwardConfig> const& vecRules, std::string& strErrorOut, std::function<bool()> const& fnIsCancelled) {
		constexpr WORD kWSAVersion = MAKEWORD(2, 2);
		constexpr int kBindTimeoutSeconds = 10;

		std::lock_guard<std::mutex> LifecycleLock(m_pImplementation->m_LifecycleMutex);
		strErrorOut.clear();

		if (m_pImplementation->m_bRunning) {
			strErrorOut = "Port forwarding is already running.";
			return false;
		}

		if (vecRules.size() > kMaximumRules) {
			strErrorOut = "At most 32 port forwarding rules are supported.";
			return false;
		}

		if (vecRules.empty()) {
			return true;
		}

		WSADATA WSAData {};
		const int nStartupResult = WSAStartup(kWSAVersion, &WSAData);
		if (nStartupResult != 0) {
			strErrorOut = "Port forwarding could not initialize Winsock (" + std::to_string(nStartupResult) + ").";
			return false;
		}

		m_pImplementation->m_bWSAStarted = true;

		try {
			Clock::time_point const BindDeadline = Clock::now() + std::chrono::seconds(kBindTimeoutSeconds);
			// The cancellation callback observes state only; it cannot reenter the locked lifecycle.
			auto const IsCancelled = [&]() -> bool {
				if (!fnIsCancelled || !fnIsCancelled()) {
					return false;
				}

				strErrorOut = "Port forwarding startup was cancelled.";

				return true;
			};

			m_pImplementation->m_vecListeners.reserve(vecRules.size());
			m_pImplementation->m_vecSessions.reserve(kMaximumSessions);
			m_pImplementation->m_vecUDPSessions.reserve(kMaximumSessions);

			for (auto const& Configuration : vecRules) {
				if (IsCancelled()) {
					m_pImplementation->Clear();
					return false;
				}

				Endpoint ListenEndpoint;
				Endpoint TargetEndpoint;
				if ((Configuration.m_unListenPort == 0) || (Configuration.m_unTargetPort == 0) || (Configuration.m_unMaximumConnections < 1) || (Configuration.m_unMaximumConnections > kMaximumSessions) ||
					(Configuration.m_unConnectTimeoutSeconds < 1) ||
					((Configuration.m_Protocol != PortForwardProtocol::Tcp) && (Configuration.m_Protocol != PortForwardProtocol::Udp)) ||
					!ParseNumericEndpoint(Configuration.m_strListenAddress, Configuration.m_unListenPort, ListenEndpoint) ||
					!ParseNumericEndpoint(Configuration.m_strTargetAddress, Configuration.m_unTargetPort, TargetEndpoint) || IsUnspecifiedEndpoint(ListenEndpoint) || !IsLoopbackEndpoint(TargetEndpoint)) {
					strErrorOut = "Port forwarding requires TCP or UDP, a specific numeric listen address, a numeric loopback target, nonzero ports, 1..256 connections and a positive connect timeout.";
					m_pImplementation->Clear();
					return false;
				}

				const bool bUDP = Configuration.m_Protocol == PortForwardProtocol::Udp;
				Socket ListenerSocket = CreateSocket(ListenEndpoint.m_nFamily, bUDP);
				const BOOL nEnabled = TRUE;
				auto const FailStartup = [&](int nErrorCode) -> bool {
					strErrorOut = "Cannot listen for " + std::string(bUDP ? "UDP" : "TCP") + " forwarding on " + FormatEndpoint(Configuration.m_strListenAddress, Configuration.m_unListenPort) +
						" (Winsock " + std::to_string(nErrorCode) + "). Check the VPN address and whether the port is already in use.";
					ListenerSocket.Reset();
					m_pImplementation->Clear();

					return false;
				};

				if (!ListenerSocket || (setsockopt(ListenerSocket.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&nEnabled), sizeof(nEnabled)) != 0) || !SetNonblocking(ListenerSocket.Get())) {
					return FailStartup(WSAGetLastError());
				}

				if ((ListenEndpoint.m_nFamily == AF_INET6) && (setsockopt(ListenerSocket.Get(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char const*>(&nEnabled), sizeof(nEnabled)) != 0)) {
					return FailStartup(WSAGetLastError());
				}

				if (bUDP) {
					if (!ConfigureUDPBuffers(ListenerSocket.Get())) {
						return FailStartup(WSAGetLastError());
					}

					BOOL nReportReset = FALSE;
					DWORD unReturnedBytes = 0;
					if (WSAIoctl(ListenerSocket.Get(), SIO_UDP_CONNRESET, &nReportReset, sizeof(nReportReset), nullptr, 0, &unReturnedBytes, nullptr, nullptr) != 0) {
						return FailStartup(WSAGetLastError());
					}
				}

				while (bind(ListenerSocket.Get(), reinterpret_cast<sockaddr const*>(&ListenEndpoint.m_Address), ListenEndpoint.m_nAddressBytes) != 0) {
					// WireGuard's service may report Running before its VPN address
					// is ready. Retry only this transient bind error, never a conflict.
					const int nErrorCode = WSAGetLastError();
					if ((nErrorCode != WSAEADDRNOTAVAIL) || (Clock::now() >= BindDeadline)) {
						return FailStartup(nErrorCode);
					}

					if (IsCancelled()) {
						ListenerSocket.Reset();
						m_pImplementation->Clear();
						return false;
					}

					std::this_thread::sleep_for(std::chrono::milliseconds(kPollMilliseconds));
				}

				if (!bUDP && (::listen(ListenerSocket.Get(), static_cast<int>(std::min<unsigned>(Configuration.m_unMaximumConnections, SOMAXCONN))) != 0)) {
					return FailStartup(WSAGetLastError());
				}

				m_pImplementation->m_vecListeners.push_back(Implementation::Listener(Configuration, TargetEndpoint, std::move(ListenerSocket), 0));
			}

			m_pImplementation->m_bStopping.store(false);
			m_pImplementation->m_bHealthy.store(true);
			m_pImplementation->m_Worker = std::thread([this]() -> void {
				m_pImplementation->Run();
			});
			m_pImplementation->m_bRunning = true;

			return true;
		} catch (std::exception const& Exception) {
			m_pImplementation->Clear();
			strErrorOut = std::string("Could not start port forwarding: ") + Exception.what();
		} catch (...) {
			m_pImplementation->Clear();
			strErrorOut = "Could not start port forwarding.";
		}

		return false;
	}

	bool PortForwarder::IsRunning() const noexcept {
		return m_pImplementation->m_bHealthy.load();
	}

	void PortForwarder::Stop() noexcept {
		try {
			std::lock_guard<std::mutex> LifecycleLock(m_pImplementation->m_LifecycleMutex);
			m_pImplementation->m_bStopping.store(true);

			if (m_pImplementation->m_Worker.joinable()) {
				m_pImplementation->m_Worker.join();
			}

			m_pImplementation->Clear();
		} catch (...) {
			// Shared worker state cannot be destroyed without successful serialization and join.
			OutputDebugStringA("WireSteam: cannot quiesce the port-forwarding worker.\n");
			std::terminate();
		}
	}

} // namespace ws
