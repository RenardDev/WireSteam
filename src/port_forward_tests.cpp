
// General
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

// C++
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>

// STL
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Project
#include "port_forward.hpp"

namespace {
	using Clock = std::chrono::steady_clock;
	using Bytes = std::vector<char>;

	constexpr std::size_t kMaximumTCPChunkBytes = 65536;
	constexpr int kDefaultWaitTimeoutMilliseconds = 5000;
	constexpr int kExpectedCloseTimeoutMilliseconds = 3000;

	void Check(bool bCondition, std::string const& strMessage) {
		if (!bCondition) {
			throw std::runtime_error(strMessage);
		}
	}

	struct WSAScope {
		WSAScope() {
			constexpr WORD kWSAVersion = MAKEWORD(2, 2);

			WSADATA WSAData {};
			const int nStartupResult = WSAStartup(kWSAVersion, &WSAData);
			if (nStartupResult != 0) {
				throw std::runtime_error("WSAStartup");
			}
		}

		~WSAScope() {
			WSACleanup();
		}

		WSAScope(WSAScope const&) = delete;
		WSAScope& operator=(WSAScope const&) = delete;
	};

	class Socket {
	public:
		explicit Socket(SOCKET hSocket = INVALID_SOCKET) :
			m_hSocket(hSocket)
		{
		}

		~Socket() {
			if (m_hSocket != INVALID_SOCKET) {
				closesocket(m_hSocket);
			}
		}

		Socket(Socket&& Other) noexcept :
			m_hSocket(std::exchange(Other.m_hSocket, INVALID_SOCKET))
		{
		}

		Socket(Socket const&) = delete;
		Socket& operator=(Socket const&) = delete;

		Socket& operator=(Socket&& Other) noexcept {
			if (this != &Other) {
				if (m_hSocket != INVALID_SOCKET) {
					closesocket(m_hSocket);
				}

				m_hSocket = std::exchange(Other.m_hSocket, INVALID_SOCKET);
			}

			return *this;
		}

