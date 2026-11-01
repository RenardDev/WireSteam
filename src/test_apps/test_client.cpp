
// General
#include <winsock2.h>

// C++
#include <cstdint>
#include <cstdio>
#include <exception>

// STL
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Project
#include "test_apps_common.hpp"

namespace st::testapp {
	namespace {
		struct Results {
			Results() {
				m_unSent = 0;
				m_unReceived = 0;
				m_unLost = 0;
				m_unErrors = 0;
				m_unBytes = 0;
				m_unDuplicates = 0;
				m_unLate = 0;
				m_dblMinimum = std::numeric_limits<double>::max();
				m_dblMaximum = 0;
				m_dblTotal = 0;
				m_bInterrupted = false;
			}

			void RecordRTT(Clock::time_point Begin) {
				const double dblMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - Begin).count();
				m_dblMinimum = std::min(m_dblMinimum, dblMilliseconds);
				m_dblMaximum = std::max(m_dblMaximum, dblMilliseconds);
				m_dblTotal += dblMilliseconds;
			}

			std::uint64_t m_unSent;
			std::uint64_t m_unReceived;
			std::uint64_t m_unLost;
			std::uint64_t m_unErrors;
			std::uint64_t m_unBytes;
			std::uint64_t m_unDuplicates;
			std::uint64_t m_unLate;
			double m_dblMinimum;
			double m_dblMaximum;
			double m_dblTotal;
			bool m_bInterrupted;
		};

		std::string GetConnectionLabel(bool bTCP, unsigned unConnection) {
			return std::string(bTCP ? "TCP" : "UDP") + " connection=" + std::to_string(unConnection + 1);
		}

		Clock::time_point GetExchangeDeadline(ClientOptions const& Options) {
			return Clock::now() + std::chrono::milliseconds(Options.m_nTimeoutMilliseconds);
		}

		bool Fail(Results& ResultsOut, std::string const& strLabel, std::string const& strReason) {
			if (Stopping()) {
				ResultsOut.m_bInterrupted = true;

			} else {
				++ResultsOut.m_unErrors;
				Log(strLabel + " ERROR " + strReason);
			}

			return false;
		}

		bool CheckDeadline(Clock::time_point Deadline, Results& ResultsOut, std::string const& strLabel) {
			if (Stopping()) {
				ResultsOut.m_bInterrupted = true;
				return false;
			}

			if (Clock::now() >= Deadline) {
				return Fail(ResultsOut, strLabel, "exchange timeout");
			}

			return true;
		}

		net::Socket Connect(ClientOptions const& Options, net::Address const& Address, bool bTCP, std::string const& strLabel, Results& ResultsOut) {
			net::Socket Socket(::socket(Address.family(), bTCP ? SOCK_STREAM : SOCK_DGRAM, bTCP ? IPPROTO_TCP : IPPROTO_UDP));
			if (!Socket || !net::Nonblocking(Socket.get())) {
				Fail(ResultsOut, strLabel, "socket setup failed: " + net::Error());
				return {};
			}

			if (bTCP) {
				net::ConfigureTcp(Socket.get());
			} else {
				net::ConfigureUdp(Socket.get());
			}

			const int nResult = ::connect(Socket.get(), Address.ptr(), Address.m_nSize);
			if (nResult == SOCKET_ERROR) {
				const int nError = WSAGetLastError();
				if (!net::WouldBlock(nError)) {
					Fail(ResultsOut, strLabel, "connect failed: " + net::Error(nError));
					return {};
				}

				std::string strError;
				if (!Wait(Socket.get(), POLLWRNORM, GetExchangeDeadline(Options), strError)) {
					Fail(ResultsOut, strLabel, "connect failed: " + strError);
					return {};
				}

				int nPendingError = 0;
				int nLength = static_cast<int>(sizeof(nPendingError));
				if (getsockopt(Socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&nPendingError), &nLength) == SOCKET_ERROR) {
					nPendingError = WSAGetLastError();
				}

				if (nPendingError) {
					Fail(ResultsOut, strLabel, "connect failed: " + net::Error(nPendingError));
					return {};
				}
			}

