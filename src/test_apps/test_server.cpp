
// General
#include <winsock2.h>
#include <Windows.h>

// C++
#include <cstdint>
#include <cstdio>
#include <exception>
#include <utility>

// STL
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// Project
#include "test_apps_common.hpp"

namespace st::testapp {
	namespace {
		constexpr std::size_t kQueueLimit = 256 * 1024;
		constexpr std::size_t kIOBudget = 128 * 1024;
		constexpr std::size_t kServiceBudget = 256;
		constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

		static_assert(kIOBudget <= std::numeric_limits<int>::max());
		static_assert(std::numeric_limits<std::size_t>::max() <= std::numeric_limits<std::uint64_t>::max());
		static_assert(std::is_same_v<std::uint8_t, unsigned char>);

		// Queue/offset extents are maintained only by Server's bounded read/write operations.
		struct TCPClient {
			TCPClient() {
				m_unOffset = 0;
				m_unQueuedBytes = 0;
				m_bEOF = false;
				m_LastActivity = Clock::now();
			}

			net::Socket m_Socket;
			std::string m_strSource;
			std::deque<Bytes> m_deqPendingChunks;
			std::size_t m_unOffset;
			std::size_t m_unQueuedBytes;
			bool m_bEOF;
			Clock::time_point m_LastActivity;
		};

		void AddCounter(std::uint64_t& unCounterOut, std::uint64_t unAmount) {
			if (unAmount > (std::numeric_limits<std::uint64_t>::max() - unCounterOut)) {
				throw std::overflow_error("Echo server counter overflow");
			}

			unCounterOut += unAmount;
		}

		class Server {
		public:
			explicit Server(ServerOptions Options) :
				m_Options(std::move(Options))
			{
				m_hNextClient = 0;
				m_bFailed = false;
				m_unTCPAccepted = 0;
				m_unTCPClosed = 0;
				m_unTCPRejected = 0;
				m_unTCPReadBytes = 0;
				m_unTCPWrittenBytes = 0;
				m_unUDPReceived = 0;
				m_unUDPEchoed = 0;
				m_unUDPDropped = 0;
				m_unUDPReadBytes = 0;
				m_unUDPWrittenBytes = 0;
			}

			int Run() {
				constexpr std::chrono::seconds kStatisticsInterval(5);

				std::string strError;
				if (m_Options.m_bTCP) {
					m_TCPSocket = net::Bind(m_Options.m_Listen, SOCK_STREAM, strError);
					if (!m_TCPSocket) {
						Log("TCP startup failed: " + strError);
						return 1;
					}

					if (listen(m_TCPSocket.get(), SOMAXCONN) == SOCKET_ERROR) {
						Log("TCP listen failed: " + net::Error());
						return 1;
					}
				}

				if (m_Options.m_bUDP) {
					m_UDPSocket = net::Bind(m_Options.m_Listen, SOCK_DGRAM, strError);
					if (!m_UDPSocket) {
						Log("UDP startup failed: " + strError);
						return 1;
					}

					net::ConfigureUdp(m_UDPSocket.get());
				}

				if (m_TCPSocket) {
					Log("READY TCP " + net::Bound(m_TCPSocket.get()).ToString());
				}

				if (m_UDPSocket) {
					Log("READY UDP " + net::Bound(m_UDPSocket.get()).ToString());
				}

				Log("Raw binary echo server; TCP queue limit=" + std::to_string(kQueueLimit) + " bytes/client; max TCP clients=" + std::to_string(m_Options.m_unMaximumClients));

				Clock::time_point const Start = Clock::now();
				Clock::time_point NextStatistics = Start + kStatisticsInterval;
				while (!Stopping() && !m_bFailed) {
					if (m_Options.m_nRunSeconds && ((Clock::now() - Start) >= std::chrono::seconds(m_Options.m_nRunSeconds))) {
						break;
					}

					Poll();
					ExpireIdle();
					Clock::time_point const Now = Clock::now();
					if (Now >= NextStatistics) {
						PrintStatistics("STATS");
						NextStatistics = Now + kStatisticsInterval;
					}
				}

				while (!m_mapClients.empty()) {
					Close(m_mapClients.begin(), "server stopping");
				}

				PrintStatistics("FINAL");

				return m_bFailed ? 1 : 0;
			}

