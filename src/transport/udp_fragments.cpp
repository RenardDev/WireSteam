#include "udp_fragments.hpp"

// C++
#include <utility>

// STL
#include <algorithm>
#include <limits>
#include <map>
#include <ratio>
#include <stdexcept>
#include <tuple>

namespace st {
	namespace {
		using Clock = std::chrono::steady_clock;
		using UnsignedClockDuration = std::chrono::duration<std::uint64_t, Clock::period>;
		using UnsignedMilliseconds = std::chrono::duration<std::uint64_t, std::milli>;

		constexpr std::size_t kU32Bytes = sizeof(std::uint32_t);
		constexpr std::size_t kBitsPerByte = 8;
		constexpr std::size_t kMaximumChunks = (kMaxUdp + kUdpFragmentChunk - 1) / kUdpFragmentChunk;

		static_assert(kU32Bytes == 0x04);
		static_assert(kUdpFragmentHeader == (2 * kU32Bytes));
		static_assert(kMaximumChunks <= std::numeric_limits<std::uint64_t>::digits);
		static_assert(std::numeric_limits<Clock::rep>::is_signed);
		static_assert(std::numeric_limits<Clock::rep>::digits == (std::numeric_limits<std::uint64_t>::digits - 1));
		static_assert((Clock::period::num == 1) && (Clock::period::den == std::nano::den));

		// Ordered signed clock counts differ by at most UINT64_MAX ticks. Unsigned
		// subtraction preserves that distance even across zero; conversion to whole
		// milliseconds only divides, so every positive public lifetime is supported.
		bool HasExpired(Clock::time_point Created, Clock::time_point Now, std::chrono::milliseconds Lifetime) noexcept {
			if (Now < Created) {
				return false;
			}

			const std::uint64_t unElapsedTicks = static_cast<std::uint64_t>(Now.time_since_epoch().count()) - static_cast<std::uint64_t>(Created.time_since_epoch().count());
			UnsignedMilliseconds const Elapsed = std::chrono::duration_cast<UnsignedMilliseconds>(UnsignedClockDuration(unElapsedTicks));

			return Elapsed.count() >= static_cast<std::uint64_t>(Lifetime.count());
		}

		bool ReadFragmentU32BE(std::uint8_t const* punInput, std::size_t unAvailableBytes, std::uint32_t& unValueOut) noexcept {
			if (!punInput || (unAvailableBytes < kU32Bytes)) {
				return false;
			}

			std::uint32_t unValue = 0;
			for (std::size_t unByteIndex = 0; unByteIndex < kU32Bytes; ++unByteIndex) {
				unValue = (unValue << kBitsPerByte) | punInput[unByteIndex];
			}

			unValueOut = unValue;

			return true;
		}

		// Split establishes fixed writable fragment-header storage before each call.
		void WriteFragmentU32BE(std::uint8_t* punOutput, std::uint32_t unValue) {
			for (std::size_t unByteIndex = 0; unByteIndex < kU32Bytes; ++unByteIndex) {
				punOutput[unByteIndex] = static_cast<std::uint8_t>(unValue >> ((kU32Bytes - 1 - unByteIndex) * kBitsPerByte));
			}
		}

		bool ValidateFragmentBounds(Frame const& Fragment, std::uint32_t& unTotalOut, std::uint32_t& unOffsetOut) {
			if ((Fragment.m_Type != FrameType::UdpFragment) || !Fragment.m_unFlow || !Fragment.m_unValue || (Fragment.m_vecPayload.size() <= kUdpFragmentHeader) || (Fragment.m_vecPayload.size() > (kUdpFragmentHeader + kUdpFragmentChunk))) {
				return false;
			}

			std::uint32_t unTotal = 0;
			std::uint32_t unOffset = 0;
			if (!ReadFragmentU32BE(Fragment.m_vecPayload.data(), Fragment.m_vecPayload.size(), unTotal) || !ReadFragmentU32BE(Fragment.m_vecPayload.data() + kU32Bytes, Fragment.m_vecPayload.size() - kU32Bytes, unOffset)) {
				return false;
			}

			if (!unTotal || (unTotal > kMaxUdp) || (unOffset >= unTotal) || ((unOffset % kUdpFragmentChunk) != 0) || ((Fragment.m_vecPayload.size() - kUdpFragmentHeader) != std::min(kUdpFragmentChunk, static_cast<std::size_t>(unTotal - unOffset)))) {
				return false;
			}

			unTotalOut = unTotal;
			unOffsetOut = unOffset;

			return true;
		}
	} // namespace

