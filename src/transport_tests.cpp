
// General
#include <winsock2.h>
#include <steam/steam_api.h>

// C++
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <utility>

// STL
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Project
#include "steam_bridge.hpp"
#include "transport/connection_gate.hpp"
#include "transport/net.hpp"
#include "transport/udp_fragments.hpp"

namespace ws {
	namespace {
		using Clock = std::chrono::steady_clock;
		using Bytes = std::vector<std::uint8_t>;
		using st::Frame;
		using st::FrameType;

		constexpr std::size_t kReceiveBatchCapacity = 64;
		constexpr std::uint16_t kReliableLane = 0;
		constexpr std::uint16_t kUnreliableLane = 1;

		static_assert(st::kMaxFrame <= static_cast<std::size_t>(std::numeric_limits<int>::max()));
		static_assert(k_HSteamNetConnection_Invalid == 0);
		static_assert(k_HSteamNetPollGroup_Invalid == 0);

		struct SDKMessageDeleter {
			void operator()(SteamNetworkingMessage_t* pMessage) const noexcept {
				try {
					pMessage->Release();
				} catch (...) {
					OutputDebugStringA("WireSteam: SDK test message release failed.\n");
				}
			}
		};

		struct ReceivedBatch {
			ReceivedBatch() :
				m_arrMessages {}
			{
				m_nCount = 0;
			}

			~ReceivedBatch() {
				// Every non-null slot is owned immediately on receive, before validation or handlers can throw.
				for (auto& pMessage : m_arrMessages) {
					SteamNetworkingMessage_t* const pOwned = std::exchange(pMessage, nullptr);
					if (pOwned) {
						SDKMessageDeleter {}(pOwned);
					}
				}
			}

			ReceivedBatch(ReceivedBatch const&) = delete;
			ReceivedBatch& operator=(ReceivedBatch const&) = delete;

			std::array<SteamNetworkingMessage_t*, kReceiveBatchCapacity> m_arrMessages;
			int m_nCount;
		};

		struct WSAContext {
			WSAContext() {
				WSADATA StartupData {};
				const int nResult = WSAStartup(MAKEWORD(2, 2), &StartupData);
				if (nResult != 0) {
					throw std::runtime_error("WSAStartup failed");
				}
			}

			~WSAContext() { /* Matching terminal best-effort release of the successful startup reference. */
				WSACleanup();
			}

			WSAContext(WSAContext const&) = delete;
			WSAContext& operator=(WSAContext const&) = delete;
		};

		void Check(bool bCondition, std::string const& strError) {
			if (!bCondition) {
				throw std::runtime_error(strError);
			}
		}

		Bytes GeneratePattern(std::size_t unSize, unsigned int unSeed) {
			constexpr std::size_t kMultiplier = 31;
			constexpr std::size_t kDivisor = 127;
			constexpr std::size_t kByteMask = 0xFF;

			Bytes vecResult(unSize);
			for (std::size_t unIndex = 0; unIndex < unSize; ++unIndex) {
				vecResult[unIndex] = static_cast<std::uint8_t>((unIndex * kMultiplier + unIndex / kDivisor + unSeed) & kByteMask);
			}

			return vecResult;
		}

