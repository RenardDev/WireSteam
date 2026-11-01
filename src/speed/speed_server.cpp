
// General
#include <winsock2.h>
#include <Windows.h>

// C++
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <utility>

// STL
#include <algorithm>
#include <array>
#include <atomic>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Project
#include "speed_common.hpp"

namespace {
	using namespace ws::speed;

	constexpr int kControlTimeoutMilliseconds = 10000;

	struct Options {
		Options() {
			constexpr std::uint16_t kDefaultPort = 5201;
			constexpr std::size_t kDefaultMaximumClients = 16;

			m_ListenEndpoint = { "127.0.0.1", kDefaultPort };
			m_unRunSeconds = 0;
			m_unMaximumClients = kDefaultMaximumClients;
			m_bOnce = false;
			m_bHelp = false;
			m_bSelfTest = false;
			m_bVersion = false;
		}

		st::Endpoint m_ListenEndpoint;
		std::uint32_t m_unRunSeconds;
		std::size_t m_unMaximumClients;
		bool m_bOnce;
		bool m_bHelp;
		bool m_bSelfTest;
		bool m_bVersion;
	};

	void PrintHelp() {
		std::cout
			<< "WireSteam Speed Server - TCP and UDP throughput test receiver/sender\n"
			<< "\n"
			<< "Usage:\n"
			<< "  WireSteamSpeedServer.exe [--listen HOST:PORT] [--max-clients N] [--run-seconds N]\n"
			<< "  WireSteamSpeedServer.exe [--listen HOST:PORT] --once\n"
			<< "  WireSteamSpeedServer.exe --help\n"
			<< "  WireSteamSpeedServer.exe --version\n"
			<< "  WireSteamSpeedServer.exe --self-test\n"
			<< "\n"
			<< "Options:\n"
			<< "  --listen HOST:PORT  Control listener (default 127.0.0.1:5201).\n"
			<< "                      Use your VPN address to test WireSteam, for example\n"
			<< "                      --listen 10.77.0.1:5201. IPv6 uses [ADDRESS]:PORT.\n"
			<< "                      Port 0 selects an available control port.\n"
			<< "  --once              Serve one control connection, then exit. A failed\n"
			<< "                      connection returns a nonzero exit code. The client limit\n"
			<< "                      becomes 1; an explicit --max-clients must also be 1.\n"
			<< "  --max-clients N     Concurrent control clients, 1-64 (default 16).\n"
			<< "                      Each client has independent temporary data ports,\n"
			<< "                      session tokens, measurement settings and counters.\n"
			<< "  --run-seconds N     Listener lifetime, 1-2592000 seconds. Finish an active\n"
			<< "                      test phase before exiting; each phase is at most 300 s.\n"
			<< "  --help              Show this help.\n"
			<< "  --version           Show the server version and concurrency support.\n"
			<< "  --self-test         Run protocol and measurement unit tests without listening.\n"
			<< "\n"
			<< "Clients run concurrently within the configured limit. Additional clients\n"
			<< "receive a busy error. Clients share the host's CPU, network and VPN capacity.\n"
			<< "Each phase uses temporary data ports on its control connection's local IP.\n"
			<< "TCP supports multiple parallel streams; UDP uses a token-bound source probe.\n"
			<< "Press Ctrl+C to stop.\n";
	}

