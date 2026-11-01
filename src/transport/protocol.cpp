#include "tunnel.hpp"

// C++
#include <cstring>
#include <utility>

// STL
#include <algorithm>
#include <stdexcept>

namespace st {
	namespace {
		constexpr std::size_t kU32Bytes = sizeof(std::uint32_t);
		constexpr std::size_t kBitsPerByte = 8;
		constexpr std::size_t kMagicBytes = 0x04;
		constexpr std::size_t kVersionOffset = 0x04;
		constexpr std::size_t kFrameTypeOffset = 0x05;
		constexpr std::size_t kFirstReservedOffset = 0x06;
		constexpr std::size_t kSecondReservedOffset = 0x07;
		constexpr std::size_t kFlowOffset = 0x08;
		constexpr std::size_t kValueOffset = 0x0C;
		constexpr std::size_t kPayloadSizeOffset = 0x10;
		constexpr std::size_t kHandshakeBytes = 0x40;
		constexpr std::uint8_t kProtocolVersion = 1;
		constexpr std::uint32_t kDatagramFlow = 1;

		static_assert(kU32Bytes == 0x04);
		static_assert(kWireHeader == (kPayloadSizeOffset + kU32Bytes));
		static_assert(kUdpFragmentHeader == (2 * kU32Bytes));

		bool ReadU32BE(std::uint8_t const* punBytes, std::size_t unAvailableBytes, std::uint32_t& unValueOut) noexcept {
			if (!punBytes || (unAvailableBytes < kU32Bytes)) {
				return false;
			}

			std::uint32_t unValue = 0;
			for (std::size_t unByteIndex = 0; unByteIndex < kU32Bytes; ++unByteIndex) {
				unValue = (unValue << kBitsPerByte) | punBytes[unByteIndex];
			}

			unValueOut = unValue;

			return true;
		}

		// Encode establishes fixed writable header storage before each call.
		void WriteU32BE(std::uint8_t* punBytes, std::uint32_t unValue) {
			for (std::size_t unByteIndex = 0; unByteIndex < kU32Bytes; ++unByteIndex) {
				punBytes[unByteIndex] = static_cast<std::uint8_t>(unValue >> ((kU32Bytes - 1 - unByteIndex) * kBitsPerByte));
			}
		}

		bool Validate(Frame const& FrameValue, std::string& strErrorOut) {
			auto const Fail = [&](char const* pszMessage) -> bool {
				strErrorOut = pszMessage;

				return false;
			};

			switch (FrameValue.m_Type) {
				case FrameType::Hello:
				case FrameType::HelloAck:
					if (FrameValue.m_unFlow || FrameValue.m_unValue || (FrameValue.m_vecPayload.size() != kHandshakeBytes)) {
						return Fail("Invalid public-key handshake");
					}

					return true;

				case FrameType::UdpData:
					if ((FrameValue.m_unFlow != kDatagramFlow) || FrameValue.m_unValue || (FrameValue.m_vecPayload.size() > kUdpFragmentChunk)) {
						return Fail("Invalid datagram");
					}

					return true;

				case FrameType::UdpFragment: {
					if ((FrameValue.m_unFlow != kDatagramFlow) || !FrameValue.m_unValue || (FrameValue.m_vecPayload.size() <= kUdpFragmentHeader) || (FrameValue.m_vecPayload.size() > (kUdpFragmentHeader + kUdpFragmentChunk))) {
						return Fail("Invalid fragment");
					}

					std::uint32_t unTotal = 0;
					std::uint32_t unOffset = 0;
					if (!ReadU32BE(FrameValue.m_vecPayload.data(), FrameValue.m_vecPayload.size(), unTotal) || !ReadU32BE(FrameValue.m_vecPayload.data() + kU32Bytes, FrameValue.m_vecPayload.size() - kU32Bytes, unOffset)) {
						return Fail("Invalid fragment");
					}

					if ((unTotal <= kUdpFragmentChunk) || (unTotal > kMaxUdp) || (unOffset >= unTotal) || (unOffset % kUdpFragmentChunk) || ((FrameValue.m_vecPayload.size() - kUdpFragmentHeader) != std::min(kUdpFragmentChunk, std::size_t(unTotal - unOffset)))) {
						return Fail("Invalid fragment bounds");
					}

					return true;
				}

				default:
					return Fail("Unknown message type");
			}
		}
	} // namespace

	bool IsReliable(FrameType Type) {
		return (Type == FrameType::Hello) || (Type == FrameType::HelloAck);
	}

	std::vector<std::uint8_t> Encode(Frame const& FrameValue) {
		std::string strError;
		if (!Validate(FrameValue, strError)) {
			throw std::invalid_argument(strError);
		}

		std::vector<std::uint8_t> vecBytes(kWireHeader + FrameValue.m_vecPayload.size());
		std::memcpy(vecBytes.data(), "WSTM", kMagicBytes);
		vecBytes[kVersionOffset] = kProtocolVersion;
		vecBytes[kFrameTypeOffset] = static_cast<std::uint8_t>(FrameValue.m_Type);
		WriteU32BE(vecBytes.data() + kFlowOffset, FrameValue.m_unFlow);
		WriteU32BE(vecBytes.data() + kValueOffset, FrameValue.m_unValue);
		WriteU32BE(vecBytes.data() + kPayloadSizeOffset, static_cast<std::uint32_t>(FrameValue.m_vecPayload.size()));
		std::copy(FrameValue.m_vecPayload.begin(), FrameValue.m_vecPayload.end(), vecBytes.begin() + kWireHeader);

		return vecBytes;
	}

	bool Decode(void const* pData, std::size_t unSize, Frame& FrameOut, std::string& strErrorOut) {
		strErrorOut.clear();
		auto const Fail = [&](char const* pszMessage) -> bool {
			strErrorOut = pszMessage;

			return false;
		};

		if (!pData || (unSize < kWireHeader)) {
			return Fail("Truncated frame");
		}

		if (unSize > kMaxFrame) {
			return Fail("Frame exceeds the size limit");
		}

		std::uint8_t const* punBytes = static_cast<std::uint8_t const*>(pData);
		if (std::memcmp(punBytes, "WSTM", kMagicBytes)) {
			return Fail("Invalid WireSteam magic");
		}

		if (punBytes[kVersionOffset] != kProtocolVersion) {
			return Fail("Unsupported WireSteam protocol version");
		}

		if (punBytes[kFirstReservedOffset] || punBytes[kSecondReservedOffset]) {
			return Fail("Reserved bits are set");
		}

		std::uint32_t unPayloadBytes = 0;
		if (!ReadU32BE(punBytes + kPayloadSizeOffset, unSize - kPayloadSizeOffset, unPayloadBytes)) {
			return Fail("Truncated frame");
		}

		if (unPayloadBytes != (unSize - kWireHeader)) {
			return Fail("Frame length mismatch");
		}

		std::uint32_t unFlow = 0;
		std::uint32_t unValue = 0;
		if (!ReadU32BE(punBytes + kFlowOffset, unSize - kFlowOffset, unFlow) || !ReadU32BE(punBytes + kValueOffset, unSize - kValueOffset, unValue)) {
			return Fail("Truncated frame");
		}

		Frame DecodedFrame { static_cast<FrameType>(punBytes[kFrameTypeOffset]), unFlow, unValue, {} };
		DecodedFrame.m_vecPayload.assign(punBytes + kWireHeader, punBytes + unSize);
		if (!Validate(DecodedFrame, strErrorOut)) {
			return false;
		}

		FrameOut = std::move(DecodedFrame);

		return true;
	}
} // namespace st