		private:
			void PrintStatistics(char const* pszKind) const {
				Log(std::string(pszKind) + " TCP active=" + std::to_string(m_mapClients.size()) + " accepted=" + std::to_string(m_unTCPAccepted) + " closed=" + std::to_string(m_unTCPClosed) + " rejected=" + std::to_string(m_unTCPRejected) + " received=" + std::to_string(m_unTCPReadBytes) + "B echoed=" + std::to_string(m_unTCPWrittenBytes) + "B");
				Log(std::string(pszKind) + " UDP received=" + std::to_string(m_unUDPReceived) + " echoed=" + std::to_string(m_unUDPEchoed) + " dropped=" + std::to_string(m_unUDPDropped) + " received_bytes=" + std::to_string(m_unUDPReadBytes) + " echoed_bytes=" + std::to_string(m_unUDPWrittenBytes));
			}

			void Close(std::map<SOCKET, TCPClient>::iterator itClient, char const* pszReason) {
				Log("TCP closed " + itClient->second.m_strSource + " (" + pszReason + ")");
				m_mapClients.erase(itClient);
				AddCounter(m_unTCPClosed, 1);
			}

			void Accept() {
				constexpr std::size_t kAcceptBudget = 32;

				for (std::size_t unCount = 0; unCount < kAcceptBudget; ++unCount) {
					net::Address Source {};
					net::Socket Socket(accept(m_TCPSocket.get(), Source.ptr(), &Source.m_nSize));
					if (!Socket) {
						const int nError = WSAGetLastError();
						if (net::WouldBlock(nError)) {
							break;
						}

						Log("TCP accept failed: " + net::Error(nError));
						m_bFailed = true;
						break;
					}

					if (m_mapClients.size() >= m_Options.m_unMaximumClients) {
						AddCounter(m_unTCPRejected, 1);

						if (m_Options.m_bVerbose) {
							Log("TCP rejected " + net::Describe(Source).ToString() + " (client limit)");
						}

						continue;
					}

					if (!net::Nonblocking(Socket.get())) {
						const int nError = WSAGetLastError();
						AddCounter(m_unTCPRejected, 1);
						Log("TCP cannot configure accepted socket: " + net::Error(nError));
						continue;
					}

					net::ConfigureTcp(Socket.get());
					TCPClient Client;
					Client.m_strSource = net::Describe(Source).ToString();
					const SOCKET hSocket = Socket.get();
					Client.m_Socket = std::move(Socket);
					m_mapClients.emplace(hSocket, std::move(Client));
					AddCounter(m_unTCPAccepted, 1);
					Log("TCP accepted " + m_mapClients.at(hSocket).m_strSource);
				}
			}

			bool Write(TCPClient& Client, std::size_t& unBudget) {
				while (!Client.m_deqPendingChunks.empty() && unBudget) {
					Bytes const& vecChunk = Client.m_deqPendingChunks.front();
					const std::size_t unLength = std::min(vecChunk.size() - Client.m_unOffset, unBudget);
					char const* pchBytes = reinterpret_cast<char const*>(vecChunk.data() + Client.m_unOffset);
					const int nSent = send(Client.m_Socket.get(), pchBytes, static_cast<int>(unLength), 0);
					if (nSent == SOCKET_ERROR) {
						const int nError = WSAGetLastError();
						if (net::WouldBlock(nError)) {
							return true;
						}

						if (m_Options.m_bVerbose) {
							Log("TCP send to " + Client.m_strSource + " failed: " + net::Error(nError));
						}

						return false;
					}

					if (!nSent) {
						return false;
					}

					const std::size_t unBytesSent = static_cast<std::size_t>(nSent);
					Client.m_unOffset += unBytesSent;
					Client.m_unQueuedBytes -= unBytesSent;
					unBudget -= unBytesSent;
					AddCounter(m_unTCPWrittenBytes, unBytesSent);
					Client.m_LastActivity = Clock::now();

					if (Client.m_unOffset == vecChunk.size()) {
						Client.m_deqPendingChunks.pop_front();
						Client.m_unOffset = 0;
					}
				}

				return true;
			}

