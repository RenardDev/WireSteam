
// General
#include <winsock2.h>
#include <Windows.h>

// C++
#include <cmath>
#include <cstdio>
#include <exception>
#include <utility>

// STL
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Project
#include "speed_common.hpp"

namespace ws::speed {
	namespace {
		struct CommandLineError : std::runtime_error {
			using std::runtime_error::runtime_error;
		};

		struct Options {
			Options() {
				constexpr std::uint16_t kDefaultPort = 5201;

				m_Target = { "10.77.0.1", kDefaultPort };
				m_strProtocol = "tcp";
				m_strDirection = "both";
				m_bQuiet = false;
				m_bSelfTest = false;
				m_bHelp = false;
			}

			st::Endpoint m_Target;
			Spec m_Specification;
			std::string m_strProtocol;
			std::string m_strDirection;
			std::string m_strJSONPath;
			bool m_bQuiet;
			bool m_bSelfTest;
			bool m_bHelp;
		};

		void PrintHelp() {
			std::cout
				<< "WireSteamSpeedClient - receiver-confirmed TCP / UDP throughput measurement\n"
				<< "\n"
				<< "Usage: WireSteamSpeedClient.exe [options]\n"
				<< "  --target HOST:PORT          Server address (default 10.77.0.1:5201)\n"
				<< "  --seconds N                 Seconds per measurement, 1..300 (default 10)\n"
				<< "  --streams N                 Parallel TCP streams, 1..32 (default 4)\n"
				<< "  --protocol tcp|udp|both      Protocols to measure (default tcp)\n"
				<< "  --direction upload|download|both\n"
				<< "                              Directions, tested sequentially (default both)\n"
				<< "  --udp-mbps N                Offered UDP rate in decimal Mbit/s (default 32)\n"
				<< "  --udp-size N                Useful UDP payload bytes (default 1100; +48 on wire)\n"
				<< "  --block-size N              TCP buffer size in bytes (default 65536)\n"
				<< "  --json PATH                 Save the completed measurements as JSON\n"
				<< "  --quiet                     Disable per-second measurement output\n"
				<< "  --self-test                 Run common protocol and measurement unit tests\n"
				<< "  --help                      Show this help\n"
				<< "\n"
				<< "No arguments opens the English setup launcher.\n"
				<< "Throughput summaries use bytes actually received by the destination.\n"
				<< "UDP uses a fixed offered rate; its result does not find the link's capacity.\n"
				<< "Mbit/s means 1,000,000 bits per second. Tests require a matching speed server.\n"
				<< "Press Ctrl+C to stop.\n";
		}

		std::string EncodeUTF8(wchar_t const* pszValue) {
			if (!pszValue || !*pszValue) {
				return {};
			}

			const int nBytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pszValue, -1, nullptr, 0, nullptr, nullptr);
			if (!nBytes) {
				throw CommandLineError("A command-line argument contains invalid Unicode");
			}