	Options ParseOptions(int nArgumentCount, char** ppszArguments) {
		constexpr int kSingleOptionArgumentCount = 2;
		constexpr std::uint32_t kMaximumRunSeconds = 2592000;
		constexpr std::uint32_t kMaximumClients = 64;

		Options Parsed;
		bool bSeenListen = false;
		bool bSeenSeconds = false;
		bool bSeenMaximumClients = false;
		const std::size_t unArgumentCount = static_cast<std::size_t>(nArgumentCount);
		for (std::size_t unArgumentIndex = 1; unArgumentIndex < unArgumentCount; ++unArgumentIndex) {
			std::string const strArgument = ppszArguments[unArgumentIndex];
			if ((strArgument == "--help") || (strArgument == "-h")) {
				Require(nArgumentCount == kSingleOptionArgumentCount, "--help does not accept other arguments");
				Parsed.m_bHelp = true;

			} else if (strArgument == "--self-test") {
				Require(nArgumentCount == kSingleOptionArgumentCount, "--self-test does not accept other arguments");
				Parsed.m_bSelfTest = true;

			} else if (strArgument == "--version") {
				Require(nArgumentCount == kSingleOptionArgumentCount, "--version does not accept other arguments");
				Parsed.m_bVersion = true;

			} else if (strArgument == "--once") {
				Require(!Parsed.m_bOnce, "Duplicate --once option");
				Parsed.m_bOnce = true;

			} else if (strArgument == "--listen") {
				Require(!bSeenListen && (unArgumentIndex < (unArgumentCount - 1)), "--listen requires one HOST:PORT value and may be specified only once");
				bSeenListen = true;
				Parsed.m_ListenEndpoint = ParseEndpoint(ppszArguments[++unArgumentIndex], true);

			} else if (strArgument == "--run-seconds") {
				Require(!bSeenSeconds && (unArgumentIndex < (unArgumentCount - 1)), "--run-seconds requires one value and may be specified only once");
				bSeenSeconds = true;
				Parsed.m_unRunSeconds = Number(ppszArguments[++unArgumentIndex], 1, kMaximumRunSeconds);

			} else if (strArgument == "--max-clients") {
				Require(!bSeenMaximumClients && (unArgumentIndex < (unArgumentCount - 1)), "--max-clients requires one value and may be specified only once");
				bSeenMaximumClients = true;
				Parsed.m_unMaximumClients = Number(ppszArguments[++unArgumentIndex], 1, kMaximumClients);

			} else {
				throw std::runtime_error("Unknown argument: " + strArgument + ". Use --help.");
			}
		}

		if (Parsed.m_bOnce) {
			Require(!bSeenMaximumClients || (Parsed.m_unMaximumClients == 1), "--once requires --max-clients 1 when the limit is specified explicitly");
			Parsed.m_unMaximumClients = 1;
		}

		return Parsed;
	}

	bool HaveSameSpecification(Spec const& Left, Spec const& Right) {
		return (Left.m_Protocol == Right.m_Protocol) && (Left.m_Direction == Right.m_Direction) && (Left.m_unSeconds == Right.m_unSeconds) && (Left.m_unStreams == Right.m_unStreams) && (Left.m_unBlockSize == Right.m_unBlockSize) && (Left.m_unUDPSize == Right.m_unUDPSize) && (Left.m_dblUDPMbps == Right.m_dblUDPMbps);
	}

	Message CreatePhaseMessage(Kind KindValue, Message const& Ready) {
		Message Result;
		Result.m_Kind = KindValue;
		Result.m_Specification = Ready.m_Specification;
		Result.m_unPhase = Ready.m_unPhase;
		Result.m_arrToken = Ready.m_arrToken;

		return Result;
	}

	Message ReceivePhase(SOCKET hControl, Kind KindValue, Message const& Ready, int nTimeoutMilliseconds = kControlTimeoutMilliseconds) {
		Message Result = ReceiveMessage(hControl, nTimeoutMilliseconds);
		Expect(Result, KindValue);
		Require((Result.m_unPhase == Ready.m_unPhase) && (Result.m_arrToken == Ready.m_arrToken) && HaveSameSpecification(Result.m_Specification, Ready.m_Specification), "Control message does not match the active test phase");

		return Result;
	}