			bool Read(TCPClient& Client) {
				constexpr std::size_t kReadBufferBytes = 16 * 1024;

				static_assert(kReadBufferBytes <= std::numeric_limits<int>::max());

				std::array<char, kReadBufferBytes> arrBuffer {};
				std::size_t unBudget = kIOBudget;
				while (!Client.m_bEOF && unBudget && (Client.m_unQueuedBytes < kQueueLimit)) {
					const std::size_t unLength = std::min({ arrBuffer.size(), unBudget, kQueueLimit - Client.m_unQueuedBytes });
					const int nReceived = recv(Client.m_Socket.get(), arrBuffer.data(), static_cast<int>(unLength), 0);
					if (nReceived == SOCKET_ERROR) {
						const int nError = WSAGetLastError();
						if (net::WouldBlock(nError)) {
							return true;
						}

						if (m_Options.m_bVerbose) {
							Log("TCP read from " + Client.m_strSource + " failed: " + net::Error(nError));
						}

						return false;
					}

					if (!nReceived) {
						Client.m_bEOF = true;
						return true;
					}

					const std::size_t unCount = static_cast<std::size_t>(nReceived);
					std::uint8_t const* punFirst = reinterpret_cast<std::uint8_t const*>(arrBuffer.data());
					Client.m_deqPendingChunks.emplace_back(punFirst, punFirst + unCount);
					Client.m_unQueuedBytes += unCount;
					unBudget -= unCount;
					AddCounter(m_unTCPReadBytes, unCount);
					Client.m_LastActivity = Clock::now();

					if (m_Options.m_bVerbose) {
						Log("TCP " + Client.m_strSource + " bytes=" + std::to_string(unCount) + " " + Preview(arrBuffer.data(), unCount));
					}
				}

				return true;
			}

			void Service(SOCKET hSocket, short nEvents) {
				auto const itClient = m_mapClients.find(hSocket);
				if (itClient == m_mapClients.end()) {
					return;
				}

				if (nEvents & POLLNVAL) {
					Close(itClient, "invalid socket");
					return;
				}

				TCPClient& Client = itClient->second;
				if (nEvents & POLLERR) {
					int nError = 0;
					int nSize = static_cast<int>(sizeof(nError));
					if (getsockopt(hSocket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&nError), &nSize) || nError) {
						Close(itClient, "socket error");
						return;
					}
				}

				std::size_t unWriteBudget = kIOBudget;
				if (!Write(Client, unWriteBudget)) {
					Close(itClient, "send failed");
					return;
				}

				if (!Client.m_bEOF && (nEvents & (POLLRDNORM | POLLIN | POLLHUP)) && !Read(Client)) {
					Close(itClient, "receive failed");
					return;
				}

				if (!Write(Client, unWriteBudget)) {
					Close(itClient, "send failed");
					return;
				}

				if (Client.m_bEOF && Client.m_deqPendingChunks.empty()) {
					// recv()==0 is handled once. All application bytes have entered the
					// kernel send queue before FIN; the peer can drain them and read EOF.
					// Terminal half-close is best effort; Close immediately tears down this owned socket.
					shutdown(hSocket, SD_SEND);
					Close(itClient, "EOF; echo drained");
				}
			}