			std::string strResult = std::string(static_cast<std::size_t>(nBytes), '\0');
			if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pszValue, -1, strResult.data(), nBytes, nullptr, nullptr) != nBytes) {
				throw CommandLineError("Cannot decode a command-line argument");
			}

			strResult.pop_back();

			return strResult;
		}

		Options ParseOptions(std::vector<std::string> const& vecArguments) {
			constexpr unsigned kMaximumStreams = 32;
			constexpr double kMinimumUDPMbps = 0.001;
			constexpr double kMaximumUDPMbps = 100000;
			constexpr std::uint32_t kMinimumUDPPayloadBytes = 64;
			constexpr std::uint32_t kMaximumUDPDatagramBytes = 65507;
			constexpr std::uint32_t kMaximumBlockBytes = 4 * 1024 * 1024;

			static_assert(kUdpHeader < kMaximumUDPDatagramBytes);

			Options Parsed;
			try {
				for (std::size_t unArgumentIndex = 0; unArgumentIndex < vecArguments.size(); ++unArgumentIndex) {
					std::string const& strFlag = vecArguments[unArgumentIndex];
					if ((strFlag == "--help") || (strFlag == "-h")) {
						Parsed.m_bHelp = true;
						return Parsed;
					}

					if (strFlag == "--quiet") {
						Parsed.m_bQuiet = true;
						continue;
					}

					if (strFlag == "--self-test") {
						Parsed.m_bSelfTest = true;
						continue;
					}

					if (unArgumentIndex == (vecArguments.size() - 1)) {
						throw CommandLineError("Missing value for " + strFlag);
					}

					std::string const& strValue = vecArguments[++unArgumentIndex];
					if (strFlag == "--target") {
						Parsed.m_Target = ParseEndpoint(strValue);

					} else if (strFlag == "--seconds") {
						Parsed.m_Specification.m_unSeconds = Number(strValue, 1, kMaxSeconds);

					} else if (strFlag == "--streams") {
						Parsed.m_Specification.m_unStreams = Number(strValue, 1, kMaximumStreams);

					} else if (strFlag == "--protocol") {
						Parsed.m_strProtocol = strValue;

					} else if (strFlag == "--direction") {
						Parsed.m_strDirection = strValue;

					} else if (strFlag == "--udp-mbps") {
						Parsed.m_Specification.m_dblUDPMbps = Decimal(strValue, kMinimumUDPMbps, kMaximumUDPMbps);

					} else if (strFlag == "--udp-size") {
						Parsed.m_Specification.m_unUDPSize = Number(strValue, kMinimumUDPPayloadBytes, kMaximumUDPDatagramBytes - static_cast<std::uint32_t>(kUdpHeader));

					} else if (strFlag == "--block-size") {
						Parsed.m_Specification.m_unBlockSize = Number(strValue, 1, kMaximumBlockBytes);

					} else if (strFlag == "--json") {
						Require(!strValue.empty(), "JSON path must not be empty");
						Parsed.m_strJSONPath = strValue;

					} else {
						throw CommandLineError("Unknown argument: " + strFlag);
					}
				}

				Require((Parsed.m_strProtocol == "tcp") || (Parsed.m_strProtocol == "udp") || (Parsed.m_strProtocol == "both"), "Protocol must be tcp, udp or both");
				Require((Parsed.m_strDirection == "upload") || (Parsed.m_strDirection == "download") || (Parsed.m_strDirection == "both"), "Direction must be upload, download or both");
				Validate(Parsed.m_Specification);

				if (Parsed.m_strProtocol != "tcp") {
					Spec UDP = Parsed.m_Specification;
					UDP.m_Protocol = Protocol::Udp;
					UDP.m_unStreams = 1;
					Validate(UDP);
				}
			} catch (CommandLineError const&) {
				throw;
			} catch (std::exception const& Error) {
				throw CommandLineError(Error.what());
			}

			return Parsed;
		}

		std::string PromptValue(std::string const& strLabel, std::string const& strFallback) {
			std::cout << strLabel << " [" << strFallback << "]: " << std::flush;
			std::string strValue;
			if (!std::getline(std::cin, strValue)) {
				throw CommandLineError("Input ended before the launcher completed");
			}

			if ((strValue == "q") || (strValue == "quit")) {
				throw CommandLineError("Launcher cancelled");
			}

			return strValue.empty() ? strFallback : strValue;
		}

		Options RunLauncher() {
			constexpr std::size_t kProtocolValueIndex = 5;

			std::cout
				<< "WireSteam speed test setup\n"
				<< "Press Enter to accept a default, or q to cancel.\n"
				<< "\n";

			std::vector<std::string> vecArguments {
				"--target", PromptValue("Speed server HOST:PORT", "10.77.0.1:5201"),
				"--seconds", PromptValue("Seconds per measurement", "10"),
				"--protocol", PromptValue("Protocol: tcp, udp, both", "tcp"),
				"--direction", PromptValue("Direction: upload, download, both", "both"),
				"--streams", PromptValue("Parallel TCP streams", "4")
			};
			std::string const strProtocol = vecArguments[kProtocolValueIndex];
			if ((strProtocol == "udp") || (strProtocol == "both")) {
				vecArguments.push_back("--udp-mbps");
				vecArguments.push_back(PromptValue("Offered UDP rate in Mbit/s", "32"));
			}

			return ParseOptions(vecArguments);
		}

		bool HaveSameSpecification(Spec const& Left, Spec const& Right) {
			constexpr double kRateTolerance = 1e-9;

			return (Left.m_Protocol == Right.m_Protocol) && (Left.m_Direction == Right.m_Direction) && (Left.m_unSeconds == Right.m_unSeconds) && (Left.m_unStreams == Right.m_unStreams) && (Left.m_unBlockSize == Right.m_unBlockSize) && (Left.m_unUDPSize == Right.m_unUDPSize) && (std::abs(Left.m_dblUDPMbps - Right.m_dblUDPMbps) <= (std::max(1.0, std::abs(Left.m_dblUDPMbps)) * kRateTolerance));
		}

		void ValidatePhase(Message const& MessageValue, Kind KindValue, Spec const& Specification, std::uint32_t unPhase, Token const& arrToken) {
			Expect(MessageValue, KindValue);
			Require((MessageValue.m_unPhase == unPhase) && (MessageValue.m_arrToken == arrToken) && HaveSameSpecification(MessageValue.m_Specification, Specification), "Server response does not match the measurement phase, token or settings");
		}

		Message CreateControlMessage(Kind KindValue, Spec const& Specification, std::uint32_t unPhase, Token const& arrToken) {
			Message Result;
			Result.m_Kind = KindValue;
			Result.m_Specification = Specification;
			Result.m_unPhase = unPhase;
			Result.m_arrToken = arrToken;

			return Result;
		}

		std::string FormatMeasurementLabel(Spec const& Specification) {
			return std::string((Specification.m_Protocol == Protocol::Tcp) ? "TCP " : "UDP ") + ((Specification.m_Direction == Direction::Upload) ? "upload" : "download");
		}

		void ValidateStatistics(Stats const& Statistics, Spec const& Specification, char const* pszName) {
			constexpr double kAdditionalDurationSeconds = 30;

			Require(std::isfinite(Statistics.m_dblSeconds) && (Statistics.m_dblSeconds >= 0) && (Statistics.m_dblSeconds <= (static_cast<double>(Specification.m_unSeconds) + kAdditionalDurationSeconds)), std::string("Invalid ") + pszName + " duration in server result");
			Require(std::isfinite(Statistics.m_dblJitterMilliseconds) && (Statistics.m_dblJitterMilliseconds >= 0) && std::isfinite(Statistics.m_dblPeakMbps) && (Statistics.m_dblPeakMbps >= 0), std::string("Invalid ") + pszName + " rate or jitter in server result");
			Require(!Statistics.m_unBytes || (Statistics.m_dblSeconds > 0), std::string("Nonempty ") + pszName + " result has no measurement duration");

			if (Specification.m_Protocol == Protocol::Udp) {
				Require((Statistics.m_unPackets <= kMaxUdpPackets) && (Statistics.m_unBytes == (Statistics.m_unPackets * static_cast<std::uint64_t>(Specification.m_unUDPSize))), std::string("Invalid ") + pszName + " UDP counters in server result");
			}
		}

		void ValidateResult(Message const& Result, Spec const& Specification, std::uint32_t unPhase, Token const& arrToken, Stats const& Local) {
			ValidatePhase(Result, Kind::Result, Specification, unPhase, arrToken);
			ValidateStatistics(Result.m_Sender, Specification, "sender");
			ValidateStatistics(Result.m_Receiver, Specification, "receiver");
			Require(Result.m_Receiver.m_unBytes <= Result.m_Sender.m_unBytes, "Server claims more received bytes than were sent");

			if (Specification.m_Protocol == Protocol::Udp) {
				Require(Result.m_Receiver.m_unPackets <= Result.m_Sender.m_unPackets, "Server claims more unique UDP packets than were sent");
			}

			Stats const& Echoed = (Specification.m_Direction == Direction::Upload) ? Result.m_Sender : Result.m_Receiver;
			Require((Echoed.m_unBytes == Local.m_unBytes) && (Echoed.m_unPackets == Local.m_unPackets) && (Echoed.m_unLocalDrops == Local.m_unLocalDrops) && (Echoed.m_unInvalid == Local.m_unInvalid) && (Echoed.m_unDuplicates == Local.m_unDuplicates) && (Echoed.m_unReordered == Local.m_unReordered), "Server result does not echo this client's measured counters");
		}

		std::array<double, 3> MeasureRTT(SOCKET hControl) {
			constexpr std::size_t kSampleCount = 5;
			constexpr int kPrecision = 2;

			std::array<double, kSampleCount> arrSamples {};
			Token const arrToken = NewToken();
			for (auto& dblSample : arrSamples) {
				Message Ping;
				Ping.m_Kind = Kind::Ping;
				Ping.m_arrToken = arrToken;
				Ping.m_unPhase = 0;
				Clock::time_point const BeganAt = Clock::now();
				SendMessage(hControl, Ping);
				Message const Pong = ReceiveMessage(hControl);
				Expect(Pong, Kind::Pong);
				Require((Pong.m_arrToken == arrToken) && (Pong.m_unPhase == Ping.m_unPhase), "RTT reply does not match the request nonce");
				dblSample = std::chrono::duration<double, std::milli>(Clock::now() - BeganAt).count();
			}

			const double dblAverage = std::accumulate(arrSamples.begin(), arrSamples.end(), 0.0) / static_cast<double>(arrSamples.size());
			auto itMinimum = std::min_element(arrSamples.begin(), arrSamples.end());
			auto itMaximum = std::max_element(arrSamples.begin(), arrSamples.end());
			std::array<double, 3> const arrResult { *itMinimum, dblAverage, *itMaximum };

			std::ostringstream OutputStream;

			OutputStream.imbue(std::locale::classic());
			OutputStream << std::fixed << std::setprecision(kPrecision) << "Control RTT over 5 samples: min=" << arrResult[0] << " ms mean=" << arrResult[1] << " ms max=" << arrResult[2] << " ms";
			Log(OutputStream.str());

			return arrResult;
		}

		net::Socket CreateUDPSocket(net::Address const& ControlRemote, std::uint16_t unPort) {
			st::Endpoint const Endpoint { Host(ControlRemote), unPort };
			std::string strError;
			net::Address Target;
			Require(net::Resolve(Endpoint, SOCK_DGRAM, Target, strError), strError);
			char const* pszAny = (Target.family() == AF_INET6) ? "::" : "0.0.0.0";
			net::Socket Socket = net::Bind({ pszAny, 0 }, SOCK_DGRAM, strError);
			Require(bool(Socket), strError);
			Tune(Socket.get(), false);

			if (connect(Socket.get(), Target.ptr(), Target.m_nSize) != 0) {
				const int nError = WSAGetLastError();
				throw std::runtime_error("Cannot connect UDP data socket: " + net::Error(nError));
			}

			return Socket;
		}

		Message RunPhase(SOCKET hControl, net::Address const& Server, Spec const& Specification, std::uint32_t unPhase, bool bIntervals) {
			constexpr std::uint32_t kMaximumPort = 65535;
			constexpr unsigned kSetupTimeoutSeconds = 10;
			constexpr unsigned kAdditionalTimeoutSeconds = 35;
			constexpr unsigned kMillisecondsPerSecond = 1000;

			static_assert(kMaxSeconds <= ((std::numeric_limits<int>::max() / kMillisecondsPerSecond) - kAdditionalTimeoutSeconds));
			static_assert(kSetupTimeoutSeconds <= (std::numeric_limits<int>::max() / kMillisecondsPerSecond));

			Validate(Specification);
			Message Request;
			Request.m_Kind = Kind::Request;
			Request.m_Specification = Specification;
			Request.m_unPhase = unPhase;
			SendMessage(hControl, Request);
			Message const Ready = ReceiveMessage(hControl);
			Expect(Ready, Kind::Ready);
			Require((Ready.m_unPhase == unPhase) && HaveSameSpecification(Ready.m_Specification, Specification), "Ready response does not match the requested phase or settings");
			Require((Ready.m_unPort > 0) && (Ready.m_unPort <= kMaximumPort), "Server returned an invalid data port");
			Require(std::any_of(Ready.m_arrToken.begin(), Ready.m_arrToken.end(), [](std::uint8_t unValue) -> bool {
				return unValue != 0x00;
			}),
				"Server returned an empty session token");
			Token const arrToken = Ready.m_arrToken;
			std::string const strLabel = FormatMeasurementLabel(Specification);
			std::vector<net::Socket> vecStreams;
			net::Socket UDPSocket;
			std::unique_ptr<UdpReceiver> pUDPReceiver;
			if (Specification.m_Protocol == Protocol::Tcp) {
				vecStreams.reserve(Specification.m_unStreams);
				Clock::time_point const SetupDeadline = Clock::now() + std::chrono::seconds(kSetupTimeoutSeconds);
				auto GetRemainingSetupMilliseconds = [SetupDeadline]() -> int {
					const std::chrono::milliseconds::rep nRemainingMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(SetupDeadline - Clock::now()).count();
					Require(nRemainingMilliseconds > 0, "TCP data stream setup timed out");

					return static_cast<int>(nRemainingMilliseconds);
				};
				for (std::uint32_t unStreamIndex = 0; unStreamIndex < Specification.m_unStreams; ++unStreamIndex) {
					net::Socket Socket = Connect({ Host(Server), static_cast<std::uint16_t>(Ready.m_unPort) }, GetRemainingSetupMilliseconds());
					SendDataHello(Socket.get(), arrToken, unPhase, unStreamIndex, GetRemainingSetupMilliseconds());
					Require(Clock::now() < SetupDeadline, "TCP data stream setup timed out");
					vecStreams.push_back(std::move(Socket));
				}

			} else {
				UDPSocket = CreateUDPSocket(Server, static_cast<std::uint16_t>(Ready.m_unPort));
				SendUdpProbe(UDPSocket.get(), arrToken, unPhase);

				if (Specification.m_Direction == Direction::Download) {
					pUDPReceiver = std::make_unique<UdpReceiver>(UDPSocket.get(), arrToken, unPhase, Specification.m_unUDPSize);
				}
			}

			SendMessage(hControl, CreateControlMessage(Kind::ClientReady, Specification, unPhase, arrToken));
			ValidatePhase(ReceiveMessage(hControl), Kind::Begin, Specification, unPhase, arrToken);
			Log("Starting " + strLabel + " for " + std::to_string(Specification.m_unSeconds) + " s" + ((Specification.m_Protocol == Protocol::Tcp) ? (" with " + std::to_string(Specification.m_unStreams) + " stream(s)") : (" at offered rate " + std::to_string(Specification.m_dblUDPMbps) + " Mbit/s")));
			Stats Local;
			const int nTimeoutMilliseconds = static_cast<int>((Specification.m_unSeconds + kAdditionalTimeoutSeconds) * kMillisecondsPerSecond);
			if (Specification.m_Direction == Direction::Upload) {
				Local = (Specification.m_Protocol == Protocol::Tcp) ? TransferTcp(vecStreams, true, Specification, strLabel, bIntervals) : SendUdp(UDPSocket.get(), Specification, arrToken, unPhase, strLabel, bIntervals);
				Message Done = CreateControlMessage(Kind::SenderDone, Specification, unPhase, arrToken);
				Done.m_Sender = Local;
				SendMessage(hControl, Done);

			} else if (Specification.m_Protocol == Protocol::Tcp) {
				Local = TransferTcp(vecStreams, false, Specification, strLabel, bIntervals);
				Message const Done = ReceiveMessage(hControl, nTimeoutMilliseconds);
				ValidatePhase(Done, Kind::SenderDone, Specification, unPhase, arrToken);
				ValidateStatistics(Done.m_Sender, Specification, "sender");
				Message Received = CreateControlMessage(Kind::ReceiverDone, Specification, unPhase, arrToken);
				Received.m_Receiver = Local;
				SendMessage(hControl, Received);

			} else {
				Message const Done = ReceiveMessage(hControl, nTimeoutMilliseconds);
				ValidatePhase(Done, Kind::SenderDone, Specification, unPhase, arrToken);
				ValidateStatistics(Done.m_Sender, Specification, "sender");
				Local = pUDPReceiver->Finish(Done.m_Sender);
				Message Received = CreateControlMessage(Kind::ReceiverDone, Specification, unPhase, arrToken);
				Received.m_Receiver = Local;
				SendMessage(hControl, Received);
			}

			Message const Result = ReceiveMessage(hControl, nTimeoutMilliseconds);
			ValidateResult(Result, Specification, unPhase, arrToken, Local);
			Log(Summary(Result));

			if (Result.m_Sender.m_unLocalDrops || Result.m_Receiver.m_unLocalDrops || Result.m_Receiver.m_unInvalid) {
				Log("Measurement warning: local socket drops or invalid datagrams occurred; inspect counters before treating this as link loss.");
			}

			return Result;
		}

		std::string EncodeJSONString(std::string const& strValue) {
			constexpr char kHexDigits[] = "0123456789abcdef";
			constexpr unsigned char kControlCharacterLimit = 0x20;
			constexpr unsigned kNibbleBits = 4;
			constexpr unsigned char kNibbleMask = 0x0F;

			std::string strResult = "\"";
			for (auto const& chRaw : strValue) {
				const unsigned char unByte = static_cast<unsigned char>(chRaw);
				if ((unByte == '"') || (unByte == '\\')) {
					strResult += '\\';
					strResult += static_cast<char>(unByte);

				} else if (unByte < kControlCharacterLimit) {
					strResult += "\\u00";
					strResult += kHexDigits[unByte >> kNibbleBits];
					strResult += kHexDigits[unByte & kNibbleMask];

				} else {
					strResult += static_cast<char>(unByte);
				}
			}

			strResult += '"';

			return strResult;
		}

		void WriteJSONReport(Options const& ClientOptions, std::array<double, 3> const& arrRTT, std::vector<Message> const& vecResults) {
			constexpr int kStatisticsPrecision = 12;

			if (ClientOptions.m_strJSONPath.empty()) {
				return;
			}

			std::ostringstream OutputStream;

			OutputStream.imbue(std::locale::classic());
			OutputStream << std::setprecision(kStatisticsPrecision);

			OutputStream
				<< "{\n"
				<< "  \"schema\": \"wiresteam.speed.v1\",\n"
				<< "  \"target\": " << EncodeJSONString(ClientOptions.m_Target.ToString()) << ",\n"
				<< "  \"units\": {\"throughput\": \"Mbit/s\", \"bytes\": \"bytes\", \"time\": \"seconds\", \"jitter\": \"milliseconds\"},\n"
				<< "  \"rttMs\": {\"min\": " << arrRTT[0] << ", \"mean\": " << arrRTT[1] << ", \"max\": " << arrRTT[2] << "},\n"
				<< "  \"results\": [\n";

			for (std::size_t unResultIndex = 0; unResultIndex < vecResults.size(); ++unResultIndex) {
				OutputStream
					<< "    " << ResultJson(vecResults[unResultIndex]) << ((unResultIndex == (vecResults.size() - 1)) ? "\n" : ",\n");
			}

			OutputStream
				<< "  ]\n"
				<< "}\n";

			std::filesystem::path const OutputPath = std::filesystem::path(std::u8string(ClientOptions.m_strJSONPath.begin(), ClientOptions.m_strJSONPath.end()));

			std::ofstream OutputFile(OutputPath, std::ios::binary | std::ios::trunc);
			Require(bool(OutputFile), "Cannot create JSON result file: " + ClientOptions.m_strJSONPath);

			OutputFile << OutputStream.str();
			OutputFile.close();
			Require(bool(OutputFile), "Cannot write the complete JSON result file: " + ClientOptions.m_strJSONPath);
			Log("JSON results saved: " + ClientOptions.m_strJSONPath);
		}

		int RunClient(Options const& ClientOptions) {
			constexpr std::array<Protocol, 2> kProtocols = { Protocol::Tcp, Protocol::Udp };
			constexpr std::array<Direction, 2> kDirections = { Direction::Upload, Direction::Download };

			Session SessionScope;
			if (ClientOptions.m_bSelfTest) {
				RunUnitTests();
				return 0;
			}

			Log("Connecting to speed server " + ClientOptions.m_Target.ToString());
			net::Socket ControlSocket = Connect(ClientOptions.m_Target);
			net::Address const Server = Remote(ControlSocket.get());
			std::array<double, 3> const arrRTT = MeasureRTT(ControlSocket.get());
			if (ClientOptions.m_strProtocol != "tcp") {
				Log("UDP uses a fixed offered load. Receiver goodput, loss and jitter are measurements at that load, not a capacity estimate.");
			}

			std::vector<Message> vecResults;
			std::uint32_t unPhase = 0;
			for (auto const& ProtocolValue : kProtocols) {
				if (((ProtocolValue == Protocol::Tcp) && (ClientOptions.m_strProtocol == "udp")) || ((ProtocolValue == Protocol::Udp) && (ClientOptions.m_strProtocol == "tcp"))) {
					continue;
				}

				for (auto const& DirectionValue : kDirections) {
					if (((DirectionValue == Direction::Upload) && (ClientOptions.m_strDirection == "download")) || ((DirectionValue == Direction::Download) && (ClientOptions.m_strDirection == "upload"))) {
						continue;
					}

					Spec Specification = ClientOptions.m_Specification;
					Specification.m_Protocol = ProtocolValue;
					Specification.m_Direction = DirectionValue;

					if (ProtocolValue == Protocol::Udp) {
						Specification.m_unStreams = 1;
					}

					vecResults.push_back(RunPhase(ControlSocket.get(), Server, Specification, ++unPhase, !ClientOptions.m_bQuiet));
				}
			}

			Message Finish;
			Finish.m_Kind = Kind::Finish;
			Finish.m_unPhase = unPhase;
			SendMessage(ControlSocket.get(), Finish);
			WriteJSONReport(ClientOptions, arrRTT, vecResults);
			Log("All requested receiver-confirmed measurements completed.");

			return 0;
		}
	} // namespace
} // namespace ws::speed