		template <class Predicate, class Step>
		void WaitUntil(Predicate fnIsFinished, Step fnPerformStep, char const* pszDescription, int nSeconds = 10) {
			Clock::time_point const Deadline = Clock::now() + std::chrono::seconds(nSeconds);
			while (!fnIsFinished()) {
				Check(Clock::now() < Deadline, std::string("Timed out: ") + pszDescription);
				fnPerformStep();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		Frame CreatePublicHello(FrameType Type, Bytes const& vecSender, Bytes const& vecRecipient) {
			Frame FrameValue { Type, 0, 0, vecSender };
			FrameValue.m_vecPayload.insert(FrameValue.m_vecPayload.end(), vecRecipient.begin(), vecRecipient.end());

			return FrameValue;
		}

		void TestConnectionGate() {
			using transport::ConnectionGate;
			constexpr std::uint64_t kExpectedSteamID = 76561198000000001;

			SteamNetConnectionInfo_t Information {};
			std::string strReason;
			for (auto const& State : { k_ESteamNetworkingConnectionState_Connecting, k_ESteamNetworkingConnectionState_FindingRoute }) {
				Information.m_eState = State;
				Check((transport::ClassifyConnection(Information, kExpectedSteamID, strReason) == ConnectionGate::Pending) && !strReason.empty(), "Pending route was treated as an authentication failure");

				Information.m_identityRemote.SetSteamID64(kExpectedSteamID);
				Information.m_nFlags = k_nSteamNetworkConnectionInfoFlags_Unauthenticated | k_nSteamNetworkConnectionInfoFlags_Unencrypted;
				Check(transport::ClassifyConnection(Information, kExpectedSteamID, strReason) == ConnectionGate::Pending, "Pending route was promoted to authenticated or rejected prematurely");

				Information.m_nFlags = 0;
				Check(transport::ClassifyConnection(Information, kExpectedSteamID, strReason) == ConnectionGate::Pending, "A pending connection bypassed the Connected gate");
			}

			Information.m_eState = k_ESteamNetworkingConnectionState_Connected;
			Check((transport::ClassifyConnection(Information, kExpectedSteamID, strReason) == ConnectionGate::Authenticated) && strReason.empty(), "Verified pending-to-connected transition did not open the authentication gate");

			Information.m_identityRemote.SetSteamID64(kExpectedSteamID + 1);
			Check((transport::ClassifyConnection(Information, kExpectedSteamID, strReason) == ConnectionGate::Rejected) && !strReason.empty(), "Connected peer with another SteamID was accepted");

			Information.m_identityRemote.Clear();
			Check(transport::ClassifyConnection(Information, kExpectedSteamID, strReason) == ConnectionGate::Rejected, "Connected peer without a Steam identity was accepted");

			Information.m_identityRemote.SetSteamID64(kExpectedSteamID);
			Check(transport::ClassifyConnection(Information, 0, strReason) == ConnectionGate::Rejected, "An empty configured Steam identity was accepted");

			for (auto const& nFlag : { k_nSteamNetworkConnectionInfoFlags_Unauthenticated, k_nSteamNetworkConnectionInfoFlags_Unencrypted }) {
				Information.m_nFlags = nFlag;
				Check((transport::ClassifyConnection(Information, kExpectedSteamID, strReason) == ConnectionGate::Rejected) && !strReason.empty(), "An insecure connected peer was accepted");
			}

			Information.m_nFlags = 0;
			for (auto const& State : { k_ESteamNetworkingConnectionState_None, k_ESteamNetworkingConnectionState_ClosedByPeer, k_ESteamNetworkingConnectionState_ProblemDetectedLocally, k_ESteamNetworkingConnectionState_FinWait, k_ESteamNetworkingConnectionState_Linger, k_ESteamNetworkingConnectionState_Dead }) {
				Information.m_eState = State;
				Check((transport::ClassifyConnection(Information, kExpectedSteamID, strReason) == ConnectionGate::Ended) && !strReason.empty(), "A terminal connection opened the authentication gate");
			}

			Information.m_eState = k_ESteamNetworkingConnectionState_ClosedByPeer;
			Information.m_eEndReason = 1002;
			Check(strcpy_s(Information.m_szEndDebug, "remote diagnostic") == 0, "Cannot initialize the connection diagnostic fixture");
			Check((transport::ClassifyConnection(Information, kExpectedSteamID, strReason) == ConnectionGate::Ended) && (strReason.find("1002") != std::string::npos) && (strReason.find("remote diagnostic") != std::string::npos), "Terminal connection lost Steam's actual termination reason");

			std::cout << "[PASS] Connection gate: pending route, authenticated transition, identity/crypto rejection and closed-before-drain\n";
		}

		void TestCodec() {
			constexpr std::array<std::size_t, 7> kCorruptOffsets { 0x00, 0x04, 0x06, 0x07, 0x08, 0x0C, 0x10 };

			Frame const Hello = CreatePublicHello(FrameType::Hello, GeneratePattern(0x20, 5), GeneratePattern(0x20, 8));
			std::array<Frame, 4> const arrSamples {
				Hello, CreatePublicHello(FrameType::HelloAck, GeneratePattern(0x20, 8), GeneratePattern(0x20, 5)),
				Frame { FrameType::UdpData, 1, 0, {} }, Frame { FrameType::UdpData, 1, 0, GeneratePattern(1024, 55) }
			};
			std::string strError;
			for (auto const& Sample : arrSamples) {
				Bytes const vecEncoded = st::Encode(Sample);
				Frame Decoded {};
				const bool bDecoded = st::Decode(vecEncoded.data(), vecEncoded.size(), Decoded, strError);
				Check(bDecoded, "Codec rejected a valid frame: " + strError);
				Check((Decoded.m_Type == Sample.m_Type) && (Decoded.m_unFlow == Sample.m_unFlow) && (Decoded.m_unValue == Sample.m_unValue) && (Decoded.m_vecPayload == Sample.m_vecPayload), "Codec changed frame contents");

				for (std::size_t unSize = 0; unSize < st::kWireHeader; ++unSize) {
					Check(!st::Decode(vecEncoded.data(), unSize, Decoded, strError), "Codec accepted a truncated header");
				}
			}

			Bytes const vecOriginal = st::Encode(Hello);
			for (auto const& unOffset : kCorruptOffsets) {
				Bytes vecDamaged = vecOriginal;
				vecDamaged[unOffset] ^= 0x80;
				Frame Decoded {};
				Check(!st::Decode(vecDamaged.data(), vecDamaged.size(), Decoded, strError), "Codec accepted damaged control metadata");
			}

			Bytes vecUnsupported = vecOriginal;
			vecUnsupported[0x05] = 0x63;
			Frame Decoded {};
			Check(!st::Decode(vecUnsupported.data(), vecUnsupported.size(), Decoded, strError), "Codec accepted an unknown type");

			Bytes vecTruncated = vecOriginal;
			vecTruncated.pop_back();
			Check(!st::Decode(vecTruncated.data(), vecTruncated.size(), Decoded, strError), "Codec accepted a truncated key");

			Bytes vecOversized(st::kMaxFrame + 1);
			Check(!st::Decode(vecOversized.data(), vecOversized.size(), Decoded, strError), "Codec accepted an oversized frame");

			for (auto const& Invalid : { Frame { FrameType::Hello, 0, 0, GeneratePattern(63, 1) }, Frame { FrameType::UdpData, 2, 0, {} }, Frame { FrameType::UdpData, 1, 0, GeneratePattern(1025, 1) } }) {
				bool bRejected = false;
				try {
					st::Encode(Invalid);
				} catch (std::invalid_argument const&) {
					bRejected = true;
				}

				Check(bRejected, "Codec encoded invalid protocol input");
			}

			Check(!st::Decode(nullptr, 0, Decoded, strError), "Codec accepted null empty input");
			Check(!st::Decode(nullptr, st::kWireHeader, Decoded, strError), "Codec accepted a null frame range");

			std::cout << "[PASS] WireSteam codec: key handshakes, datagram boundaries, malformed and bounded input\n";
		}

		void TestFragments() {
			Bytes const vecPayload = GeneratePattern(st::kMaxUdp, 57);
			std::vector<Frame> vecParts = st::UdpFragments::Split(Frame { FrameType::UdpData, 1, 0, vecPayload }, 111);
			Check(vecParts.size() == 64, "Maximum datagram fragment count is wrong");
			st::UdpFragments Assembly;
			std::optional<Frame> Complete;
			std::string strError;
			for (auto itFragment = vecParts.rbegin(); itFragment != vecParts.rend(); ++itFragment) {
				Bytes const vecWire = st::Encode(*itFragment);
				Check(vecWire.size() <= st::kMaxFrame, "Fragment exceeds the wire limit");
				Frame Decoded {};
				Check(st::Decode(vecWire.data(), vecWire.size(), Decoded, strError), "Fragment codec failed");
				Complete = Assembly.Accept(1, Decoded);

				if ((itFragment + 1) != vecParts.rend()) {
					Check(!Complete, "Incomplete datagram was delivered");
				}
			}

			Check(Complete && (Complete->m_vecPayload == vecPayload), "Reordered maximum datagram was corrupted");

			for (auto const& Part : vecParts) {
				Check(!Assembly.Accept(1, Part), "Duplicate completed datagram was delivered twice");
			}

			Frame Conflicting = vecParts.front();
			st::UdpFragments Conflict;
			Check(!Conflict.Accept(1, Conflicting), "Single fragment completed a large datagram");
			Conflicting.m_vecPayload.back() ^= 0x01;
			Check(!Conflict.Accept(1, Conflicting) && (Conflict.Dropped() == 1), "Conflicting duplicate was not rejected");

			for (auto const& Part : vecParts) {
				Check(!Conflict.Accept(1, Part), "Conflicting datagram escaped its tombstone");
			}

			st::UdpFragments Limited(st::kMaxUdp, 1, st::kMaxUdp, 1);
			Check(!Limited.Accept(1, vecParts.front()), "Bounded assembler completed early");
			Frame Second = vecParts.front();
			Second.m_unValue = 222;
			Check(!Limited.Accept(2, Second) && (Limited.Dropped() == 1), "Global reassembly budget was exceeded");
			Limited.PeerDown(1);

			for (auto& Part : vecParts) {
				Part.m_unValue = 222;
			}

			for (auto const& Part : vecParts) {
				Complete = Limited.Accept(2, Part);
			}

			Check(Complete && (Complete->m_vecPayload == vecPayload), "PeerDown did not release reassembly budget");

			st::UdpFragments Expiring(st::kMaxUdp, 1, st::kMaxUdp, 1, std::chrono::milliseconds(5));
			Expiring.Accept(1, vecParts.front());
			std::this_thread::sleep_for(std::chrono::milliseconds(8));
			Expiring.Tick();
			Check(Expiring.Dropped() == 1, "Incomplete datagram did not expire");

			st::UdpFragments Malformed;
			for (std::size_t unLength = 0; unLength <= (st::kUdpFragmentHeader + 1); ++unLength) {
				Frame const Invalid { FrameType::UdpFragment, 1, 333, Bytes(unLength) };
				Check(!Malformed.Accept(1, Invalid), "Malformed fragment header produced a datagram");
			}

			st::UdpFragments LongLived(st::kMaxUdp, 1, st::kMaxUdp, 1, std::chrono::milliseconds::max());
			Check(!LongLived.Accept(1, vecParts.front()), "Maximum-lifetime reassembly completed early");
			LongLived.Tick();
			Check(LongLived.Dropped() == 0, "Maximum positive lifetime expired an incomplete datagram");

			for (auto itFragment = vecParts.begin() + 1; itFragment != vecParts.end(); ++itFragment) {
				Complete = LongLived.Accept(1, *itFragment);
			}

			Check(Complete && (Complete->m_vecPayload == vecPayload), "Maximum positive lifetime corrupted reassembly");

			LongLived.Tick();
			Check(!LongLived.Accept(1, vecParts.front()), "Maximum positive lifetime lost its duplicate tombstone");

			for (auto const& Lifetime : { std::chrono::milliseconds(0), std::chrono::milliseconds(-1) }) {
				bool bRejected = false;
				try {
					st::UdpFragments InvalidLifetime(st::kMaxUdp, 1, st::kMaxUdp, 1, Lifetime);
				} catch (std::invalid_argument const& Exception) {
					bRejected = std::strcmp(Exception.what(), "UDP reassembly limits and lifetime must be positive") == 0;
				}

				Check(bRejected, "Nonpositive reassembly lifetime changed its rejection contract");
			}

			std::cout << "[PASS] Unreliable fragments: 65507 bytes, reverse order, duplicates, conflicts, budgets, expiry and disconnect cleanup\n";
		}

		void TestSockets() {
			constexpr std::size_t kPeerCount = 2;
			constexpr int kReceiveCapacity = 8;

			WSAContext WSAState;
			std::string strError;
			st::net::Socket WireGuard = st::net::Bind({ "127.0.0.1", 0 }, SOCK_DGRAM, strError);
			Check(static_cast<bool>(WireGuard), strError);
			st::Endpoint const Target = st::net::Bound(WireGuard.get());
			st::net::Address Destination {};
			Check(st::net::Resolve(Target, SOCK_DGRAM, Destination, strError), strError);
			std::array<st::net::Socket, kPeerCount> arrPeers;
			std::array<std::uint16_t, kPeerCount> arrPorts {};
			for (std::size_t unSide = 0; unSide < arrPeers.size(); ++unSide) {
				arrPeers[unSide] = st::net::Bind({ "127.0.0.1", 0 }, SOCK_DGRAM, strError);
				Check(static_cast<bool>(arrPeers[unSide]), strError);
				st::net::ConfigureUdp(arrPeers[unSide].get());
				Check(connect(arrPeers[unSide].get(), Destination.ptr(), Destination.m_nSize) == 0, "Cannot connect adapter socket");
				arrPorts[unSide] = st::net::Bound(arrPeers[unSide].get()).m_unPort;
				const char chRequest = static_cast<char>('A' + unSide);
				Check(send(arrPeers[unSide].get(), &chRequest, 1, 0) == 1, "Cannot send to simulated WireGuard");
			}

			Check(arrPorts[0] && arrPorts[1] && (arrPorts[0] != arrPorts[1]), "Peer adapter endpoints are not distinct");
			st::net::Socket Conflict = st::net::Bind({ "127.0.0.1", arrPorts[0] }, SOCK_DGRAM, strError);
			Check(!Conflict, "Exclusive peer endpoint allowed a competing bind");
			std::size_t unReceived = 0;
			WaitUntil([&unReceived]() -> bool {
				return unReceived == kPeerCount;
			},
				[&WireGuard, &arrPorts, &unReceived]() -> void {
					std::array<char, kReceiveCapacity> arrBuffer {};
					st::net::Address Source {};
					const int nReceivedBytes = recvfrom(WireGuard.get(), arrBuffer.data(), kReceiveCapacity, 0, Source.ptr(), &Source.m_nSize);
					if ((nReceivedBytes == SOCKET_ERROR) && st::net::WouldBlock(WSAGetLastError())) {
						return;
					}

					Check((nReceivedBytes == 1) && ((arrBuffer[0x00] == 'A') || (arrBuffer[0x00] == 'B')), "Simulated WireGuard received corrupt payload");
					const std::size_t unIndex = static_cast<std::size_t>(arrBuffer[0x00] - 'A');
					Check(st::net::Describe(Source).m_unPort == arrPorts[unIndex], "Incoming peer source port changed");
					Check(sendto(WireGuard.get(), arrBuffer.data(), nReceivedBytes, 0, Source.ptr(), Source.m_nSize) == nReceivedBytes, "Cannot send simulated WireGuard reply");
					++unReceived;
				},
				"simulated WireGuard incoming datagrams");
			for (std::size_t unSide = 0; unSide < arrPeers.size(); ++unSide) {
				bool bReplied = false;
				WaitUntil([&bReplied]() -> bool {
					return bReplied;
				},
					[&arrPeers, &bReplied, unSide]() -> void {
						std::array<char, kReceiveCapacity> arrBuffer {};
						const int nReceivedBytes = recv(arrPeers[unSide].get(), arrBuffer.data(), kReceiveCapacity, 0);
						if ((nReceivedBytes == SOCKET_ERROR) && st::net::WouldBlock(WSAGetLastError())) {
							return;
						}

						Check((nReceivedBytes == 1) && (arrBuffer[0x00] == static_cast<char>('A' + unSide)), "WireGuard reply reached the wrong peer adapter");
						bReplied = true;
					},
					"stable per-peer UDP reply routing");
			}

			// A connected adapter accepts datagrams only from WireGuard's local endpoint.
			st::net::Socket Stranger = st::net::Bind({ "127.0.0.1", 0 }, SOCK_DGRAM, strError);
			Check(static_cast<bool>(Stranger), strError);
			st::net::Address Adapter {};
			Check(st::net::Resolve({ "127.0.0.1", arrPorts[0] }, SOCK_DGRAM, Adapter, strError), strError);
			Check(sendto(Stranger.get(), "X", 1, 0, Adapter.ptr(), Adapter.m_nSize) == 1, "Cannot send unexpected source test");
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			char chByte {};
			const int nReceivedBytes = recv(arrPeers[0].get(), &chByte, 1, 0);
			const int nError = (nReceivedBytes == SOCKET_ERROR) ? WSAGetLastError() : 0;
			Check((nReceivedBytes == SOCKET_ERROR) && st::net::WouldBlock(nError), "Adapter accepted a datagram from an unexpected UDP source");
			Check(st::net::Bound(arrPeers[0].get()).m_unPort == arrPorts[0], "Adapter endpoint changed during traffic");

			std::cout << "[PASS] Exclusive stable loopback endpoints: multiple peers, reply routing and UDP source filtering\n";
		}

		class SteamTest {
			static constexpr std::size_t kPeerCount = 2;

		public:
			SteamTest() :
				m_arrConnections {},
				m_arrKeys { GeneratePattern(0x20, 7), GeneratePattern(0x20, 61) }
			{
				m_bInitialized = false;
				m_bClosed = false;
				m_pSockets = nullptr;
				m_pUtilities = nullptr;
				m_hPollGroup = k_HSteamNetPollGroup_Invalid;
			}

			SteamTest(SteamTest const&) = delete;
			SteamTest& operator=(SteamTest const&) = delete;

			~SteamTest() {
				try {
					m_Callback.Unregister();

					// Terminal close/destroy statuses are best effort; shutdown ends this owned SDK session.
					if (m_pSockets) {
						for (auto const& hConnection : m_arrConnections) {
							if (hConnection) {
								m_pSockets->CloseConnection(hConnection, 1000, "WireSteam SDK test cleanup", false);
							}
						}

						if (m_hPollGroup) {
							m_pSockets->DestroyPollGroup(m_hPollGroup);
						}
					}

					if (m_bInitialized) {
						SteamAPI_Shutdown();
					}
				} catch (...) {
					OutputDebugStringA("WireSteam: SDK test cleanup could not complete.\n");
					std::terminate();
				}
			}

			void Run(std::uint32_t unAppID) {
				constexpr std::size_t kLaneCount = 2;
				constexpr int kSendBufferBytes = 256 * 1024;
				constexpr int kSendRateBytes = 4 * 1024 * 1024;

				std::array<int, kLaneCount> const arrPriorities { 0, 0 };
				std::array<uint16, kLaneCount> const arrWeights { 2, 1 };

				std::string const strAppID = std::to_string(unAppID);
				Check(SetEnvironmentVariableA("SteamAppId", strAppID.c_str()) && SetEnvironmentVariableA("SteamGameId", strAppID.c_str()), "Cannot set Steam AppID");
				SteamErrMsg szError {};
				const ESteamAPIInitResult InitializationResult = SteamAPI_InitEx(&szError);
				m_bInitialized = InitializationResult == k_ESteamAPIInitResult_OK;
				Check(m_bInitialized, "SteamAPI_InitEx failed: " + std::string(szError));
				Check(SteamUser() && SteamUser()->BLoggedOn(), "Steam SDK test requires a signed-in Steam account");
				Check(SteamUtils() && (SteamUtils()->GetAppID() == unAppID), "Steam selected an unexpected AppID");
				m_pSockets = SteamNetworkingSockets();
				m_pUtilities = SteamNetworkingUtils();
				Check(m_pSockets && m_pUtilities, "SteamNetworkingSockets013 is unavailable");
				m_Callback.Register(this, &SteamTest::OnConnection);
				Check(m_pSockets->CreateSocketPair(&m_arrConnections[0], &m_arrConnections[1], true, nullptr, nullptr), "Steam network-loopback socket pair failed");
				m_hPollGroup = m_pSockets->CreatePollGroup();
				Check(m_hPollGroup != k_HSteamNetPollGroup_Invalid, "Cannot create Steam poll group");

				for (auto const& hConnection : m_arrConnections) {
					Check(m_pSockets->ConfigureConnectionLanes(hConnection, static_cast<int>(arrPriorities.size()), arrPriorities.data(), arrWeights.data()) == k_EResultOK, "Cannot configure SDK lanes");
					Check(m_pUtilities->SetConnectionConfigValueInt32(hConnection, k_ESteamNetworkingConfig_SendBufferSize, kSendBufferBytes), "Cannot set SDK send buffer");
					Check(m_pUtilities->SetConnectionConfigValueInt32(hConnection, k_ESteamNetworkingConfig_RecvMaxMessageSize, static_cast<int>(st::kMaxFrame)), "Cannot bound SDK receive message size");
					Check(m_pUtilities->SetConnectionConfigValueInt32(hConnection, k_ESteamNetworkingConfig_SendRateMin, kSendRateBytes) && m_pUtilities->SetConnectionConfigValueInt32(hConnection, k_ESteamNetworkingConfig_SendRateMax, kSendRateBytes), "Cannot configure SDK send rate");
				}

				TestHandshakes();
				TestDatagrams();
				const bool bClosedSuccessfully = m_pSockets->CloseConnection(m_arrConnections[0], 1000, "WireSteam callback test", false);
				if (bClosedSuccessfully) {
					m_arrConnections[0] = k_HSteamNetConnection_Invalid;
				}

				Check(bClosedSuccessfully, "Cannot close SDK connection");
				WaitUntil([this]() -> bool {
					return m_bClosed;
				},
					[]() -> void {
						SteamAPI_RunCallbacks();
					},
					"Steam peer-close callback");

				std::cout << "[PASS] Steam SDK callback dispatch and connection cleanup\n";
				std::cout << "Real Steam SDK network-loopback tests passed. Remote P2P, NAT traversal and SDR are not tested by this mode.\n";
			}

		private:
			// Dispatch is confined to this test thread, which alone calls SteamAPI_RunCallbacks.
			void OnConnection(SteamNetConnectionStatusChangedCallback_t* pEvent) noexcept {
				if (!pEvent) {
					return;
				}

				if ((pEvent->m_hConn == m_arrConnections[1]) && (pEvent->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer)) {
					m_bClosed = true;
				}
			}

			bool SendFrame(std::size_t unSide, Frame const& FrameValue) {
				Check(unSide < m_arrConnections.size(), "SDK send side is out of range");
				Bytes const vecWire = st::Encode(FrameValue);
				std::unique_ptr<SteamNetworkingMessage_t, SDKMessageDeleter> pMessage(m_pUtilities->AllocateMessage(static_cast<int>(vecWire.size())));
				Check(pMessage != nullptr, "SDK message allocation failed");
				Check(pMessage->m_pData != nullptr, "SDK message buffer allocation failed");
				std::memcpy(pMessage->m_pData, vecWire.data(), vecWire.size());
				pMessage->m_cbSize = static_cast<int>(vecWire.size());
				pMessage->m_conn = m_arrConnections[unSide];
				pMessage->m_idxLane = st::IsReliable(FrameValue.m_Type) ? kReliableLane : kUnreliableLane;
				pMessage->m_nFlags = st::IsReliable(FrameValue.m_Type) ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoNagle;
				int64 nResult = 0;
				// With bDeleteFailedMessages=true the SDK assumes every message, including failed sends.
				SteamNetworkingMessage_t* pTransferred = pMessage.release();
				m_pSockets->SendMessages(1, &pTransferred, &nResult, true);

				if ((nResult == -static_cast<int64>(k_EResultLimitExceeded)) || (nResult == -static_cast<int64>(k_EResultIgnored))) {
					return false;
				}

				Check(nResult > 0, "SDK send failed: " + std::to_string(nResult));

				return true;
			}

			template <class Handler>
			void ReceiveFrames(Handler fnHandleFrame) {
				constexpr std::size_t kMaximumReceiveTurns = 16;

				for (std::size_t unTurn = 0; unTurn < kMaximumReceiveTurns; ++unTurn) {
					ReceivedBatch Batch;
					Batch.m_nCount = m_pSockets->ReceiveMessagesOnPollGroup(m_hPollGroup, Batch.m_arrMessages.data(), static_cast<int>(Batch.m_arrMessages.size()));
					Check((Batch.m_nCount >= 0) && (static_cast<std::size_t>(Batch.m_nCount) <= Batch.m_arrMessages.size()), "SDK receive failed");

					if (!Batch.m_nCount) {
						return;
					}

					for (std::size_t unMessageIndex = 0; unMessageIndex < static_cast<std::size_t>(Batch.m_nCount); ++unMessageIndex) {
						SteamNetworkingMessage_t const* pMessage = Batch.m_arrMessages[unMessageIndex];
						Check(pMessage && (pMessage->m_cbSize >= 0) && ((pMessage->m_conn == m_arrConnections[0]) || (pMessage->m_conn == m_arrConnections[1])), "Unexpected SDK message connection");
						Frame FrameValue {};
						std::string strError;
						const bool bDecoded = st::Decode(pMessage->m_pData, static_cast<std::size_t>(pMessage->m_cbSize), FrameValue, strError);
						Check(bDecoded, "SDK frame failed validation: " + strError);
						const bool bReliable = st::IsReliable(FrameValue.m_Type);
						Check((pMessage->m_idxLane == (bReliable ? kReliableLane : kUnreliableLane)) && (static_cast<bool>(pMessage->m_nFlags & k_nSteamNetworkingSend_Reliable) == bReliable), "SDK lane or reliability mismatch");
						fnHandleFrame((pMessage->m_conn == m_arrConnections[0]) ? std::size_t(0) : std::size_t(1), FrameValue);
					}

					if (Batch.m_nCount < static_cast<int>(Batch.m_arrMessages.size())) {
						return;
					}
				}
			}

			void TestHandshakes() {
				for (auto const& Type : { FrameType::Hello, FrameType::HelloAck }) {
					std::array<bool, kPeerCount> arrSent {};
					std::array<bool, kPeerCount> arrReceived {};

					if (Type == FrameType::Hello) {
						// Reproduce the important ordering without depending on real SDR
						// timing: HELLO arrives in the SDK queue before the application
						// registers this connection in its authenticated poll group.
						WaitUntil([&]() -> bool {
							if (!arrSent[0] || !arrSent[1]) {
								return false;
							}

							for (auto const& hConnection : m_arrConnections) {
								SteamNetConnectionRealTimeStatus_t Status {};
								if ((m_pSockets->GetConnectionRealTimeStatus(hConnection, &Status, 0, nullptr) != k_EResultOK) ||
									Status.m_cbPendingReliable || Status.m_cbSentUnackedReliable) {
									return false;
								}
							}

							return true;
						},
							[&]() -> void {
								SteamAPI_RunCallbacks();

								for (std::size_t unSide = 0; unSide < m_arrConnections.size(); ++unSide) {
									if (!arrSent[unSide]) {
										arrSent[unSide] = SendFrame(unSide, CreatePublicHello(Type, m_arrKeys[unSide], m_arrKeys[1 - unSide]));
									}
								}
							},
							"reliable HELLO queued before poll-group registration");

						ReceivedBatch EarlyBatch;
						EarlyBatch.m_nCount = m_pSockets->ReceiveMessagesOnPollGroup(m_hPollGroup, EarlyBatch.m_arrMessages.data(), static_cast<int>(EarlyBatch.m_arrMessages.size()));
						Check(EarlyBatch.m_nCount == 0, "An unregistered connection exposed early HELLO to the authenticated poll group");

						for (auto const& hConnection : m_arrConnections) {
							Check(m_pSockets->SetConnectionPollGroup(hConnection, m_hPollGroup), "Cannot register SDK connection after the application gate");
						}
					}

					WaitUntil([&arrReceived]() -> bool {
						return arrReceived[0] && arrReceived[1];
					},
						[&]() -> void {
							SteamAPI_RunCallbacks();

							for (std::size_t unSide = 0; unSide < m_arrConnections.size(); ++unSide) {
								if (!arrSent[unSide]) {
									arrSent[unSide] = SendFrame(unSide, CreatePublicHello(Type, m_arrKeys[unSide], m_arrKeys[1 - unSide]));
								}
							}

							ReceiveFrames([&](std::size_t unSide, Frame const& FrameValue) -> void {
								Check((FrameValue.m_Type == Type) && !arrReceived[unSide], "Unexpected or duplicated SDK handshake");
								Frame const Expected = CreatePublicHello(Type, m_arrKeys[1 - unSide], m_arrKeys[unSide]);
								Check(FrameValue.m_vecPayload == Expected.m_vecPayload, "SDK public-key hello sender/recipient did not match");
								arrReceived[unSide] = true;
							});
						},
						"mutual reliable public-key hello and acknowledgement");

					if (Type == FrameType::Hello) {
						ReceiveFrames([](std::size_t, Frame const&) -> void {
							Check(false, "Delayed HELLO was delivered more than once");
						});

						std::cout << "[PASS] Steam SDK delayed poll registration: early HELLO retained and delivered exactly once after attachment\n";
					}
				}

				std::cout << "[PASS] Steam SDK reliable lane: mutual sender/recipient public-key hello and acknowledgement\n";
			}

			void TestDatagrams() {
				constexpr std::size_t kPayloadCount = 3;
				constexpr std::size_t kExpectedDatagrams = kPeerCount * kPayloadCount;

				std::array<Bytes, kPayloadCount> const arrPayloads { Bytes {}, GeneratePattern(937, 23), GeneratePattern(st::kMaxUdp, 59) };
				std::vector<Frame> vecOutbound;
				for (std::size_t unPayloadIndex = 0; unPayloadIndex < arrPayloads.size(); ++unPayloadIndex) {
					std::vector<Frame> vecParts = st::UdpFragments::Split(Frame { FrameType::UdpData, 1, 0, arrPayloads[unPayloadIndex] }, static_cast<std::uint32_t>(unPayloadIndex + 1));
					vecOutbound.insert(vecOutbound.end(), vecParts.begin(), vecParts.end());
				}

				std::array<std::size_t, kPeerCount> arrSent {};
				std::array<std::array<bool, kPayloadCount>, kPeerCount> arrReceived {};
				std::array<std::unique_ptr<st::UdpFragments>, kPeerCount> arrReassembly;
				for (auto& pAssembler : arrReassembly) {
					pAssembler = std::make_unique<st::UdpFragments>();
				}

				std::size_t unTotal = 0;
				WaitUntil([&unTotal]() -> bool {
					return unTotal == kExpectedDatagrams;
				},
					[&]() -> void {
						SteamAPI_RunCallbacks();

						for (std::size_t unSide = 0; unSide < m_arrConnections.size(); ++unSide) {
							while (arrSent[unSide] < vecOutbound.size()) {
								if (!SendFrame(unSide, vecOutbound[arrSent[unSide]])) {
									break;
								}

								++arrSent[unSide];
							}
						}

						ReceiveFrames([&](std::size_t unSide, Frame const& FrameValue) -> void {
							Check((FrameValue.m_Type == FrameType::UdpData) || (FrameValue.m_Type == FrameType::UdpFragment), "Unexpected SDK datagram type");
							Frame Datagram = FrameValue;
							if (FrameValue.m_Type == FrameType::UdpFragment) {
								std::optional<Frame> Complete = arrReassembly[unSide]->Accept(1, FrameValue);
								if (!Complete) {
									return;
								}

								Datagram = std::move(*Complete);
							}

							std::size_t unMatchedIndex = arrPayloads.size();
							for (std::size_t unPayloadIndex = 0; unPayloadIndex < arrPayloads.size(); ++unPayloadIndex) {
								if (arrPayloads[unPayloadIndex] == Datagram.m_vecPayload) {
									unMatchedIndex = unPayloadIndex;
								}
							}

							Check(unMatchedIndex < arrPayloads.size(), "SDK unreliable datagram was corrupted");

							if (!arrReceived[unSide][unMatchedIndex]) {
								arrReceived[unSide][unMatchedIndex] = true;
								++unTotal;
							}
						});
					},
					"zero, 937 and 65507 byte datagrams in both directions");

				std::cout << "[PASS] Steam SDK unreliable lane: 0, 937 and 65507 bytes per direction; every 1024-byte fragment remains unreliable\n";
			}

			bool m_bInitialized;
			bool m_bClosed;
			ISteamNetworkingSockets* m_pSockets;
			ISteamNetworkingUtils* m_pUtilities;
			std::array<HSteamNetConnection, kPeerCount> m_arrConnections;
			HSteamNetPollGroup m_hPollGroup;
			CCallbackManual<SteamTest, SteamNetConnectionStatusChangedCallback_t> m_Callback;
			std::array<Bytes, kPeerCount> const m_arrKeys;
		};
	} // namespace

	int RunTransportTests(bool bUseSteam, std::uint32_t unAppID) {
		try {
			TestConnectionGate();
			TestCodec();
			TestFragments();
			TestSockets();

			if (bUseSteam) {
				SteamTest SDKTest;
				SDKTest.Run(unAppID);
			}

			std::cout << "All WireSteam transport tests passed.\n";

			return 0;
		} catch (std::exception const& Exception) {
			std::cerr << "[FAIL] WireSteam transport test: " << Exception.what() << '\n';
			return 1;
		}
	}
} // namespace ws