	struct UdpFragments::Implementation {
		using Key = std::tuple<PeerId, std::uint32_t, std::uint32_t>;

		struct Assembly {
			Assembly() {
				m_unTotal = 0;
				m_unPresent = 0;
				m_unReceived = 0;
				m_unChunks = 0;
			}

			std::vector<std::uint8_t> m_vecBytes;
			std::uint32_t m_unTotal;
			std::uint64_t m_unPresent;
			std::size_t m_unReceived;
			std::size_t m_unChunks;
			Clock::time_point m_Created;
		};

		struct Usage {
			Usage() {
				m_unBytes = 0;
				m_unCount = 0;
			}

			std::size_t m_unBytes;
			std::size_t m_unCount;
		};

		static constexpr std::size_t kRecentLimit = 1024;

		Implementation(std::size_t unAllBytes, std::size_t unAllCount, std::size_t unOnePeerBytes, std::size_t unOnePeerCount, std::chrono::milliseconds Lifetime) :
			m_unGlobalBytes(unAllBytes),
			m_unGlobalCount(unAllCount),
			m_unPeerBytes(unOnePeerBytes),
			m_unPeerCount(unOnePeerCount),
			m_TimeToLive(Lifetime)
		{
			m_unUsedBytes = 0;
			m_unDropped = 0;

			if (!m_unGlobalBytes || !m_unGlobalCount || !m_unPeerBytes || !m_unPeerCount || (m_TimeToLive.count() <= 0)) {
				throw std::invalid_argument("UDP reassembly limits and lifetime must be positive");
			}
		}

		void Remove(std::map<Key, Assembly>::iterator itAssembly) {
			const PeerId unPeer = std::get<0>(itAssembly->first);
			auto itUsage = m_mapPeers.find(unPeer);
			m_unUsedBytes -= itAssembly->second.m_unTotal;
			itUsage->second.m_unBytes -= itAssembly->second.m_unTotal;

			if (!--itUsage->second.m_unCount) {
				m_mapPeers.erase(itUsage);
			}

			m_mapAssemblies.erase(itAssembly);
		}

		void Remember(Key const& AssemblyKey, Clock::time_point Now) {
			if (m_mapRecent.size() >= kRecentLimit) {
				auto itOldest = std::min_element(m_mapRecent.begin(), m_mapRecent.end(), [](std::pair<Key const, Clock::time_point> const& Left, std::pair<Key const, Clock::time_point> const& Right) -> bool {
					return Left.second < Right.second;
				});
				m_mapRecent.erase(itOldest);
			}

			m_mapRecent[AssemblyKey] = Now;
		}

		void Tick() {
			Clock::time_point const Now = Clock::now();
			for (auto itAssembly = m_mapAssemblies.begin(); itAssembly != m_mapAssemblies.end();) {
				if (HasExpired(itAssembly->second.m_Created, Now, m_TimeToLive)) {
					auto itOld = itAssembly++;
					++m_unDropped;
					Key const AssemblyKey = itOld->first;
					Remove(itOld);
					Remember(AssemblyKey, Now);
				} else {
					++itAssembly;
				}
			}

			for (auto itRecent = m_mapRecent.begin(); itRecent != m_mapRecent.end();) {
				if (HasExpired(itRecent->second, Now, m_TimeToLive)) {
					itRecent = m_mapRecent.erase(itRecent);
				} else {
					++itRecent;
				}
			}
		}