int wmain(int nArgumentCount, wchar_t** ppszArguments) {
	try {
		// Console encoding is best effort; redirected output and UTF-8 argument decoding are independent.
		SetConsoleOutputCP(CP_UTF8);
		SetConsoleCP(CP_UTF8);
		std::vector<std::string> vecArguments;
		for (std::size_t unArgumentIndex = 1; unArgumentIndex < static_cast<std::size_t>(nArgumentCount); ++unArgumentIndex) {
			vecArguments.push_back(ws::speed::EncodeUTF8(ppszArguments[unArgumentIndex]));
		}

		ws::speed::Options const ClientOptions = (nArgumentCount == 1) ? ws::speed::RunLauncher() : ws::speed::ParseOptions(vecArguments);
		if (ClientOptions.m_bHelp) {
			ws::speed::PrintHelp();
			return 0;
		}

		return ws::speed::RunClient(ClientOptions);
	} catch (ws::speed::CommandLineError const& Error) {
		std::fprintf(stderr, "Argument error: %s\nUse --help for usage.\n", Error.what());
		return 2;
	} catch (std::exception const& Error) {
		if (ws::speed::Stopping()) {
			std::fputs("Interrupted.\n", stderr);
			return 130;
		}

		std::fprintf(stderr, "Speed test failed: %s\n", Error.what());
		return 1;
	} catch (...) {
		std::fputs("Speed test failed: unknown exception\n", stderr);
		return 1;
	}
}