			void EchoUDP() {
				constexpr std::size_t kReceiveBytes = kMaxUdp + 1;
				constexpr std::size_t kPacketBudget = 256;

				static_assert(kMaxUdp < std::numeric_limits<std::size_t>::max());
				static_assert(kReceiveBytes <= std::numeric_limits<int>::max());

				std::array<char, kReceiveBytes> arrBuffer {};
				for (std::size_t unPacket = 0; unPacket < kPacketBudget; ++unPacket) {
					net::Address Source {};
					const int nReceived = recvfrom(m_UDPSocket.get(), arrBuffer.data(), static_cast<int>(arrBuffer.size()), 0, Source.ptr(), &Source.m_nSize);
					if (nReceived == SOCKET_ERROR) {
						const int nError = WSAGetLastError();
						if (net::WouldBlock(nError)) {
							break;
						}

						AddCounter(m_unUDPDropped, 1);

						if ((nError == WSAEMSGSIZE) || (nError == WSAECONNRESET)) {
							continue;
						}

						Log("UDP receive failed: " + net::Error(nError));
						m_bFailed = true;
						break;
					}

					AddCounter(m_unUDPReceived, 1);
					AddCounter(m_unUDPReadBytes, static_cast<std::uint64_t>(nReceived));

					if (static_cast<std::size_t>(nReceived) > kMaxUdp) {
						AddCounter(m_unUDPDropped, 1);
						continue;
					}

					if (m_unUDPReceived == 1) {
						Log("UDP first source " + net::Describe(Source).ToString());
					}

					if (m_Options.m_bVerbose) {
						Log("UDP " + net::Describe(Source).ToString() + " bytes=" + std::to_string(nReceived) + " " + Preview(arrBuffer.data(), static_cast<std::size_t>(nReceived)));
					}

					const int nSent = sendto(m_UDPSocket.get(), arrBuffer.data(), nReceived, 0, Source.ptr(), Source.m_nSize);
					const int nSendError = (nSent == SOCKET_ERROR) ? WSAGetLastError() : 0;
					if ((nSent == SOCKET_ERROR) || (nSent != nReceived)) {
						AddCounter(m_unUDPDropped, 1);

						if ((nSent == SOCKET_ERROR) && m_Options.m_bVerbose) {
							Log("UDP echo dropped: " + net::Error(nSendError));
						}
					} else {
						AddCounter(m_unUDPEchoed, 1);
						AddCounter(m_unUDPWrittenBytes, static_cast<std::uint64_t>(nSent));
					}
				}
			}

			void Poll() {
				constexpr std::size_t kListenerSlots = 2;
				constexpr int kPollMilliseconds = 20;

				if (m_mapClients.size() > (std::numeric_limits<std::size_t>::max() - kListenerSlots)) {
					throw std::length_error("Echo poll descriptor count overflow");
				}

				std::vector<WSAPOLLFD> vecDescriptors;
				vecDescriptors.reserve(m_mapClients.size() + kListenerSlots);
				std::size_t unTCPIndex = kNoIndex;
				std::size_t unUDPIndex = kNoIndex;
				if (m_TCPSocket) {
					unTCPIndex = vecDescriptors.size();
					vecDescriptors.push_back({ m_TCPSocket.get(), POLLRDNORM, 0 });
				}

				if (m_UDPSocket) {
					unUDPIndex = vecDescriptors.size();
					vecDescriptors.push_back({ m_UDPSocket.get(), POLLRDNORM, 0 });
				}

				const std::size_t unFirstClient = vecDescriptors.size();
				std::vector<SOCKET> vecKeys;
				vecKeys.reserve(m_mapClients.size());
				for (auto const& ClientEntry : m_mapClients) {
					const SOCKET hSocket = ClientEntry.first;
					TCPClient const& Client = ClientEntry.second;
					short nEvents = 0;
					if (!Client.m_bEOF && (Client.m_unQueuedBytes < kQueueLimit)) {
						nEvents |= POLLRDNORM;
					}

					if (!Client.m_deqPendingChunks.empty()) {
						nEvents |= POLLWRNORM;
					}

					vecDescriptors.push_back({ hSocket, nEvents, 0 });
					vecKeys.push_back(hSocket);
				}

				if (vecDescriptors.empty()) {
					Pause(kPollMilliseconds);
					return;
				}

				if (vecDescriptors.size() > std::numeric_limits<ULONG>::max()) {
					throw std::length_error("Echo poll descriptor count exceeds Winsock limit");
				}

				const int nResult = WSAPoll(vecDescriptors.data(), static_cast<ULONG>(vecDescriptors.size()), kPollMilliseconds);
				if (nResult == SOCKET_ERROR) {
					const int nError = WSAGetLastError();
					if (nError != WSAEINTR) {
						Log("WSAPoll failed: " + net::Error(nError));
						m_bFailed = true;
					}

					return;
				}

				if ((unTCPIndex != kNoIndex) && vecDescriptors[unTCPIndex].revents) {
					Accept();
				}

				if ((unUDPIndex != kNoIndex) && vecDescriptors[unUDPIndex].revents) {
					EchoUDP();
				}

				if (vecKeys.empty()) {
					return;
				}

				// lower_bound lies in this same range, so its nonnegative difference fits size_t.
				std::size_t unIndex = static_cast<std::size_t>(std::lower_bound(vecKeys.begin(), vecKeys.end(), m_hNextClient) - vecKeys.begin());
				if (unIndex == vecKeys.size()) {
					unIndex = 0;
				}

				const std::size_t unCount = std::min(vecKeys.size(), kServiceBudget);
				for (std::size_t unServiceIndex = 0; unServiceIndex < unCount; ++unServiceIndex) {
					const SOCKET hSocket = vecKeys[unIndex];
					m_hNextClient = (hSocket == std::numeric_limits<SOCKET>::max()) ? 0 : (hSocket + 1);

					if (vecDescriptors[unFirstClient + unIndex].revents) {
						Service(hSocket, vecDescriptors[unFirstClient + unIndex].revents);
					}

					// unIndex < vecKeys.size(); increment and wrap avoid overflowing start+serviceIndex.
					++unIndex;
					if (unIndex == vecKeys.size()) {
						unIndex = 0;
					}
				}
			}