		std::optional<Frame> Accept(PeerId unPeer, Frame const& Fragment) {
			std::uint32_t unTotal = 0;
			std::uint32_t unOffset = 0;
			if (!unPeer || !ValidateFragmentBounds(Fragment, unTotal, unOffset)) {
				++m_unDropped;
				return std::nullopt;
			}

			Tick();

			Key const AssemblyKey = Key(unPeer, Fragment.m_unFlow, Fragment.m_unValue);
			if (m_mapRecent.count(AssemblyKey)) {
				return std::nullopt;
			}

			auto itFound = m_mapAssemblies.find(AssemblyKey);
			if (itFound == m_mapAssemblies.end()) {
				auto itUsage = m_mapPeers.find(unPeer);
				const std::size_t unBytes = itUsage == m_mapPeers.end() ? 0 : itUsage->second.m_unBytes;
				const std::size_t unCount = itUsage == m_mapPeers.end() ? 0 : itUsage->second.m_unCount;
				if ((m_mapAssemblies.size() >= m_unGlobalCount) || (unTotal > (m_unGlobalBytes - m_unUsedBytes)) ||
					(unCount >= m_unPeerCount) || (unTotal > (m_unPeerBytes - unBytes))) {
					++m_unDropped;
					return std::nullopt;
				}

				Assembly CurrentAssembly;
				CurrentAssembly.m_unTotal = unTotal;
				CurrentAssembly.m_unChunks = (unTotal + kUdpFragmentChunk - 1) / kUdpFragmentChunk;
				CurrentAssembly.m_Created = Clock::now();
				CurrentAssembly.m_vecBytes.resize(unTotal);
				const bool bNewPeer = itUsage == m_mapPeers.end();
				if (bNewPeer) {
					itUsage = m_mapPeers.try_emplace(unPeer).first;
				}

				// Both nodes must exist before accounting commits. A failed assembly
				// allocation releases only the empty peer reservation made by this call.
				try {
					itFound = m_mapAssemblies.emplace(AssemblyKey, std::move(CurrentAssembly)).first;
				} catch (...) {
					if (bNewPeer) {
						m_mapPeers.erase(itUsage);
					}

					throw;
				}

				m_unUsedBytes += unTotal;
				itUsage->second.m_unBytes += unTotal;
				++itUsage->second.m_unCount;
			}

			Assembly& CurrentAssembly = itFound->second;
			if (CurrentAssembly.m_unTotal != unTotal) {
				++m_unDropped;
				Remove(itFound);
				Remember(AssemblyKey, Clock::now());
				return std::nullopt;
			}

			const std::size_t unChunkIndex = unOffset / kUdpFragmentChunk;
			const std::uint64_t unChunkMask = std::uint64_t(0x01) << unChunkIndex;
			auto itFirst = Fragment.m_vecPayload.begin() + kUdpFragmentHeader;
			if (CurrentAssembly.m_unPresent & unChunkMask) {
				if (!std::equal(itFirst, Fragment.m_vecPayload.end(), CurrentAssembly.m_vecBytes.begin() + unOffset)) {
					++m_unDropped;
					Remove(itFound);
					Remember(AssemblyKey, Clock::now());
				}

				return std::nullopt;
			}

			std::copy(itFirst, Fragment.m_vecPayload.end(), CurrentAssembly.m_vecBytes.begin() + unOffset);
			CurrentAssembly.m_unPresent |= unChunkMask;

			if (++CurrentAssembly.m_unReceived != CurrentAssembly.m_unChunks) {
				return std::nullopt;
			}

			Frame Datagram { FrameType::UdpData, Fragment.m_unFlow, 0, std::move(CurrentAssembly.m_vecBytes) };
			Remove(itFound);
			Remember(AssemblyKey, Clock::now());

			return Datagram;
		}