			if (Stopping()) {
				ResultsOut.m_bInterrupted = true;
				return {};
			}

			Log(strLabel + " connected target=" + Options.m_Target.ToString());

			return Socket;
		}

		bool ExchangeTCP(SOCKET hSocket, Bytes const& vecExpected, ClientOptions const& Options, std::string const& strLabel, Results& ResultsOut, bool bFinalHalfClose) {
			constexpr std::size_t kBufferBytes = 65536;

			static_assert(kBufferBytes <= std::numeric_limits<int>::max());

			Clock::time_point const Begin = Clock::now();
			Clock::time_point const Deadline = Begin + std::chrono::milliseconds(Options.m_nTimeoutMilliseconds);
			std::size_t unSent = 0;
			std::size_t unReceived = 0;
			bool bCountedSend = false;
			std::array<std::uint8_t, kBufferBytes> arrBuffer {};
			while ((unSent < vecExpected.size()) || (unReceived < vecExpected.size())) {
				if (!CheckDeadline(Deadline, ResultsOut, strLabel)) {
					return false;
				}

				bool bProgress = false;
				if (unSent < vecExpected.size()) {
					const std::size_t unAmount = std::min(vecExpected.size() - unSent, arrBuffer.size());
					const int nWritten = ::send(hSocket, reinterpret_cast<char const*>(vecExpected.data() + unSent), static_cast<int>(unAmount), 0);
					if (nWritten == SOCKET_ERROR) {
						const int nError = WSAGetLastError();
						if (!net::WouldBlock(nError)) {
							return Fail(ResultsOut, strLabel, "send failed: " + net::Error(nError));
						}
					} else if (nWritten == 0) {
						return Fail(ResultsOut, strLabel, "send returned zero");
					} else {
						unSent += static_cast<std::size_t>(nWritten);
						bProgress = true;
					}

					if ((unSent == vecExpected.size()) && !bCountedSend) {
						++ResultsOut.m_unSent;
						bCountedSend = true;
						// Send FIN before draining the rest of the final echo so this
						// mode verifies replies still arrive after our write half closes.
						if (bFinalHalfClose && (::shutdown(hSocket, SD_SEND) == SOCKET_ERROR)) {
							return Fail(ResultsOut, strLabel, "shutdown failed: " + net::Error());
						}
					}
				}

				// Drain replies while sending. A bounded echo server/tunnel can stop
				// reading the request until its own response queue has drained.
				if (unReceived < vecExpected.size()) {
					const std::size_t unAmount = std::min(vecExpected.size() - unReceived, arrBuffer.size());
					const int nReceived = ::recv(hSocket, reinterpret_cast<char*>(arrBuffer.data()), static_cast<int>(unAmount), 0);
					if (nReceived == SOCKET_ERROR) {
						const int nError = WSAGetLastError();
						if (!net::WouldBlock(nError)) {
							return Fail(ResultsOut, strLabel, "receive failed: " + net::Error(nError));
						}
					} else if (nReceived == 0) {
						return Fail(ResultsOut, strLabel, "unexpected EOF before the complete echo");
					} else {
						if (!std::equal(arrBuffer.begin(), arrBuffer.begin() + nReceived, vecExpected.begin() + unReceived)) {
							return Fail(ResultsOut, strLabel, "echo corruption at byte " + std::to_string(unReceived));
						}

						unReceived += static_cast<std::size_t>(nReceived);
						bProgress = true;
					}
				}

				if (!bProgress) {
					short nEvents = 0;
					if (unSent < vecExpected.size()) {
						nEvents |= POLLWRNORM;
					}

					if (unReceived < vecExpected.size()) {
						nEvents |= POLLRDNORM;
					}

					std::string strError;
					if (!Wait(hSocket, nEvents, Deadline, strError)) {
						return Fail(ResultsOut, strLabel, "exchange failed: " + strError);
					}
				}
			}

			++ResultsOut.m_unReceived;
			ResultsOut.m_unBytes += vecExpected.size();
			ResultsOut.RecordRTT(Begin);

			return true;
		}

		bool AwaitEOF(SOCKET hSocket, ClientOptions const& Options, std::string const& strLabel, Results& ResultsOut) {
			Clock::time_point const Deadline = GetExchangeDeadline(Options);
			while (CheckDeadline(Deadline, ResultsOut, strLabel)) {
				char chExtra = 0;
				const int nReceived = ::recv(hSocket, &chExtra, 1, 0);
				if (nReceived == 0) {
					Log(strLabel + " half-close EOF verified");
					return true;
				}

				if (nReceived > 0) {
					return Fail(ResultsOut, strLabel, "unexpected extra data after the final echo");
				}

				const int nError = WSAGetLastError();
				if (!net::WouldBlock(nError)) {
					return Fail(ResultsOut, strLabel, "half-close receive failed: " + net::Error(nError));
				}

				std::string strError;
				if (!Wait(hSocket, POLLRDNORM, Deadline, strError)) {
					return Fail(ResultsOut, strLabel, "half-close failed: " + strError);
				}
			}

			return false;
		}

		void RunTCP(ClientOptions const& Options, net::Address const& Target, unsigned unConnection, Results& ResultsOut) {
			std::string const strLabel = GetConnectionLabel(true, unConnection);
			net::Socket Socket = Connect(Options, Target, true, strLabel, ResultsOut);
			if (!Socket) {
				return;
			}

			for (unsigned unSequence = 0; unSequence < Options.m_unCount; ++unSequence) {
				if (Stopping()) {
					ResultsOut.m_bInterrupted = true;
					return;
				}

				Bytes const vecBytes = Payload(Options, true, unConnection, unSequence);
				if (!ExchangeTCP(Socket.get(), vecBytes, Options, strLabel + " sequence=" + std::to_string(unSequence + 1), ResultsOut, Options.m_bHalfClose && ((unSequence + 1) == Options.m_unCount))) {
					return;
				}

				if ((unSequence + 1) < Options.m_unCount) {
					Pause(Options.m_nIntervalMilliseconds);
				}
			}

			if (Options.m_bHalfClose) {
				// EOF verification records failure/interruption in ResultsOut; no exchange follows.
				AwaitEOF(Socket.get(), Options, strLabel, ResultsOut);
			}
		}

		enum class Previous {
			None = 0,
			Duplicate,
			Late
		};

		Previous FindPreviousUDPEcho(Bytes const& vecBytes, ClientOptions const& Options, unsigned unConnection, unsigned unSequence, std::vector<bool> const& vecReceived, Clock::time_point Deadline) {
			constexpr std::size_t kSequenceHeaderBytes = 0x10;

			// The caller guarantees unSequence <= vecReceived.size(); earlier indices are in range.
			if (Options.m_bHasMessage) {
				return Previous::None;
			}

			unsigned unCandidate = 0;
			if (PayloadSequence(vecBytes, false, unConnection, unCandidate)) {
				if ((unCandidate < unSequence) && (Payload(Options, false, unConnection, unCandidate) == vecBytes)) {
					return vecReceived[unCandidate] ? Previous::Duplicate : Previous::Late;
				}

				return Previous::None;
			}

			// Short generated datagrams have no sequence header. Check earlier exact
			// patterns within the exchange deadline; identical patterns are ambiguous.
			if (vecBytes.size() < kSequenceHeaderBytes) {
				for (unsigned unPrior = unSequence; unPrior > 0;) {
					if (Stopping() || (Clock::now() >= Deadline)) {
						break;
					}

					--unPrior;
					if (Payload(Options, false, unConnection, unPrior) == vecBytes) {
						return vecReceived[unPrior] ? Previous::Duplicate : Previous::Late;
					}
				}
			}

			return Previous::None;
		}

		void RunUDP(ClientOptions const& Options, net::Address const& Target, unsigned unConnection, Results& ResultsOut) {
			constexpr std::size_t kReceiveBytes = kMaxUdp + 1;
			constexpr std::uint64_t kMaximumIgnoredEchoLogs = 8;
			constexpr std::uint64_t kMaximumCounter = std::numeric_limits<std::uint64_t>::max();

			static_assert(kMaxUdp < std::numeric_limits<std::size_t>::max());
			static_assert(kReceiveBytes <= std::numeric_limits<int>::max());

			std::string const strLabel = GetConnectionLabel(false, unConnection);
			net::Socket Socket = Connect(Options, Target, false, strLabel, ResultsOut);
			if (!Socket) {
				return;
			}

			std::vector<bool> vecCompleted = std::vector<bool>(Options.m_unCount, false);
			std::array<std::uint8_t, kReceiveBytes> arrStorage {};
			for (unsigned unSequence = 0; unSequence < Options.m_unCount; ++unSequence) {
				if (Stopping()) {
					ResultsOut.m_bInterrupted = true;
					return;
				}

				Bytes const vecExpected = Payload(Options, false, unConnection, unSequence);
				Clock::time_point const Begin = Clock::now();
				Clock::time_point const Deadline = Begin + std::chrono::milliseconds(Options.m_nTimeoutMilliseconds);
				std::string const strExchange = strLabel + " sequence=" + std::to_string(unSequence + 1);
				bool bSent = false;
				while (!bSent && !Stopping() && (Clock::now() < Deadline)) {
					char const* pchData = vecExpected.empty() ? "" : reinterpret_cast<char const*>(vecExpected.data());
					const int nWritten = ::send(Socket.get(), pchData, static_cast<int>(vecExpected.size()), 0);
					if (nWritten != SOCKET_ERROR) {
						if (static_cast<std::size_t>(nWritten) != vecExpected.size()) {
							Fail(ResultsOut, strExchange, "partial UDP send");
							return;
						}

						++ResultsOut.m_unSent;
						bSent = true;
					} else {
						const int nError = WSAGetLastError();
						if (!net::WouldBlock(nError)) {
							Fail(ResultsOut, strExchange, "send failed: " + net::Error(nError));
							return;
						}

						std::string strError;
						if (!Wait(Socket.get(), POLLWRNORM, Deadline, strError)) {
							if ((strError != "timeout") && !Stopping()) {
								Fail(ResultsOut, strExchange, "send wait failed: " + strError);
								return;
							}

							break;
						}
					}
				}

				if (Stopping()) {
					ResultsOut.m_bInterrupted = true;
					return;
				}

				if (!bSent) {
					Fail(ResultsOut, strExchange, "UDP send deadline expired before the datagram was sent");
					return;
				}

				while (!Stopping() && (Clock::now() < Deadline)) {
					const int nReceived = ::recv(Socket.get(), reinterpret_cast<char*>(arrStorage.data()), static_cast<int>(arrStorage.size()), 0);
					if (nReceived == SOCKET_ERROR) {
						const int nError = WSAGetLastError();
						if (!net::WouldBlock(nError)) {
							Fail(ResultsOut, strExchange, "receive failed: " + net::Error(nError));
							return;
						}

						std::string strError;
						if (!Wait(Socket.get(), POLLRDNORM, Deadline, strError)) {
							if ((strError != "timeout") && !Stopping()) {
								Fail(ResultsOut, strExchange, "receive wait failed: " + strError);
								return;
							}

							break;
						}

						continue;
					}

					Bytes vecResponse = Bytes(arrStorage.begin(), arrStorage.begin() + nReceived);
					if (vecResponse == vecExpected) {
						vecCompleted[unSequence] = true;
						++ResultsOut.m_unReceived;
						ResultsOut.m_unBytes += vecExpected.size();
						ResultsOut.RecordRTT(Begin);
						break;
					}

					const Previous PreviousEcho = FindPreviousUDPEcho(vecResponse, Options, unConnection, unSequence, vecCompleted, Deadline);
					if (PreviousEcho != Previous::None) {
						if (PreviousEcho == Previous::Duplicate) {
							if (ResultsOut.m_unDuplicates == kMaximumCounter) {
								Fail(ResultsOut, strExchange, "duplicate echo counter overflow");
								return;
							}

							++ResultsOut.m_unDuplicates;

						} else {
							if (ResultsOut.m_unLate == kMaximumCounter) {
								Fail(ResultsOut, strExchange, "late echo counter overflow");
								return;
							}

							++ResultsOut.m_unLate;
						}

						if ((ResultsOut.m_unDuplicates <= kMaximumIgnoredEchoLogs) && (ResultsOut.m_unLate <= (kMaximumIgnoredEchoLogs - ResultsOut.m_unDuplicates))) {
							Log(strExchange + ((PreviousEcho == Previous::Duplicate) ? " ignored duplicate earlier echo" : " ignored late/out-of-order earlier echo"));
						}

						continue;
					}

					Fail(ResultsOut, strExchange, "echo corruption/unexpected datagram bytes=" + std::to_string(vecResponse.size()) + " preview=" + Preview(vecResponse.data(), vecResponse.size()));
					return;
				}

				if (Stopping()) {
					ResultsOut.m_bInterrupted = true;
					return;
				}

				if (!vecCompleted[unSequence]) {
					++ResultsOut.m_unLost;
					Log(strExchange + " LOST echo timeout (no retry)");
				}

				if ((unSequence + 1) < Options.m_unCount) {
					Pause(Options.m_nIntervalMilliseconds);
				}
			}
		}

		void RunWorker(ClientOptions const& Options, net::Address const& Target, bool bTCP, unsigned unConnection, std::atomic<bool>& bFailedOut) noexcept {
			try {
				Results ResultsValue;
				try {
					if (bTCP) {
						RunTCP(Options, Target, unConnection, ResultsValue);
					} else {
						RunUDP(Options, Target, unConnection, ResultsValue);
					}
				} catch (std::exception const& Error) {
					Fail(ResultsValue, GetConnectionLabel(bTCP, unConnection), Error.what());
				} catch (...) {
					Fail(ResultsValue, GetConnectionLabel(bTCP, unConnection), "unexpected exception");
				}

				if (Stopping()) {
					ResultsValue.m_bInterrupted = true;
				}

				std::ostringstream ReportStream;

				ReportStream << std::fixed << std::setprecision(3) << "RESULT " << GetConnectionLabel(bTCP, unConnection) << " sent=" << ResultsValue.m_unSent << " received=" << ResultsValue.m_unReceived << " lost=" << ResultsValue.m_unLost << " errors=" << ResultsValue.m_unErrors << " bytes=" << ResultsValue.m_unBytes << " duplicates=" << ResultsValue.m_unDuplicates << " late=" << ResultsValue.m_unLate << " rtt_min_ms=" << (ResultsValue.m_unReceived ? ResultsValue.m_dblMinimum : 0.0) << " rtt_avg_ms=" << (ResultsValue.m_unReceived ? (ResultsValue.m_dblTotal / static_cast<double>(ResultsValue.m_unReceived)) : 0.0) << " rtt_max_ms=" << ResultsValue.m_dblMaximum;

				if (ResultsValue.m_bInterrupted) {
					ReportStream << " interrupted=1";
				}

				Log(ReportStream.str());

				if (ResultsValue.m_unErrors || ResultsValue.m_bInterrupted || (ResultsValue.m_unLost && (bTCP || !Options.m_bAllowUDPLoss))) {
					bFailedOut.store(true);
				}
			} catch (...) {
				bFailedOut.store(true);
				std::fputs("ERROR: test client worker failed while reporting\n", stderr);
			}
		}

		class WorkerJoinGuard {
		public:
			explicit WorkerJoinGuard(std::vector<std::thread>& vecWorkers) noexcept :
				m_vecWorkers(vecWorkers)
			{
				m_nUncaughtExceptions = std::uncaught_exceptions();
			}

			~WorkerJoinGuard() {
				// Borrowed options/targets must outlive every worker, including a throwing launch/log path.
				if (std::uncaught_exceptions() > m_nUncaughtExceptions) {
					Stop();
				}

				for (auto& WorkerThread : m_vecWorkers) {
					if (WorkerThread.joinable()) {
						try {
							WorkerThread.join();
						} catch (...) {
							std::fputs("ERROR: cannot join test client worker\n", stderr);
							std::terminate();
						}
					}
				}
			}

			WorkerJoinGuard(WorkerJoinGuard const&) = delete;
			WorkerJoinGuard& operator=(WorkerJoinGuard const&) = delete;

		private:
			std::vector<std::thread>& m_vecWorkers;
			int m_nUncaughtExceptions;
		};

		int RunClient(int nArgumentCount, char** ppszArguments) {
			constexpr std::size_t kSequenceHeaderBytes = 0x10;

			ClientOptions Options;
			std::string strError;
			if (!ParseClient(nArgumentCount, ppszArguments, Options, strError)) {
				Log("ERROR " + strError + "\n" + ClientHelp());
				return 2;
			}

			if (Options.m_bHelp) {
				Log(ClientHelp());
				return 0;
			}

			Session NetworkSession;
			if (!NetworkSession) {
				Log("FAIL WSAStartup failed");
				return 1;
			}

			net::Address TCPTarget {};
			net::Address UDPTarget {};
			if ((Options.m_bTCP && !net::Resolve(Options.m_Target, SOCK_STREAM, TCPTarget, strError)) ||
				(Options.m_bUDP && !net::Resolve(Options.m_Target, SOCK_DGRAM, UDPTarget, strError))) {
				Log("FAIL " + strError);
				return 1;
			}

			Log("WireSteamTestClient target=" + Options.m_Target.ToString() + " count=" + std::to_string(Options.m_unCount) + " connections_per_protocol=" + std::to_string(Options.m_unConnections));

			if (Options.m_bHasMessage) {
				Log("MESSAGE bytes=" + std::to_string(Options.m_strMessage.size()) + " preview=" + Preview(Options.m_strMessage.data(), Options.m_strMessage.size()));
			}

			if (Options.m_bUDP && (Options.m_bHasMessage || (Options.m_unUDPSize < kSequenceHeaderBytes))) {
				Log("UDP NOTE: identical/short payloads can make late replies and duplicates indistinguishable; use generated payloads >=16 bytes for sequence identification.");
			}

			std::atomic<bool> bFailed = false;
			std::vector<std::thread> vecWorkers;
			WorkerJoinGuard WorkerGuard(vecWorkers);
			Clock::time_point const Begin = Clock::now();
			try {
				for (unsigned unConnection = 0; unConnection < Options.m_unConnections; ++unConnection) {
					if (Options.m_bTCP) {
						vecWorkers.emplace_back([&Options, &TCPTarget, unConnection, &bFailed]() -> void {
							RunWorker(Options, TCPTarget, true, unConnection, bFailed);
						});
					}

					if (Options.m_bUDP) {
						vecWorkers.emplace_back([&Options, &UDPTarget, unConnection, &bFailed]() -> void {
							RunWorker(Options, UDPTarget, false, unConnection, bFailed);
						});
					}
				}
			} catch (std::exception const& Error) {
				bFailed.store(true);
				Stop();
				Log(std::string("ERROR cannot start worker: ") + Error.what());
			}

			for (auto& WorkerThread : vecWorkers) {
				if (WorkerThread.joinable()) {
					WorkerThread.join();
				}
			}

			const bool bSuccess = !bFailed.load() && !Stopping();

			std::ostringstream ReportStream;

			ReportStream << (bSuccess ? "PASS" : "FAIL") << " elapsed_ms=" << std::fixed << std::setprecision(3) << std::chrono::duration<double, std::milli>(Clock::now() - Begin).count();

			if (Options.m_bAllowUDPLoss) {
				ReportStream << " udp_loss_allowed=1";
			}

			Log(ReportStream.str());

			return bSuccess ? 0 : 1;
		}
	} // namespace
} // namespace st::testapp

int main(int nArgumentCount, char** ppszArguments) {
	try {
		return st::testapp::RunClient(nArgumentCount, ppszArguments);
	} catch (std::exception const& Error) {
		// Terminal failure reporting is best effort and must not throw at the process boundary.
		std::fprintf(stdout, "FAIL %s\n", Error.what());
		std::fflush(stdout);
		return 1;
	} catch (...) {
		std::fputs("FAIL unexpected exception\n", stdout);
		std::fflush(stdout);
		return 1;
	}
}