			void ExpireIdle() {
				if (m_Options.m_nIdleSeconds <= 0) {
					return;
				}

				Clock::time_point const Now = Clock::now();
				for (auto itClient = m_mapClients.begin(); itClient != m_mapClients.end();) {
					if ((Now - itClient->second.m_LastActivity) >= std::chrono::seconds(m_Options.m_nIdleSeconds)) {
						auto const itExpired = itClient++;
						Close(itExpired, "idle timeout");
					} else {
						++itClient;
					}
				}
			}

			ServerOptions m_Options;
			net::Socket m_TCPSocket;
			net::Socket m_UDPSocket;
			std::map<SOCKET, TCPClient> m_mapClients;
			SOCKET m_hNextClient;
			bool m_bFailed;
			std::uint64_t m_unTCPAccepted;
			std::uint64_t m_unTCPClosed;
			std::uint64_t m_unTCPRejected;
			std::uint64_t m_unTCPReadBytes;
			std::uint64_t m_unTCPWrittenBytes;
			std::uint64_t m_unUDPReceived;
			std::uint64_t m_unUDPEchoed;
			std::uint64_t m_unUDPDropped;
			std::uint64_t m_unUDPReadBytes;
			std::uint64_t m_unUDPWrittenBytes;
		};
	} // namespace
} // namespace st::testapp

int main(int nArgumentCount, char** ppszArguments) {
	// Console encoding is best effort so redirected/headless use remains available.
	SetConsoleOutputCP(CP_UTF8);

	try {
		st::testapp::ServerOptions Options;
		std::string strError;
		if (!st::testapp::ParseServer(nArgumentCount, ppszArguments, Options, strError)) {
			std::cerr
				<< "Error: " << strError << "\n"
				<< "Use WireSteamTestServer.exe --help\n";
			return 2;
		}

		if (Options.m_bHelp) {
			std::cout << st::testapp::ServerHelp();
			return 0;
		}

		st::testapp::Session NetworkSession;
		if (!NetworkSession) {
			std::cerr << "WSAStartup failed\n";
			return 1;
		}

		return st::testapp::Server(std::move(Options)).Run();
	} catch (std::exception const& Error) {
		// Failure output is best effort and cannot throw beyond the process entry point.
		std::fprintf(stderr, "Fatal: %s\n", Error.what());
		std::fflush(stderr);
		return 1;
	} catch (...) {
		std::fputs("Fatal: unexpected exception\n", stderr);
		std::fflush(stderr);
		return 1;
	}
}