		SOCKET Release() noexcept {
			return std::exchange(m_hSocket, INVALID_SOCKET);
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

	Socket CreateSocket(int nFamily = AF_INET) {
		constexpr DWORD kSocketTimeoutMilliseconds = 8000;

		// An overlapped-capable Windows handle also permits simultaneous blocking
		// send/recv in separate threads. A synchronous file handle serializes them.
		Socket SocketValue(WSASocketW(nFamily, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
		Check(static_cast<bool>(SocketValue), "test socket");
		Check(setsockopt(SocketValue.Get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&kSocketTimeoutMilliseconds), sizeof(kSocketTimeoutMilliseconds)) == 0, "recv timeout");
		Check(setsockopt(SocketValue.Get(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char const*>(&kSocketTimeoutMilliseconds), sizeof(kSocketTimeoutMilliseconds)) == 0, "send timeout");

		return SocketValue;
	}

	template <class TPredicate>
	void WaitUntil(TPredicate fnPredicate, char const* pszMessage, int nTimeoutMilliseconds = kDefaultWaitTimeoutMilliseconds) {
		constexpr int kPollDelayMilliseconds = 2;

		Clock::time_point const Deadline = Clock::now() + std::chrono::milliseconds(nTimeoutMilliseconds);
		while (!fnPredicate()) {
			Check(Clock::now() < Deadline, pszMessage);
			std::this_thread::sleep_for(std::chrono::milliseconds(kPollDelayMilliseconds));
		}
	}

	struct Listener {
		explicit Listener(std::uint16_t unRequestedPort = 0, int nFamily = AF_INET) :
			m_Socket(CreateSocket(nFamily))
		{
			const BOOL nEnabled = TRUE;
			Check(setsockopt(m_Socket.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&nEnabled), sizeof(nEnabled)) == 0, "exclusive test listener");

			if (nFamily == AF_INET) {
				sockaddr_in IPv4Address {};
				IPv4Address.sin_family = AF_INET;
				IPv4Address.sin_port = htons(unRequestedPort);
				IPv4Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				Check(bind(m_Socket.Get(), reinterpret_cast<sockaddr*>(&IPv4Address), sizeof(IPv4Address)) == 0, "bind test IPv4 listener");

				int nAddressBytes = sizeof(IPv4Address);
				Check(getsockname(m_Socket.Get(), reinterpret_cast<sockaddr*>(&IPv4Address), &nAddressBytes) == 0, "test getsockname");
				m_unPort = ntohs(IPv4Address.sin_port);

			} else {
				sockaddr_in6 IPv6Address {};
				IPv6Address.sin6_family = AF_INET6;
				IPv6Address.sin6_port = htons(unRequestedPort);
				IPv6Address.sin6_addr = in6addr_loopback;
				Check(setsockopt(m_Socket.Get(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char const*>(&nEnabled), sizeof(nEnabled)) == 0, "IPv6 test listener");
				Check(bind(m_Socket.Get(), reinterpret_cast<sockaddr*>(&IPv6Address), sizeof(IPv6Address)) == 0, "bind test IPv6 listener");

				int nAddressBytes = sizeof(IPv6Address);
				Check(getsockname(m_Socket.Get(), reinterpret_cast<sockaddr*>(&IPv6Address), &nAddressBytes) == 0, "IPv6 test getsockname");
				m_unPort = ntohs(IPv6Address.sin6_port);
			}

			Check(listen(m_Socket.Get(), SOMAXCONN) == 0, "listen test listener");

			u_long unNonblockingEnabled = 1;
			Check(ioctlsocket(m_Socket.Get(), FIONBIO, &unNonblockingEnabled) == 0, "nonblocking test listener");
		}

		Socket m_Socket;
		std::uint16_t m_unPort;
	};

	std::uint16_t FindAvailablePort(int nFamily = AF_INET) {
		return Listener(0, nFamily).m_unPort;
	}

	std::uint16_t FindAvailableTCPUDPPort() {
		constexpr std::size_t kMaximumPortAttempts = 256;
		constexpr BOOL kExclusiveAccess = TRUE;

		// TCP and UDP reservations are independent; probe both before exercising the forwarder.
		for (std::size_t unAttempt = 0; unAttempt != kMaximumPortAttempts; ++unAttempt) {
			Listener TCPListener;
			Socket UDPProbe(WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
			Check(static_cast<bool>(UDPProbe), "shared-port UDP probe socket");
			Check(setsockopt(UDPProbe.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&kExclusiveAccess), sizeof(kExclusiveAccess)) == 0, "exclusive shared-port UDP probe");

			sockaddr_in Address {};
			Address.sin_family = AF_INET;
			Address.sin_port = htons(TCPListener.m_unPort);
			Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (bind(UDPProbe.Get(), reinterpret_cast<sockaddr*>(&Address), sizeof(Address)) == 0) {
				return TCPListener.m_unPort;
			}

			const int nErrorCode = WSAGetLastError();
			if ((nErrorCode != WSAEACCES) && (nErrorCode != WSAEADDRINUSE)) {
				throw std::runtime_error("Shared TCP/UDP test port probe failed (Winsock " + std::to_string(nErrorCode) + ").");
			}
		}

		throw std::runtime_error("No shared TCP/UDP loopback test port was available.");
	}

	Socket Connect(std::uint16_t unPort, int nFamily = AF_INET) {
		Socket SocketValue = CreateSocket(nFamily);
		if (nFamily == AF_INET) {
			sockaddr_in IPv4Address {};
			IPv4Address.sin_family = AF_INET;
			IPv4Address.sin_port = htons(unPort);
			IPv4Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			Check(connect(SocketValue.Get(), reinterpret_cast<sockaddr*>(&IPv4Address), sizeof(IPv4Address)) == 0, "connect test IPv4 client");

		} else {
			sockaddr_in6 IPv6Address {};
			IPv6Address.sin6_family = AF_INET6;
			IPv6Address.sin6_port = htons(unPort);
			IPv6Address.sin6_addr = in6addr_loopback;
			Check(connect(SocketValue.Get(), reinterpret_cast<sockaddr*>(&IPv6Address), sizeof(IPv6Address)) == 0, "connect test IPv6 client");
		}

		return SocketValue;
	}

	Bytes BuildPattern(std::size_t unSize, unsigned unSeed = 0) {
		constexpr std::size_t kPatternMultiplier = 31;
		constexpr std::size_t kPatternDivisor = 103;
		constexpr std::size_t kPatternByteMask = 0xFF;

		Bytes vecResult(unSize);
		for (std::size_t unByteIndex = 0; unByteIndex < unSize; ++unByteIndex) {
			vecResult[unByteIndex] = static_cast<char>((unByteIndex * kPatternMultiplier + unByteIndex / kPatternDivisor + unSeed) & kPatternByteMask);
		}

		return vecResult;
	}

	void Send(SOCKET hSocket, char const* pchBytes, std::size_t unSize) {
		while (unSize != 0) {
			const int nSentBytes = send(hSocket, pchBytes, static_cast<int>(std::min<std::size_t>(unSize, kMaximumTCPChunkBytes)), 0);
			Check(nSentBytes > 0, "test TCP send (" + std::to_string(WSAGetLastError()) + ")");
			pchBytes += nSentBytes;
			unSize -= static_cast<std::size_t>(nSentBytes);
		}
	}

	void Send(SOCKET hSocket, Bytes const& vecBytes) {
		Send(hSocket, vecBytes.data(), vecBytes.size());
	}

	Bytes Receive(SOCKET hSocket, std::size_t unSize, bool bSlow = false) {
		constexpr int kInitialDelayMilliseconds = 150;
		constexpr int kChunkDelayMilliseconds = 1;

		Bytes vecBytes(unSize);
		std::size_t unTotalBytes = 0;
		if (bSlow) {
			std::this_thread::sleep_for(std::chrono::milliseconds(kInitialDelayMilliseconds));
		}

		while (unTotalBytes != unSize) {
			const std::size_t unChunkBytes = std::min<std::size_t>(unSize - unTotalBytes, kMaximumTCPChunkBytes);
			const int nReceivedBytes = recv(hSocket, vecBytes.data() + unTotalBytes, static_cast<int>(unChunkBytes), 0);
			Check(nReceivedBytes > 0, "test TCP receive (" + std::to_string(WSAGetLastError()) + ")");
			unTotalBytes += static_cast<std::size_t>(nReceivedBytes);

			if (bSlow) {
				std::this_thread::sleep_for(std::chrono::milliseconds(kChunkDelayMilliseconds));
			}
		}

		return vecBytes;
	}

	void ExpectClosed(SOCKET hSocket, int nTimeoutMilliseconds = kExpectedCloseTimeoutMilliseconds) {
		WSAPOLLFD PollDescriptor { hSocket, POLLRDNORM, 0 };
		Check(WSAPoll(&PollDescriptor, 1, nTimeoutMilliseconds) > 0, "connection was not closed within its bound");

		char chByte = 0;
		const int nResult = recv(hSocket, &chByte, 1, 0);
		const int nSocketError = WSAGetLastError();
		Check((nResult == 0) || ((nResult == SOCKET_ERROR) && (nSocketError != WSAETIMEDOUT) && (nSocketError != WSAEWOULDBLOCK)), "expected TCP EOF/reset");
	}

	struct Task {
		explicit Task(std::function<void()> fnWork) :
			m_Thread([this, fnWork = std::move(fnWork)]() -> void {
				try {
					fnWork();
				} catch (...) {
					m_Exception = std::current_exception();
				}
			})
		{
		}

		~Task() {
			if (m_Thread.joinable()) {
				m_Thread.join();
			}
		}

		void Join() {
			if (m_Thread.joinable()) {
				m_Thread.join();
			}

			if (m_Exception) {
				std::rethrow_exception(m_Exception);
			}
		}

		std::exception_ptr m_Exception;
		std::thread m_Thread;
	};

	void Echo(SOCKET hSocket) {
		constexpr std::size_t kEchoBufferBytes = 16384;

		std::array<char, kEchoBufferBytes> arrBytes {};
		while (true) {
			const int nResult = recv(hSocket, arrBytes.data(), static_cast<int>(arrBytes.size()), 0);
			if (nResult == 0) {
				break;
			}

			Check(nResult > 0, "echo receive");
			Send(hSocket, arrBytes.data(), static_cast<std::size_t>(nResult));
		}

		// EOF already ended the fixture stream; the socket owner closes it on return.
		shutdown(hSocket, SD_SEND);
	}

	// Small test-only local server; shutdown interrupts every owned socket before join.
	struct Server {
		explicit Server(std::function<void(SOCKET)> fnHandler = Echo, std::uint16_t unPort = 0, int nFamily = AF_INET) :
			m_Listener(unPort, nFamily),
			m_Acceptor([this, fnHandler = std::move(fnHandler)]() -> void {
				constexpr int kAcceptPollMilliseconds = 20;
				constexpr DWORD kSocketTimeoutMilliseconds = 8000;

				try {
					while (!m_bStopped.load()) {
						WSAPOLLFD PollDescriptor { m_Listener.m_Socket.Get(), POLLRDNORM, 0 };
						if (WSAPoll(&PollDescriptor, 1, kAcceptPollMilliseconds) <= 0) {
							continue;
						}

						Socket AcceptedClient(accept(m_Listener.m_Socket.Get(), nullptr, nullptr));
						if (!AcceptedClient) {
							continue;
						}

						u_long unNonblockingEnabled = 0;
						Check(ioctlsocket(AcceptedClient.Get(), FIONBIO, &unNonblockingEnabled) == 0, "blocking test server client");
						Check(setsockopt(AcceptedClient.Get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&kSocketTimeoutMilliseconds), sizeof(kSocketTimeoutMilliseconds)) == 0, "test server receive timeout");
						Check(setsockopt(AcceptedClient.Get(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char const*>(&kSocketTimeoutMilliseconds), sizeof(kSocketTimeoutMilliseconds)) == 0, "test server send timeout");

						const SOCKET hClientSocket = AcceptedClient.Get();
						{
							std::lock_guard<std::mutex> Lock(m_Mutex);
							m_vecActiveSockets.push_back(hClientSocket);
						}

						m_unAccepted.fetch_add(1);
						try {
							m_vecHandlers.emplace_back([this, fnHandler, hClientSocket]() -> void {
								Socket Client(hClientSocket);
								try {
									fnHandler(Client.Get());
								} catch (...) {
									// Client cancellation is intentional in cleanup/limit tests.
								}

								try {
									std::lock_guard<std::mutex> Lock(m_Mutex);
									std::erase(m_vecActiveSockets, Client.Get());
								} catch (...) {
									OutputDebugStringA("WireSteam test server client cleanup failed.\n");
									std::terminate();
								}
							});
						} catch (...) {
							try {
								std::lock_guard<std::mutex> Lock(m_Mutex);
								std::erase(m_vecActiveSockets, hClientSocket);
							} catch (...) {
								OutputDebugStringA("WireSteam test server launch rollback failed.\n");
								std::terminate();
							}

							throw;
						}

						// The worker owns the handle after launch; only invalidate this local owner, without throwing.
						AcceptedClient.Release();
					}
				} catch (...) {
					OutputDebugStringA("WireSteam test server acceptor failed.\n");
					m_bStopped.store(true);
				}
			})
		{
		}

		~Server() {
			m_bStopped.store(true);
			m_Acceptor.join();

			{
				std::lock_guard<std::mutex> Lock(m_Mutex);
				for (auto const& hSocket : m_vecActiveSockets) {
					// Interrupt best-effort; each handler still owns and closes its socket.
					shutdown(hSocket, SD_BOTH);
				}
			}

			for (auto& Handler : m_vecHandlers) {
				Handler.join();
			}
		}

		Listener m_Listener;
		std::atomic<bool> m_bStopped;
		std::atomic<std::size_t> m_unAccepted;
		std::mutex m_Mutex;
		std::vector<SOCKET> m_vecActiveSockets;
		std::vector<std::thread> m_vecHandlers;
		std::thread m_Acceptor;
	};

	ws::PortForwardConfig BuildForwardRule(std::uint16_t unTargetPort, std::uint16_t unListenPort = 0) {
		ws::PortForwardConfig Rule;
		Rule.m_strListenAddress = "127.0.0.1";
		Rule.m_unListenPort = (unListenPort == 0) ? FindAvailablePort() : unListenPort;
		Rule.m_strTargetAddress = "127.0.0.1";
		Rule.m_unTargetPort = unTargetPort;

		return Rule;
	}

	void Start(ws::PortForwarder& Forwarder, std::vector<ws::PortForwardConfig> const& vecRules) {
		std::string strError;
		Check(Forwarder.Start(vecRules, strError), strError);
		Check(Forwarder.IsRunning(), "forwarder health after startup");
	}

	void TestParallelBackpressure() {
		constexpr std::size_t kClientCount = 4;
		constexpr std::size_t kPayloadBytes = 4 * 1024 * 1024;
		constexpr std::size_t kClientPayloadIncrement = 19;

		Server TargetServer;
		ws::PortForwardConfig Rule = BuildForwardRule(TargetServer.m_Listener.m_unPort);
		ws::PortForwarder Forwarder;
		Start(Forwarder, { Rule });

		std::vector<std::unique_ptr<Task>> vecClients;
		for (std::size_t unClientIndex = 0; unClientIndex != kClientCount; ++unClientIndex) {
			vecClients.push_back(std::make_unique<Task>([&, unClientIndex]() -> void {
				Socket Client = Connect(Rule.m_unListenPort);
				Bytes const vecPayload = BuildPattern(kPayloadBytes + unClientIndex * kClientPayloadIncrement, static_cast<unsigned>(unClientIndex));
				Task Sender([&]() -> void {
					Send(Client.Get(), vecPayload);
					// The receive assertion observes FIN propagation; ownership still closes the socket.
					shutdown(Client.Get(), SD_SEND);
				});
				Check(Receive(Client.Get(), vecPayload.size(), true) == vecPayload, "parallel slow-reader payload corruption");
				ExpectClosed(Client.Get());
				Sender.Join();
			}));
		}

		for (auto& pClient : vecClients) {
			pClient->Join();
		}

		Check(TargetServer.m_unAccepted.load() == kClientCount, "parallel clients did not reach the service");
	}

	void TestHalfCloseLateReply() {
		constexpr std::size_t kUploadBytes = 2 * 1024 * 1024 + 91;
		constexpr unsigned kUploadSeed = 29;
		constexpr std::size_t kReplyBytes = 256 * 1024 + 103;
		constexpr unsigned kReplySeed = 51;
		constexpr int kReplyDelayMilliseconds = 200;

		Bytes const vecUpload = BuildPattern(kUploadBytes, kUploadSeed);
		Bytes const vecReply = BuildPattern(kReplyBytes, kReplySeed);
		std::atomic<bool> bUploaded = false;
		Server TargetServer([&](SOCKET hSocket) -> void {
			Check(Receive(hSocket, vecUpload.size()) == vecUpload, "half-close upload corruption");

			char chByte = 0;
			Check(recv(hSocket, &chByte, 1, 0) == 0, "target did not receive forwarded FIN");
			bUploaded.store(true);
			std::this_thread::sleep_for(std::chrono::milliseconds(kReplyDelayMilliseconds));
			Send(hSocket, vecReply);
			// EOF is verified by the client; the worker owner closes the socket afterwards.
			shutdown(hSocket, SD_SEND);
		});
		ws::PortForwardConfig Rule = BuildForwardRule(TargetServer.m_Listener.m_unPort);
		ws::PortForwarder Forwarder;
		Start(Forwarder, { Rule });

		Socket Client = Connect(Rule.m_unListenPort);
		Send(Client.Get(), vecUpload);
		// The following receive/EOF checks validate this terminal send half-close.
		shutdown(Client.Get(), SD_SEND);
		Check(Receive(Client.Get(), vecReply.size()) == vecReply, "late response lost after client FIN");
		ExpectClosed(Client.Get());
		Check(bUploaded.load(), "target did not validate upload");
	}

	void TestRefusalRecovery() {
		constexpr int kRefusalTimeoutMilliseconds = 2500;
		constexpr std::size_t kPayloadBytes = 8191;

		const std::uint16_t unTargetPort = FindAvailablePort();
		ws::PortForwardConfig Rule = BuildForwardRule(unTargetPort);
		Rule.m_unConnectTimeoutSeconds = 1;
		ws::PortForwarder Forwarder;
		Start(Forwarder, { Rule });

		Socket FailedClient = Connect(Rule.m_unListenPort);
		ExpectClosed(FailedClient.Get(), kRefusalTimeoutMilliseconds);
		Check(Forwarder.IsRunning(), "refused target killed listener");

		Server TargetServer(Echo, unTargetPort);
		Socket RecoveredClient = Connect(Rule.m_unListenPort);
		Bytes const vecPayload = BuildPattern(kPayloadBytes);
		Send(RecoveredClient.Get(), vecPayload);
		Check(Receive(RecoveredClient.Get(), vecPayload.size()) == vecPayload, "target recovery failed");
	}

	void TestConnectionAndIdleLimits() {
		constexpr int kExpiryTimeoutMilliseconds = 2500;
		constexpr std::size_t kPayloadBytes = 77;

		Server TargetServer;
		ws::PortForwardConfig Rule = BuildForwardRule(TargetServer.m_Listener.m_unPort);
		Rule.m_unMaximumConnections = 1;
		Rule.m_unIdleTimeoutSeconds = 1;
		ws::PortForwarder Forwarder;
		Start(Forwarder, { Rule });

		Socket FirstClient = Connect(Rule.m_unListenPort);
		WaitUntil([&]() -> bool {
			return TargetServer.m_unAccepted.load() == 1;
		},
			"first capacity-test target connection");

		Socket ExcessClient = Connect(Rule.m_unListenPort);
		ExpectClosed(ExcessClient.Get());

		Bytes const vecPayload = BuildPattern(kPayloadBytes);
		Send(FirstClient.Get(), vecPayload);
		Check(Receive(FirstClient.Get(), vecPayload.size()) == vecPayload, "rejecting excess client damaged active client");
		ExpectClosed(FirstClient.Get(), kExpiryTimeoutMilliseconds);

		Socket NextClient = Connect(Rule.m_unListenPort);
		Send(NextClient.Get(), vecPayload);
		Check(Receive(NextClient.Get(), vecPayload.size()) == vecPayload, "capacity was not released after idle expiry");
	}

	void TestGlobalConnectionLimit() {
		constexpr unsigned kConnectionCount = 256;
		constexpr std::size_t kFirstRuleClientCount = 128;
		constexpr int kAcceptPollMilliseconds = 20;
		constexpr int kMaximumStopSeconds = 2;

		Listener TargetListener;
		std::atomic<bool> bDone = false;
		std::atomic<std::size_t> unAccepted = 0;
		// Hold target sockets in one thread so the test itself does not create a
		// per-connection thread, and check the engine's aggregate cap across rules.
		Task Collector([&]() -> void {
			std::vector<Socket> vecSockets;
			while (!bDone.load()) {
				WSAPOLLFD PollDescriptor { TargetListener.m_Socket.Get(), POLLRDNORM, 0 };
				if (WSAPoll(&PollDescriptor, 1, kAcceptPollMilliseconds) <= 0) {
					continue;
				}

				Socket Client(accept(TargetListener.m_Socket.Get(), nullptr, nullptr));
				if (Client) {
					vecSockets.push_back(std::move(Client));
					unAccepted.fetch_add(1);
				}
			}
		});

		struct StopOnExit {
			explicit StopOnExit(std::atomic<bool>& bDone) :
				m_bDone(bDone)
			{
			}

			~StopOnExit() {
				m_bDone.store(true);
			}

			std::atomic<bool>& m_bDone;
		};

		StopOnExit Finish(bDone);
		ws::PortForwardConfig FirstRule = BuildForwardRule(TargetListener.m_unPort);
		ws::PortForwardConfig SecondRule = BuildForwardRule(TargetListener.m_unPort);
		FirstRule.m_unMaximumConnections = kConnectionCount;
		SecondRule.m_unMaximumConnections = kConnectionCount;
		ws::PortForwarder Forwarder;
		Start(Forwarder, { FirstRule, SecondRule });

		std::vector<Socket> vecClients;
		for (std::size_t unClientIndex = 0; unClientIndex != kConnectionCount; ++unClientIndex) {
			vecClients.push_back(Connect((unClientIndex < kFirstRuleClientCount) ? FirstRule.m_unListenPort : SecondRule.m_unListenPort));
			WaitUntil([&]() -> bool {
				return unAccepted.load() == (unClientIndex + 1);
			},
				"global-cap target connection");
		}

		Socket ExcessClient = Connect(SecondRule.m_unListenPort);
		ExpectClosed(ExcessClient.Get());
		Check(unAccepted.load() == kConnectionCount, "aggregate forwarding cap exceeded 256");

		Clock::time_point const StartTime = Clock::now();
		Forwarder.Stop();
		Check((Clock::now() - StartTime) < std::chrono::seconds(kMaximumStopSeconds), "Stop blocked with 256 active sessions");
		ExpectClosed(vecClients.front().Get());
		Check(!Forwarder.IsRunning(), "health remains running after Stop");
		Forwarder.Stop();
		bDone.store(true);
		Collector.Join();
	}

	void TestStartupRollbackAndCancellation() {
		constexpr int kCancellationDelayMilliseconds = 100;
		constexpr int kMaximumCancellationSeconds = 2;

		Listener OccupiedListener;
		ws::PortForwardConfig FirstRule = BuildForwardRule(OccupiedListener.m_unPort);
		ws::PortForwardConfig SecondRule = BuildForwardRule(OccupiedListener.m_unPort, OccupiedListener.m_unPort);
		ws::PortForwarder Forwarder;
		std::string strError;
		Check(!Forwarder.Start({ FirstRule, SecondRule }, strError) && !strError.empty(), "conflicting startup did not fail");
		Check(!Forwarder.IsRunning(), "failed startup was left running");

		{
			Listener ReclaimedListener(FirstRule.m_unListenPort);
			Check(ReclaimedListener.m_unPort == FirstRule.m_unListenPort, "startup failure leaked its first listener");
		}

		Check(!Forwarder.Start({ FirstRule }, strError, []() -> bool {
			return true;
		}),
			"cancelled startup did not fail");
		Check(strError.find("cancelled") != std::string::npos, "cancelled startup diagnostic");

		for (auto const& pszAddress : { "0.0.0.0", "::" }) {
			ws::PortForwardConfig WildcardRule = FirstRule;
			WildcardRule.m_strListenAddress = pszAddress;
			Check(!Forwarder.Start({ WildcardRule }, strError), "wildcard forwarding listener was accepted");
		}

		ws::PortForwardConfig RemoteRule = FirstRule;
		RemoteRule.m_strTargetAddress = "192.0.2.1";
		Check(!Forwarder.Start({ RemoteRule }, strError), "a non-loopback forwarding target was accepted");

		ws::PortForwardConfig UnavailableRule = FirstRule;
		UnavailableRule.m_strListenAddress = "192.0.2.1"; // Numeric, nonlocal; no outbound traffic is generated.
		Clock::time_point const StartTime = Clock::now();
		Check(!Forwarder.Start({ UnavailableRule }, strError, [&]() -> bool {
			return (Clock::now() - StartTime) > std::chrono::milliseconds(kCancellationDelayMilliseconds);
		}),
			"address-readiness retry ignored cancellation");
		Check((Clock::now() - StartTime) < std::chrono::seconds(kMaximumCancellationSeconds), "cancelled address readiness retry was not bounded");

		Start(Forwarder, { FirstRule });
		Check(!Forwarder.Start({ FirstRule }, strError), "double Start was accepted");
		Forwarder.Stop();

		Listener ReclaimedListener(FirstRule.m_unListenPort);
	}

	void TestFamilyIndependence() {
		constexpr std::size_t kPayloadBytes = 65537;

		for (auto const& bIPv6Listener : { false, true }) {
			Server TargetServer(Echo, 0, bIPv6Listener ? AF_INET : AF_INET6);
			ws::PortForwardConfig Rule = BuildForwardRule(TargetServer.m_Listener.m_unPort);
			if (bIPv6Listener) {
				Rule.m_strListenAddress = "::1";
				Rule.m_unListenPort = FindAvailablePort(AF_INET6);

			} else {
				Rule.m_strTargetAddress = "::1";
			}

			ws::PortForwarder Forwarder;
			Start(Forwarder, { Rule });

			Socket Client = Connect(Rule.m_unListenPort, bIPv6Listener ? AF_INET6 : AF_INET);
			Bytes const vecPayload = BuildPattern(kPayloadBytes);
			Send(Client.Get(), vecPayload);
			Check(Receive(Client.Get(), vecPayload.size()) == vecPayload, "IPv4/IPv6 cross-family payload corruption");
		}
	}

	void TestDisabledIdleAndActiveStop() {
		constexpr int kIdleDelayMilliseconds = 1150;
		constexpr std::size_t kPayloadBytes = 511;
		constexpr std::size_t kLargePayloadBytes = 8 * 1024 * 1024;
		constexpr int kUploadDelayMilliseconds = 30;
		constexpr int kMaximumStopSeconds = 2;

		Server TargetServer;
		ws::PortForwardConfig Rule = BuildForwardRule(TargetServer.m_Listener.m_unPort);
		Rule.m_unIdleTimeoutSeconds = 0;
		ws::PortForwarder Forwarder;
		Start(Forwarder, { Rule });

		Socket Client = Connect(Rule.m_unListenPort);
		WaitUntil([&]() -> bool {
			return TargetServer.m_unAccepted.load() == 1;
		},
			"idle-disabled target connection");
		std::this_thread::sleep_for(std::chrono::milliseconds(kIdleDelayMilliseconds));

		Bytes const vecPayload = BuildPattern(kPayloadBytes);
		Send(Client.Get(), vecPayload);
		Check(Receive(Client.Get(), vecPayload.size()) == vecPayload, "zero idle timeout did not disable expiry");

		Task Writer([&]() -> void {
			Bytes const vecLargePayload = BuildPattern(kLargePayloadBytes);
			try {
				Send(Client.Get(), vecLargePayload);
			} catch (...) {
				// Stop interrupts this upload.
			}
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(kUploadDelayMilliseconds));

		Clock::time_point const StartTime = Clock::now();
		Forwarder.Stop();
		Check((Clock::now() - StartTime) < std::chrono::seconds(kMaximumStopSeconds), "Stop blocked behind a backpressured client");
		Writer.Join();
		Forwarder.Stop();
	}

	constexpr int kUDPReceiveTimeoutMilliseconds = 3000;

	struct UDPSocket {
		explicit UDPSocket(std::uint16_t unRequestedPort = 0, int nAddressFamily = AF_INET) :
			m_Socket(WSASocketW(nAddressFamily, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT)),
			m_nFamily(nAddressFamily)
		{
			constexpr int kReceiveBufferBytes = 256 * 1024;
			constexpr BOOL kExclusiveAccess = TRUE;

			Check(static_cast<bool>(m_Socket), "UDP test socket");
			Check(setsockopt(m_Socket.Get(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&kReceiveBufferBytes), sizeof(kReceiveBufferBytes)) == 0, "UDP test receive buffer");
			Check(setsockopt(m_Socket.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&kExclusiveAccess), sizeof(kExclusiveAccess)) == 0, "exclusive UDP test socket");

			if (m_nFamily == AF_INET) {
				sockaddr_in IPv4Address {};
				IPv4Address.sin_family = AF_INET;
				IPv4Address.sin_port = htons(unRequestedPort);
				IPv4Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				Check(bind(m_Socket.Get(), reinterpret_cast<sockaddr*>(&IPv4Address), sizeof(IPv4Address)) == 0, "UDP IPv4 bind");

				int nAddressBytes = sizeof(IPv4Address);
				Check(getsockname(m_Socket.Get(), reinterpret_cast<sockaddr*>(&IPv4Address), &nAddressBytes) == 0, "UDP IPv4 getsockname");
				m_unPort = ntohs(IPv4Address.sin_port);

			} else {
				sockaddr_in6 IPv6Address {};
				IPv6Address.sin6_family = AF_INET6;
				IPv6Address.sin6_port = htons(unRequestedPort);
				IPv6Address.sin6_addr = in6addr_loopback;
				Check(setsockopt(m_Socket.Get(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char const*>(&kExclusiveAccess), sizeof(kExclusiveAccess)) == 0, "IPv6-only UDP test socket");
				Check(bind(m_Socket.Get(), reinterpret_cast<sockaddr*>(&IPv6Address), sizeof(IPv6Address)) == 0, "UDP IPv6 bind");

				int nAddressBytes = sizeof(IPv6Address);
				Check(getsockname(m_Socket.Get(), reinterpret_cast<sockaddr*>(&IPv6Address), &nAddressBytes) == 0, "UDP IPv6 getsockname");
				m_unPort = ntohs(IPv6Address.sin6_port);
			}

			u_long unNonblockingEnabled = 1;
			Check(ioctlsocket(m_Socket.Get(), FIONBIO, &unNonblockingEnabled) == 0, "nonblocking UDP test socket");
		}

		void ConnectTo(std::uint16_t unDestinationPort) {
			if (m_nFamily == AF_INET) {
				sockaddr_in IPv4Address {};
				IPv4Address.sin_family = AF_INET;
				IPv4Address.sin_port = htons(unDestinationPort);
				IPv4Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				Check(connect(m_Socket.Get(), reinterpret_cast<sockaddr*>(&IPv4Address), sizeof(IPv4Address)) == 0, "UDP IPv4 connect");

			} else {
				sockaddr_in6 IPv6Address {};
				IPv6Address.sin6_family = AF_INET6;
				IPv6Address.sin6_port = htons(unDestinationPort);
				IPv6Address.sin6_addr = in6addr_loopback;
				Check(connect(m_Socket.Get(), reinterpret_cast<sockaddr*>(&IPv6Address), sizeof(IPv6Address)) == 0, "UDP IPv6 connect");
			}
		}

		Socket m_Socket;
		int m_nFamily;
		std::uint16_t m_unPort;
	};

	struct Datagram {
		Datagram() :
			m_RemoteAddress {}
		{
			m_nRemoteAddressBytes = sizeof(m_RemoteAddress);
		}

		Bytes m_vecBytes;
		sockaddr_storage m_RemoteAddress;
		int m_nRemoteAddressBytes;
	};

	bool IsUDPReady(SOCKET hSocket, int nTimeoutMilliseconds) {
		WSAPOLLFD PollDescriptor { hSocket, POLLRDNORM, 0 };
		const int nResult = WSAPoll(&PollDescriptor, 1, nTimeoutMilliseconds);
		Check(nResult != SOCKET_ERROR, "UDP test poll");

		return nResult > 0;
	}

	Datagram ReceiveUDP(SOCKET hSocket, int nTimeoutMilliseconds = kUDPReceiveTimeoutMilliseconds) {
		constexpr std::size_t kMaximumDatagramBytes = 65535;

		Check(IsUDPReady(hSocket, nTimeoutMilliseconds), "UDP datagram timeout");

		Datagram Packet;
		Packet.m_vecBytes.resize(kMaximumDatagramBytes);
		const int nBytes = recvfrom(hSocket, Packet.m_vecBytes.data(), static_cast<int>(Packet.m_vecBytes.size()), 0, reinterpret_cast<sockaddr*>(&Packet.m_RemoteAddress), &Packet.m_nRemoteAddressBytes);
		Check(nBytes >= 0, "UDP receive (" + std::to_string(WSAGetLastError()) + ")");
		Packet.m_vecBytes.resize(static_cast<std::size_t>(nBytes));

		return Packet;
	}

	void SendUDP(SOCKET hSocket, Bytes const& vecBytes) {
		const char chEmpty = 0;
		const int nResult = send(hSocket, vecBytes.empty() ? &chEmpty : vecBytes.data(), static_cast<int>(vecBytes.size()), 0);
		Check(nResult == static_cast<int>(vecBytes.size()), "UDP send (" + std::to_string(WSAGetLastError()) + ")");
	}

	void SendUDPTo(SOCKET hSocket, Bytes const& vecBytes, Datagram const& Destination) {
		const char chEmpty = 0;
		const int nResult = sendto(hSocket, vecBytes.empty() ? &chEmpty : vecBytes.data(), static_cast<int>(vecBytes.size()), 0, reinterpret_cast<sockaddr const*>(&Destination.m_RemoteAddress), Destination.m_nRemoteAddressBytes);
		Check(nResult == static_cast<int>(vecBytes.size()), "UDP sendto (" + std::to_string(WSAGetLastError()) + ")");
	}

	ws::PortForwardConfig BuildUDPRule(std::uint16_t unTargetPort, int nListenFamily = AF_INET, int nTargetFamily = AF_INET) {
		ws::PortForwardConfig Rule = BuildForwardRule(unTargetPort);
		Rule.m_Protocol = ws::PortForwardProtocol::Udp;
		Rule.m_strListenAddress = (nListenFamily == AF_INET) ? "127.0.0.1" : "::1";
		Rule.m_strTargetAddress = (nTargetFamily == AF_INET) ? "127.0.0.1" : "::1";
		Rule.m_unListenPort = UDPSocket(0, nListenFamily).m_unPort;

		return Rule;
	}

	struct UDPServer {
		explicit UDPServer(std::uint16_t unPort = 0, int nFamily = AF_INET, bool bTwoReplies = false) :
			m_Socket(unPort, nFamily),
			m_Worker([this, bTwoReplies]() -> void {
				constexpr int kPollTimeoutMilliseconds = 20;

				while (!m_bDone.load()) {
					if (!IsUDPReady(m_Socket.m_Socket.Get(), kPollTimeoutMilliseconds)) {
						continue;
					}

					Datagram Packet = ReceiveUDP(m_Socket.m_Socket.Get());
					{
						std::lock_guard<std::mutex> Lock(m_Mutex);
						m_vecObserved.push_back(Packet);
					}

					m_unReceived.fetch_add(1);
					SendUDPTo(m_Socket.m_Socket.Get(), Packet.m_vecBytes, Packet);

					if (bTwoReplies) {
						Bytes vecReply = Packet.m_vecBytes;
						vecReply.insert(vecReply.begin(), 'R');
						SendUDPTo(m_Socket.m_Socket.Get(), vecReply, Packet);
					}
				}
			})
		{
		}

		~UDPServer() {
			m_bDone.store(true);

			if (m_Worker.m_Thread.joinable()) {
				m_Worker.m_Thread.join();
			}
		}

		Datagram CopyLastPacket() {
			std::lock_guard<std::mutex> Lock(m_Mutex);
			Check(!m_vecObserved.empty(), "UDP server has no observed peer");

			return m_vecObserved.back();
		}

		UDPSocket m_Socket;
		std::atomic<bool> m_bDone;
		std::atomic<std::size_t> m_unReceived;
		std::mutex m_Mutex;
		std::vector<Datagram> m_vecObserved;
		Task m_Worker;
	};

	void TestParallelUDPDatagrams() {
		constexpr std::size_t kClientCount = 4;
		constexpr std::size_t kRequestCount = 12;
		constexpr std::size_t kLargePayloadBytes = 65000;
		constexpr std::size_t kBasePayloadBytes = 257;
		constexpr std::size_t kPayloadIncrementBytes = 19;
		constexpr std::size_t kClientSeedMultiplier = 71;

		UDPServer TargetServer(0, AF_INET, true);
		ws::PortForwardConfig Rule = BuildUDPRule(TargetServer.m_Socket.m_unPort);
		ws::PortForwarder Forwarder;
		Start(Forwarder, { Rule });

		std::vector<std::unique_ptr<Task>> vecTasks;
		for (std::size_t unClientIndex = 0; unClientIndex != kClientCount; ++unClientIndex) {
			vecTasks.push_back(std::make_unique<Task>([&, unClientIndex]() -> void {
				UDPSocket Client;
				Client.ConnectTo(Rule.m_unListenPort);

				for (std::size_t unRequestIndex = 0; unRequestIndex != kRequestCount; ++unRequestIndex) {
					Bytes const vecPayload = BuildPattern((unRequestIndex == 0) ? 0 : (unRequestIndex == 1) ? kLargePayloadBytes : (kBasePayloadBytes + unRequestIndex * kPayloadIncrementBytes), static_cast<unsigned>(unClientIndex * kClientSeedMultiplier + unRequestIndex));
					SendUDP(Client.m_Socket.Get(), vecPayload);
					Check(ReceiveUDP(Client.m_Socket.Get()).m_vecBytes == vecPayload, "UDP source isolation / first reply boundary");

					Bytes vecSecondReply = vecPayload;
					vecSecondReply.insert(vecSecondReply.begin(), 'R');
					Check(ReceiveUDP(Client.m_Socket.Get()).m_vecBytes == vecSecondReply, "UDP source isolation / second reply boundary");
				}
			}));
		}

		for (auto& pTask : vecTasks) {
			pTask->Join();
		}

		std::set<std::uint16_t> setSourcePorts;
		{
			std::lock_guard<std::mutex> Lock(TargetServer.m_Mutex);
			for (auto const& Packet : TargetServer.m_vecObserved) {
				setSourcePorts.insert(ntohs(reinterpret_cast<sockaddr_in const*>(&Packet.m_RemoteAddress)->sin_port));
			}
		}

		Check(setSourcePorts.size() == kClientCount, "UDP clients shared a target-side source port");
	}

	void TestUDPForeignRepliesAndMaximumSize() {
		constexpr std::array<std::size_t, 3> kDatagramSizes { 0, 1, 65507 };
		constexpr std::size_t kForeignPayloadBytes = 123;
		constexpr unsigned kForeignSeed = 99;
		constexpr int kForeignReplyTimeoutMilliseconds = 150;
		constexpr std::size_t kPushPayloadBytes = 77;
		constexpr unsigned kPushSeed = 13;

		UDPServer TargetServer;
		ws::PortForwardConfig Rule = BuildUDPRule(TargetServer.m_Socket.m_unPort);
		ws::PortForwarder Forwarder;
		Start(Forwarder, { Rule });

		UDPSocket Client;
		Client.ConnectTo(Rule.m_unListenPort);

		for (auto const& unBytes : kDatagramSizes) {
			Bytes const vecPayload = BuildPattern(unBytes, static_cast<unsigned>(unBytes));
			SendUDP(Client.m_Socket.Get(), vecPayload);
			Check(ReceiveUDP(Client.m_Socket.Get()).m_vecBytes == vecPayload, "UDP maximum/zero datagram integrity");
		}

		Datagram const TargetPeer = TargetServer.CopyLastPacket();
		UDPSocket ForeignSender;
		SendUDPTo(ForeignSender.m_Socket.Get(), BuildPattern(kForeignPayloadBytes, kForeignSeed), TargetPeer);
		Check(!IsUDPReady(Client.m_Socket.Get(), kForeignReplyTimeoutMilliseconds), "connected UDP target accepted a foreign sender");

		Bytes const vecPush = BuildPattern(kPushPayloadBytes, kPushSeed);
		SendUDPTo(TargetServer.m_Socket.m_Socket.Get(), vecPush, TargetPeer);
		Check(ReceiveUDP(Client.m_Socket.Get()).m_vecBytes == vecPush, "UDP service-initiated reply was not relayed");
	}

	void TestUDPRefusalRecoveryAndIdleLimits() {
		constexpr std::size_t kInitialPayloadBytes = 21;
		constexpr int kRefusalDelayMilliseconds = 100;
		constexpr std::size_t kPayloadBytes = 129;
		constexpr unsigned kPayloadSeed = 53;
		constexpr std::size_t kMaximumAttempts = 8;
		constexpr int kRetryTimeoutMilliseconds = 200;
		constexpr int kExcessReplyTimeoutMilliseconds = 150;
		constexpr int kExpiryDelayMilliseconds = 1150;

		const std::uint16_t unTargetPort = UDPSocket().m_unPort;
		ws::PortForwardConfig Rule = BuildUDPRule(unTargetPort);
		Rule.m_unMaximumConnections = 1;
		Rule.m_unIdleTimeoutSeconds = 1;
		ws::PortForwarder Forwarder;
		Start(Forwarder, { Rule });

		UDPSocket FirstClient;
		FirstClient.ConnectTo(Rule.m_unListenPort);
		SendUDP(FirstClient.m_Socket.Get(), BuildPattern(kInitialPayloadBytes));
		std::this_thread::sleep_for(std::chrono::milliseconds(kRefusalDelayMilliseconds));
		Check(Forwarder.IsRunning(), "UDP refusal killed listener");

		UDPServer TargetServer(unTargetPort);
		Bytes const vecPayload = BuildPattern(kPayloadBytes, kPayloadSeed);
		// UDP ICMP is asynchronous. Retrying a request is normal datagram semantics;
		// a failed old mapping may consume the first retry before being removed.
		bool bRecovered = false;
		for (std::size_t unAttempt = 0; (unAttempt != kMaximumAttempts) && !bRecovered; ++unAttempt) {
			SendUDP(FirstClient.m_Socket.Get(), vecPayload);

			if (IsUDPReady(FirstClient.m_Socket.Get(), kRetryTimeoutMilliseconds)) {
				bRecovered = ReceiveUDP(FirstClient.m_Socket.Get()).m_vecBytes == vecPayload;
			}
		}

		Check(bRecovered, "UDP target did not recover after refusal");

		UDPSocket ExcessClient;
		ExcessClient.ConnectTo(Rule.m_unListenPort);
		SendUDP(ExcessClient.m_Socket.Get(), vecPayload);
		Check(!IsUDPReady(ExcessClient.m_Socket.Get(), kExcessReplyTimeoutMilliseconds), "UDP per-rule mapping cap was exceeded");
		std::this_thread::sleep_for(std::chrono::milliseconds(kExpiryDelayMilliseconds));
		SendUDP(ExcessClient.m_Socket.Get(), vecPayload);
		Check(ReceiveUDP(ExcessClient.m_Socket.Get()).m_vecBytes == vecPayload, "UDP idle expiry did not release mapping capacity");
		Forwarder.Stop();
		Forwarder.Stop();

		UDPSocket ReboundListener(Rule.m_unListenPort);
	}

	void TestMixedTCPUDPSharedPortAndGlobalLimit() {
		constexpr unsigned kGlobalConnectionLimit = 256;
		constexpr std::size_t kUDPClientCount = 255;
		constexpr std::size_t kPayloadBytes = 513;
		constexpr unsigned kPayloadSeed = 17;
		constexpr int kExcessReplyTimeoutMilliseconds = 150;
		constexpr int kMaximumStopSeconds = 2;

		Server TCPServer;
		UDPServer TargetUDPServer;
		const std::uint16_t unSharedPort = FindAvailableTCPUDPPort();
		ws::PortForwardConfig TCPRule = BuildForwardRule(TCPServer.m_Listener.m_unPort, unSharedPort);
		ws::PortForwardConfig UDPRule = BuildUDPRule(TargetUDPServer.m_Socket.m_unPort);
		UDPRule.m_unListenPort = unSharedPort;
		UDPRule.m_unMaximumConnections = kGlobalConnectionLimit;
		ws::PortForwarder Forwarder;
		Start(Forwarder, { TCPRule, UDPRule });

		Socket TCPClient = Connect(TCPRule.m_unListenPort);
		Bytes const vecPayload = BuildPattern(kPayloadBytes, kPayloadSeed);
		Send(TCPClient.Get(), vecPayload);
		Check(Receive(TCPClient.Get(), vecPayload.size()) == vecPayload, "mixed TCP same-port relay failed");

		std::vector<std::unique_ptr<UDPSocket>> vecUDPClients;
		for (std::size_t unClientIndex = 0; unClientIndex != kUDPClientCount; ++unClientIndex) {
			std::unique_ptr<UDPSocket> pClient = std::make_unique<UDPSocket>();
			pClient->ConnectTo(UDPRule.m_unListenPort);
			SendUDP(pClient->m_Socket.Get(), vecPayload);
			Check(ReceiveUDP(pClient->m_Socket.Get()).m_vecBytes == vecPayload, "mixed-protocol global-cap mapping creation");
			vecUDPClients.push_back(std::move(pClient));
		}

		UDPSocket ExcessClient;
		ExcessClient.ConnectTo(UDPRule.m_unListenPort);
		SendUDP(ExcessClient.m_Socket.Get(), vecPayload);
		Check(!IsUDPReady(ExcessClient.m_Socket.Get(), kExcessReplyTimeoutMilliseconds), "TCP/UDP aggregate cap exceeded 256");
		Send(TCPClient.Get(), vecPayload);
		Check(Receive(TCPClient.Get(), vecPayload.size()) == vecPayload, "UDP capacity rejection damaged TCP session");

		Clock::time_point const StartTime = Clock::now();
		Forwarder.Stop();
		Check((Clock::now() - StartTime) < std::chrono::seconds(kMaximumStopSeconds), "Stop blocked with mixed UDP/TCP mappings");

		Listener TCPReboundListener(TCPRule.m_unListenPort);
		UDPSocket UDPReboundListener(UDPRule.m_unListenPort);
	}

	void TestUDPOversizeAndFamilies() {
		constexpr std::array<std::size_t, 3> kDatagramSizes { 0, 32771, 65507 };
		constexpr unsigned kPayloadSeed = 37;
		constexpr std::size_t kOversizeDatagramBytes = 65508;
		constexpr int kOversizeReplyTimeoutMilliseconds = 150;
		constexpr std::size_t kRecoveryPayloadBytes = 13;
		constexpr std::size_t kReplyRecoveryPayloadBytes = 17;

		for (auto const& bIPv6Listener : { false, true }) {
			const int nListenerFamily = bIPv6Listener ? AF_INET6 : AF_INET;
			const int nTargetFamily = bIPv6Listener ? AF_INET : AF_INET6;
			UDPServer TargetServer(0, nTargetFamily);
			ws::PortForwardConfig Rule = BuildUDPRule(TargetServer.m_Socket.m_unPort, nListenerFamily, nTargetFamily);
			ws::PortForwarder Forwarder;
			Start(Forwarder, { Rule });

			UDPSocket Client(0, nListenerFamily);
			Client.ConnectTo(Rule.m_unListenPort);

			for (auto const& unBytes : kDatagramSizes) {
				Bytes const vecPayload = BuildPattern(unBytes, kPayloadSeed);
				SendUDP(Client.m_Socket.Get(), vecPayload);
				Check(ReceiveUDP(Client.m_Socket.Get()).m_vecBytes == vecPayload, "UDP IPv4/IPv6 independence failed");
			}

			if (bIPv6Listener) {
				const std::size_t unReceivedBefore = TargetServer.m_unReceived.load();
				SendUDP(Client.m_Socket.Get(), BuildPattern(kOversizeDatagramBytes));
				Check(!IsUDPReady(Client.m_Socket.Get(), kOversizeReplyTimeoutMilliseconds), "oversized UDP datagram was relayed/truncated");
				Check(TargetServer.m_unReceived.load() == unReceivedBefore, "oversized UDP prefix reached target service");

				Bytes const vecPayload = BuildPattern(kRecoveryPayloadBytes);
				SendUDP(Client.m_Socket.Get(), vecPayload);
				Check(ReceiveUDP(Client.m_Socket.Get()).m_vecBytes == vecPayload, "oversized UDP input damaged listener");
				// This target is IPv4, so its datagram limit is already 65507.
				// IPv6-target oversize replies are checked in the opposite case below.
				TargetServer.CopyLastPacket();

			} else {
				Datagram const MappedPeer = TargetServer.CopyLastPacket();
				SendUDPTo(TargetServer.m_Socket.m_Socket.Get(), BuildPattern(kOversizeDatagramBytes), MappedPeer);
				Check(!IsUDPReady(Client.m_Socket.Get(), kOversizeReplyTimeoutMilliseconds), "oversized UDP target reply was truncated/relayed");

				Bytes const vecPayload = BuildPattern(kReplyRecoveryPayloadBytes);
				SendUDP(Client.m_Socket.Get(), vecPayload);
				Check(ReceiveUDP(Client.m_Socket.Get()).m_vecBytes == vecPayload, "oversized target reply damaged UDP mapping");
			}
		}
	}

	void TestUDPStartupRollbackAndDisabledIdle() {
		constexpr std::size_t kPayloadBytes = 81;
		constexpr int kIdleDelayMilliseconds = 1150;

		UDPServer TargetServer;
		ws::PortForwardConfig Rule = BuildUDPRule(TargetServer.m_Socket.m_unPort);
		Rule.m_unIdleTimeoutSeconds = 0;
		ws::PortForwarder Forwarder;
		std::string strError;
		{
			UDPSocket OccupiedListener(Rule.m_unListenPort);
			Check(!Forwarder.Start({ Rule }, strError) && !strError.empty(), "UDP occupied-port startup succeeded");
		}

		ws::PortForwardConfig OtherRule = Rule;
		OtherRule.m_unListenPort = UDPSocket().m_unPort;
		{
			UDPSocket OccupiedListener(OtherRule.m_unListenPort);
			Check(!Forwarder.Start({ Rule, OtherRule }, strError), "UDP multi-rule rollback was not triggered");

			UDPSocket ReclaimedListener(Rule.m_unListenPort);
		}

		Check(!Forwarder.Start({ Rule }, strError, []() -> bool {
			return true;
		}),
			"UDP startup ignored cancellation");
		Start(Forwarder, { Rule });

		UDPSocket Client;
		Client.ConnectTo(Rule.m_unListenPort);

		Bytes const vecPayload = BuildPattern(kPayloadBytes);
		SendUDP(Client.m_Socket.Get(), vecPayload);
		Check(ReceiveUDP(Client.m_Socket.Get()).m_vecBytes == vecPayload, "UDP idle-disabled first datagram");
		std::this_thread::sleep_for(std::chrono::milliseconds(kIdleDelayMilliseconds));
		SendUDP(Client.m_Socket.Get(), vecPayload);
		Check(ReceiveUDP(Client.m_Socket.Get()).m_vecBytes == vecPayload, "UDP idle timeout zero removed mapping");

		std::set<std::uint16_t> setSourcePorts;
		{
			std::lock_guard<std::mutex> Lock(TargetServer.m_Mutex);
			for (auto const& Packet : TargetServer.m_vecObserved) {
				setSourcePorts.insert(ntohs(reinterpret_cast<sockaddr_in const*>(&Packet.m_RemoteAddress)->sin_port));
			}
		}

		Check(setSourcePorts.size() == 1, "UDP idle-disabled mapping source port changed");
	}

	void TestForwardRuleLimit() {
		constexpr std::size_t kMaximumRuleCount = 32;
		constexpr std::size_t kPayloadBytes = 67;

		Server TCPServer;
		UDPServer TargetUDPServer;
		std::vector<Listener> vecTCPReservations;
		std::vector<UDPSocket> vecUDPReservations;
		std::vector<ws::PortForwardConfig> vecRules;
		for (std::size_t unRuleIndex = 0; unRuleIndex < kMaximumRuleCount; ++unRuleIndex) {
			if ((unRuleIndex % 2) == 0) {
				vecTCPReservations.emplace_back();
				vecRules.push_back(BuildForwardRule(TCPServer.m_Listener.m_unPort, vecTCPReservations.back().m_unPort));

			} else {
				vecUDPReservations.emplace_back();
				ws::PortForwardConfig Rule = BuildForwardRule(TargetUDPServer.m_Socket.m_unPort, vecUDPReservations.back().m_unPort);
				Rule.m_Protocol = ws::PortForwardProtocol::Udp;
				vecRules.push_back(Rule);
			}
		}

		ws::PortForwarder Forwarder;
		std::string strError;
		vecRules.push_back(vecRules.front());
		Check(!Forwarder.Start(vecRules, strError) && !Forwarder.IsRunning() && (strError.find("At most 32 port forwarding rules") != std::string::npos), "33rd forwarding rule was not rejected before startup");
		vecRules.pop_back();
		vecTCPReservations.clear();
		vecUDPReservations.clear();
		Start(Forwarder, vecRules);

		for (std::size_t unRuleIndex = 0; unRuleIndex < vecRules.size(); ++unRuleIndex) {
			ws::PortForwardConfig const& Rule = vecRules[unRuleIndex];
			Bytes const vecPayload = BuildPattern(kPayloadBytes, static_cast<unsigned>(unRuleIndex));
			if (Rule.m_Protocol == ws::PortForwardProtocol::Udp) {
				UDPSocket Client;
				Client.ConnectTo(Rule.m_unListenPort);
				SendUDP(Client.m_Socket.Get(), vecPayload);
				Check(ReceiveUDP(Client.m_Socket.Get()).m_vecBytes == vecPayload, "UDP forwarding failed at the 32-rule limit");

			} else {
				Socket Client = Connect(Rule.m_unListenPort);
				Send(Client.Get(), vecPayload);
				Check(Receive(Client.Get(), vecPayload.size()) == vecPayload, "TCP forwarding failed at the 32-rule limit");
			}
		}

		Forwarder.Stop();
		Check(!Forwarder.IsRunning(), "32-rule forwarder remained running after Stop");
	}
} // namespace

int main() {
	try {
		WSAScope WSA;

		std::cout << "Parallel streaming/backpressure..." << std::endl;
		TestParallelBackpressure();

		std::cout << "Half-close late response..." << std::endl;
		TestHalfCloseLateReply();

		std::cout << "Refused target/recovery..." << std::endl;
		TestRefusalRecovery();

		std::cout << "Connection/idle limits..." << std::endl;
		TestConnectionAndIdleLimits();

		std::cout << "Global connection limit..." << std::endl;
		TestGlobalConnectionLimit();

		std::cout << "Startup rollback/cancel..." << std::endl;
		TestStartupRollbackAndCancellation();

		std::cout << "IPv4/IPv6 independence..." << std::endl;
		TestFamilyIndependence();

		std::cout << "Disabled idle/active Stop..." << std::endl;
		TestDisabledIdleAndActiveStop();

		std::cout << "UDP parallel clients/datagram boundaries..." << std::endl;
		TestParallelUDPDatagrams();

		std::cout << "UDP zero/maximum size/foreign-reply isolation..." << std::endl;
		TestUDPForeignRepliesAndMaximumSize();

		std::cout << "UDP refusal/idle/per-rule limits..." << std::endl;
		TestUDPRefusalRecoveryAndIdleLimits();

		std::cout << "Mixed TCP/UDP same-port/global-cap..." << std::endl;
		TestMixedTCPUDPSharedPortAndGlobalLimit();

		std::cout << "UDP oversize/IPv4/IPv6..." << std::endl;
		TestUDPOversizeAndFamilies();

		std::cout << "UDP startup rollback/cancel/disabled idle..." << std::endl;
		TestUDPStartupRollbackAndDisabledIdle();

		std::cout << "32-rule startup/33rd-rule rejection..." << std::endl;
		TestForwardRuleLimit();

		std::cout << "TCP and UDP forwarding tests passed (parallel streams/datagrams, bounded buffers, half-close, source isolation, refusal/recovery, per-rule/shared global/idle/32-rule limits, rollback/rebind, IPv4/IPv6, Stop).\n";

		return 0;
	} catch (std::exception const& Exception) {
		std::cerr << "TCP forwarding test failed: " << Exception.what() << '\n';
		return 1;
	}
}
