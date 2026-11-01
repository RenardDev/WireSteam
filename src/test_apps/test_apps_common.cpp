#include "test_apps_common.hpp"

// General
#include <Windows.h>

// C++
#include <charconv>
#include <cstring>
#include <utility>

// STL
#include <algorithm>
#include <array>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace st::testapp {
	namespace {
		constexpr std::size_t kU32Bytes = sizeof(std::uint32_t);
		constexpr std::size_t kBitsPerByte = 8;
		constexpr std::size_t kPayloadHeaderBytes = 0x10;
		constexpr char kPayloadMagic[] = "STEC";
		constexpr std::size_t kMagicBytes = 0x04;
		constexpr std::size_t kTransportOffset = 0x04;
		constexpr std::size_t kReservedOffset = 0x05;
		constexpr std::size_t kConnectionOffset = 0x08;
		constexpr std::size_t kSequenceOffset = 0x0C;
		constexpr std::uint8_t kTCPTransport = 0x01;
		constexpr std::uint8_t kUDPTransport = 0x02;

		static_assert(kU32Bytes == 0x04);
		static_assert(std::numeric_limits<std::uint8_t>::digits == kBitsPerByte);
		static_assert(std::numeric_limits<unsigned>::digits == std::numeric_limits<std::uint32_t>::digits);
		static_assert((sizeof(kPayloadMagic) - 1) == kMagicBytes);
		static_assert((kReservedOffset + 0x03) <= kPayloadHeaderBytes);
		static_assert((kConnectionOffset + kU32Bytes) <= kPayloadHeaderBytes);
		static_assert((kSequenceOffset + kU32Bytes) <= kPayloadHeaderBytes);

		constinit std::atomic<bool> g_bStopping = false;
		constinit std::mutex g_OutputMutex;

		BOOL WINAPI Control(DWORD unEvent) noexcept {
			if ((unEvent == CTRL_C_EVENT) || (unEvent == CTRL_BREAK_EVENT) || (unEvent == CTRL_CLOSE_EVENT) || (unEvent == CTRL_SHUTDOWN_EVENT)) {
				g_bStopping.store(true);
				return TRUE;
			}

			return FALSE;
		}

		bool ParseUnsignedNumber(std::string_view strvText, std::uint64_t unMinimum, std::uint64_t unMaximum, std::uint64_t& unValueOut) {
			if (strvText.empty()) {
				return false;
			}

			std::uint64_t unParsed = 0;
			std::from_chars_result const Result = std::from_chars(strvText.data(), strvText.data() + strvText.size(), unParsed);
			if ((Result.ec != std::errc {}) || (Result.ptr != (strvText.data() + strvText.size())) || (unParsed < unMinimum) || (unParsed > unMaximum)) {
				return false;
			}

			unValueOut = unParsed;

			return true;
		}

		bool ParseEndpointAddress(std::string_view strvText, bool bAllowZeroPort, Endpoint& EndpointOut) {
			constexpr std::uint64_t kMaximumPort = 65535;

			std::string_view strvHost;
			std::string_view strvPort;
			if (!strvText.empty() && (strvText.front() == '[')) {
				const std::size_t unBracketEnd = strvText.find(']');
				if ((unBracketEnd == std::string_view::npos) || ((unBracketEnd + 1) >= strvText.size()) || (strvText[unBracketEnd + 1] != ':')) {
					return false;
				}

				strvHost = strvText.substr(1, unBracketEnd - 1);
				strvPort = strvText.substr(unBracketEnd + 2);

			} else {
				const std::size_t unColon = strvText.find(':');
				if ((unColon == std::string_view::npos) || (strvText.find(':', unColon + 1) != std::string_view::npos)) {
					return false;
				}

				strvHost = strvText.substr(0, unColon);
				strvPort = strvText.substr(unColon + 1);
			}

			std::uint64_t unPort = 0;
			if (strvHost.empty() || (strvHost.find_first_of(" \r\n\t[]") != std::string_view::npos) || !ParseUnsignedNumber(strvPort, bAllowZeroPort ? 0 : 1, kMaximumPort, unPort)) {
				return false;
			}

			EndpointOut = { std::string(strvHost), static_cast<std::uint16_t>(unPort) };

			return true;
		}

		void SelectProtocol(std::string const& strFlag, bool& bSelected, bool& bTCPOut, bool& bUDPOut) {
			if (!bSelected) {
				bTCPOut = bUDPOut = false;
				bSelected = true;
			}

			if (strFlag == "--tcp") {
				bTCPOut = true;

			} else {
				bUDPOut = true;
			}
		}

		// Callers establish at least kU32Bytes of fixed writable header storage.
		void WriteU32BE(std::uint8_t* punData, std::uint32_t unValue) {
			for (std::size_t unByteIndex = 0; unByteIndex < kU32Bytes; ++unByteIndex) {
				punData[unByteIndex] = static_cast<std::uint8_t>(unValue >> ((kU32Bytes - 1 - unByteIndex) * kBitsPerByte));
			}
		}

		bool ReadU32BE(std::uint8_t const* punData, std::size_t unAvailableBytes, std::uint32_t& unValueOut) noexcept {
			if (!punData || (unAvailableBytes < kU32Bytes)) {
				return false;
			}

			std::uint32_t unValue = 0;
			for (std::size_t unByteIndex = 0; unByteIndex < kU32Bytes; ++unByteIndex) {
				unValue = (unValue << kBitsPerByte) | punData[unByteIndex];
			}

			unValueOut = unValue;

			return true;
		}
	} // namespace

	bool ParseServer(int nArgumentCount, char** ppszArguments, ServerOptions& OptionsOut, std::string& strErrorOut) {
		constexpr std::uint64_t kMaximumClients = 4096;
		constexpr std::uint64_t kMaximumSeconds = 86400;

		static_assert(kMaximumClients <= std::numeric_limits<std::size_t>::max());
		static_assert(kMaximumSeconds <= std::numeric_limits<int>::max());

		ServerOptions Parsed;
		bool bProtocolSelected = false;
		auto Fail = [&strErrorOut](std::string const& strReason) -> bool {
			strErrorOut = strReason;

			return false;
		};

		strErrorOut.clear();

		if (nArgumentCount < 0) {
			return Fail("Command-line argument count is negative");
		}

		if ((nArgumentCount > 1) && !ppszArguments) {
			return Fail("Command-line argument vector is null");
		}

		const std::size_t unArgumentCount = static_cast<std::size_t>(nArgumentCount);
		for (std::size_t unArgumentIndex = 1; unArgumentIndex < unArgumentCount; ++unArgumentIndex) {
			if (!ppszArguments[unArgumentIndex]) {
				return Fail("Null command-line argument at index " + std::to_string(unArgumentIndex));
			}

			std::string const strFlag = ppszArguments[unArgumentIndex];
			if ((strFlag == "--help") || (strFlag == "-h") || (strFlag == "--version")) {
				Parsed.m_bHelp = true;

			} else if ((strFlag == "--tcp") || (strFlag == "--udp")) {
				SelectProtocol(strFlag, bProtocolSelected, Parsed.m_bTCP, Parsed.m_bUDP);

			} else if (strFlag == "--verbose") {
				Parsed.m_bVerbose = true;

			} else {
				if ((strFlag != "--listen") && (strFlag != "--max-clients") && (strFlag != "--idle-timeout") && (strFlag != "--run-seconds")) {
					return Fail("Unknown option: " + strFlag);
				}

				if (++unArgumentIndex >= unArgumentCount) {
					return Fail("Missing value for " + strFlag);
				}

				char const* pszValue = ppszArguments[unArgumentIndex];
				if (!pszValue) {
					return Fail("Missing value for " + strFlag);
				}

				if (strFlag == "--listen") {
					if (!ParseEndpointAddress(pszValue, true, Parsed.m_Listen)) {
						return Fail("Invalid --listen: use host:port or [IPv6]:port");
					}

				} else {
					std::uint64_t unValue = 0;
					const std::uint64_t unMinimum = (strFlag == "--max-clients") ? 1 : 0;
					const std::uint64_t unMaximum = (strFlag == "--max-clients") ? kMaximumClients : kMaximumSeconds;
					if (!ParseUnsignedNumber(pszValue, unMinimum, unMaximum, unValue)) {
						return Fail("Invalid numeric value for " + strFlag);
					}

					if (strFlag == "--max-clients") {
						Parsed.m_unMaximumClients = static_cast<std::size_t>(unValue);

					} else if (strFlag == "--idle-timeout") {
						Parsed.m_nIdleSeconds = static_cast<int>(unValue);

					} else {
						Parsed.m_nRunSeconds = static_cast<int>(unValue);
					}
				}
			}
		}

		OptionsOut = std::move(Parsed);

		return true;
	}

	bool ParseClient(int nArgumentCount, char** ppszArguments, ClientOptions& OptionsOut, std::string& strErrorOut) {
		constexpr std::uint64_t kMaximumCount = 1000000;
		constexpr std::uint64_t kMaximumTCPBytes = 16 * 1024 * 1024;
		constexpr std::uint64_t kMaximumConnections = 32;
		constexpr std::uint64_t kMinimumTimeoutMilliseconds = 50;
		constexpr std::uint64_t kMaximumMilliseconds = 600000;

		static_assert(kMaximumCount <= std::numeric_limits<unsigned>::max());
		static_assert(kMaximumConnections <= std::numeric_limits<unsigned>::max());
		static_assert(kMaximumTCPBytes <= std::numeric_limits<std::size_t>::max());
		static_assert(kMaxUdp <= std::numeric_limits<std::size_t>::max());
		static_assert(kMaximumMilliseconds <= std::numeric_limits<int>::max());

		ClientOptions Parsed;
		bool bProtocolSelected = false;
		bool bPayloadSizesSet = false;
		auto Fail = [&strErrorOut](std::string const& strReason) -> bool {
			strErrorOut = strReason;

			return false;
		};

		strErrorOut.clear();

		if (nArgumentCount < 0) {
			return Fail("Command-line argument count is negative");
		}

		if ((nArgumentCount > 1) && !ppszArguments) {
			return Fail("Command-line argument vector is null");
		}

		const std::size_t unArgumentCount = static_cast<std::size_t>(nArgumentCount);
		for (std::size_t unArgumentIndex = 1; unArgumentIndex < unArgumentCount; ++unArgumentIndex) {
			if (!ppszArguments[unArgumentIndex]) {
				return Fail("Null command-line argument at index " + std::to_string(unArgumentIndex));
			}

			std::string const strFlag = ppszArguments[unArgumentIndex];
			if ((strFlag == "--help") || (strFlag == "-h") || (strFlag == "--version")) {
				Parsed.m_bHelp = true;

			} else if ((strFlag == "--tcp") || (strFlag == "--udp")) {
				SelectProtocol(strFlag, bProtocolSelected, Parsed.m_bTCP, Parsed.m_bUDP);

			} else if (strFlag == "--half-close") {
				Parsed.m_bHalfClose = true;

			} else if (strFlag == "--allow-udp-loss") {
				Parsed.m_bAllowUDPLoss = true;

			} else {
				if ((strFlag != "--target") && (strFlag != "--message") && (strFlag != "--tcp-size") && (strFlag != "--udp-size") && (strFlag != "--count") && (strFlag != "--connections") && (strFlag != "--timeout-ms") && (strFlag != "--interval-ms")) {
					return Fail("Unknown option: " + strFlag);
				}

				if (++unArgumentIndex >= unArgumentCount) {
					return Fail("Missing value for " + strFlag);
				}

				char const* pszValue = ppszArguments[unArgumentIndex];
				if (!pszValue) {
					return Fail("Missing value for " + strFlag);
				}

				if (strFlag == "--target") {
					if (!ParseEndpointAddress(pszValue, false, Parsed.m_Target)) {
						return Fail("Invalid --target: use host:port or [IPv6]:port");
					}

				} else if (strFlag == "--message") {
					Parsed.m_bHasMessage = true;
					Parsed.m_strMessage = pszValue;

				} else {
					std::uint64_t unValue = 0;
					std::uint64_t unMinimum = 1;
					std::uint64_t unMaximum = kMaximumCount;
					if (strFlag == "--tcp-size") {
						unMaximum = kMaximumTCPBytes;
						bPayloadSizesSet = true;

					} else if (strFlag == "--udp-size") {
						unMinimum = 0;
						unMaximum = kMaxUdp;
						bPayloadSizesSet = true;

					} else if (strFlag == "--connections") {
						unMaximum = kMaximumConnections;

					} else if (strFlag == "--timeout-ms") {
						unMinimum = kMinimumTimeoutMilliseconds;
						unMaximum = kMaximumMilliseconds;

					} else if (strFlag == "--interval-ms") {
						unMinimum = 0;
						unMaximum = kMaximumMilliseconds;
					}

					if (!ParseUnsignedNumber(pszValue, unMinimum, unMaximum, unValue)) {
						return Fail("Invalid numeric value for " + strFlag);
					}

					if (strFlag == "--tcp-size") {
						Parsed.m_unTCPSize = static_cast<std::size_t>(unValue);

					} else if (strFlag == "--udp-size") {
						Parsed.m_unUDPSize = static_cast<std::size_t>(unValue);

					} else if (strFlag == "--count") {
						Parsed.m_unCount = static_cast<unsigned>(unValue);

					} else if (strFlag == "--connections") {
						Parsed.m_unConnections = static_cast<unsigned>(unValue);

					} else if (strFlag == "--timeout-ms") {
						Parsed.m_nTimeoutMilliseconds = static_cast<int>(unValue);

					} else {
						Parsed.m_nIntervalMilliseconds = static_cast<int>(unValue);
					}
				}
			}
		}

		if (Parsed.m_bHasMessage) {
			if (bPayloadSizesSet) {
				return Fail("--message cannot be combined with --tcp-size or --udp-size");
			}

			if (Parsed.m_bTCP && Parsed.m_strMessage.empty()) {
				return Fail("TCP --message must not be empty (use --udp --udp-size 0 for empty datagrams)");
			}

			const std::size_t unMaximumMessageBytes = Parsed.m_bUDP ? kMaxUdp : kMaximumTCPBytes;
			if (Parsed.m_strMessage.size() > unMaximumMessageBytes) {
				return Fail("--message is too large for the selected protocol");
			}
		}

		OptionsOut = std::move(Parsed);

		return true;
	}

	std::string ServerHelp() {
		std::ostringstream OutputStream;

		OutputStream
			<< "WireSteamTestServer 1.0.0 - standalone binary TCP/UDP echo server\n"
			<< "Usage: WireSteamTestServer.exe [options]\n"
			<< "  --listen HOST:PORT   Bind address (default 127.0.0.1:27015; [IPv6]:PORT supported)\n"
			<< "  --tcp, --udp         Select protocols; both enabled by default\n"
			<< "  --max-clients N     Concurrent TCP clients, 1..4096 (default 64)\n"
			<< "  --idle-timeout N    TCP idle seconds, 0 disables (default 60)\n"
			<< "  --run-seconds N     Stop after N seconds, 0 waits for Ctrl+C\n"
			<< "  --verbose          Log payload previews\n"
			<< "  --help             Show help\n"
			<< "No Steam account, SDK DLL, or administrator rights are required.\n";

		return OutputStream.str();
	}

	std::string ClientHelp() {
		std::ostringstream OutputStream;

		OutputStream
			<< "WireSteamTestClient 1.0.0 - standalone TCP/UDP echo integrity test\n"
			<< "Usage: WireSteamTestClient.exe [options]\n"
			<< "  --target HOST:PORT  Echo server or tunnel listener (default 127.0.0.1:27016)\n"
			<< "  --tcp, --udp        Select protocols; both enabled by default\n"
			<< "  --count N          Exchanges per connection (default 10)\n"
			<< "  --connections N    Parallel connections per selected protocol, 1..32 (default 1)\n"
			<< "  --tcp-size N       Bytes per TCP exchange, 1..16777216 (default 65536)\n"
			<< "  --udp-size N       Bytes per UDP datagram, 0..65507 (default 1024)\n"
			<< "  --message TEXT     Echo this exact text instead of generated binary payloads\n"
			<< "  --timeout-ms N     Deadline per connect/exchange, 50..600000 (default 3000)\n"
			<< "  --interval-ms N    Pause between exchanges, 0..600000 (default 100)\n"
			<< "  --half-close       Check TCP response completion and EOF after shutdown(SD_SEND)\n"
			<< "  --allow-udp-loss   Permit missing UDP echoes; corruption and socket errors fail\n"
			<< "  --help            Show help\n"
			<< "Exit codes: 0 PASS, 1 connection/data/loss failure, 2 invalid command line.\n"
			<< "No Steam account, SDK DLL, or administrator rights are required.\n";

		return OutputStream.str();
	}

	Session::Session() {
		constexpr BYTE kWinsockMajorVersion = 2;
		constexpr BYTE kWinsockMinorVersion = 2;

		// Console presentation and Ctrl+C handling are best effort for headless test processes.
		SetConsoleOutputCP(CP_UTF8);
		WSADATA Data {};
		m_bInitialized = WSAStartup(MAKEWORD(kWinsockMajorVersion, kWinsockMinorVersion), &Data) == 0;
		if (m_bInitialized) {
			SetConsoleCtrlHandler(Control, TRUE);
		}
	}

	Session::~Session() {
		if (m_bInitialized) {
			// The handler touches only process-lifetime atomic state; removal is best effort.
			SetConsoleCtrlHandler(Control, FALSE);
			// Release this session's acquired Winsock reference during terminal cleanup.
			WSACleanup();
		}
	}

	bool Stopping() {
		return g_bStopping.load();
	}

	void Stop() {
		g_bStopping.store(true);
	}

	void Log(std::string const& strMessage) {
		// Serialize and flush complete records; the stream sink must not reenter Log.
		std::lock_guard<std::mutex> OutputLock(g_OutputMutex);
		std::cout << strMessage << std::endl;
	}

	void Pause(int nMilliseconds) {
		constexpr std::chrono::milliseconds kStopCheckInterval(20);

		Clock::time_point const Deadline = Clock::now() + std::chrono::milliseconds(nMilliseconds);
		while (!Stopping()) {
			Clock::time_point const Now = Clock::now();
			if (Now >= Deadline) {
				break;
			}

			std::this_thread::sleep_for(std::min(kStopCheckInterval, std::chrono::ceil<std::chrono::milliseconds>(Deadline - Now)));
		}
	}

	bool Wait(SOCKET hSocket, short nEvents, Clock::time_point Deadline, std::string& strErrorOut) {
		constexpr std::chrono::milliseconds::rep kMinimumPollMilliseconds = 1;
		constexpr std::chrono::milliseconds::rep kMaximumPollMilliseconds = 50;

		static_assert(kMaximumPollMilliseconds <= std::numeric_limits<int>::max());

		while (!Stopping()) {
			Clock::time_point const Now = Clock::now();
			if (Now >= Deadline) {
				strErrorOut = "timeout";
				return false;
			}

			const std::chrono::milliseconds::rep nRemainingMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - Now).count();
			WSAPOLLFD Descriptor { hSocket, nEvents, 0 };
			const int nResult = WSAPoll(&Descriptor, 1, static_cast<int>(std::min(kMaximumPollMilliseconds, std::max(kMinimumPollMilliseconds, nRemainingMilliseconds))));
			if (nResult == SOCKET_ERROR) {
				const int nError = WSAGetLastError();
				strErrorOut = net::Error(nError);
				return false;
			}

			if (nResult > 0) {
				return true;
			}
		}

		strErrorOut = "interrupted";

		return false;
	}

	Bytes Payload(ClientOptions const& Options, bool bTCP, unsigned unConnection, unsigned unSequence) {
		constexpr std::uint32_t kConnectionFactor = 0x9E3779B9;
		constexpr std::uint32_t kSequenceFactor = 0x85EBCA6B;
		constexpr std::uint32_t kTCPSeed = 0x715C4021;
		constexpr std::uint32_t kUDPSeed = 0x0297C231;
		constexpr std::size_t kPatternFactor = 29;
		constexpr std::size_t kPatternDivisor = 251;

		if (Options.m_bHasMessage) {
			return Bytes(Options.m_strMessage.begin(), Options.m_strMessage.end());
		}

		Bytes vecBytes(bTCP ? Options.m_unTCPSize : Options.m_unUDPSize);
		// Test-data integer mixing intentionally wraps; only each resulting low byte is emitted.
		const std::uint32_t unSeed = ((unConnection + 1) * kConnectionFactor) ^ ((unSequence + 1) * kSequenceFactor) ^ (bTCP ? kTCPSeed : kUDPSeed);
		for (std::size_t unByteIndex = 0; unByteIndex < vecBytes.size(); ++unByteIndex) {
			vecBytes[unByteIndex] = static_cast<std::uint8_t>((unSeed >> ((unByteIndex % kU32Bytes) * kBitsPerByte)) ^ (unByteIndex * kPatternFactor + unByteIndex / kPatternDivisor));
		}

		if (vecBytes.size() >= kPayloadHeaderBytes) {
			// STEC: magic[0..3], transport[4], zero[5..7], BE32(connection+1)[8], BE32(sequence+1)[12].
			std::memcpy(vecBytes.data(), kPayloadMagic, kMagicBytes);
			vecBytes[kTransportOffset] = bTCP ? kTCPTransport : kUDPTransport;
			vecBytes[kReservedOffset] = vecBytes[kReservedOffset + 0x01] = vecBytes[kReservedOffset + 0x02] = 0x00;
			WriteU32BE(vecBytes.data() + kConnectionOffset, unConnection + 1);
			WriteU32BE(vecBytes.data() + kSequenceOffset, unSequence + 1);
		}

		return vecBytes;
	}

	bool PayloadSequence(Bytes const& vecBytes, bool bTCP, unsigned unConnection, unsigned& unSequenceOut) {
		if ((vecBytes.size() < kPayloadHeaderBytes) || std::memcmp(vecBytes.data(), kPayloadMagic, kMagicBytes) || (vecBytes[kTransportOffset] != (bTCP ? kTCPTransport : kUDPTransport)) || vecBytes[kReservedOffset] || vecBytes[kReservedOffset + 0x01] || vecBytes[kReservedOffset + 0x02]) {
			return false;
		}

		std::uint32_t unConnectionNumber = 0;
		if (!ReadU32BE(vecBytes.data() + kConnectionOffset, vecBytes.size() - kConnectionOffset, unConnectionNumber) || (unConnectionNumber != (unConnection + 1))) {
			return false;
		}

		std::uint32_t unSequenceNumber = 0;
		if (!ReadU32BE(vecBytes.data() + kSequenceOffset, vecBytes.size() - kSequenceOffset, unSequenceNumber) || !unSequenceNumber) {
			return false;
		}

		unSequenceOut = unSequenceNumber - 1;

		return true;
	}

	std::string Preview(void const* pData, std::size_t unBytes) {
		constexpr std::size_t kPreviewBytes = 48;
		constexpr unsigned char kFirstPrintableASCII = 0x20;
		constexpr unsigned char kLastPrintableASCII = 0x7E;
		constexpr int kHexadecimalByteDigits = 2;

		if ((unBytes != 0) && !pData) {
			throw std::invalid_argument("Payload preview data is null");
		}

		unsigned char const* punData = static_cast<unsigned char const*>(pData);

		std::ostringstream OutputStream;

		OutputStream << '"';
		for (std::size_t unByteIndex = 0; unByteIndex < std::min(unBytes, kPreviewBytes); ++unByteIndex) {
			const unsigned char unByte = punData[unByteIndex];
			if ((unByte >= kFirstPrintableASCII) && (unByte <= kLastPrintableASCII) && (unByte != '"') && (unByte != '\\')) {
				OutputStream << static_cast<char>(unByte);

			} else {
				OutputStream << "\\x" << std::hex << std::setw(kHexadecimalByteDigits) << std::setfill('0') << static_cast<unsigned>(unByte) << std::dec;
			}
		}

		OutputStream << '"';

		if (unBytes > kPreviewBytes) {
			OutputStream << " (+" << unBytes - kPreviewBytes << " bytes)";
		}

		return OutputStream.str();
	}
} // namespace st::testapp