	std::vector<net::Socket> AcceptStreams(SOCKET hListener, Message const& Ready, net::Address const& ControlPeer) {
		constexpr int kSetupTimeoutSeconds = 10;
		constexpr std::chrono::milliseconds::rep kMinimumReadTimeoutMilliseconds = 1;
		constexpr std::chrono::milliseconds::rep kMaximumReadTimeoutMilliseconds = 10000;

		static_assert(kMaximumReadTimeoutMilliseconds <= std::numeric_limits<int>::max());

		std::vector<net::Socket> vecStreams(Ready.m_Specification.m_unStreams);
		Clock::time_point const Deadline = Clock::now() + std::chrono::seconds(kSetupTimeoutSeconds);
		for (std::size_t unAcceptedCount = 0; unAcceptedCount < Ready.m_Specification.m_unStreams; ++unAcceptedCount) {
			net::Address RemoteAddress;
			net::Socket Socket = Accept(hListener, Deadline, &RemoteAddress);
			Require(bool(Socket), "Cannot accept a TCP data stream");
			Require(Host(RemoteAddress) == Host(ControlPeer), "TCP data stream came from another IP address");
			const std::chrono::milliseconds::rep nRemainingMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - Clock::now()).count();
			Require(nRemainingMilliseconds > 0, "Timed out accepting TCP data streams");
			const std::uint32_t unStreamIndex = ReceiveDataHello(Socket.get(), Ready.m_arrToken, Ready.m_unPhase, static_cast<int>(std::clamp(nRemainingMilliseconds, kMinimumReadTimeoutMilliseconds, kMaximumReadTimeoutMilliseconds)));
			Require(Clock::now() <= Deadline, "Timed out accepting TCP data streams");
			Require((unStreamIndex < vecStreams.size()) && !vecStreams[unStreamIndex], "Invalid or duplicate TCP data stream index");
			Tune(Socket.get(), true);
			vecStreams[unStreamIndex] = std::move(Socket);
		}

