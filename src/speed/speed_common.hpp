#pragma once

// C++
#include <cstddef>
#include <cstdint>

// STL
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

// Project
#include "net.hpp"

namespace ws::speed {
	namespace net = st::net;
	using Clock = std::chrono::steady_clock;
	using Token = std::array<std::uint8_t, 0x10>;

	enum class Protocol : std::uint32_t {
		Tcp = 1,
		Udp = 2
	};

	enum class Direction : std::uint32_t {
		Upload = 1,
		Download = 2
	};

	enum class Kind : std::uint32_t {
		Request = 1,
		Ready,
		ClientReady,
		Begin,
		SenderDone,
		ReceiverDone,
		Result,
		Ping,
		Pong,
		Finish,
		Error
	};

	struct Spec {
		Protocol m_Protocol = Protocol::Tcp;
		Direction m_Direction = Direction::Upload;
		std::uint32_t m_unSeconds = 10;
		std::uint32_t m_unStreams = 4;
		std::uint32_t m_unBlockSize = 65536;
		std::uint32_t m_unUDPSize = 1100;
		double m_dblUDPMbps = 32;
	};

	struct Stats {
		std::uint64_t m_unBytes = 0;
		std::uint64_t m_unPackets = 0;
		std::uint64_t m_unDuplicates = 0;
		std::uint64_t m_unReordered = 0;
		std::uint64_t m_unInvalid = 0;
		std::uint64_t m_unLocalDrops = 0;
		double m_dblSeconds = 0;
		double m_dblJitterMilliseconds = 0;
		double m_dblPeakMbps = 0;
	};

	struct Message {
		Kind m_Kind = Kind::Error;
		Spec m_Specification;
		Token m_arrToken {};
		std::uint32_t m_unPort = 0;
		std::uint32_t m_unPhase = 0;
		Stats m_Sender;
		Stats m_Receiver;
		std::string m_strText;
	};

	inline constexpr std::uint32_t kMaxSeconds = 300;
	inline constexpr std::size_t kUdpHeader = 0x30;
	inline constexpr std::uint64_t kMaxUdpPackets = 128 * 1024 * 1024;

	// Owns the process's Winsock reference and console stop handler.
	// Keep one Session alive until all speed sockets and workers have stopped.
	class Session {
	public:
		Session();
		~Session();
		Session(Session const&) = delete;
		Session& operator=(Session const&) = delete;
	};

	// Stop state is shared safely across the console callback and worker threads.
	bool Stopping();
	void RequestStop();
	// Serializes complete flushed records across calling threads.
	void Log(std::string const& strText);
	void Require(bool bCondition, std::string const& strText);
	std::uint32_t Number(std::string const& strText, std::uint32_t unMinimum, std::uint32_t unMaximum);
	double Decimal(std::string const& strText, double dblMinimum, double dblMaximum);
	st::Endpoint ParseEndpoint(std::string const& strText, bool bAllowZero = false);
	void Validate(Spec const& Specification);
	Token NewToken();
	std::string Host(net::Address const& AddressValue);
	net::Address Remote(SOCKET hSocket);
	net::Address Local(SOCKET hSocket);
	void Tune(SOCKET hSocket, bool bTCP);
	bool Wait(SOCKET hSocket, short nEvents, Clock::time_point Deadline);
	net::Socket Connect(st::Endpoint const& EndpointValue, int nTimeoutMilliseconds = 10000);
	net::Socket Listen(st::Endpoint const& EndpointValue);
	net::Socket Accept(SOCKET hListener, Clock::time_point Deadline, net::Address* pRemoteOut = nullptr);
	void SendMessage(SOCKET hSocket, Message const& MessageValue, int nTimeoutMilliseconds = 10000);
	Message ReceiveMessage(SOCKET hSocket, int nTimeoutMilliseconds = 10000);
	void Expect(Message const& MessageValue, Kind KindValue);
	void SendDataHello(SOCKET hSocket, Token const& arrToken, std::uint32_t unPhase, std::uint32_t unStream, int nTimeoutMilliseconds = 10000);
	std::uint32_t ReceiveDataHello(SOCKET hSocket, Token const& arrToken, std::uint32_t unPhase, int nTimeoutMilliseconds = 5000);
	void SendUdpProbe(SOCKET hSocket, Token const& arrToken, std::uint32_t unPhase);
	net::Address ReceiveUdpProbe(SOCKET hSocket, Token const& arrToken, std::uint32_t unPhase, net::Address const& ControlPeer);
	// Synchronous transfer/drain: keep the vector and its sockets alive and exclusively accessible until return.
	Stats TransferTcp(std::vector<net::Socket>& vecSockets, bool bSender, Spec const& Specification, std::string const& strLabel, bool bIntervals = true);
	// Borrows the socket synchronously; keep it alive and avoid concurrent writers until return.
	Stats SendUdp(SOCKET hSocket, Spec const& Specification, Token const& arrToken, std::uint32_t unPhase, std::string const& strLabel, bool bIntervals = true);

	// Copies the token and borrows an already nonblocking socket. Keep the socket
	// alive until Finish succeeds or receiver destruction completes. Calls and
	// destruction belong to one owner thread; successful Finish joins the worker.
	class UdpReceiver {
	public:
		UdpReceiver(SOCKET hSocket, Token const& arrToken, std::uint32_t unPhase, std::uint32_t unPayloadBytes);
		~UdpReceiver();
		Stats Finish(Stats const& Sender, unsigned unDrainMilliseconds = 2000);

	private:
		struct Implementation;
		std::unique_ptr<Implementation> m_pImplementation;
	};

	// Reporting accepts default zero-time records or measured transfer statistics.
	// Supply finite elapsed times from TransferTcp, SendUdp or Finish with measured sender statistics.
	double Mbps(Stats const& Statistics);
	double Loss(Stats const& Sender, Stats const& Receiver);
	std::string Summary(Message const& Result);
	std::string ResultJson(Message const& Result);
	void RunUnitTests();
} // namespace ws::speed