		void PeerDown(PeerId unPeer) {
			for (auto itAssembly = m_mapAssemblies.begin(); itAssembly != m_mapAssemblies.end();) {
				if (std::get<0>(itAssembly->first) == unPeer) {
					auto itOld = itAssembly++;
					Remove(itOld);
				} else {
					++itAssembly;
				}
			}

			for (auto itRecent = m_mapRecent.begin(); itRecent != m_mapRecent.end();) {
				if (std::get<0>(itRecent->first) == unPeer) {
					itRecent = m_mapRecent.erase(itRecent);
				} else {
					++itRecent;
				}
			}
		}

		std::size_t m_unGlobalBytes;
		std::size_t m_unGlobalCount;
		std::size_t m_unPeerBytes;
		std::size_t m_unPeerCount;
		std::chrono::milliseconds m_TimeToLive;
		std::size_t m_unUsedBytes;
		std::uint64_t m_unDropped;
		std::map<Key, Assembly> m_mapAssemblies;
		std::map<PeerId, Usage> m_mapPeers;
		// Bounded short-lived creation timestamps suppress duplicate completion and
		// avoid immediately rebuilding a datagram after conflicting fragments.
		std::map<Key, Clock::time_point> m_mapRecent;
	};

	UdpFragments::UdpFragments(std::size_t unGlobalBytes, std::size_t unGlobalCount, std::size_t unPeerBytes, std::size_t unPeerCount, std::chrono::milliseconds TimeToLive) :
		m_pImplementation(std::make_unique<Implementation>(unGlobalBytes, unGlobalCount, unPeerBytes, unPeerCount, TimeToLive))
	{
	}

	UdpFragments::~UdpFragments() = default;

	std::vector<Frame> UdpFragments::Split(Frame const& Datagram, std::uint32_t unDatagramID) {
		if ((Datagram.m_Type != FrameType::UdpData) || !Datagram.m_unFlow || Datagram.m_unValue || (Datagram.m_vecPayload.size() > kMaxUdp)) {
			throw std::invalid_argument("Invalid UDP datagram to fragment");
		}

		if (Datagram.m_vecPayload.size() <= kUdpFragmentChunk) {
			return { Datagram };
		}

		if (!unDatagramID) {
			throw std::invalid_argument("UDP fragment datagram ID cannot be zero");
		}

		std::vector<Frame> vecFragments;
		vecFragments.reserve((Datagram.m_vecPayload.size() + kUdpFragmentChunk - 1) / kUdpFragmentChunk);
		for (std::size_t unOffset = 0; unOffset < Datagram.m_vecPayload.size(); unOffset += kUdpFragmentChunk) {
			const std::size_t unCount = std::min(kUdpFragmentChunk, Datagram.m_vecPayload.size() - unOffset);
			Frame Fragment { FrameType::UdpFragment, Datagram.m_unFlow, unDatagramID, {} };
			Fragment.m_vecPayload.resize(kUdpFragmentHeader + unCount);
			WriteFragmentU32BE(Fragment.m_vecPayload.data(), static_cast<std::uint32_t>(Datagram.m_vecPayload.size()));
			WriteFragmentU32BE(Fragment.m_vecPayload.data() + kU32Bytes, static_cast<std::uint32_t>(unOffset));
			std::copy_n(Datagram.m_vecPayload.begin() + unOffset, unCount, Fragment.m_vecPayload.begin() + kUdpFragmentHeader);
			vecFragments.push_back(std::move(Fragment));
		}

		return vecFragments;
	}

	std::optional<Frame> UdpFragments::Accept(PeerId unPeer, Frame const& Fragment) {
		return m_pImplementation->Accept(unPeer, Fragment);
	}

	void UdpFragments::PeerDown(PeerId unPeer) {
		m_pImplementation->PeerDown(unPeer);
	}

	void UdpFragments::Tick() {
		m_pImplementation->Tick();
	}

	std::uint64_t UdpFragments::Dropped() const {
		return m_pImplementation->m_unDropped;
	}
} // namespace st
