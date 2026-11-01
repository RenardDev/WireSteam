#include "speed_common.hpp"

// General
#include <windows.h>
#include <bcrypt.h>

// C++
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>

// STL
#include <algorithm>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace ws::speed {
	namespace {
		constexpr std::size_t kU32Bytes = sizeof(std::uint32_t);
		constexpr std::size_t kU64Bytes = sizeof(std::uint64_t);
		constexpr std::size_t kBitsPerByte = 8;
		constexpr std::size_t kMaximumControlTextBytes = 512;
		constexpr double kMaximumStatisticsSeconds = 3600;

		static_assert(kU32Bytes == 0x04);
		static_assert(kU64Bytes == 0x08);
		static_assert(sizeof(double) == kU64Bytes);

		constinit std::atomic_bool g_bStopping = false;
		constinit std::mutex g_OutputMutex;

		BOOL WINAPI HandleControl(DWORD unEvent) noexcept {
			if ((unEvent == CTRL_C_EVENT) || (unEvent == CTRL_BREAK_EVENT) || (unEvent == CTRL_CLOSE_EVENT) || (unEvent == CTRL_LOGOFF_EVENT) || (unEvent == CTRL_SHUTDOWN_EVENT)) {
				g_bStopping = true;
				return TRUE;
			}

			return FALSE;
		}

		double GetElapsedSeconds(Clock::duration Elapsed) {
			return std::chrono::duration<double>(Elapsed).count();
		}

		void RequireActive() {
			Require(!Stopping(), "Interrupted");
		}

		// Callers establish fixed writable storage before these private byte writers.
		void WriteU32BE(std::uint8_t* punData, std::uint32_t unValue) {
			for (std::size_t unByteIndex = 0; unByteIndex < kU32Bytes; ++unByteIndex) {
				punData[unByteIndex] = static_cast<std::uint8_t>(unValue >> ((kU32Bytes - 1 - unByteIndex) * kBitsPerByte));
			}
		}

		void WriteU64BE(std::uint8_t* punData, std::uint64_t unValue) {
			for (std::size_t unByteIndex = 0; unByteIndex < kU64Bytes; ++unByteIndex) {
				punData[unByteIndex] = static_cast<std::uint8_t>(unValue >> ((kU64Bytes - 1 - unByteIndex) * kBitsPerByte));
			}
		}

		std::uint32_t ReadU32BE(std::uint8_t const* punData, std::size_t unDataBytes) {
			if (!punData || (unDataBytes < kU32Bytes)) {
				throw std::runtime_error("Truncated speed-test control frame");
			}

			std::uint32_t unValue = 0;
			for (std::size_t unByteIndex = 0; unByteIndex < kU32Bytes; ++unByteIndex) {
				unValue = (unValue << kBitsPerByte) | punData[unByteIndex];
			}

			return unValue;
		}

		std::uint64_t ReadU64BE(std::uint8_t const* punData, std::size_t unDataBytes) {
			if (!punData || (unDataBytes < kU64Bytes)) {
				throw std::runtime_error("Truncated speed-test control frame");
			}

			std::uint64_t unValue = 0;
			for (std::size_t unByteIndex = 0; unByteIndex < kU64Bytes; ++unByteIndex) {
				unValue = (unValue << kBitsPerByte) | punData[unByteIndex];
			}

			return unValue;
		}

		class Writer {
		public:
			void WriteU32(std::uint32_t unValue) {
				const std::size_t unOffset = m_vecBytes.size();
				Require(kU32Bytes <= (m_vecBytes.max_size() - unOffset), "Speed-test control frame exceeds storage capacity");
				m_vecBytes.resize(unOffset + kU32Bytes);
				WriteU32BE(m_vecBytes.data() + unOffset, unValue);
			}

			void WriteU64(std::uint64_t unValue) {
				const std::size_t unOffset = m_vecBytes.size();
				Require(kU64Bytes <= (m_vecBytes.max_size() - unOffset), "Speed-test control frame exceeds storage capacity");
				m_vecBytes.resize(unOffset + kU64Bytes);
				WriteU64BE(m_vecBytes.data() + unOffset, unValue);
			}

			void WriteReal(double dblValue) {
				std::uint64_t unBits = 0;
				std::memcpy(&unBits, &dblValue, kU64Bytes);
				WriteU64(unBits);
			}

			void WriteBytes(void const* pData, std::size_t unDataBytes) {
				std::uint8_t const* const punData = static_cast<std::uint8_t const*>(pData);
				m_vecBytes.insert(m_vecBytes.end(), punData, punData + unDataBytes);
			}

			void WriteStatistics(Stats const& Statistics) {
				WriteU64(Statistics.m_unBytes);
				WriteU64(Statistics.m_unPackets);
				WriteU64(Statistics.m_unDuplicates);
				WriteU64(Statistics.m_unReordered);
				WriteU64(Statistics.m_unInvalid);
				WriteU64(Statistics.m_unLocalDrops);
				WriteReal(Statistics.m_dblSeconds);
				WriteReal(Statistics.m_dblJitterMilliseconds);
				WriteReal(Statistics.m_dblPeakMbps);
			}

			std::vector<std::uint8_t> m_vecBytes;
		};

		class Reader {
		public:
			explicit Reader(std::vector<std::uint8_t> const& vecData) :
				m_vecBytes(vecData)
			{
				m_unOffset = 0;
			}

			std::uint8_t const* ReadBytes(std::size_t unDataBytes) {
				Require(unDataBytes <= (m_vecBytes.size() - m_unOffset), "Truncated speed-test control frame");
				std::uint8_t const* const punData = m_vecBytes.data() + m_unOffset;
				m_unOffset += unDataBytes;

				return punData;
			}

			std::uint32_t ReadU32() {
				return ReadU32BE(ReadBytes(kU32Bytes), kU32Bytes);
			}

			std::uint64_t ReadU64() {
				return ReadU64BE(ReadBytes(kU64Bytes), kU64Bytes);
			}

			double ReadReal() {
				const std::uint64_t unBits = ReadU64();
				double dblValue = 0;
				std::memcpy(&dblValue, &unBits, kU64Bytes);
				Require(std::isfinite(dblValue) && (dblValue >= 0), "Invalid numeric control field");

				return dblValue;
			}

			Stats ReadStatistics() {
				Stats Statistics;
				Statistics.m_unBytes = ReadU64();
				Statistics.m_unPackets = ReadU64();
				Statistics.m_unDuplicates = ReadU64();
				Statistics.m_unReordered = ReadU64();
				Statistics.m_unInvalid = ReadU64();
				Statistics.m_unLocalDrops = ReadU64();
				Statistics.m_dblSeconds = ReadReal();
				Statistics.m_dblJitterMilliseconds = ReadReal();
				Statistics.m_dblPeakMbps = ReadReal();
				Require(Statistics.m_dblSeconds <= kMaximumStatisticsSeconds, "Invalid measurement duration");

				return Statistics;
			}

			std::vector<std::uint8_t> const& m_vecBytes;
			std::size_t m_unOffset;
		};

		std::vector<std::uint8_t> EncodeMessage(Message const& MessageValue) {
			Require(MessageValue.m_strText.size() <= kMaximumControlTextBytes, "Control text exceeds 512 bytes");
			Writer Output;
			Output.WriteU32(static_cast<std::uint32_t>(MessageValue.m_Kind));
			Output.WriteU32(static_cast<std::uint32_t>(MessageValue.m_Specification.m_Protocol));
			Output.WriteU32(static_cast<std::uint32_t>(MessageValue.m_Specification.m_Direction));
			Output.WriteU32(MessageValue.m_Specification.m_unSeconds);
			Output.WriteU32(MessageValue.m_Specification.m_unStreams);
			Output.WriteU32(MessageValue.m_Specification.m_unBlockSize);
			Output.WriteU32(MessageValue.m_Specification.m_unUDPSize);
			Output.WriteReal(MessageValue.m_Specification.m_dblUDPMbps);
			Output.WriteBytes(MessageValue.m_arrToken.data(), MessageValue.m_arrToken.size());
			Output.WriteU32(MessageValue.m_unPort);
			Output.WriteU32(MessageValue.m_unPhase);
			Output.WriteStatistics(MessageValue.m_Sender);
			Output.WriteStatistics(MessageValue.m_Receiver);
			Output.WriteU32(static_cast<std::uint32_t>(MessageValue.m_strText.size()));
			Output.WriteBytes(MessageValue.m_strText.data(), MessageValue.m_strText.size());

			return Output.m_vecBytes;
		}

		Message DecodeMessage(std::vector<std::uint8_t> const& vecBytes) {
			Reader Input(vecBytes);
			Message Result;
			Result.m_Kind = static_cast<Kind>(Input.ReadU32());
			Require((Result.m_Kind >= Kind::Request) && (Result.m_Kind <= Kind::Error), "Unknown speed-test control type");
			Result.m_Specification.m_Protocol = static_cast<Protocol>(Input.ReadU32());
			Result.m_Specification.m_Direction = static_cast<Direction>(Input.ReadU32());
			Result.m_Specification.m_unSeconds = Input.ReadU32();
			Result.m_Specification.m_unStreams = Input.ReadU32();
			Result.m_Specification.m_unBlockSize = Input.ReadU32();
			Result.m_Specification.m_unUDPSize = Input.ReadU32();
			Result.m_Specification.m_dblUDPMbps = Input.ReadReal();
			std::memcpy(Result.m_arrToken.data(), Input.ReadBytes(Result.m_arrToken.size()), Result.m_arrToken.size());
			Result.m_unPort = Input.ReadU32();
			Result.m_unPhase = Input.ReadU32();
			Result.m_Sender = Input.ReadStatistics();
			Result.m_Receiver = Input.ReadStatistics();
			const std::uint32_t unTextBytes = Input.ReadU32();
			Require(unTextBytes <= kMaximumControlTextBytes, "Oversized control text");
			std::uint8_t const* const punText = Input.ReadBytes(unTextBytes);
			Result.m_strText.assign(reinterpret_cast<char const*>(punText), unTextBytes);
			Require(Input.m_unOffset == vecBytes.size(), "Trailing control data");

			return Result;
		}

		void SendAllBytes(SOCKET hSocket, void const* pData, std::size_t unDataBytes, Clock::time_point Deadline) {
			constexpr std::size_t kTransferChunkBytes = 65536;

			char const* pchData = static_cast<char const*>(pData);
			while (unDataBytes) {
				RequireActive();
				const int nSentBytes = send(hSocket, pchData, static_cast<int>(std::min(unDataBytes, kTransferChunkBytes)), 0);
				if (nSentBytes == SOCKET_ERROR) {
					const int nErrorCode = WSAGetLastError();
					Require(net::WouldBlock(nErrorCode), "TCP send failed: " + net::Error(nErrorCode));
					Require(Wait(hSocket, POLLWRNORM, Deadline), "TCP send timed out");

				} else {
					Require(nSentBytes > 0, "TCP send returned zero");
					pchData += nSentBytes;
					unDataBytes -= static_cast<std::size_t>(nSentBytes);
				}

				Require(Clock::now() <= Deadline, "TCP send timed out");
			}
		}

		void ReceiveAllBytes(SOCKET hSocket, void* pData, std::size_t unDataBytes, Clock::time_point Deadline) {
			constexpr std::size_t kTransferChunkBytes = 65536;

			char* pchData = static_cast<char*>(pData);
			while (unDataBytes) {
				RequireActive();
				const int nReceivedBytes = recv(hSocket, pchData, static_cast<int>(std::min(unDataBytes, kTransferChunkBytes)), 0);
				if (nReceivedBytes == SOCKET_ERROR) {
					const int nErrorCode = WSAGetLastError();
					Require(net::WouldBlock(nErrorCode), "TCP receive failed: " + net::Error(nErrorCode));
					Require(Wait(hSocket, POLLRDNORM, Deadline), "TCP receive timed out");

				} else {
					Require(nReceivedBytes > 0, "Control/data connection closed unexpectedly");
					pchData += nReceivedBytes;
					unDataBytes -= static_cast<std::size_t>(nReceivedBytes);
				}

				Require(Clock::now() <= Deadline, "TCP receive timed out");
			}
		}

		class FineWait {
		public:
			FineWait() {
				// High-resolution timer setup is optional; Pause retains its sleep fallback.
				m_hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
			}

			~FineWait() {
				if (m_hTimer) {
					CloseHandle(m_hTimer);
				}
			}

			FineWait(FineWait const&) = delete;
			FineWait& operator=(FineWait const&) = delete;

			void Pause(double dblSeconds) {
				constexpr double kMinimumPauseSeconds = 0.0001;
				constexpr double kMaximumPauseSeconds = 0.01;
				constexpr double kTimerTicksPerSecond = 1e7;
				constexpr DWORD kTimerWaitMilliseconds = 50;
				constexpr int kFallbackWaitMilliseconds = 1;

				dblSeconds = std::clamp(dblSeconds, kMinimumPauseSeconds, kMaximumPauseSeconds);
				LARGE_INTEGER DueTime {};
				DueTime.QuadPart = -static_cast<LONGLONG>(dblSeconds * kTimerTicksPerSecond);

				if (m_hTimer && SetWaitableTimerEx(m_hTimer, &DueTime, 0, nullptr, nullptr, nullptr, 0)) {
					// Pacing is best effort; the transfer loop independently checks its deadline.
					WaitForSingleObject(m_hTimer, kTimerWaitMilliseconds);

				} else {
					std::this_thread::sleep_for(std::chrono::milliseconds(kFallbackWaitMilliseconds));
				}
			}

		private:
			HANDLE m_hTimer;
		};

		std::vector<std::uint8_t> CreatePayload(std::size_t unSize) {
			constexpr std::uint64_t kPayloadSeed = 0xA17CE49B35F208D1;
			constexpr unsigned kFirstLeftShift = 13;
			constexpr unsigned kRightShift = 7;
			constexpr unsigned kSecondLeftShift = 17;

			std::vector<std::uint8_t> vecResult(unSize);
			std::uint64_t unState = kPayloadSeed;
			for (auto& unByte : vecResult) {
				unState ^= unState << kFirstLeftShift;
				unState ^= unState >> kRightShift;
				unState ^= unState << kSecondLeftShift;
				unByte = static_cast<std::uint8_t>(unState);
			}

			return vecResult;
		}

		// Callers provide at least kUdpHeader writable bytes for this fixed wire header.
		void WriteDatagramHeader(std::uint8_t* punData, Token const& arrToken, std::uint32_t unPhase, std::uint64_t unSequence, std::uint64_t unMicroseconds, std::uint32_t unPayloadBytes) {
			constexpr std::size_t kMagicBytes = 0x04;
			constexpr std::size_t kVersionOffset = 0x04;
			constexpr std::size_t kTokenOffset = 0x08;
			constexpr std::size_t kPhaseOffset = 0x18;
			constexpr std::size_t kSequenceOffset = 0x1C;
			constexpr std::size_t kTimestampOffset = 0x24;
			constexpr std::size_t kPayloadSizeOffset = 0x2C;
			constexpr std::uint32_t kDatagramVersion = 1;

			std::memcpy(punData, "WSSU", kMagicBytes);
			WriteU32BE(punData + kVersionOffset, kDatagramVersion);
			std::memcpy(punData + kTokenOffset, arrToken.data(), arrToken.size());
			WriteU32BE(punData + kPhaseOffset, unPhase);
			WriteU64BE(punData + kSequenceOffset, unSequence);
			WriteU64BE(punData + kTimestampOffset, unMicroseconds);
			WriteU32BE(punData + kPayloadSizeOffset, unPayloadBytes);
		}

		bool MatchesDatagram(std::uint8_t const* punData, std::size_t unDataBytes, Token const& arrToken, std::uint32_t unPhase) {
			constexpr std::size_t kMagicBytes = 0x04;
			constexpr std::size_t kVersionOffset = 0x04;
			constexpr std::size_t kTokenOffset = 0x08;
			constexpr std::size_t kPhaseOffset = 0x18;
			constexpr std::uint32_t kDatagramVersion = 1;

			return (unDataBytes >= kUdpHeader) && !std::memcmp(punData, "WSSU", kMagicBytes) && (ReadU32BE(punData + kVersionOffset, unDataBytes - kVersionOffset) == kDatagramVersion) && !std::memcmp(punData + kTokenOffset, arrToken.data(), arrToken.size()) && (ReadU32BE(punData + kPhaseOffset, unDataBytes - kPhaseOffset) == unPhase);
		}

		void SampleInterval(Stats& Statistics, std::uint64_t unBytes, std::uint64_t& unPreviousBytes, Clock::time_point& PreviousSample, std::string const& strLabel, bool bIntervals) {
			constexpr double kSampleIntervalSeconds = 1;
			constexpr double kBitsPerMegabit = 1e6;
			constexpr int kRatePrecision = 2;

			Clock::time_point const Now = Clock::now();
			const double dblElapsedSeconds = GetElapsedSeconds(Now - PreviousSample);
			if (dblElapsedSeconds < kSampleIntervalSeconds) {
				return;
			}

			const double dblRate = static_cast<double>(unBytes - unPreviousBytes) * kBitsPerByte / dblElapsedSeconds / kBitsPerMegabit;
			Statistics.m_dblPeakMbps = std::max(Statistics.m_dblPeakMbps, dblRate);

			if (bIntervals) {
				std::ostringstream OutputStream;

				OutputStream << strLabel << " interval " << std::fixed << std::setprecision(kRatePrecision) << dblRate << " Mbit/s";
				Log(OutputStream.str());
			}

			unPreviousBytes = unBytes;
			PreviousSample = Now;
		}

		std::string FormatStatisticsJSON(Stats const& Statistics, bool bReceiver) {
			constexpr int kStatisticsPrecision = 12;

			std::ostringstream OutputStream;

			OutputStream << std::setprecision(kStatisticsPrecision);
			OutputStream << "{\"bytes\":" << Statistics.m_unBytes << ",\"seconds\":" << Statistics.m_dblSeconds << ",\"mbps\":" << Mbps(Statistics) << ",\"packets\":" << Statistics.m_unPackets << ",\"peakMbps\":" << Statistics.m_dblPeakMbps;

			if (bReceiver) {
				OutputStream << ",\"duplicates\":" << Statistics.m_unDuplicates << ",\"reordered\":" << Statistics.m_unReordered << ",\"invalid\":" << Statistics.m_unInvalid << ",\"jitterMs\":" << Statistics.m_dblJitterMilliseconds;

			} else {
				OutputStream << ",\"localDrops\":" << Statistics.m_unLocalDrops;
			}

			OutputStream << '}';

			return OutputStream.str();
		}
	} // namespace

	Session::Session() {
		constexpr BYTE kWinsockMajorVersion = 2;
		constexpr BYTE kWinsockMinorVersion = 2;

		WSADATA Data {};
		const int nStartupResult = WSAStartup(MAKEWORD(kWinsockMajorVersion, kWinsockMinorVersion), &Data);
		if (nStartupResult != 0) {
			throw std::runtime_error("WSAStartup failed");
		}

		g_bStopping = false;

		if (!SetConsoleCtrlHandler(HandleControl, TRUE)) {
			const DWORD unErrorCode = GetLastError();
			// Constructor failure must release this invocation's successful Winsock reference.
			WSACleanup();
			throw std::runtime_error("Cannot register console control handler (Windows error " + std::to_string(unErrorCode) + ")");
		}
	}

	Session::~Session() {
		// Best effort at shutdown; the static callback never borrows Session storage.
		SetConsoleCtrlHandler(HandleControl, FALSE);
		WSACleanup();
	}

	bool Stopping() {
		return g_bStopping.load();
	}

	void RequestStop() {
		g_bStopping = true;
	}

	void Log(std::string const& strText) {
		// Keep the stream write under this lock so each flushed log record stays intact.
		std::lock_guard<std::mutex> OutputLock(g_OutputMutex);
		std::cout << strText << std::endl;
	}

	void Require(bool bCondition, std::string const& strText) {
		if (!bCondition) {
			throw std::runtime_error(strText);
		}
	}

	std::uint32_t Number(std::string const& strText, std::uint32_t unMinimum, std::uint32_t unMaximum) {
		std::uint32_t unNumber = 0;
		std::from_chars_result const Parsed = std::from_chars(strText.data(), strText.data() + strText.size(), unNumber);
		Require(!strText.empty() && (Parsed.ec == std::errc {}) && (Parsed.ptr == (strText.data() + strText.size())) && (unNumber >= unMinimum) && (unNumber <= unMaximum), "Expected an integer from " + std::to_string(unMinimum) + " to " + std::to_string(unMaximum));

		return unNumber;
	}

	double Decimal(std::string const& strText, double dblMinimum, double dblMaximum) {
		double dblNumber = 0;
		std::from_chars_result const Parsed = std::from_chars(strText.data(), strText.data() + strText.size(), dblNumber);
		Require(!strText.empty() && (Parsed.ec == std::errc {}) && (Parsed.ptr == (strText.data() + strText.size())) && std::isfinite(dblNumber) && (dblNumber >= dblMinimum) && (dblNumber <= dblMaximum), "Numeric argument is outside the supported range");

		return dblNumber;
	}

	st::Endpoint ParseEndpoint(std::string const& strText, bool bAllowZero) {
		constexpr std::size_t kMinimumBracketedHostCharacters = 3;
		constexpr std::size_t kMaximumHostCharacters = 255;
		constexpr std::size_t kBracketCharacters = 2;
		constexpr std::uint32_t kMaximumPort = 65535;

		const std::size_t unSeparator = strText.rfind(':');
		Require((unSeparator != std::string::npos) && (unSeparator != 0), "Use HOST:PORT or [IPv6]:PORT");
		std::string strHost = strText.substr(0, unSeparator);
		if (strHost.front() == '[') {
			Require((strHost.size() >= kMinimumBracketedHostCharacters) && (strHost.back() == ']'), "Invalid bracketed IPv6 address");
			strHost = strHost.substr(1, strHost.size() - kBracketCharacters);

		} else {
			Require(strHost.find(':') == std::string::npos, "IPv6 addresses require [brackets]");
		}

		Require(!strHost.empty() && (strHost.size() <= kMaximumHostCharacters), "Invalid host name");

		return { strHost, static_cast<std::uint16_t>(Number(strText.substr(unSeparator + 1), bAllowZero ? 0 : 1, kMaximumPort)) };
	}

	void Validate(Spec const& Specification) {
		constexpr std::uint32_t kMinimumSeconds = 1;
		constexpr std::uint32_t kMinimumStreams = 1;
		constexpr std::uint32_t kMaximumStreams = 32;
		constexpr std::uint32_t kMinimumBlockBytes = 1;
		constexpr std::uint32_t kMaximumBlockBytes = 4 * 1024 * 1024;
		constexpr std::uint32_t kMinimumUDPPayloadBytes = 64;
		constexpr double kMinimumUDPMegabitsPerSecond = 0.001;
		constexpr double kMaximumUDPMegabitsPerSecond = 100000;
		constexpr double kBitsPerMegabit = 1e6;

		Require((Specification.m_Protocol == Protocol::Tcp) || (Specification.m_Protocol == Protocol::Udp), "Invalid test protocol");
		Require((Specification.m_Direction == Direction::Upload) || (Specification.m_Direction == Direction::Download), "Invalid test direction");
		Require((Specification.m_unSeconds >= kMinimumSeconds) && (Specification.m_unSeconds <= kMaxSeconds), "Test duration must be 1-300 seconds");
		Require((Specification.m_unStreams >= kMinimumStreams) && (Specification.m_unStreams <= kMaximumStreams), "TCP stream count must be 1-32");
		Require((Specification.m_unBlockSize >= kMinimumBlockBytes) && (Specification.m_unBlockSize <= kMaximumBlockBytes), "TCP block size must be 1-4194304 bytes");
		Require((Specification.m_unUDPSize >= kMinimumUDPPayloadBytes) && (Specification.m_unUDPSize <= (st::kMaxUdp - kUdpHeader)), "UDP payload must be 64-65459 bytes");
		Require(std::isfinite(Specification.m_dblUDPMbps) && (Specification.m_dblUDPMbps >= kMinimumUDPMegabitsPerSecond) && (Specification.m_dblUDPMbps <= kMaximumUDPMegabitsPerSecond), "UDP rate must be 0.001-100000 Mbit/s");

		if (Specification.m_Protocol == Protocol::Udp) {
			Require(Specification.m_unStreams == kMinimumStreams, "UDP tests use one flow");
			const double dblPackets = std::ceil(Specification.m_dblUDPMbps * kBitsPerMegabit / kBitsPerByte / Specification.m_unUDPSize * Specification.m_unSeconds);
			Require(dblPackets <= static_cast<double>(kMaxUdpPackets), "UDP test exceeds the bounded sequence tracking capacity; reduce rate/duration or increase payload size");
		}
	}

	Token NewToken() {
		Token arrToken {};
		Require(BCryptGenRandom(nullptr, arrToken.data(), static_cast<ULONG>(arrToken.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0, "System random generator failed");

		return arrToken;
	}

	std::string Host(net::Address const& AddressValue) {
		return net::Describe(AddressValue).m_strHost;
	}

	net::Address Remote(SOCKET hSocket) {
		net::Address Result;
		Require(getpeername(hSocket, Result.ptr(), &Result.m_nSize) == 0, "Cannot query remote address");

		return Result;
	}

	net::Address Local(SOCKET hSocket) {
		net::Address Result;
		Require(getsockname(hSocket, Result.ptr(), &Result.m_nSize) == 0, "Cannot query local address");

		return Result;
	}

	void Tune(SOCKET hSocket, bool bTCP) {
		constexpr int kBufferBytes = 1024 * 1024;

		Require(net::Nonblocking(hSocket), "Cannot configure nonblocking socket");

		if (bTCP) {
			net::ConfigureTcp(hSocket);

		} else {
			net::ConfigureUdp(hSocket);
		}

		// Buffer-size hints are optional; Windows may retain different effective limits.
		setsockopt(hSocket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char const*>(&kBufferBytes), sizeof(kBufferBytes));
		setsockopt(hSocket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&kBufferBytes), sizeof(kBufferBytes));
	}

	bool Wait(SOCKET hSocket, short nEvents, Clock::time_point Deadline) {
		constexpr std::int64_t kMinimumPollMilliseconds = 1;
		constexpr std::int64_t kMaximumPollMilliseconds = 50;

		while (!Stopping() && (Clock::now() < Deadline)) {
			WSAPOLLFD PollEntry { hSocket, nEvents, 0 };
			const std::int64_t nRemainingMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - Clock::now()).count();
			const int nReady = WSAPoll(&PollEntry, 1, static_cast<int>(std::clamp(nRemainingMilliseconds, kMinimumPollMilliseconds, kMaximumPollMilliseconds)));
			if (nReady == SOCKET_ERROR) {
				const int nErrorCode = WSAGetLastError();
				throw std::runtime_error("Socket poll failed: " + net::Error(nErrorCode));
			}

			if (nReady && PollEntry.revents) {
				return true;
			}
		}

		return false;
	}

	net::Socket Connect(st::Endpoint const& EndpointValue, int nTimeoutMilliseconds) {
		std::string strError;
		net::Address AddressValue;
		Require(net::Resolve(EndpointValue, SOCK_STREAM, AddressValue, strError), strError);
		net::Socket Socket(::socket(AddressValue.family(), SOCK_STREAM, IPPROTO_TCP));
		Require(static_cast<bool>(Socket), "Cannot create TCP socket");
		Tune(Socket.get(), true);

		if (connect(Socket.get(), AddressValue.ptr(), AddressValue.m_nSize) == SOCKET_ERROR) {
			const int nErrorCode = WSAGetLastError();
			Require(net::WouldBlock(nErrorCode), "Cannot connect to " + EndpointValue.ToString() + ": " + net::Error(nErrorCode));
			Require(Wait(Socket.get(), POLLWRNORM, Clock::now() + std::chrono::milliseconds(nTimeoutMilliseconds)), "TCP connection timed out");
			int nPendingError = 0;
			int nOptionBytes = sizeof(nPendingError);
			if (getsockopt(Socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&nPendingError), &nOptionBytes) != 0) {
				const int nOptionError = WSAGetLastError();
				throw std::runtime_error("Cannot connect to " + EndpointValue.ToString() + ": " + net::Error(nOptionError));
			}

			Require(nPendingError == 0, "Cannot connect to " + EndpointValue.ToString() + ": " + net::Error(nPendingError));
		}

		return Socket;
	}

	net::Socket Listen(st::Endpoint const& EndpointValue) {
		std::string strError;
		net::Socket Socket = net::Bind(EndpointValue, SOCK_STREAM, strError);
		Require(static_cast<bool>(Socket), strError);
		Tune(Socket.get(), true);

		if (listen(Socket.get(), SOMAXCONN) != 0) {
			const int nErrorCode = WSAGetLastError();
			throw std::runtime_error("TCP listen failed: " + net::Error(nErrorCode));
		}

		return Socket;
	}

	net::Socket Accept(SOCKET hListener, Clock::time_point Deadline, net::Address* pRemoteOut) {
		while (!Stopping() && (Clock::now() < Deadline)) {
			net::Address AddressValue;
			net::Socket Socket(accept(hListener, AddressValue.ptr(), &AddressValue.m_nSize));
			if (Socket) {
				Tune(Socket.get(), true);

				if (pRemoteOut) {
					*pRemoteOut = AddressValue;
				}

				return Socket;
			}

			const int nErrorCode = WSAGetLastError();
			Require(net::WouldBlock(nErrorCode), "TCP accept failed: " + net::Error(nErrorCode));

			if (!Wait(hListener, POLLRDNORM, Deadline)) {
				break;
			}
		}

		throw std::runtime_error(Stopping() ? "Interrupted" : "TCP accept timed out");
	}

	void SendMessage(SOCKET hSocket, Message const& MessageValue, int nTimeoutMilliseconds) {
		constexpr std::size_t kPrefixBytes = 0x08;
		constexpr std::size_t kMagicBytes = 0x04;
		constexpr std::size_t kPayloadSizeOffset = 0x04;
		constexpr std::size_t kMaximumControlBytes = 2048;

		std::vector<std::uint8_t> const vecPayload = EncodeMessage(MessageValue);
		Require(vecPayload.size() <= kMaximumControlBytes, "Speed-test control length exceeds bounds");
		std::array<std::uint8_t, kPrefixBytes> arrPrefix {};
		std::memcpy(arrPrefix.data(), "WSS1", kMagicBytes);
		WriteU32BE(arrPrefix.data() + kPayloadSizeOffset, static_cast<std::uint32_t>(vecPayload.size()));
		Clock::time_point const Deadline = Clock::now() + std::chrono::milliseconds(nTimeoutMilliseconds);
		SendAllBytes(hSocket, arrPrefix.data(), arrPrefix.size(), Deadline);
		SendAllBytes(hSocket, vecPayload.data(), vecPayload.size(), Deadline);
	}

	Message ReceiveMessage(SOCKET hSocket, int nTimeoutMilliseconds) {
		constexpr std::size_t kPrefixBytes = 0x08;
		constexpr std::size_t kMagicBytes = 0x04;
		constexpr std::size_t kPayloadSizeOffset = 0x04;
		constexpr std::uint32_t kMinimumControlBytes = 64;
		constexpr std::uint32_t kMaximumControlBytes = 2048;

		Clock::time_point const Deadline = Clock::now() + std::chrono::milliseconds(nTimeoutMilliseconds);
		std::array<std::uint8_t, kPrefixBytes> arrPrefix {};
		ReceiveAllBytes(hSocket, arrPrefix.data(), arrPrefix.size(), Deadline);
		Require(!std::memcmp(arrPrefix.data(), "WSS1", kMagicBytes), "Invalid speed-test control magic");
		const std::uint32_t unPayloadBytes = ReadU32BE(arrPrefix.data() + kPayloadSizeOffset, arrPrefix.size() - kPayloadSizeOffset);
		Require((unPayloadBytes <= kMaximumControlBytes) && (unPayloadBytes >= kMinimumControlBytes), "Speed-test control length exceeds bounds");
		std::vector<std::uint8_t> vecData(unPayloadBytes);
		ReceiveAllBytes(hSocket, vecData.data(), unPayloadBytes, Deadline);

		return DecodeMessage(vecData);
	}

	void Expect(Message const& MessageValue, Kind KindValue) {
		if (MessageValue.m_Kind == Kind::Error) {
			throw std::runtime_error("Server: " + MessageValue.m_strText);
		}

		Require(MessageValue.m_Kind == KindValue, "Unexpected speed-test control message");
	}

	void SendDataHello(SOCKET hSocket, Token const& arrToken, std::uint32_t unPhase, std::uint32_t unStream, int nTimeoutMilliseconds) {
		constexpr int kMaximumTimeoutMilliseconds = 10000;
		constexpr std::size_t kHelloBytes = 0x1C;
		constexpr std::size_t kMagicBytes = 0x04;
		constexpr std::size_t kTokenOffset = 0x04;
		constexpr std::size_t kPhaseOffset = 0x14;
		constexpr std::size_t kStreamOffset = 0x18;

		Require((nTimeoutMilliseconds > 0) && (nTimeoutMilliseconds <= kMaximumTimeoutMilliseconds), "Invalid data stream handshake deadline");
		std::array<std::uint8_t, kHelloBytes> arrHello {};
		std::memcpy(arrHello.data(), "WSSD", kMagicBytes);
		std::memcpy(arrHello.data() + kTokenOffset, arrToken.data(), arrToken.size());
		WriteU32BE(arrHello.data() + kPhaseOffset, unPhase);
		WriteU32BE(arrHello.data() + kStreamOffset, unStream);
		SendAllBytes(hSocket, arrHello.data(), arrHello.size(), Clock::now() + std::chrono::milliseconds(nTimeoutMilliseconds));
	}

	std::uint32_t ReceiveDataHello(SOCKET hSocket, Token const& arrToken, std::uint32_t unPhase, int nTimeoutMilliseconds) {
		constexpr int kMaximumTimeoutMilliseconds = 10000;
		constexpr std::size_t kHelloBytes = 0x1C;
		constexpr std::size_t kMagicBytes = 0x04;
		constexpr std::size_t kTokenOffset = 0x04;
		constexpr std::size_t kPhaseOffset = 0x14;
		constexpr std::size_t kStreamOffset = 0x18;

		Require((nTimeoutMilliseconds > 0) && (nTimeoutMilliseconds <= kMaximumTimeoutMilliseconds), "Invalid data stream handshake deadline");
		std::array<std::uint8_t, kHelloBytes> arrHello {};
		ReceiveAllBytes(hSocket, arrHello.data(), arrHello.size(), Clock::now() + std::chrono::milliseconds(nTimeoutMilliseconds));
		Require(!std::memcmp(arrHello.data(), "WSSD", kMagicBytes) && !std::memcmp(arrHello.data() + kTokenOffset, arrToken.data(), arrToken.size()) && (ReadU32BE(arrHello.data() + kPhaseOffset, arrHello.size() - kPhaseOffset) == unPhase), "Invalid data stream session token/phase");

		return ReadU32BE(arrHello.data() + kStreamOffset, arrHello.size() - kStreamOffset);
	}

	void SendUdpProbe(SOCKET hSocket, Token const& arrToken, std::uint32_t unPhase) {
		constexpr std::uint64_t kProbeSequence = std::numeric_limits<std::uint64_t>::max();
		constexpr int kProbeTimeoutSeconds = 5;
		constexpr int kProbeAttempts = 3;

		std::array<std::uint8_t, kUdpHeader> arrData {};
		WriteDatagramHeader(arrData.data(), arrToken, unPhase, kProbeSequence, 0, 0);
		Clock::time_point const Deadline = Clock::now() + std::chrono::seconds(kProbeTimeoutSeconds);
		// A would-block send rewinds this signed retry state before the loop increment.
		for (int nAttemptIndex = 0; nAttemptIndex < kProbeAttempts; ++nAttemptIndex) {
			RequireActive();
			Require(Clock::now() < Deadline, "UDP probe send timed out");
			const int nSentBytes = send(hSocket, reinterpret_cast<char const*>(arrData.data()), static_cast<int>(arrData.size()), 0);
			if (nSentBytes == SOCKET_ERROR) {
				const int nErrorCode = WSAGetLastError();
				Require(net::WouldBlock(nErrorCode), "UDP probe send failed: " + net::Error(nErrorCode));
				Require(Wait(hSocket, POLLWRNORM, Deadline), "UDP probe send timed out");
				--nAttemptIndex;
			}
		}
	}

	net::Address ReceiveUdpProbe(SOCKET hSocket, Token const& arrToken, std::uint32_t unPhase, net::Address const& ControlPeer) {
		constexpr int kProbeTimeoutSeconds = 10;
		constexpr std::size_t kSequenceOffset = 0x1C;
		constexpr std::size_t kPayloadSizeOffset = 0x2C;
		constexpr std::uint64_t kProbeSequence = std::numeric_limits<std::uint64_t>::max();

		Clock::time_point const Deadline = Clock::now() + std::chrono::seconds(kProbeTimeoutSeconds);
		std::array<std::uint8_t, st::kMaxUdp> arrData {};
		while (Wait(hSocket, POLLRDNORM, Deadline)) {
			net::Address Peer;
			const int nReceivedBytes = recvfrom(hSocket, reinterpret_cast<char*>(arrData.data()), static_cast<int>(arrData.size()), 0, Peer.ptr(), &Peer.m_nSize);
			if (nReceivedBytes == SOCKET_ERROR) {
				const int nErrorCode = WSAGetLastError();
				Require(net::WouldBlock(nErrorCode) || (nErrorCode == WSAECONNRESET), "UDP probe receive failed");
				continue;
			}

			if ((nReceivedBytes == kUdpHeader) && (Host(Peer) == Host(ControlPeer)) && MatchesDatagram(arrData.data(), static_cast<std::size_t>(nReceivedBytes), arrToken, unPhase) && (ReadU64BE(arrData.data() + kSequenceOffset, static_cast<std::size_t>(nReceivedBytes) - kSequenceOffset) == kProbeSequence) && (ReadU32BE(arrData.data() + kPayloadSizeOffset, static_cast<std::size_t>(nReceivedBytes) - kPayloadSizeOffset) == 0)) {
				return Peer;
			}
		}

		throw std::runtime_error(Stopping() ? "Interrupted" : "UDP data probe timed out");
	}

	Stats TransferTcp(std::vector<net::Socket>& vecSockets, bool bSender, Spec const& Specification, std::string const& strLabel, bool bIntervals) {
		constexpr int kDrainTimeoutSeconds = 30;
		constexpr int kStallTimeoutSeconds = 15;
		constexpr std::size_t kTransferBatchCount = 16;
		constexpr int kPollTimeoutMilliseconds = 10;
		constexpr double kMinimumMeasuredSeconds = 1e-6;
		constexpr std::uint64_t kMaximumByteCount = std::numeric_limits<std::uint64_t>::max();

		Validate(Specification);
		Require(!vecSockets.empty() && (vecSockets.size() == Specification.m_unStreams), "Incorrect TCP data stream count");
		std::vector<std::uint8_t> vecBuffer = CreatePayload(Specification.m_unBlockSize);
		Stats Statistics;
		Clock::time_point const Begin = Clock::now();
		Clock::time_point FirstReceived = Begin;
		Clock::time_point LastProgress = Begin;
		Clock::time_point Sampled = Begin;
		Clock::time_point const StopSending = Begin + std::chrono::seconds(Specification.m_unSeconds);
		Clock::time_point const HardDeadline = StopSending + std::chrono::seconds(kDrainTimeoutSeconds);
		bool bReceivedAny = false;
		std::uint64_t unPreviousBytes = 0;
		std::vector<bool> vecCompleted = std::vector<bool>(vecSockets.size(), false);
		std::size_t unFinished = 0;
		while (unFinished < vecSockets.size()) {
			RequireActive();
			Clock::time_point const Now = Clock::now();
			Require(Now < HardDeadline, "TCP data drain timed out");

			if (bSender && (Now >= StopSending)) {
				for (auto& Socket : vecSockets) {
					if (shutdown(Socket.get(), SD_SEND) != 0) {
						const int nErrorCode = WSAGetLastError();
						throw std::runtime_error("Cannot finish TCP stream: " + net::Error(nErrorCode));
					}
				}

				Statistics.m_dblSeconds = GetElapsedSeconds(Now - Begin);
				break;
			}

			bool bProgress = false;
			for (std::size_t unSocketIndex = 0; unSocketIndex < vecSockets.size(); ++unSocketIndex) {
				if (vecCompleted[unSocketIndex]) {
					continue;
				}

				for (std::size_t unBatchIndex = 0; unBatchIndex < kTransferBatchCount; ++unBatchIndex) {
					if (bSender && (Clock::now() >= StopSending)) {
						break;
					}

					const int nTransferredBytes = bSender ? send(vecSockets[unSocketIndex].get(), reinterpret_cast<char const*>(vecBuffer.data()), static_cast<int>(vecBuffer.size()), 0) : recv(vecSockets[unSocketIndex].get(), reinterpret_cast<char*>(vecBuffer.data()), static_cast<int>(vecBuffer.size()), 0);
					if (nTransferredBytes == SOCKET_ERROR) {
						const int nErrorCode = WSAGetLastError();
						Require(net::WouldBlock(nErrorCode), std::string("TCP ") + (bSender ? "send" : "receive") + " failed: " + net::Error(nErrorCode));
						break;
					}

					if (nTransferredBytes == 0) {
						Require(!bSender, "TCP send returned zero");
						vecCompleted[unSocketIndex] = true;
						++unFinished;
						bProgress = true;
						break;
					}

					Clock::time_point const Observed = Clock::now();
					if (!bSender && !bReceivedAny) {
						FirstReceived = Observed;
						Sampled = Observed;
						bReceivedAny = true;
					}

					const std::uint64_t unTransferredBytes = static_cast<std::uint64_t>(nTransferredBytes);
					if (unTransferredBytes > (kMaximumByteCount - Statistics.m_unBytes)) {
						throw std::runtime_error("TCP byte counter exceeds supported range");
					}

					Statistics.m_unBytes += unTransferredBytes;
					bProgress = true;
					LastProgress = Observed;
				}
			}

			SampleInterval(Statistics, Statistics.m_unBytes, unPreviousBytes, Sampled, strLabel, bIntervals);

			if (unFinished == vecSockets.size()) {
				break;
			}

			if (!bProgress) {
				std::vector<WSAPOLLFD> vecPollEntries;
				vecPollEntries.reserve(vecSockets.size());

				for (std::size_t unSocketIndex = 0; unSocketIndex < vecSockets.size(); ++unSocketIndex) {
					if (!vecCompleted[unSocketIndex]) {
						vecPollEntries.push_back({ vecSockets[unSocketIndex].get(), static_cast<SHORT>(bSender ? POLLWRNORM : POLLRDNORM), 0 });
					}
				}

				const int nReady = WSAPoll(vecPollEntries.data(), static_cast<ULONG>(vecPollEntries.size()), kPollTimeoutMilliseconds);
				Require(nReady != SOCKET_ERROR, "TCP throughput poll failed");
			}

			Require(bSender || ((Clock::now() - LastProgress) < std::chrono::seconds(kStallTimeoutSeconds)), "TCP data stream stalled");
		}

		if (!bSender) {
			Require(bReceivedAny, "TCP test received no payload");
			Statistics.m_dblSeconds = std::max(GetElapsedSeconds(Clock::now() - FirstReceived), kMinimumMeasuredSeconds);
		}

		Require((Statistics.m_unBytes > 0) && (Statistics.m_dblSeconds > 0), "No TCP throughput data measured");
		Statistics.m_dblPeakMbps = std::max(Statistics.m_dblPeakMbps, Mbps(Statistics));

		return Statistics;
	}

	Stats SendUdp(SOCKET hSocket, Spec const& Specification, Token const& arrToken, std::uint32_t unPhase, std::string const& strLabel, bool bIntervals) {
		constexpr double kBitsPerMegabit = 1e6;
		constexpr double kMicrosecondsPerSecond = 1e6;
		constexpr double kMaximumPauseSeconds = 0.002;
		constexpr std::uint64_t kMaximumCatchUpPackets = 256;
		constexpr std::size_t kTransferBatchCount = 64;
		constexpr std::uint64_t kMaximumCounter = std::numeric_limits<std::uint64_t>::max();
		// Exactly 2^64; an exclusive bound avoids rounding UINT64_MAX up during validation.
		constexpr double kCounterExclusiveLimit = 18446744073709551616.0;

		static_assert(kMaxUdpPackets <= (kMaximumCounter / st::kMaxUdp));

		Validate(Specification);
		std::vector<std::uint8_t> vecData = CreatePayload(kUdpHeader + Specification.m_unUDPSize);
		Stats Statistics;
		FineWait PacingWait;
		Clock::time_point const Begin = Clock::now();
		Clock::time_point const Finish = Begin + std::chrono::seconds(Specification.m_unSeconds);
		Clock::time_point Sampled = Begin;
		const double dblPacketSeconds = static_cast<double>(Specification.m_unUDPSize) * kBitsPerByte / (Specification.m_dblUDPMbps * kBitsPerMegabit);
		std::uint64_t unScheduled = 0;
		std::uint64_t unPreviousBytes = 0;
		while (Clock::now() < Finish) {
			RequireActive();
			Clock::time_point const Now = Clock::now();
			const double dblElapsedSeconds = GetElapsedSeconds(Now - Begin);
			const double dblNextSeconds = static_cast<double>(unScheduled) * dblPacketSeconds;
			if (dblElapsedSeconds < dblNextSeconds) {
				PacingWait.Pause(std::min(dblNextSeconds - dblElapsedSeconds, kMaximumPauseSeconds));
				continue;
			}

			// Pace small batches, never emit an unbounded catch-up burst after a
			// scheduler pause. Missed slots count as local sender limitations.
			const double dblDue = dblElapsedSeconds / dblPacketSeconds;
			if (!std::isfinite(dblDue) || (dblDue < 0) || (dblDue >= kCounterExclusiveLimit)) {
				throw std::runtime_error("UDP pacing counter exceeds supported range");
			}

			const std::uint64_t unDue = static_cast<std::uint64_t>(dblDue);
			if ((unDue > unScheduled) && ((unDue - unScheduled) > kMaximumCatchUpPackets)) {
				// m_unLocalDrops <= scheduled at loop boundaries, so this addition fits the new scheduled count.
				Statistics.m_unLocalDrops += unDue - unScheduled - kMaximumCatchUpPackets;
				unScheduled = unDue - kMaximumCatchUpPackets;
			}

			if (unScheduled > (kMaximumCounter - kTransferBatchCount)) {
				throw std::runtime_error("UDP pacing counter exceeds supported range");
			}

			for (std::size_t unBatchIndex = 0; (unBatchIndex < kTransferBatchCount) && (Clock::now() < Finish); ++unBatchIndex) {
				Clock::time_point const Tick = Clock::now();
				if ((static_cast<double>(unScheduled) * dblPacketSeconds) > GetElapsedSeconds(Tick - Begin)) {
					break;
				}

				Require(Statistics.m_unPackets < kMaxUdpPackets, "UDP sequence limit reached");
				const double dblMicroseconds = GetElapsedSeconds(Tick - Begin) * kMicrosecondsPerSecond;
				if (!std::isfinite(dblMicroseconds) || (dblMicroseconds < 0) || (dblMicroseconds >= kCounterExclusiveLimit)) {
					throw std::runtime_error("UDP timestamp exceeds supported range");
				}

				WriteDatagramHeader(vecData.data(), arrToken, unPhase, Statistics.m_unPackets, static_cast<std::uint64_t>(dblMicroseconds), Specification.m_unUDPSize);
				const int nSentBytes = send(hSocket, reinterpret_cast<char const*>(vecData.data()), static_cast<int>(vecData.size()), 0);
				if (nSentBytes == SOCKET_ERROR) {
					const int nErrorCode = WSAGetLastError();
					Require(net::WouldBlock(nErrorCode) || (nErrorCode == WSAENOBUFS), "UDP send failed: " + net::Error(nErrorCode));
					++Statistics.m_unLocalDrops;

				} else {
					Require(static_cast<std::size_t>(nSentBytes) == vecData.size(), "Partial UDP send");
					Statistics.m_unBytes += Specification.m_unUDPSize;
					++Statistics.m_unPackets;
				}

				++unScheduled;
			}

			SampleInterval(Statistics, Statistics.m_unBytes, unPreviousBytes, Sampled, strLabel, bIntervals);
		}

		Statistics.m_dblSeconds = GetElapsedSeconds(Clock::now() - Begin);
		Statistics.m_dblPeakMbps = std::max(Statistics.m_dblPeakMbps, Mbps(Statistics));

		return Statistics;
	}

	struct UdpReceiver::Implementation {
		Implementation(SOCKET hSocket, Token const& arrToken, std::uint32_t unPhase, std::uint32_t unPayloadBytes) :
			m_hSocket(hSocket),
			m_arrToken(arrToken),
			m_unPhase(unPhase),
			m_unPayloadBytes(unPayloadBytes)
		{
			m_unHighestSequence = 0;
			m_bDiagnosticFailure = false;
			// C++20 atomics and the remaining member state are initialized before worker publication.
			m_Worker = std::thread([this]() noexcept -> void {
				Run();
			});
		}

		~Implementation() {
			m_bStop = true;

			try {
				if (m_Worker.joinable()) {
					m_Worker.join();
				}
			} catch (...) {
				// Destroying state still borrowed by the worker cannot safely continue.
				OutputDebugStringA("WireSteam UDP receiver worker could not join.\n");
				std::terminate();
			}
		}

		Implementation(Implementation const&) = delete;
		Implementation& operator=(Implementation const&) = delete;

		void Run() noexcept {
			constexpr std::size_t kReceiveBufferBytes = st::kMaxUdp + 1;
			constexpr int kPollTimeoutMilliseconds = 10;
			constexpr std::size_t kReceiveBatchCount = 256;
			constexpr std::size_t kSequenceOffset = 0x1C;
			constexpr std::size_t kTimestampOffset = 0x24;
			constexpr std::size_t kPayloadSizeOffset = 0x2C;
			constexpr std::size_t kTrackingWordBits = kU64Bytes * kBitsPerByte;
			constexpr std::uint64_t kMaximumTimestampMicroseconds = std::uint64_t(3600) * 1000000;
			constexpr std::uint64_t kProbeSequence = std::numeric_limits<std::uint64_t>::max();
			constexpr double kMillisecondsPerSecond = 1000;
			constexpr double kMicrosecondsPerMillisecond = 1000;
			constexpr double kJitterSmoothingFactor = 16;

			static_assert(kMaxUdpPackets <= std::numeric_limits<std::size_t>::max());
			static_assert(kMaxUdpPackets <= (std::numeric_limits<std::uint64_t>::max() / st::kMaxUdp));

			try {
				std::vector<std::uint64_t> vecSeen;
				std::array<std::uint8_t, kReceiveBufferBytes> arrData {};
				bool bReceivedAny = false;
				bool bHavePreviousTimestamp = false;
				std::uint64_t unHighestSequence = 0;
				std::uint64_t unPreviousTimestamp = 0;
				std::uint64_t unPreviousBytes = 0;
				Clock::time_point FirstArrival = Clock::now();
				Clock::time_point LastArrival = FirstArrival;
				Clock::time_point PreviousArrival = FirstArrival;
				Clock::time_point SampledAt = FirstArrival;
				while (!m_bStop.load() && !Stopping()) {
					Clock::time_point const Now = Clock::now();
					if (m_bFinishing.load() && ((m_Statistics.m_unPackets >= m_unExpectedPackets.load()) || (Now.time_since_epoch().count() >= m_nDeadlineTicks.load()))) {
						break;
					}

					WSAPOLLFD PollEntry { m_hSocket, POLLRDNORM, 0 };
					const int nReady = WSAPoll(&PollEntry, 1, kPollTimeoutMilliseconds);
					Require(nReady != SOCKET_ERROR, "UDP receive poll failed");

					if (nReady == 0) {
						continue;
					}

					for (std::size_t unBatchIndex = 0; (unBatchIndex < kReceiveBatchCount) && !m_bStop.load(); ++unBatchIndex) {
						const int nReceivedBytes = recv(m_hSocket, reinterpret_cast<char*>(arrData.data()), static_cast<int>(arrData.size()), 0);
						if (nReceivedBytes == SOCKET_ERROR) {
							const int nErrorCode = WSAGetLastError();
							if (net::WouldBlock(nErrorCode)) {
								break;
							}

							if ((nErrorCode == WSAEMSGSIZE) || (nErrorCode == WSAECONNRESET)) {
								++m_Statistics.m_unInvalid;
								continue;
							}

							throw std::runtime_error("UDP receive failed: " + net::Error(nErrorCode));
						}

						if ((nReceivedBytes == kUdpHeader) && MatchesDatagram(arrData.data(), static_cast<std::size_t>(nReceivedBytes), m_arrToken, m_unPhase) && (ReadU64BE(arrData.data() + kSequenceOffset, static_cast<std::size_t>(nReceivedBytes) - kSequenceOffset) == kProbeSequence)) {
							continue;
						}

						if ((nReceivedBytes != static_cast<int>(kUdpHeader + m_unPayloadBytes)) || !MatchesDatagram(arrData.data(), static_cast<std::size_t>(nReceivedBytes), m_arrToken, m_unPhase) || (ReadU32BE(arrData.data() + kPayloadSizeOffset, static_cast<std::size_t>(nReceivedBytes) - kPayloadSizeOffset) != m_unPayloadBytes)) {
							++m_Statistics.m_unInvalid;
							continue;
						}

						const std::uint64_t unSequence = ReadU64BE(arrData.data() + kSequenceOffset, static_cast<std::size_t>(nReceivedBytes) - kSequenceOffset);
						const std::uint64_t unTimestamp = ReadU64BE(arrData.data() + kTimestampOffset, static_cast<std::size_t>(nReceivedBytes) - kTimestampOffset);
						if ((unSequence >= kMaxUdpPackets) || (unTimestamp > kMaximumTimestampMicroseconds)) {
							++m_Statistics.m_unInvalid;
							continue;
						}

						const std::size_t unWordIndex = static_cast<std::size_t>(unSequence / kTrackingWordBits);
						const std::uint64_t unMask = std::uint64_t(0x01) << (unSequence % kTrackingWordBits);
						if (unWordIndex >= vecSeen.size()) {
							vecSeen.resize(unWordIndex + 1);
						}

						if (vecSeen[unWordIndex] & unMask) {
							++m_Statistics.m_unDuplicates;
							continue;
						}

						vecSeen[unWordIndex] |= unMask;
						Clock::time_point const Arrival = Clock::now();
						if (!bReceivedAny) {
							FirstArrival = Arrival;
							SampledAt = Arrival;
							bReceivedAny = true;
						}

						if (m_Statistics.m_unPackets && (unSequence < unHighestSequence)) {
							++m_Statistics.m_unReordered;

						} else {
							if (bHavePreviousTimestamp && (unTimestamp >= unPreviousTimestamp)) {
								const double dblDifference = std::abs(GetElapsedSeconds(Arrival - PreviousArrival) * kMillisecondsPerSecond - static_cast<double>(unTimestamp - unPreviousTimestamp) / kMicrosecondsPerMillisecond);
								m_Statistics.m_dblJitterMilliseconds += (dblDifference - m_Statistics.m_dblJitterMilliseconds) / kJitterSmoothingFactor;
							}

							PreviousArrival = Arrival;
							unPreviousTimestamp = unTimestamp;
							bHavePreviousTimestamp = true;
						}

						unHighestSequence = std::max(unHighestSequence, unSequence);
						m_unHighestSequence = unHighestSequence;
						++m_Statistics.m_unPackets;
						m_Statistics.m_unBytes += m_unPayloadBytes;
						LastArrival = Arrival;
						SampleInterval(m_Statistics, m_Statistics.m_unBytes, unPreviousBytes, SampledAt, {}, false);
					}
				}

				m_Statistics.m_dblSeconds = bReceivedAny ? GetElapsedSeconds(LastArrival - FirstArrival) : 0;
			} catch (std::exception const& Exception) {
				try {
					m_strError = Exception.what();
				} catch (...) {
					m_bDiagnosticFailure = true;
					OutputDebugStringA("WireSteam UDP receiver could not retain its failure.\n");
				}
			} catch (...) {
				m_bDiagnosticFailure = true;
				OutputDebugStringA("WireSteam UDP receiver encountered an unexpected exception.\n");
			}
		}

		SOCKET m_hSocket;
		Token m_arrToken;
		std::uint32_t m_unPhase;
		std::uint32_t m_unPayloadBytes;
		std::thread m_Worker;
		std::atomic_bool m_bStop;
		std::atomic_bool m_bFinishing;
		std::atomic<std::uint64_t> m_unExpectedPackets;
		std::atomic<std::int64_t> m_nDeadlineTicks;
		Stats m_Statistics;
		std::string m_strError;
		std::uint64_t m_unHighestSequence;
		// Written by the worker and read by Finish only after joining it.
		bool m_bDiagnosticFailure;
	};

	UdpReceiver::UdpReceiver(SOCKET hSocket, Token const& arrToken, std::uint32_t unPhase, std::uint32_t unPayloadBytes) {
		constexpr std::uint32_t kMinimumPayloadBytes = 64;

		Require((unPayloadBytes >= kMinimumPayloadBytes) && (unPayloadBytes <= (st::kMaxUdp - kUdpHeader)), "Invalid UDP receive payload size");
		m_pImplementation = std::make_unique<Implementation>(hSocket, arrToken, unPhase, unPayloadBytes);
	}

	UdpReceiver::~UdpReceiver() = default;

	Stats UdpReceiver::Finish(Stats const& Sender, unsigned unDrainMilliseconds) {
		constexpr unsigned kMaximumDrainMilliseconds = 10000;
		constexpr double kMaximumSenderSeconds = kMaxSeconds + 10;

		static_assert(kMaxUdpPackets <= (std::numeric_limits<std::uint64_t>::max() / st::kMaxUdp));

		Require((Sender.m_unPackets <= kMaxUdpPackets) && (Sender.m_unBytes == (Sender.m_unPackets * m_pImplementation->m_unPayloadBytes)) && std::isfinite(Sender.m_dblSeconds) && (Sender.m_dblSeconds > 0) && (Sender.m_dblSeconds <= kMaximumSenderSeconds), "Invalid UDP sender accounting");
		Require(unDrainMilliseconds <= kMaximumDrainMilliseconds, "UDP drain period exceeds bounds");
		m_pImplementation->m_unExpectedPackets = Sender.m_unPackets;
		m_pImplementation->m_nDeadlineTicks = (Clock::now() + std::chrono::milliseconds(unDrainMilliseconds)).time_since_epoch().count();
		m_pImplementation->m_bFinishing = true;

		if (m_pImplementation->m_Worker.joinable()) {
			m_pImplementation->m_Worker.join();
		}

		if (m_pImplementation->m_bDiagnosticFailure) {
			throw std::runtime_error("UDP receiver worker failed without a diagnostic");
		}

		Require(m_pImplementation->m_strError.empty(), m_pImplementation->m_strError);
		RequireActive();
		Require(m_pImplementation->m_Statistics.m_unPackets <= Sender.m_unPackets, "UDP receiver counted more unique datagrams than sender reported");
		Require(!m_pImplementation->m_Statistics.m_unPackets || (m_pImplementation->m_unHighestSequence < Sender.m_unPackets), "UDP sequence exceeds the sender's reported packet window");
		Stats Result = m_pImplementation->m_Statistics;
		Result.m_dblSeconds = std::max(Sender.m_dblSeconds, Result.m_dblSeconds);
		Result.m_dblPeakMbps = std::max(Result.m_dblPeakMbps, Mbps(Result));

		return Result;
	}

	double Mbps(Stats const& Statistics) {
		constexpr double kBitsPerMegabit = 1e6;

		return (Statistics.m_dblSeconds > 0) ? (static_cast<double>(Statistics.m_unBytes) * kBitsPerByte / Statistics.m_dblSeconds / kBitsPerMegabit) : 0;
	}

	double Loss(Stats const& Sender, Stats const& Receiver) {
		constexpr double kPercentScale = 100.0;

		return Sender.m_unPackets ? (kPercentScale * static_cast<double>(Sender.m_unPackets - std::min(Sender.m_unPackets, Receiver.m_unPackets)) / static_cast<double>(Sender.m_unPackets)) : 0;
	}

	std::string Summary(Message const& Result) {
		constexpr int kRatePrecision = 2;

		const bool bUDP = Result.m_Specification.m_Protocol == Protocol::Udp;

		std::ostringstream OutputStream;

		OutputStream << std::fixed << std::setprecision(kRatePrecision);
		OutputStream << (bUDP ? "UDP" : "TCP") << ' ' << ((Result.m_Specification.m_Direction == Direction::Upload) ? "UPLOAD (client -> server)" : "DOWNLOAD (server -> client)") << " RESULT: receiver " << Mbps(Result.m_Receiver) << " Mbit/s (" << Mbps(Result.m_Receiver) / kBitsPerByte << " MB/s), " << Result.m_Receiver.m_unBytes << " bytes / " << Result.m_Receiver.m_dblSeconds << " s; sender " << Mbps(Result.m_Sender) << " Mbit/s";

		if (bUDP) {
			OutputStream << "; target " << Result.m_Specification.m_dblUDPMbps << " Mbit/s; loss " << Loss(Result.m_Sender, Result.m_Receiver) << "% (" << Result.m_Sender.m_unPackets - std::min(Result.m_Sender.m_unPackets, Result.m_Receiver.m_unPackets) << '/' << Result.m_Sender.m_unPackets << "); duplicates " << Result.m_Receiver.m_unDuplicates << "; reordered " << Result.m_Receiver.m_unReordered << "; jitter " << Result.m_Receiver.m_dblJitterMilliseconds << " ms; local sender drops " << Result.m_Sender.m_unLocalDrops;

		} else {
			OutputStream << "; streams " << Result.m_Specification.m_unStreams;
		}

		OutputStream << "; best sampled receiver interval " << Result.m_Receiver.m_dblPeakMbps << " Mbit/s";

		return OutputStream.str();
	}

	std::string ResultJson(Message const& Result) {
		constexpr int kStatisticsPrecision = 12;

		const bool bUDP = Result.m_Specification.m_Protocol == Protocol::Udp;

		std::ostringstream OutputStream;

		OutputStream << std::setprecision(kStatisticsPrecision);
		OutputStream << "{\"protocol\":\"" << (bUDP ? "udp" : "tcp") << "\",\"direction\":\"" << ((Result.m_Specification.m_Direction == Direction::Upload) ? "upload" : "download") << "\",\"requestedSeconds\":" << Result.m_Specification.m_unSeconds << ",\"streams\":" << Result.m_Specification.m_unStreams << ",\"udpTargetMbps\":" << (bUDP ? Result.m_Specification.m_dblUDPMbps : 0) << ",\"receiverGoodputMbps\":" << Mbps(Result.m_Receiver) << ",\"lossPercent\":" << (bUDP ? Loss(Result.m_Sender, Result.m_Receiver) : 0) << ",\"sender\":" << FormatStatisticsJSON(Result.m_Sender, false) << ",\"receiver\":" << FormatStatisticsJSON(Result.m_Receiver, true) << '}';

		return OutputStream.str();
	}

	void RunUnitTests() {
		constexpr std::uint32_t kControlPhase = 44;
		constexpr std::uint16_t kControlPort = 5201;
		constexpr std::uint32_t kHeaderPhase = 7;
		constexpr std::uint32_t kOtherHeaderPhase = 8;
		constexpr std::size_t kSequenceOffset = 0x1C;
		constexpr std::uint32_t kTrackingPhase = 9;
		constexpr std::uint32_t kWindowPhase = 10;
		constexpr std::uint32_t kCancellationPhase = 11;
		constexpr std::uint32_t kPayloadBytes = 64;
		constexpr std::uint64_t kTimestampStep = 1000;
		constexpr std::size_t kTokenMutationOffset = 0x08;
		constexpr unsigned kDrainMilliseconds = 100;
		constexpr unsigned kCancellationDrainMilliseconds = 2000;
		constexpr std::array<unsigned, 2> kInvalidDurations = { 0, 301 };
		constexpr std::array<std::uint32_t, 4> kTrackingSequences = { 2, 0, 1, 1 };
		constexpr std::array<std::uint32_t, 3> kInvalidWindowSequences = { 100, 101, 102 };

		static_assert((kUdpHeader + kPayloadBytes) <= static_cast<std::size_t>(std::numeric_limits<int>::max()));

		Message Original;
		Original.m_Kind = Kind::Result;
		Original.m_arrToken = NewToken();
		Original.m_unPhase = kControlPhase;
		Original.m_unPort = kControlPort;
		Original.m_Sender = { 12345, 42, 0, 0, 0, 1, 2.5, 0, 1.25 };
		Original.m_Receiver = { 12000, 41, 1, 2, 3, 0, 2.5, 0.12, 1.2 };
		Original.m_strText = "round trip";
		std::vector<std::uint8_t> const vecEncoded = EncodeMessage(Original);
		Message const Decoded = DecodeMessage(vecEncoded);
		Require((Decoded.m_arrToken == Original.m_arrToken) && (Decoded.m_Sender.m_unBytes == 12345) && (Decoded.m_Receiver.m_dblJitterMilliseconds == 0.12) && (Decoded.m_strText == Original.m_strText) && (Decoded.m_unPhase == kControlPhase), "Control frame round trip failed");

		for (std::size_t unTruncatedBytes = 0; unTruncatedBytes < vecEncoded.size(); ++unTruncatedBytes) {
			bool bRejected = false;
			try {
				DecodeMessage({ vecEncoded.begin(), vecEncoded.begin() + static_cast<std::ptrdiff_t>(unTruncatedBytes) });
			} catch (...) {
				bRejected = true;
			}

			Require(bRejected, "Truncated control frame accepted");
		}

		Spec Specification;
		Validate(Specification);
		Specification.m_Protocol = Protocol::Udp;
		Specification.m_unStreams = 1;
		Validate(Specification);

		for (auto const& unInvalidSeconds : kInvalidDurations) {
			Spec Invalid = Specification;
			Invalid.m_unSeconds = unInvalidSeconds;
			bool bRejected = false;
			try {
				Validate(Invalid);
			} catch (...) {
				bRejected = true;
			}

			Require(bRejected, "Invalid duration accepted");
		}

		Specification.m_unUDPSize = 65507;
		bool bRejected = false;
		try {
			Validate(Specification);
		} catch (...) {
			bRejected = true;
		}

		Require(bRejected, "Oversized UDP payload accepted");

		std::array<std::uint8_t, kUdpHeader> arrPacket {};
		WriteDatagramHeader(arrPacket.data(), Original.m_arrToken, kHeaderPhase, 9, 11, 0);
		Require(MatchesDatagram(arrPacket.data(), arrPacket.size(), Original.m_arrToken, kHeaderPhase) && !MatchesDatagram(arrPacket.data(), arrPacket.size(), Original.m_arrToken, kOtherHeaderPhase) && (ReadU64BE(arrPacket.data() + kSequenceOffset, arrPacket.size() - kSequenceOffset) == 9), "UDP session header validation failed");

		Stats Sent;
		Sent.m_unPackets = 100;
		Stats Received;
		Received.m_unPackets = 90;
		Require(Loss(Sent, Received) == 10, "UDP loss accounting failed");
		Sent.m_unBytes = 125000;
		Sent.m_dblSeconds = 1;
		Require(Mbps(Sent) == 1, "Decimal Mbps units are incorrect");
		Require(ParseEndpoint("[::1]:5201").m_strHost == "::1", "IPv6 endpoint parsing failed");

		std::string strError;
		net::Socket TransmitSocket = net::Bind({ "127.0.0.1", 0 }, SOCK_DGRAM, strError);
		Require(bool(TransmitSocket), strError);
		net::Socket ReceiveSocket = net::Bind({ "127.0.0.1", 0 }, SOCK_DGRAM, strError);
		Require(bool(ReceiveSocket), strError);
		Tune(TransmitSocket.get(), false);
		Tune(ReceiveSocket.get(), false);
		net::Address const TransmitAddress = Local(TransmitSocket.get());
		net::Address const ReceiveAddress = Local(ReceiveSocket.get());
		Require((connect(TransmitSocket.get(), ReceiveAddress.ptr(), ReceiveAddress.m_nSize) == 0) && (connect(ReceiveSocket.get(), TransmitAddress.ptr(), TransmitAddress.m_nSize) == 0), "Cannot connect UDP accounting test sockets");

		UdpReceiver Tracking(ReceiveSocket.get(), Original.m_arrToken, kTrackingPhase, kPayloadBytes);
		SendUdpProbe(TransmitSocket.get(), Original.m_arrToken, kTrackingPhase);
		std::array<std::uint8_t, kUdpHeader + kPayloadBytes> arrDatagram {};
		for (auto const& unSequence : kTrackingSequences) {
			WriteDatagramHeader(arrDatagram.data(), Original.m_arrToken, kTrackingPhase, unSequence, unSequence * kTimestampStep, kPayloadBytes);
			Require(send(TransmitSocket.get(), reinterpret_cast<char const*>(arrDatagram.data()), static_cast<int>(arrDatagram.size()), 0) == static_cast<int>(arrDatagram.size()), "Cannot send UDP accounting test packet");
		}

		arrDatagram[kTokenMutationOffset] ^= std::uint8_t(0x01); // Same source, invalid session token: must not count as useful traffic.
		Require(send(TransmitSocket.get(), reinterpret_cast<char const*>(arrDatagram.data()), static_cast<int>(arrDatagram.size()), 0) == static_cast<int>(arrDatagram.size()), "Cannot send invalid session test packet");
		Stats Offered;
		Offered.m_unPackets = 5;
		Offered.m_unBytes = 5 * kPayloadBytes;
		Offered.m_dblSeconds = 1;
		Stats const Tracked = Tracking.Finish(Offered, kDrainMilliseconds);
		Require((Tracked.m_unPackets == 3) && (Tracked.m_unBytes == (3 * kPayloadBytes)) && (Tracked.m_unDuplicates == 1) && (Tracked.m_unReordered == 2) && (Tracked.m_unInvalid == 1) && (Loss(Offered, Tracked) == 40), "Live UDP loss, duplicate, reorder or session filtering accounting is incorrect");

		UdpReceiver InvalidWindow(ReceiveSocket.get(), Original.m_arrToken, kWindowPhase, kPayloadBytes);
		for (auto const& unSequence : kInvalidWindowSequences) {
			WriteDatagramHeader(arrDatagram.data(), Original.m_arrToken, kWindowPhase, unSequence, unSequence * kTimestampStep, kPayloadBytes);
			Require(send(TransmitSocket.get(), reinterpret_cast<char const*>(arrDatagram.data()), static_cast<int>(arrDatagram.size()), 0) == static_cast<int>(arrDatagram.size()), "Cannot send UDP window test packet");
		}

		Offered.m_unPackets = 3;
		Offered.m_unBytes = 3 * kPayloadBytes;
		bRejected = false;
		try {
			InvalidWindow.Finish(Offered, kDrainMilliseconds);
		} catch (std::runtime_error const&) {
			bRejected = true;
		}

		Require(bRejected, "UDP sequence numbers outside the reported sender window were accepted");

		UdpReceiver CanceledReceive(ReceiveSocket.get(), Original.m_arrToken, kCancellationPhase, kPayloadBytes);
		Clock::time_point const CanceledAt = Clock::now();
		RequestStop();
		bRejected = false;
		try {
			CanceledReceive.Finish(Offered, kCancellationDrainMilliseconds);
		} catch (std::runtime_error const& Error) {
			bRejected = std::string(Error.what()) == "Interrupted";
		}

		Require(bRejected && Stopping() && ((Clock::now() - CanceledAt) < std::chrono::seconds(1)), "UDP receive cancellation did not join promptly");
		Log("PASS speed-test unit checks: framed control, truncation, bounds, tokens, live UDP loss/duplicates/reordering, cancellation and units");
	}
} // namespace ws::speed
