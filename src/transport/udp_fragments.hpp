#pragma once

// STL
#include <chrono>
#include <memory>
#include <optional>

// Project
#include "tunnel.hpp"

namespace st {
	// A transport-only layer: callers continue to send/receive UdpData.
	// Missing fragments are discarded, never retransmitted. Limits reserve the
	// whole declared datagram size before accepting its first fragment.
	// Instances require external synchronization, including counter reads.
	// All limits and the millisecond lifetime must be positive; invalid arguments
	// are rejected. Allocating operations may throw while preserving valid accounting.
	class UdpFragments {
	public:
		explicit UdpFragments(std::size_t unGlobalBytes = 8 * 1024 * 1024, std::size_t unGlobalCount = 256, std::size_t unPeerBytes = 1024 * 1024, std::size_t unPeerCount = 64, std::chrono::milliseconds TimeToLive = std::chrono::seconds(2));
		~UdpFragments();
		UdpFragments(UdpFragments const&) = delete;
		UdpFragments& operator=(UdpFragments const&) = delete;

		// <=1024 bytes (including an empty datagram) are returned as UdpData.
		// Larger datagrams require a nonzero ID, unique while the receiver retains
		// its in-flight assembly or duplicate tombstone.
		// Borrows stable input for the call and returns owned frames; invalid input throws.
		static std::vector<Frame> Split(Frame const& Datagram, std::uint32_t unDatagramID);

		// Borrows stable input; incomplete, duplicate or discarded input returns no frame.
		// A completed result owns its payload.
		std::optional<Frame> Accept(PeerId unPeer, Frame const& Fragment);

		// Releases this peer's in-flight assemblies and duplicate tombstones.
		void PeerDown(PeerId unPeer);

		// Expires assemblies and tombstones using monotonic elapsed milliseconds.
		void Tick();

		// Returns the cumulative unsigned discard-event counter.
		std::uint64_t Dropped() const;

	private:
		struct Implementation;
		std::unique_ptr<Implementation> m_pImplementation;
	};
} // namespace st
