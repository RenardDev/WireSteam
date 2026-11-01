#pragma once

// C++
#include <cstddef>
#include <cstdint>

// STL
#include <string>
#include <vector>

namespace st {
	using PeerId = std::uint32_t;

	struct Endpoint {
		std::string m_strHost = "127.0.0.1";
		std::uint16_t m_unPort = 0;

		std::string ToString() const {
			return ((m_strHost.find(':') != std::string::npos) ? ("[" + m_strHost + "]") : m_strHost) + ":" + std::to_string(m_unPort);
		}
	};

	enum class FrameType : std::uint8_t {
		Hello = 0x01,
		HelloAck = 0x02,
		UdpData = 0x14,
		UdpFragment = 0x15
	};

	struct Frame {
		FrameType m_Type = FrameType::Hello;
		std::uint32_t m_unFlow = 0;
		std::uint32_t m_unValue = 0;
		std::vector<std::uint8_t> m_vecPayload;
	};

	inline constexpr std::size_t kMaxUdp = 65507;
	inline constexpr std::size_t kUdpFragmentChunk = 1024;
	inline constexpr std::size_t kWireHeader = 0x14;
	inline constexpr std::size_t kUdpFragmentHeader = 0x08;
	inline constexpr std::size_t kMaxFrame = kWireHeader + kUdpFragmentChunk + kUdpFragmentHeader;

	// Stateless helpers; callers synchronize mutable inputs/outputs used by concurrent calls.
	bool IsReliable(FrameType Type);
	// Returns owned wire bytes with big-endian header fields; invalid frames throw and allocation failures propagate.
	std::vector<std::uint8_t> Encode(Frame const& FrameValue);
	// Borrows unSize readable bytes, stable for the call and not overlapping strErrorOut.
	// Clears strErrorOut on entry; FrameOut commits only on success. Allocation failures propagate.
	bool Decode(void const* pData, std::size_t unSize, Frame& FrameOut, std::string& strErrorOut);

	enum class SendResult {
		Sent = 0,
		WouldBlock,
		Disconnected
	};
} // namespace st