		return vecStreams;
	}

	void RunPhase(SOCKET hControl, Message const& Request, net::Address const& ControlPeer, std::string const& strLocalHost, std::string const& strPrefix) {
		constexpr int kPostTransferTimeoutMilliseconds = 35000;
		constexpr unsigned kMillisecondsPerSecond = 1000;
		constexpr double kAdditionalDurationSeconds = 30;

		static_assert(kMaxSeconds <= ((std::numeric_limits<int>::max() - kPostTransferTimeoutMilliseconds) / kMillisecondsPerSecond));
		static_assert(kMaxUdpPackets <= (std::numeric_limits<std::uint64_t>::max() / std::numeric_limits<std::uint32_t>::max()));

		Validate(Request.m_Specification);
		Message Ready;
		Ready.m_Kind = Kind::Ready;
		Ready.m_Specification = Request.m_Specification;
		Ready.m_unPhase = Request.m_unPhase;
		Ready.m_arrToken = NewToken();
		Spec const& Specification = Ready.m_Specification;
		const bool bUpload = Specification.m_Direction == Direction::Upload;
		std::string const strDescription = std::string((Specification.m_Protocol == Protocol::Tcp) ? "TCP " : "UDP ") + (bUpload ? "upload" : "download");
		std::string const strLabel = strPrefix + strDescription;
		Log(strPrefix + "Starting " + strDescription + " phase " + std::to_string(Ready.m_unPhase) + " for " + std::to_string(Specification.m_unSeconds) + " s");
		Stats Sender;
		Stats Receiver;
		if (Specification.m_Protocol == Protocol::Tcp) {
			net::Socket Listener = Listen({ strLocalHost, 0 });
			Ready.m_unPort = net::Bound(Listener.get()).m_unPort;
			Require(Ready.m_unPort != 0, "TCP data listener has no assigned port");
			SendMessage(hControl, Ready);
			std::vector<net::Socket> vecStreams = AcceptStreams(Listener.get(), Ready, ControlPeer);
			Listener.reset();
			ReceivePhase(hControl, Kind::ClientReady, Ready);
			SendMessage(hControl, CreatePhaseMessage(Kind::Begin, Ready));

			if (bUpload) {
				Receiver = TransferTcp(vecStreams, false, Specification, strLabel);
				Sender = ReceivePhase(hControl, Kind::SenderDone, Ready, kPostTransferTimeoutMilliseconds).m_Sender;

			} else {
				Sender = TransferTcp(vecStreams, true, Specification, strLabel);
				Message Done = CreatePhaseMessage(Kind::SenderDone, Ready);
				Done.m_Sender = Sender;
				SendMessage(hControl, Done);
				Receiver = ReceivePhase(hControl, Kind::ReceiverDone, Ready, kPostTransferTimeoutMilliseconds).m_Receiver;
			}

		} else {
			std::string strError;
			net::Socket Socket = net::Bind({ strLocalHost, 0 }, SOCK_DGRAM, strError);
			Require(bool(Socket), "Cannot bind UDP data socket: " + strError);
			Tune(Socket.get(), false);
			Ready.m_unPort = net::Bound(Socket.get()).m_unPort;
			Require(Ready.m_unPort != 0, "UDP data socket has no assigned port");
			SendMessage(hControl, Ready);
			net::Address const Destination = ReceiveUdpProbe(Socket.get(), Ready.m_arrToken, Ready.m_unPhase, ControlPeer);
			Require(Host(Destination) == Host(ControlPeer), "UDP probe came from another IP address");

			if (connect(Socket.get(), Destination.ptr(), Destination.m_nSize) != 0) {
				const int nError = WSAGetLastError();
				throw std::runtime_error("Cannot connect UDP data socket: " + net::Error(nError));
			}

			ReceivePhase(hControl, Kind::ClientReady, Ready);

			if (bUpload) {
				UdpReceiver Receiving = UdpReceiver(Socket.get(), Ready.m_arrToken, Ready.m_unPhase, Specification.m_unUDPSize);
				SendMessage(hControl, CreatePhaseMessage(Kind::Begin, Ready));
				Sender = ReceivePhase(hControl, Kind::SenderDone, Ready, static_cast<int>(Specification.m_unSeconds * kMillisecondsPerSecond + kPostTransferTimeoutMilliseconds)).m_Sender;
				Receiver = Receiving.Finish(Sender);

			} else {
				SendMessage(hControl, CreatePhaseMessage(Kind::Begin, Ready));
				Sender = SendUdp(Socket.get(), Specification, Ready.m_arrToken, Ready.m_unPhase, strLabel);
				Message Done = CreatePhaseMessage(Kind::SenderDone, Ready);
				Done.m_Sender = Sender;
				SendMessage(hControl, Done);
				Receiver = ReceivePhase(hControl, Kind::ReceiverDone, Ready, kPostTransferTimeoutMilliseconds).m_Receiver;
			}
		}

		Require(std::isfinite(Sender.m_dblSeconds) && (Sender.m_dblSeconds > 0) && std::isfinite(Receiver.m_dblSeconds) && (Receiver.m_dblSeconds > 0), "Transfer reported an invalid measurement duration");
		Require((Sender.m_dblSeconds <= (static_cast<double>(Specification.m_unSeconds) + kAdditionalDurationSeconds)) && (Receiver.m_dblSeconds <= (static_cast<double>(Specification.m_unSeconds) + kAdditionalDurationSeconds)), "Transfer measurement duration exceeds the allowed drain period");

		if (Specification.m_Protocol == Protocol::Tcp) {
			Require(Sender.m_unBytes == Receiver.m_unBytes, "TCP sender and receiver byte totals do not match");

		} else {
			Require((Sender.m_unPackets <= kMaxUdpPackets) && (Sender.m_unBytes == (Sender.m_unPackets * Specification.m_unUDPSize)), "UDP sender packet and byte totals are inconsistent");
			Require((Receiver.m_unPackets <= Sender.m_unPackets) && (Receiver.m_unBytes == (Receiver.m_unPackets * Specification.m_unUDPSize)), "UDP receiver packet and byte totals are inconsistent");
		}

		Message Result = CreatePhaseMessage(Kind::Result, Ready);
		Result.m_Sender = Sender;
		Result.m_Receiver = Receiver;
		SendMessage(hControl, Result);
		Log(strPrefix + Summary(Result));
	}

	void FinishExpiredControl(SOCKET hControl, std::string const& strPrefix, Clock::time_point Deadline) {
		Log(strPrefix + "Listener lifetime ended; allowing control Finish after the completed phase");
		while (!Stopping() && (Clock::now() < Deadline)) {
			const std::chrono::milliseconds::rep nRemainingMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - Clock::now()).count();
			if (nRemainingMilliseconds <= 0) {
				return;
			}

			if (nRemainingMilliseconds > std::numeric_limits<int>::max()) {
				throw std::runtime_error("Control finish grace timeout exceeds supported range");
			}

			Message MessageValue;
			try {
				// Read one complete frame against the fixed grace deadline. Do not
				// restart a partially consumed frame when the listener expires.
				MessageValue = ReceiveMessage(hControl, static_cast<int>(nRemainingMilliseconds));
			} catch (...) {
				if (Stopping() || (Clock::now() >= Deadline)) {
					return;
				}

				throw;
			}

			if (MessageValue.m_Kind == Kind::Finish) {
				Log(strPrefix + "Control client finished");
				return;
			}

			if (MessageValue.m_Kind == Kind::Ping) {
				const std::chrono::milliseconds::rep nReplyRemainingMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - Clock::now()).count();
				if (nReplyRemainingMilliseconds <= 0) {
					return;
				}

				if (nReplyRemainingMilliseconds > std::numeric_limits<int>::max()) {
					throw std::runtime_error("Control finish grace timeout exceeds supported range");
				}

				MessageValue.m_Kind = Kind::Pong;
				SendMessage(hControl, MessageValue, static_cast<int>(nReplyRemainingMilliseconds));
				continue;
			}

			throw std::runtime_error("Server listener lifetime ended; no new test phases are accepted");
		}
	}

	void RunControl(SOCKET hControl, Clock::time_point ExpiresAt, std::string const& strPrefix) {
		constexpr std::chrono::milliseconds::rep kMaximumReadTimeoutMilliseconds = kControlTimeoutMilliseconds;
		constexpr int kFinishGraceSeconds = 2;

		Tune(hControl, true);
		net::Address const RemoteAddress = Remote(hControl);
		net::Address const LocalAddress = Local(hControl);
		std::string const strLocalHost = Host(LocalAddress);
		Require(!strLocalHost.empty() && (strLocalHost != "?") && (strLocalHost != "0.0.0.0") && (strLocalHost != "::"), "Cannot determine the control connection's local IP address");
		Log(strPrefix + "Control client connected");
		std::uint32_t unLastPhase = 0;
		Clock::time_point FinishGraceEnds = Clock::time_point::min();
		while (!Stopping()) {
			if (Clock::now() >= ExpiresAt) {
				if (unLastPhase && (Clock::now() < FinishGraceEnds)) {
					FinishExpiredControl(hControl, strPrefix, FinishGraceEnds);
				}

				return;
			}

			int nTimeoutMilliseconds = kControlTimeoutMilliseconds;
			if (ExpiresAt != Clock::time_point::max()) {
				// After a completed phase, read the next complete frame through the
				// finish grace deadline even if it straddles listener expiry.
				// Otherwise a partial Finish could be consumed at the old deadline
				// and its remaining bytes mistaken for a new frame.
				Clock::time_point const ReadUntil = unLastPhase ? FinishGraceEnds : ExpiresAt;
				const std::chrono::milliseconds::rep nRemainingMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(ReadUntil - Clock::now()).count();
				if (nRemainingMilliseconds <= 0) {
					return;
				}

				nTimeoutMilliseconds = static_cast<int>(std::min(nRemainingMilliseconds, kMaximumReadTimeoutMilliseconds));
			}

			Message MessageValue;
			try {
				MessageValue = ReceiveMessage(hControl, nTimeoutMilliseconds);
			} catch (...) {
				if ((Clock::now() >= ExpiresAt) || Stopping()) {
					return;
				}

				throw;
			}

			if (MessageValue.m_Kind == Kind::Ping) {
				MessageValue.m_Kind = Kind::Pong;

				if (Clock::now() >= ExpiresAt) {
					// No phase means no grace deadline; never subtract from the minimum sentinel.
					if (!unLastPhase) {
						return;
					}

					const std::chrono::milliseconds::rep nRemainingMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(FinishGraceEnds - Clock::now()).count();
					if (nRemainingMilliseconds <= 0) {
						return;
					}

					if (nRemainingMilliseconds > std::numeric_limits<int>::max()) {
						throw std::runtime_error("Control finish grace timeout exceeds supported range");
					}

					SendMessage(hControl, MessageValue, static_cast<int>(nRemainingMilliseconds));
					FinishExpiredControl(hControl, strPrefix, FinishGraceEnds);
					return;
				}

				SendMessage(hControl, MessageValue);

			} else if (MessageValue.m_Kind == Kind::Finish) {
				Log(strPrefix + "Control client finished");
				return;

			} else {
				Require(Clock::now() < ExpiresAt, "Server listener lifetime ended; no new test phases are accepted");
				Expect(MessageValue, Kind::Request);
				Require((MessageValue.m_unPhase != 0) && (MessageValue.m_unPhase > unLastPhase), "Each test phase must have a nonzero, increasing phase number");
				unLastPhase = MessageValue.m_unPhase;
				RunPhase(hControl, MessageValue, RemoteAddress, strLocalHost, strPrefix);

				if (ExpiresAt != Clock::time_point::max()) {
					FinishGraceEnds = std::max(ExpiresAt, Clock::now()) + std::chrono::seconds(kFinishGraceSeconds);
				}

				if (!Stopping() && (Clock::now() >= ExpiresAt)) {
					FinishExpiredControl(hControl, strPrefix, FinishGraceEnds);
					return;
				}
			}
		}
	}

	void LogSafely(std::string const& strText) noexcept {
		try {
			Log(strText);
		} catch (...) {
			OutputDebugStringA("WireSteam speed server logging failed\n");
		}
	}

	// Reject immediately instead of creating a waiting worker. A short read drain
	// lets a normal client's queued Ping arrive before closing, so the peer can
	// receive the framed busy response without an unread-data TCP reset.
	void RejectClient(SOCKET hControl, std::string_view strvText) noexcept {
		constexpr std::size_t kMaximumReplyTextBytes = 512;
		constexpr int kReplyTimeoutMilliseconds = 100;
		constexpr int kDrainTimeoutMilliseconds = 100;
		constexpr std::size_t kDrainBufferBytes = 4096;
		constexpr std::size_t kDrainThresholdBytes = 64 * 1024;

		static_assert(kDrainBufferBytes <= std::numeric_limits<int>::max());
		static_assert(kDrainThresholdBytes <= (std::numeric_limits<std::size_t>::max() - kDrainBufferBytes));

		try {
			Message Reply;
			Reply.m_Kind = Kind::Error;
			Reply.m_strText.assign(strvText.data(), std::min(strvText.size(), kMaximumReplyTextBytes));
			SendMessage(hControl, Reply, kReplyTimeoutMilliseconds);
			// Half-close is best effort; the caller closes this rejected socket after the bounded drain.
			shutdown(hControl, SD_SEND);
			Clock::time_point const Deadline = Clock::now() + std::chrono::milliseconds(kDrainTimeoutMilliseconds);
			std::array<char, kDrainBufferBytes> arrBuffer {};
			std::size_t unDrainedBytes = 0;
			while (!Stopping() && (Clock::now() < Deadline) && (unDrainedBytes < kDrainThresholdBytes)) {
				const int nReceivedBytes = recv(hControl, arrBuffer.data(), static_cast<int>(arrBuffer.size()), 0);
				if (nReceivedBytes > 0) {
					unDrainedBytes += static_cast<std::size_t>(nReceivedBytes);
					continue;
				}

				if (!nReceivedBytes) {
					break;
				}

				const int nError = WSAGetLastError();
				if (!net::WouldBlock(nError) || !Wait(hControl, POLLRDNORM, Deadline)) {
					break;
				}
			}
		} catch (...) {
			OutputDebugStringA("WireSteam speed server rejection failed\n");
		}
	}

	struct ClientJob {
		ClientJob(net::Socket Socket, std::string strLabel) :
			m_ControlSocket(std::move(Socket)),
			m_strPrefix(std::move(strLabel))
		{
			m_nResult = 0;
		}

		~ClientJob() noexcept {
			Join();
		}

		ClientJob(ClientJob const&) = delete;
		ClientJob& operator=(ClientJob const&) = delete;

		void Join() noexcept {
			try {
				if (m_Worker.joinable()) {
					m_Worker.join();
				}
			} catch (...) {
				OutputDebugStringA("WireSteam speed server worker join failed\n");
				std::terminate();
			}
		}

		net::Socket m_ControlSocket;
		std::string m_strPrefix;
		std::atomic<bool> m_bFinished;
		int m_nResult;
		std::thread m_Worker;
	};

	// The launcher or manager owns pJob until its worker has joined.
	void RunWorker(ClientJob* pJob, Clock::time_point ExpiresAt) noexcept {
		int nResult = 0;
		try {
			RunControl(pJob->m_ControlSocket.get(), ExpiresAt, pJob->m_strPrefix);
		} catch (std::exception const& Error) {
			nResult = Stopping() ? 130 : 1;
			try {
				LogSafely(pJob->m_strPrefix + "Control client failed: " + Error.what());
			} catch (...) {
				OutputDebugStringA("WireSteam speed server worker diagnostic failed\n");
			}

			if (!Stopping()) {
				RejectClient(pJob->m_ControlSocket.get(), Error.what());
			}
		} catch (...) {
			nResult = Stopping() ? 130 : 1;
			try {
				LogSafely(pJob->m_strPrefix + "Control client failed: unknown worker error");
			} catch (...) {
				OutputDebugStringA("WireSteam speed server worker diagnostic failed\n");
			}

			if (!Stopping()) {
				RejectClient(pJob->m_ControlSocket.get(), "Unknown server worker error");
			}
		}

		pJob->m_ControlSocket.reset();
		pJob->m_nResult = nResult;
		// Release publishes the result and closed socket before owner-thread acquisition/join/erasure.
		pJob->m_bFinished.store(true, std::memory_order_release);
	}

	class ClientJobs {
	public:
		explicit ClientJobs(std::size_t unMaximumClients) {
			m_nUnwinding = std::uncaught_exceptions();
			m_vecJobs.reserve(unMaximumClients);
		}

		~ClientJobs() noexcept {
			// Cancel blocked control/data work before joining on an exceptional
			// unwind. Normal listener expiry lets each active phase finish.
			if (std::uncaught_exceptions() > m_nUnwinding) {
				RequestStop();
			}

			for (auto const& pJob : m_vecJobs) {
				pJob->Join();
			}
		}

		ClientJobs(ClientJobs const&) = delete;
		ClientJobs& operator=(ClientJobs const&) = delete;

		void Reap() {
			for (auto itJob = m_vecJobs.begin(); itJob != m_vecJobs.end();) {
				if (!(*itJob)->m_bFinished.load(std::memory_order_acquire)) {
					++itJob;
					continue;
				}

				(*itJob)->Join();
				itJob = m_vecJobs.erase(itJob);
			}
		}

		void Add(std::unique_ptr<ClientJob> pJob) {
			try {
				// The owner thread checks its reserved capacity before launching this worker.
				m_vecJobs.push_back(std::move(pJob));
			} catch (...) {
				// Cancel before the untracked parameter owner destructs and joins its worker.
				RequestStop();
				throw;
			}
		}

		int JoinFirst() {
			// --once reaches this only after publishing its sole job successfully.
			m_vecJobs.front()->Join();

			return m_vecJobs.front()->m_nResult;
		}

		std::size_t GetSize() const noexcept {
			return m_vecJobs.size();
		}

	private:
		int m_nUnwinding;
		std::vector<std::unique_ptr<ClientJob>> m_vecJobs;
	};

	int Run(Options const& ServerOptions) {
		constexpr int kPollMilliseconds = 250;

		Session SessionScope;
		net::Socket Listener = Listen(ServerOptions.m_ListenEndpoint);
		Log("Speed server listening on " + net::Bound(Listener.get()).ToString());
		Log("Concurrent client limit: " + std::to_string(ServerOptions.m_unMaximumClients));
		Clock::time_point const ExpiresAt = ServerOptions.m_unRunSeconds ? (Clock::now() + std::chrono::seconds(ServerOptions.m_unRunSeconds)) : Clock::time_point::max();

		// Join worker owners before choosing status; Session keeps Winsock alive through cleanup.
		{
			ClientJobs Jobs(ServerOptions.m_unMaximumClients);
			std::uint64_t unNextClient = 1;
			while (!Stopping() && (Clock::now() < ExpiresAt)) {
				Jobs.Reap();
				Clock::time_point const PollUntil = std::min(ExpiresAt, Clock::now() + std::chrono::milliseconds(kPollMilliseconds));
				if (!Wait(Listener.get(), POLLRDNORM, PollUntil)) {
					continue;
				}

				net::Socket ClientSocket;
				net::Address RemoteAddress;
				try {
					ClientSocket = Accept(Listener.get(), std::min(ExpiresAt, Clock::now() + std::chrono::milliseconds(kPollMilliseconds)), &RemoteAddress);
				} catch (std::exception const& Error) {
					if (Stopping()) {
						break;
					}

					Log(std::string("Control accept failed: ") + Error.what());
					continue;
				}

				if (!ClientSocket) {
					continue;
				}

				if (unNextClient == std::numeric_limits<std::uint64_t>::max()) {
					throw std::runtime_error("Control client identifier range exhausted");
				}

				std::string const strPrefix = "[client " + std::to_string(unNextClient++) + " " + net::Describe(RemoteAddress).ToString() + "] ";
				Jobs.Reap();

				if (Jobs.GetSize() >= ServerOptions.m_unMaximumClients) {
					std::string const strReason = "Server busy: maximum concurrent clients reached";
					Log(strPrefix + strReason);
					RejectClient(ClientSocket.get(), strReason);
					continue;
				}

				std::unique_ptr<ClientJob> pJob;
				try {
					pJob = std::make_unique<ClientJob>(std::move(ClientSocket), strPrefix);
					pJob->m_Worker = std::thread(RunWorker, pJob.get(), ExpiresAt);
				} catch (std::exception const& Error) {
					LogSafely(strPrefix + "Cannot start client worker: " + Error.what());
					const SOCKET hControl = pJob ? pJob->m_ControlSocket.get() : ClientSocket.get();
					if (hControl != INVALID_SOCKET) {
						RejectClient(hControl, "Server cannot start another client worker");
					}

					if (ServerOptions.m_bOnce) {
						return 1;
					}

					continue;
				}

				Jobs.Add(std::move(pJob));

				if (ServerOptions.m_bOnce) {
					Listener.reset();
					const int nResult = Jobs.JoinFirst();
					return Stopping() ? 130 : nResult;
				}
			}

			Listener.reset();
			Log(Stopping() ? "Stopping speed server and active clients" : "Listener lifetime ended; waiting for active test phases");
		}

		return Stopping() ? 130 : 0;
	}
} // namespace

int main(int nArgumentCount, char** ppszArguments) {
	try {
		Options const ServerOptions = ParseOptions(nArgumentCount, ppszArguments);
		if (ServerOptions.m_bHelp) {
			PrintHelp();
			return 0;
		}

		if (ServerOptions.m_bVersion) {
			std::cout << "WireSteamSpeedServer 0.2.0 (concurrent sessions)\n";
			return 0;
		}

		if (ServerOptions.m_bSelfTest) {
			Session SessionScope;
			RunUnitTests();
			return 0;
		}

		return Run(ServerOptions);
	} catch (std::exception const& Error) {
		std::fprintf(stderr, "Speed server error: %s\n", Error.what());
		return 1;
	} catch (...) {
		std::fputs("Speed server error: unknown exception\n", stderr);
		return 1;
	}
}
