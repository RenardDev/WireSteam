#include "steam_bridge.hpp"

// General
#include <winsock2.h>
#include <windows.h>
#include <steam/steam_api.h>

// C++
#include <cstddef>
#include <cstring>
#include <utility>

// STL
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

// Project
#include "steam_identity.hpp"
#include "transport/connection_gate.hpp"
#include "transport/net.hpp"
#include "transport/udp_fragments.hpp"

namespace ws {
	namespace {
		using Clock = std::chrono::steady_clock;

		struct SteamMessageDeleter {
			void operator()(SteamNetworkingMessage_t* pMessage) const noexcept {
				try {
					pMessage->Release();
				} catch (...) {
					// No SDK release failure may unwind through the owning smart pointer's destructor.
					OutputDebugStringA("WireSteam: Steam message release failed.\n");
				}
			}
		};

		constinit std::atomic<bool> g_bStopping = false;
		constinit std::atomic<bool> g_bVerboseSteam = false;
		constinit std::mutex g_LogMutex;

		void Log(std::string const& strText) {
			constexpr int kTimestampFieldWidth = 2;

			// Serialize each complete record and its flush; this sink does not call back into Log.
			std::lock_guard<std::mutex> Lock(g_LogMutex);
			SYSTEMTIME Time {};
			GetSystemTime(&Time);

			std::cout << '[' << std::setfill('0') << std::setw(kTimestampFieldWidth) << Time.wHour << ':' << std::setw(kTimestampFieldWidth) << Time.wMinute << ':' << std::setw(kTimestampFieldWidth) << Time.wSecond << "Z] " << strText << std::endl;
		}

		void SteamDebug(ESteamNetworkingSocketsDebugOutputType Severity, char const* pszText) {
			try {
				if ((Severity > k_ESteamNetworkingSocketsDebugOutputType_Warning) && !g_bVerboseSteam.load()) {
					return;
				}

				std::string strMessage = pszText ? pszText : "";
				while (!strMessage.empty() && ((strMessage.back() == '\r') || (strMessage.back() == '\n'))) {
					strMessage.pop_back();
				}

				Log("Steam: " + strMessage);
			} catch (...) {
				// The SDK callback must not propagate allocation or stream failures across its boundary.
				OutputDebugStringA("WireSteam: Steam debug callback failed.\n");
			}
		}

		BOOL WINAPI ConsoleHandler(DWORD unEvent) {
			if ((unEvent != CTRL_C_EVENT) && (unEvent != CTRL_BREAK_EVENT) && (unEvent != CTRL_CLOSE_EVENT) && (unEvent != CTRL_LOGOFF_EVENT) && (unEvent != CTRL_SHUTDOWN_EVENT)) {
				return FALSE;
			}

			g_bStopping = true;

			return TRUE;
		}

		std::string DescribeRoute(SteamNetConnectionInfo_t const& Information) {
			if (Information.m_idPOPRelay) {
				return "SDR via " + std::string(SteamNetworkingPOPIDRender(Information.m_idPOPRelay).c_str());
			}

			if (Information.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) {
				return "relayed";
			}

			return "direct";
		}

		// The transport never consumes WireGuard secrets or adapter-only settings.
		Config CreateTransportConfiguration(Config const& Configuration) {
			Config Result;
			Result.m_unListenPort = Configuration.m_unListenPort;
			Result.m_unAppID = Configuration.m_unAppID;
			Result.m_nVirtualPort = Configuration.m_nVirtualPort;
			Result.m_bRelayOnly = Configuration.m_bRelayOnly;
			Result.m_nReconnectSeconds = Configuration.m_nReconnectSeconds;
			Result.m_nSendRate = Configuration.m_nSendRate;
			Result.m_nStatisticsSeconds = Configuration.m_nStatisticsSeconds;
			Result.m_vecPortForwards = Configuration.m_vecPortForwards;
			Result.m_vecPeers.reserve(Configuration.m_vecPeers.size());
			for (auto const& SourcePeer : Configuration.m_vecPeers) {
				PeerConfig Peer;
				Peer.m_strPublicKey = SourcePeer.m_strPublicKey;
				Peer.m_unSteamID = SourcePeer.m_unSteamID;
				Peer.m_nSteamPort = SourcePeer.m_nSteamPort;
				Peer.m_unBridgePort = SourcePeer.m_unBridgePort;
				Peer.m_bDial = SourcePeer.m_bDial;
				Result.m_vecPeers.push_back(std::move(Peer));
			}

			return Result;
		}

		class PollWait {
		public:
			PollWait() {
				m_hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
			}

			PollWait(PollWait const&) = delete;
			PollWait& operator=(PollWait const&) = delete;

			~PollWait() {
				if (m_hTimer) {
					CloseHandle(m_hTimer);
				}
			}

			void Wait() const {
				constexpr LONGLONG kRelativeDelay100Nanoseconds = -20000;
				constexpr DWORD kTimerTimeoutMilliseconds = 100;
				constexpr int kPollingDelayMilliseconds = 2;

				LARGE_INTEGER DueTime {};
				DueTime.QuadPart = kRelativeDelay100Nanoseconds; // Relative two milliseconds, in 100-nanosecond units.
				if (m_hTimer && SetWaitableTimerEx(m_hTimer, &DueTime, 0, nullptr, nullptr, nullptr, 0)) {
					// Pacing is best effort; callers check stopping and deadlines independently.
					WaitForSingleObject(m_hTimer, kTimerTimeoutMilliseconds);

				} else {
					std::this_thread::sleep_for(std::chrono::milliseconds(kPollingDelayMilliseconds));
				}
			}

		private:
			HANDLE m_hTimer;
		};
	} // namespace

	struct SteamBridge::Implementation {
		struct Peer {
			Peer() :
				m_arrPublicKey {}
			{
				m_unPort = 0;
				m_hConnection = k_HSteamNetConnection_Invalid;
				m_unSentBytes = 0;
				m_unReceivedBytes = 0;
				m_unDroppedDatagrams = 0;
				m_unNextDatagram = 1;
			}

			PeerConfig m_Configuration;
			Key m_arrPublicKey;
			st::net::Socket m_Socket;
			std::uint16_t m_unPort;
			HSteamNetConnection m_hConnection;
			Clock::time_point m_NextDial;
			std::uint64_t m_unSentBytes;
			std::uint64_t m_unReceivedBytes;
			std::uint64_t m_unDroppedDatagrams;
			std::uint32_t m_unNextDatagram;
		};

		struct Connection {
			explicit Connection(std::size_t unPeerIndex = 0) :
				m_unPeerIndex(unPeerIndex)
			{
				m_bConnected = false;
				m_bHelloSent = false;
				m_bHelloReceived = false;
				m_bAcknowledgementSent = false;
				m_bAcknowledgementReceived = false;
				m_bReady = false;
				m_CreatedAt = Clock::now();
			}

			std::size_t m_unPeerIndex;
			bool m_bConnected;
			bool m_bHelloSent;
			bool m_bHelloReceived;
			bool m_bAcknowledgementSent;
			bool m_bAcknowledgementReceived;
			bool m_bReady;
			Clock::time_point m_CreatedAt;
			Clock::time_point m_ConnectedAt;
		};

		Implementation(Config const& Configuration, std::string strLocalPublicKey, bool bVerbose) :
			m_Configuration(CreateTransportConfiguration(Configuration)),
			m_strLocalPublicKey(std::move(strLocalPublicKey)),
			m_arrLocalKey {},
			m_bVerbose(bVerbose)
		{
			constexpr int kInitialRelayStatus = 99999;

			m_bWSAInitialized = false;
			m_bSteamInitialized = false;
			m_bConsoleInstalled = false;
			m_bStarted = false;
			m_bPrepared = false;
			m_unWireGuardPort = 0;
			m_pSockets = nullptr;
			m_pUtilities = nullptr;
			m_hListener = k_HSteamListenSocket_Invalid;
			m_hPollGroup = k_HSteamNetPollGroup_Invalid;
			m_unLocalSteamID = 0;
			m_unNextSocketIndex = 0;
			m_nLastRelayStatus = kInitialRelayStatus;
		}

		~Implementation() {
			constexpr int kStoppedReason = 1000;

			m_ConnectionCallback.Unregister();
			m_PersonaCallback.Unregister();

			if (m_bConsoleInstalled) {
				SetConsoleCtrlHandler(ConsoleHandler, FALSE);
			}

			if (m_pSockets) {
				for (auto const& Entry : m_mapConnections) {
					m_pSockets->CloseConnection(Entry.first, kStoppedReason, "WireSteam stopped", false);
				}

				if (m_hListener) {
					m_pSockets->CloseListenSocket(m_hListener);
				}

				if (m_hPollGroup) {
					m_pSockets->DestroyPollGroup(m_hPollGroup);
				}
			}

			m_mapConnections.clear();
			m_vecPeers.clear(); // Winsock must remain initialized until sockets are closed.

			if (m_pUtilities) {
				m_pUtilities->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_None, nullptr);
			}

			if (m_bSteamInitialized) {
				SteamAPI_Shutdown();
			}

			if (m_bWSAInitialized) {
				WSACleanup();
			}
		}

		void PublishPersona(std::uint64_t unSteamID) {
			if (!SteamFriends() || ((unSteamID != m_unLocalSteamID) && !m_mapPeerBySteamID.count(unSteamID))) {
				return;
			}

			char const* pszName = (unSteamID == m_unLocalSteamID) ? SteamFriends()->GetPersonaName() : SteamFriends()->GetFriendPersonaName(CSteamID(unSteamID));
			std::string strName = pszName ? pszName : "";
			auto itPrevious = m_mapPersonaNames.find(unSteamID);
			if ((itPrevious != m_mapPersonaNames.end()) && (itPrevious->second == strName)) {
				return;
			}

			m_mapPersonaNames[unSteamID] = strName;
			Log(SteamIdentityLine(unSteamID, strName, unSteamID == m_unLocalSteamID));
		}

		void OnPersona(PersonaStateChange_t* pEvent) {
			try {
				if (pEvent) {
					PublishPersona(pEvent->m_ulSteamID);
				}
			} catch (...) {
				// Persona publication must not unwind through the SDK callback dispatcher.
				OutputDebugStringA("WireSteam: Steam persona callback failed.\n");
			}
		}

		void RefreshPersonas() {
			constexpr int kPersonaRefreshSeconds = 5;

			if (Clock::now() < m_NextPersonaRefresh) {
				return;
			}

			m_NextPersonaRefresh = Clock::now() + std::chrono::seconds(kPersonaRefreshSeconds);
			PublishPersona(m_unLocalSteamID);
			for (auto const& Peer : m_vecPeers) {
				PublishPersona(Peer.m_Configuration.m_unSteamID);
			}
		}

		std::vector<SteamNetworkingConfigValue_t> BuildConnectionOptions(bool bOutgoing = false) const {
			constexpr int kBufferBytes = 4 * 1024 * 1024;
			constexpr int kReceiveBufferMessages = 4096;
			constexpr int kInitialTimeoutMilliseconds = 20000;
			constexpr int kConnectedTimeoutMilliseconds = 15000;

			std::vector<SteamNetworkingConfigValue_t> vecOptions;
			auto const AddOption = [&](ESteamNetworkingConfigValue OptionKey, int nValue) -> void {
				SteamNetworkingConfigValue_t Option {};
				Option.SetInt32(OptionKey, nValue);
				vecOptions.push_back(Option);
			};
			AddOption(k_ESteamNetworkingConfig_SymmetricConnect, 1);

			if (bOutgoing) {
				AddOption(k_ESteamNetworkingConfig_LocalVirtualPort, m_Configuration.m_nVirtualPort);
			}

			AddOption(k_ESteamNetworkingConfig_SendBufferSize, kBufferBytes);
			AddOption(k_ESteamNetworkingConfig_RecvBufferSize, kBufferBytes);
			AddOption(k_ESteamNetworkingConfig_RecvBufferMessages, kReceiveBufferMessages);
			AddOption(k_ESteamNetworkingConfig_RecvMaxMessageSize, static_cast<int>(st::kMaxFrame));
			AddOption(k_ESteamNetworkingConfig_TimeoutInitial, kInitialTimeoutMilliseconds);
			AddOption(k_ESteamNetworkingConfig_TimeoutConnected, kConnectedTimeoutMilliseconds);
			AddOption(k_ESteamNetworkingConfig_SendRateMin, m_Configuration.m_nSendRate);
			AddOption(k_ESteamNetworkingConfig_SendRateMax, m_Configuration.m_nSendRate);

			if (m_Configuration.m_bRelayOnly) {
				AddOption(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, 0);
			}

			return vecOptions;
		}

		bool ConfigureConnectionLanes(HSteamNetConnection hConnection) {
			constexpr int kLaneCount = 2;
			constexpr int kLanePriorities[kLaneCount] = { 0, 0 };
			constexpr uint16 kLaneWeights[kLaneCount] = { 2, 1 };

			// A P2P peer can send its HELLO while this end is still FindingRoute.
			// Do not expose those messages to ReceiveSteam yet: the SDK retains
			// them in its bounded per-connection queue until MarkConnected verifies
			// the identity and registers the connection in the receive poll group.
			return m_pSockets->ConfigureConnectionLanes(hConnection, kLaneCount, kLanePriorities, kLaneWeights) == k_EResultOK;
		}

		bool PrepareSockets(std::string& strErrorOut) {
			constexpr WORD kWSAVersion = MAKEWORD(2, 2);
			constexpr std::size_t kMaximumPortReservationAttempts = 64;

			if (m_bPrepared) {
				return true;
			}

			if (!DecodeKey(m_strLocalPublicKey, m_arrLocalKey)) {
				strErrorOut = "Invalid local public key";
				return false;
			}

			if (m_Configuration.m_vecPeers.empty()) {
				strErrorOut = "At least one configured Steam peer is required";
				return false;
			}

			WSADATA WSAData {};
			if (WSAStartup(kWSAVersion, &WSAData) != 0) {
				strErrorOut = "WSAStartup failed";
				return false;
			}

			m_bWSAInitialized = true;

			// Hold an ephemeral WireGuard port until all adapter endpoints are bound,
			// so no adapter can accidentally take the port chosen for WireGuard.
			st::net::Socket Reservation;
			m_unWireGuardPort = m_Configuration.m_unListenPort;
			if (!m_unWireGuardPort) {
				for (std::size_t unAttempt = 0; unAttempt < kMaximumPortReservationAttempts; ++unAttempt) {
					Reservation = st::net::Bind({ "127.0.0.1", 0 }, SOCK_DGRAM, strErrorOut);
					if (!Reservation) {
						return false;
					}

					m_unWireGuardPort = st::net::Bound(Reservation.get()).m_unPort;
					if (!m_unWireGuardPort) {
						strErrorOut = "Cannot reserve an ephemeral WireGuard port";
						return false;
					}

					if (!PortForwardConflictsWithWireGuardPort(m_Configuration, m_unWireGuardPort)) {
						break;
					}

					Reservation.reset();
					m_unWireGuardPort = 0;
				}

				if (!m_unWireGuardPort) {
					strErrorOut = "Cannot reserve a WireGuard port distinct from UDP forwarding ports";
					return false;
				}
			}

			m_vecPeers.reserve(m_Configuration.m_vecPeers.size());
			st::net::Address TargetAddress;
			if (!st::net::Resolve({ "127.0.0.1", m_unWireGuardPort }, SOCK_DGRAM, TargetAddress, strErrorOut)) {
				return false;
			}

			for (auto const& PeerConfiguration : m_Configuration.m_vecPeers) {
				Peer CurrentPeer;
				CurrentPeer.m_Configuration = PeerConfiguration;

				if (!PeerConfiguration.m_unSteamID || !DecodeKey(PeerConfiguration.m_strPublicKey, CurrentPeer.m_arrPublicKey)) {
					strErrorOut = "Invalid configured SteamID or peer public key";
					return false;
				}

				if (m_mapPeerBySteamID.count(PeerConfiguration.m_unSteamID)) {
					strErrorOut = "Duplicate peer SteamID";
					return false;
				}

				if (PeerConfiguration.m_unBridgePort && (PeerConfiguration.m_unBridgePort == m_unWireGuardPort)) {
					strErrorOut = "BridgePort must differ from WireGuard ListenPort";
					return false;
				}

				for (std::size_t unAttempt = 0; unAttempt < kMaximumPortReservationAttempts; ++unAttempt) {
					CurrentPeer.m_Socket = st::net::Bind({ "127.0.0.1", PeerConfiguration.m_unBridgePort }, SOCK_DGRAM, strErrorOut);
					if (!CurrentPeer.m_Socket) {
						strErrorOut = "Cannot bind peer bridge: " + strErrorOut;
						return false;
					}

					CurrentPeer.m_unPort = st::net::Bound(CurrentPeer.m_Socket.get()).m_unPort;
					if (!PortForwardConflictsWithBridgePort(m_Configuration, CurrentPeer.m_unPort)) {
						break;
					}

					CurrentPeer.m_Socket.reset();
					CurrentPeer.m_unPort = 0;

					if (PeerConfiguration.m_unBridgePort) {
						strErrorOut = "BridgePort conflicts with a UDP forwarding target";
						return false;
					}
				}

				if (!CurrentPeer.m_unPort) {
					strErrorOut = "Cannot reserve a peer bridge port distinct from UDP forwarding targets";
					return false;
				}

				st::net::ConfigureUdp(CurrentPeer.m_Socket.get());
				if (connect(CurrentPeer.m_Socket.get(), TargetAddress.ptr(), TargetAddress.m_nSize) == SOCKET_ERROR) {
					strErrorOut = "Cannot connect peer socket to WireGuard: " + st::net::Error();
					return false;
				}

				m_mapPeerBySteamID.emplace(PeerConfiguration.m_unSteamID, m_vecPeers.size());
				m_vecPeers.push_back(std::move(CurrentPeer));
			}

			m_bPrepared = true;
			g_bStopping = false;
			m_bConsoleInstalled = SetConsoleCtrlHandler(ConsoleHandler, TRUE) != FALSE;

			return true;
		}

		bool Start(std::string& strErrorOut) {
			if (m_bStarted) {
				strErrorOut = "WireSteam transport is already started";
				return false;
			}

			if (!PrepareSockets(strErrorOut)) {
				return false;
			}

			std::string const strAppID = std::to_string(m_Configuration.m_unAppID);
			if (!SetEnvironmentVariableA("SteamAppId", strAppID.c_str()) || !SetEnvironmentVariableA("SteamGameId", strAppID.c_str())) {
				strErrorOut = "Cannot set process-local Steam AppID";
				return false;
			}

			SteamErrMsg szSteamError {};
			if (SteamAPI_InitEx(&szSteamError) != k_ESteamAPIInitResult_OK) {
				strErrorOut = "Steam initialization failed: " + std::string(szSteamError) + ". Run Steam and WireSteam in the same signed-in Windows user session and access level.";
				return false;
			}

			m_bSteamInitialized = true;

			if (!SteamUser() || !SteamUser()->BLoggedOn()) {
				strErrorOut = "Sign into Steam before starting WireSteam";
				return false;
			}

			if (!SteamUtils() || (SteamUtils()->GetAppID() != m_Configuration.m_unAppID)) {
				strErrorOut = "Steam selected a different AppID; check steam_appid.txt and the launch environment";
				return false;
			}

			const std::uint64_t unLocalSteamID = SteamUser()->GetSteamID().ConvertToUint64();
			m_unLocalSteamID = unLocalSteamID;

			if (m_mapPeerBySteamID.count(unLocalSteamID)) {
				strErrorOut = "A configured peer uses this account's SteamID; use a different Steam account on each computer";
				return false;
			}

			m_pSockets = SteamNetworkingSockets();
			m_pUtilities = SteamNetworkingUtils();
			if (!m_pSockets || !m_pUtilities) {
				strErrorOut = "SteamNetworkingSockets013 is unavailable; update the Steam client";
				return false;
			}

			g_bVerboseSteam = m_bVerbose;
			m_pUtilities->SetDebugOutputFunction(m_bVerbose ? k_ESteamNetworkingSocketsDebugOutputType_Msg : k_ESteamNetworkingSocketsDebugOutputType_Warning, SteamDebug);
			m_ConnectionCallback.Register(this, &Implementation::OnConnection);
			m_PersonaCallback.Register(this, &Implementation::OnPersona);
			m_hPollGroup = m_pSockets->CreatePollGroup();
			if (!m_hPollGroup) {
				strErrorOut = "Cannot create Steam poll group";
				return false;
			}

			std::vector<SteamNetworkingConfigValue_t> vecOptions = BuildConnectionOptions();
			m_hListener = m_pSockets->CreateListenSocketP2P(m_Configuration.m_nVirtualPort, static_cast<int>(vecOptions.size()), vecOptions.data());
			if (!m_hListener) {
				strErrorOut = "Cannot create Steam P2P listener; another WireSteam instance may use this virtual port";
				return false;
			}

			m_pUtilities->InitRelayNetworkAccess();
			m_pSockets->InitAuthentication();
			m_bStarted = true;

			Log("WireSteam | Steamworks SDK 1.65 | SteamNetworkingSockets013 | AppID " + strAppID);
			Log("SteamID " + std::to_string(unLocalSteamID) + " | virtual port " + std::to_string(m_Configuration.m_nVirtualPort));

			if (SteamFriends()) {
				for (auto const& CurrentPeer : m_vecPeers) {
					SteamFriends()->RequestUserInformation(CSteamID(CurrentPeer.m_Configuration.m_unSteamID), true);
				}

				RefreshPersonas();
			}

			Log(m_Configuration.m_bRelayOnly ? "Transport: SDR only (ICE disabled)" : "Transport: Steam P2P with automatic route selection");
			for (auto const& CurrentPeer : m_vecPeers) {
				Log("Peer " + std::to_string(CurrentPeer.m_Configuration.m_unSteamID) + " -> WireGuard endpoint 127.0.0.1:" + std::to_string(CurrentPeer.m_unPort));
			}

			return true;
		}

		void CloseSoon(HSteamNetConnection hConnection, std::string const& strReason) {
			m_mapClosingConnections.try_emplace(hConnection, strReason);
		}

		void Dial(std::size_t unPeerIndex) {
			constexpr int kMinimumReconnectSeconds = 1;

			Peer& CurrentPeer = m_vecPeers[unPeerIndex];
			CurrentPeer.m_NextDial = Clock::now() + std::chrono::seconds(std::max(kMinimumReconnectSeconds, m_Configuration.m_nReconnectSeconds));
			if (!CurrentPeer.m_Configuration.m_bDial || CurrentPeer.m_hConnection) {
				return;
			}

			SteamNetworkingIdentity Identity {};
			Identity.Clear();
			Identity.SetSteamID64(CurrentPeer.m_Configuration.m_unSteamID);
			std::vector<SteamNetworkingConfigValue_t> vecOptions = BuildConnectionOptions(true);
			const int nVirtualPort = (CurrentPeer.m_Configuration.m_nSteamPort < 0) ? m_Configuration.m_nVirtualPort : CurrentPeer.m_Configuration.m_nSteamPort;
			const HSteamNetConnection hConnection = m_pSockets->ConnectP2P(Identity, nVirtualPort, static_cast<int>(vecOptions.size()), vecOptions.data());
			if (!hConnection) {
				Log("Cannot initiate Steam connection to " + std::to_string(CurrentPeer.m_Configuration.m_unSteamID));
				return;
			}

			// In symmetric mode ConnectP2P may have accepted an incoming connection
			// and returned its existing handle. Preserve any state already attached.
			std::pair<std::map<HSteamNetConnection, Connection>::iterator, bool> const Insertion = m_mapConnections.try_emplace(hConnection, Connection { unPeerIndex });
			if (Insertion.first->second.m_unPeerIndex != unPeerIndex) {
				CloseSoon(hConnection, "Symmetric connection identity mismatch");
				return;
			}

			CurrentPeer.m_hConnection = hConnection;

			if (Insertion.second && !ConfigureConnectionLanes(hConnection)) {
				CloseSoon(hConnection, "Cannot configure Steam connection");
				return;
			}

			Log("Connecting to SteamID " + std::to_string(CurrentPeer.m_Configuration.m_unSteamID) + ", virtual port " + std::to_string(nVirtualPort));
		}

		bool MarkConnected(HSteamNetConnection hConnection, Connection& ConnectionState, SteamNetConnectionInfo_t const& Information) {
			std::string strReason;
			const transport::ConnectionGate Gate = transport::ClassifyConnection(Information, m_vecPeers[ConnectionState.m_unPeerIndex].m_Configuration.m_unSteamID, strReason);
			if (Gate == transport::ConnectionGate::Pending) {
				return false;
			}

			if (Gate != transport::ConnectionGate::Authenticated) {
				CloseSoon(hConnection, strReason);
				return false;
			}

			if (!ConnectionState.m_bConnected) {
				if (!m_pSockets->SetConnectionPollGroup(hConnection, m_hPollGroup)) {
					CloseSoon(hConnection, "Cannot register authenticated Steam connection for receiving");
					return false;
				}

				ConnectionState.m_bConnected = true;
				ConnectionState.m_ConnectedAt = Clock::now();
				Log("SteamID " + std::to_string(Information.m_identityRemote.GetSteamID64()) + " connected; route=" + DescribeRoute(Information));
			}

			return true;
		}

		void OnConnection(SteamNetConnectionStatusChangedCallback_t* pEvent) {
			constexpr int kRejectedConnectionReason = 1001;

			try {
				const HSteamNetConnection hConnection = pEvent->m_hConn;
				SteamNetConnectionInfo_t const& Information = pEvent->m_info;
				const std::uint64_t unSteamID = Information.m_identityRemote.GetSteamID64();
				auto itConnection = m_mapConnections.find(hConnection);

				if ((Information.m_eState == k_ESteamNetworkingConnectionState_Connecting) && Information.m_hListenSocket) {
					auto itConfiguredPeer = m_mapPeerBySteamID.find(unSteamID);
					if ((Information.m_hListenSocket != m_hListener) || (itConfiguredPeer == m_mapPeerBySteamID.end())) {
						m_pSockets->CloseConnection(hConnection, kRejectedConnectionReason, "SteamID is not configured", false);
						return;
					}

					Peer& CurrentPeer = m_vecPeers[itConfiguredPeer->second];
					if (CurrentPeer.m_hConnection && (CurrentPeer.m_hConnection != hConnection)) {
						m_pSockets->CloseConnection(hConnection, kRejectedConnectionReason, "A connection for this peer already exists", false);
						return;
					}

					const bool bUntrackedConnection = itConnection == m_mapConnections.end();
					std::pair<std::map<HSteamNetConnection, Connection>::iterator, bool> Insertion {};
					try {
						Insertion = m_mapConnections.try_emplace(hConnection, Connection { itConfiguredPeer->second });
					} catch (...) {
						if (bUntrackedConnection) {
							try {
								m_pSockets->CloseConnection(hConnection, kRejectedConnectionReason, "Cannot track incoming Steam connection", false);
							} catch (...) {
								// Preserve the registration failure for the existing callback boundary.
							}
						}

						throw;
					}

					itConnection = Insertion.first;
					if (itConnection->second.m_unPeerIndex != itConfiguredPeer->second) {
						CloseSoon(hConnection, "Incoming Steam identity mismatch");
						return;
					}

					CurrentPeer.m_hConnection = hConnection;

					if (Insertion.second && !ConfigureConnectionLanes(hConnection)) {
						CloseSoon(hConnection, "Cannot configure incoming connection");
						return;
					}

					const EResult Result = m_pSockets->AcceptConnection(hConnection);
					if ((Result != k_EResultOK) && (Result != k_EResultDuplicateRequest)) {
						CloseSoon(hConnection, "Cannot accept incoming connection");
						return;
					}

					// The queued callback can be stale after a symmetric implicit accept.
					SteamNetConnectionInfo_t CurrentInformation {};
					if (m_pSockets->GetConnectionInfo(hConnection, &CurrentInformation) && (CurrentInformation.m_eState == k_ESteamNetworkingConnectionState_Connected)) {
						MarkConnected(hConnection, itConnection->second, CurrentInformation);
					}
				}

				if (itConnection == m_mapConnections.end()) {
					return;
				}

				if (Information.m_eState == k_ESteamNetworkingConnectionState_Connected) {
					MarkConnected(hConnection, itConnection->second, Information);
				} else if ((Information.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer) || (Information.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)) {
					CloseSoon(hConnection, "Steam connection ended (" + std::to_string(Information.m_eEndReason) + "): " + Information.m_szEndDebug);
				}
			} catch (...) {
				// The SDK dispatcher cannot receive C++ failures from connection bookkeeping or logging.
				OutputDebugStringA("WireSteam: Steam connection callback failed.\n");
			}
		}

		st::Frame BuildHelloFrame(st::FrameType Type, Peer const& CurrentPeer) const {
			st::Frame FrameValue { Type, 0, 0, {} };
			FrameValue.m_vecPayload.insert(FrameValue.m_vecPayload.end(), m_arrLocalKey.begin(), m_arrLocalKey.end());
			FrameValue.m_vecPayload.insert(FrameValue.m_vecPayload.end(), CurrentPeer.m_arrPublicKey.begin(), CurrentPeer.m_arrPublicKey.end());

			return FrameValue;
		}

		st::SendResult SendWire(HSteamNetConnection hConnection, st::Frame const& FrameValue) {
			auto itConnection = m_mapConnections.find(hConnection);
			if ((itConnection == m_mapConnections.end()) || !itConnection->second.m_bConnected || m_mapClosingConnections.count(hConnection)) {
				return st::SendResult::Disconnected;
			}

			if (!st::IsReliable(FrameValue.m_Type) && !itConnection->second.m_bReady) {
				return st::SendResult::WouldBlock;
			}

			std::vector<std::uint8_t> const vecBytes = st::Encode(FrameValue);
			SteamNetworkingMessage_t* pMessage = m_pUtilities->AllocateMessage(static_cast<int>(vecBytes.size()));
			if (!pMessage) {
				return st::SendResult::WouldBlock;
			}

			std::memcpy(pMessage->m_pData, vecBytes.data(), vecBytes.size());
			pMessage->m_cbSize = static_cast<int>(vecBytes.size());
			pMessage->m_conn = hConnection;
			pMessage->m_idxLane = st::IsReliable(FrameValue.m_Type) ? 0 : 1;
			pMessage->m_nFlags = st::IsReliable(FrameValue.m_Type) ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoNagle;
			int64 nSendResult = 0;
			// SDK 1.65 takes ownership even when a send fails.
			m_pSockets->SendMessages(1, &pMessage, &nSendResult, true);

			if (nSendResult > 0) {
				return st::SendResult::Sent;
			}

			if ((nSendResult == -static_cast<int64>(k_EResultLimitExceeded)) || (nSendResult == -static_cast<int64>(k_EResultIgnored))) {
				return st::SendResult::WouldBlock;
			}

			CloseSoon(hConnection, "Steam send failed: " + std::to_string(nSendResult));

			return st::SendResult::Disconnected;
		}

		void CheckReady(Connection& ConnectionState) {
			if (ConnectionState.m_bReady || !ConnectionState.m_bHelloSent || !ConnectionState.m_bHelloReceived || !ConnectionState.m_bAcknowledgementSent || !ConnectionState.m_bAcknowledgementReceived) {
				return;
			}

			ConnectionState.m_bReady = true;
			Log("WIRESTEAM READY with SteamID " + std::to_string(m_vecPeers[ConnectionState.m_unPeerIndex].m_Configuration.m_unSteamID) + " (public-key configuration matched)");
		}

		void ProcessHandshakes() {
			constexpr int kAuthenticationTimeoutSeconds = 30;
			constexpr int kHandshakeTimeoutSeconds = 10;

			Clock::time_point const CurrentTime = Clock::now();
			for (auto& Entry : m_mapConnections) {
				const HSteamNetConnection hConnection = Entry.first;
				Connection& ConnectionState = Entry.second;
				if (m_mapClosingConnections.count(hConnection)) {
					continue;
				}

				if (!ConnectionState.m_bConnected) {
					// Callbacks are snapshots delivered later than the underlying
					// network transition. Poll as well, so pending connections can
					// enter the receive group even before their callback is drained.
					SteamNetConnectionInfo_t CurrentInformation {};
					if (!m_pSockets->GetConnectionInfo(hConnection, &CurrentInformation)) {
						CloseSoon(hConnection, "Steam connection handle is no longer available");
						continue;
					}

					if (!MarkConnected(hConnection, ConnectionState, CurrentInformation)) {
						if (!m_mapClosingConnections.count(hConnection) && ((CurrentTime - ConnectionState.m_CreatedAt) > std::chrono::seconds(kAuthenticationTimeoutSeconds))) {
							CloseSoon(hConnection, "Steam connection timed out while waiting for an authenticated route");
						}

						continue;
					}
				}

				if (ConnectionState.m_bReady) {
					continue;
				}

				if ((CurrentTime - ConnectionState.m_ConnectedAt) > std::chrono::seconds(kHandshakeTimeoutSeconds)) {
					CloseSoon(hConnection, "WireSteam handshake timed out");
					continue;
				}

				Peer& CurrentPeer = m_vecPeers[ConnectionState.m_unPeerIndex];
				if (!ConnectionState.m_bHelloSent && (SendWire(hConnection, BuildHelloFrame(st::FrameType::Hello, CurrentPeer)) == st::SendResult::Sent)) {
					ConnectionState.m_bHelloSent = true;
				}

				if (ConnectionState.m_bHelloSent && ConnectionState.m_bHelloReceived && !ConnectionState.m_bAcknowledgementSent &&
					(SendWire(hConnection, BuildHelloFrame(st::FrameType::HelloAck, CurrentPeer)) == st::SendResult::Sent)) {
					ConnectionState.m_bAcknowledgementSent = true;
				}

				CheckReady(ConnectionState);
			}
		}

		void Deliver(Peer& CurrentPeer, st::Frame const& Datagram) {
			char const* pchBytes = Datagram.m_vecPayload.empty() ? "" : reinterpret_cast<char const*>(Datagram.m_vecPayload.data());
			const int nSentBytes = send(CurrentPeer.m_Socket.get(), pchBytes, static_cast<int>(Datagram.m_vecPayload.size()), 0);
			if ((nSentBytes < 0) || (static_cast<std::size_t>(nSentBytes) != Datagram.m_vecPayload.size())) {
				++CurrentPeer.m_unDroppedDatagrams;
			} else {
				CurrentPeer.m_unReceivedBytes += Datagram.m_vecPayload.size();
			}
		}

		void HandleMessage(SteamNetworkingMessage_t* pMessage) {
			constexpr uint16 kReliableLane = 0;
			constexpr uint16 kUnreliableLane = 1;

			const HSteamNetConnection hConnection = pMessage->m_conn;
			auto itConnection = m_mapConnections.find(hConnection);
			if ((itConnection == m_mapConnections.end()) || m_mapClosingConnections.count(hConnection)) {
				return;
			}

			Connection& ConnectionState = itConnection->second;
			Peer& CurrentPeer = m_vecPeers[ConnectionState.m_unPeerIndex];
			if ((pMessage->m_cbSize < 0) || (static_cast<std::size_t>(pMessage->m_cbSize) > st::kMaxFrame)) {
				CloseSoon(hConnection, "Oversized WireSteam message");
				return;
			}

			// Only MarkConnected can register a handle in this poll group, after
			// validating its connected-state Steam identity and security flags.
			if (!ConnectionState.m_bConnected) {
				CloseSoon(hConnection, "Receive poll group contains an unverified connection");
				return;
			}

			st::Frame FrameValue;
			std::string strError;
			if (!st::Decode(pMessage->m_pData, static_cast<std::size_t>(pMessage->m_cbSize), FrameValue, strError)) {
				CloseSoon(hConnection, "Invalid WireSteam frame: " + strError);
				return;
			}

			const bool bReliable = st::IsReliable(FrameValue.m_Type);
			if ((pMessage->m_idxLane != (bReliable ? kReliableLane : kUnreliableLane)) || (static_cast<bool>(pMessage->m_nFlags & k_nSteamNetworkingSend_Reliable) != bReliable)) {
				CloseSoon(hConnection, "Invalid frame lane or delivery mode");
				return;
			}

			if (bReliable) {
				if (!std::equal(CurrentPeer.m_arrPublicKey.begin(), CurrentPeer.m_arrPublicKey.end(), FrameValue.m_vecPayload.begin()) ||
					!std::equal(m_arrLocalKey.begin(), m_arrLocalKey.end(), FrameValue.m_vecPayload.begin() + m_arrLocalKey.size())) {
					CloseSoon(hConnection, "WireGuard public keys do not match the configured Steam peer");
					return;
				}

				if (FrameValue.m_Type == st::FrameType::Hello) {
					if (ConnectionState.m_bHelloReceived) {
						CloseSoon(hConnection, "Duplicate WireSteam hello");
						return;
					}

					ConnectionState.m_bHelloReceived = true;
				} else {
					if (!ConnectionState.m_bHelloSent || ConnectionState.m_bAcknowledgementReceived) {
						CloseSoon(hConnection, "Unexpected WireSteam hello acknowledgement");
						return;
					}

					ConnectionState.m_bAcknowledgementReceived = true;
				}

				CheckReady(ConnectionState);

			} else if (!ConnectionState.m_bReady) {
				// Unreliable traffic on another lane may overtake the final ACK.
				++CurrentPeer.m_unDroppedDatagrams;
			} else if (FrameValue.m_Type == st::FrameType::UdpData) {
				Deliver(CurrentPeer, FrameValue);
			} else if (auto Datagram = m_Fragments.Accept(hConnection, FrameValue)) {
				Deliver(CurrentPeer, *Datagram);
			}
		}

		void ReceiveSteam() {
			constexpr std::size_t kMaximumBatches = 16;
			constexpr int kMessageBatchCapacity = 64;

			for (std::size_t unBatch = 0; unBatch < kMaximumBatches; ++unBatch) {
				SteamNetworkingMessage_t* arrMessages[kMessageBatchCapacity] {};
				const int nMessageCount = m_pSockets->ReceiveMessagesOnPollGroup(m_hPollGroup, arrMessages, kMessageBatchCapacity);
				if (nMessageCount <= 0) {
					break;
				}

				// Own the complete batch before decoding can allocate or throw.
				std::array<std::unique_ptr<SteamNetworkingMessage_t, SteamMessageDeleter>, kMessageBatchCapacity> arrOwners;
				for (std::size_t unMessageIndex = 0; unMessageIndex < static_cast<std::size_t>(nMessageCount); ++unMessageIndex) {
					arrOwners[unMessageIndex].reset(arrMessages[unMessageIndex]);
				}

				for (std::size_t unMessageIndex = 0; unMessageIndex < static_cast<std::size_t>(nMessageCount); ++unMessageIndex) {
					HandleMessage(arrOwners[unMessageIndex].get());
					arrOwners[unMessageIndex].reset();
				}

				if (nMessageCount < kMessageBatchCapacity) {
					break;
				}
			}
		}

		void ReceiveLocal() {
			constexpr std::size_t kReceiveBufferBytes = st::kMaxUdp + 1;
			constexpr std::size_t kDatagramBudget = 256;
			constexpr std::size_t kMaximumDatagramsPerPeer = 16;
			constexpr std::uint32_t kUDPChannel = 1;

			std::array<char, kReceiveBufferBytes> arrBuffer {};
			std::size_t unDatagramBudget = kDatagramBudget;
			for (std::size_t unVisitedPeers = 0; (unVisitedPeers < m_vecPeers.size()) && unDatagramBudget; ++unVisitedPeers) {
				Peer& CurrentPeer = m_vecPeers[m_unNextSocketIndex++ % m_vecPeers.size()];
				for (std::size_t unTurn = 0; (unTurn < kMaximumDatagramsPerPeer) && unDatagramBudget; ++unTurn) {
					const int nDatagramBytes = recv(CurrentPeer.m_Socket.get(), arrBuffer.data(), static_cast<int>(arrBuffer.size()), 0);
					if (nDatagramBytes == SOCKET_ERROR) {
						const int nErrorCode = WSAGetLastError();
						if (st::net::WouldBlock(nErrorCode)) {
							break;
						}

						++CurrentPeer.m_unDroppedDatagrams;
						--unDatagramBudget;

						if ((nErrorCode == WSAEMSGSIZE) || (nErrorCode == WSAECONNRESET)) {
							continue;
						}

						if (m_bVerbose) {
							Log("Local UDP receive: " + st::net::Error(nErrorCode));
						}

						break;
					}

					--unDatagramBudget;
					auto itConnection = m_mapConnections.find(CurrentPeer.m_hConnection);
					if ((static_cast<std::size_t>(nDatagramBytes) > st::kMaxUdp) || (itConnection == m_mapConnections.end()) || !itConnection->second.m_bReady || m_mapClosingConnections.count(CurrentPeer.m_hConnection)) {
						++CurrentPeer.m_unDroppedDatagrams;
						continue;
					}

					st::Frame Datagram { st::FrameType::UdpData, kUDPChannel, 0, {} };
					Datagram.m_vecPayload.assign(arrBuffer.begin(), arrBuffer.begin() + nDatagramBytes);
					const std::uint32_t unDatagramID = CurrentPeer.m_unNextDatagram++;
					if (!CurrentPeer.m_unNextDatagram) {
						CurrentPeer.m_unNextDatagram = 1;
					}

					bool bSent = true;
					for (auto const& Part : st::UdpFragments::Split(Datagram, unDatagramID)) {
						if (SendWire(CurrentPeer.m_hConnection, Part) != st::SendResult::Sent) {
							bSent = false;
							break;
						}
					}

					if (bSent) {
						CurrentPeer.m_unSentBytes += static_cast<std::size_t>(nDatagramBytes);
					} else {
						++CurrentPeer.m_unDroppedDatagrams;
					}
				}
			}
		}

		void FlushClosures() {
			constexpr int kDisconnectReason = 1002;
			constexpr std::size_t kMaximumReasonBytes = 120;
			constexpr int kMinimumReconnectSeconds = 1;

			for (auto const& Entry : m_mapClosingConnections) {
				const HSteamNetConnection hConnection = Entry.first;
				std::string const& strReason = Entry.second;
				auto itConnection = m_mapConnections.find(hConnection);
				if (itConnection == m_mapConnections.end()) {
					continue;
				}

				Peer& CurrentPeer = m_vecPeers[itConnection->second.m_unPeerIndex];
				Log("Disconnect SteamID " + std::to_string(CurrentPeer.m_Configuration.m_unSteamID) + ": " + strReason);
				m_Fragments.PeerDown(hConnection);
				m_pSockets->CloseConnection(hConnection, kDisconnectReason, strReason.substr(0, kMaximumReasonBytes).c_str(), false);

				if (CurrentPeer.m_hConnection == hConnection) {
					CurrentPeer.m_hConnection = k_HSteamNetConnection_Invalid;
					CurrentPeer.m_NextDial = Clock::now() + std::chrono::seconds(std::max(kMinimumReconnectSeconds, m_Configuration.m_nReconnectSeconds));
				}

				m_mapConnections.erase(itConnection);
			}

			m_mapClosingConnections.clear();
		}

		void LogStatistics() {
			for (auto const& CurrentPeer : m_vecPeers) {
				std::ostringstream Line;

				Line << "Peer " << CurrentPeer.m_Configuration.m_unSteamID << " tx=" << CurrentPeer.m_unSentBytes << "B rx=" << CurrentPeer.m_unReceivedBytes << "B dropped=" << CurrentPeer.m_unDroppedDatagrams;

				SteamNetConnectionRealTimeStatus_t Status {};
				SteamNetConnectionInfo_t Information {};
				if (CurrentPeer.m_hConnection && (m_pSockets->GetConnectionRealTimeStatus(CurrentPeer.m_hConnection, &Status, 0, nullptr) == k_EResultOK) && m_pSockets->GetConnectionInfo(CurrentPeer.m_hConnection, &Information)) {
					Line << " route=" << DescribeRoute(Information) << " ping=" << Status.m_nPing << "ms pending=" << static_cast<std::int64_t>(Status.m_cbPendingReliable) + Status.m_cbPendingUnreliable << 'B';
				}

				Log(Line.str());
			}

			if (m_Fragments.Dropped()) {
				Log("Fragment reassembly dropped=" + std::to_string(m_Fragments.Dropped()));
			}
		}

		bool WaitForGateway(unsigned unTimeoutSeconds, std::string& strErrorOut) {
			if (!m_bStarted) {
				strErrorOut = "Start Steam before waiting for a gateway";
				return false;
			}

			Log("Waiting for the authenticated Steam gateway before enabling Internet routes");

			Clock::time_point const Deadline = Clock::now() + std::chrono::seconds(unTimeoutSeconds);
			PollWait Waiter;
			while (!g_bStopping.load() && (Clock::now() < Deadline)) {
				SteamAPI_RunCallbacks();
				RefreshPersonas();
				ReceiveSteam();
				ProcessHandshakes();
				FlushClosures();

				for (std::size_t unPeerIndex = 0; unPeerIndex < m_vecPeers.size(); ++unPeerIndex) {
					if (!m_vecPeers[unPeerIndex].m_hConnection && m_vecPeers[unPeerIndex].m_Configuration.m_bDial && (Clock::now() >= m_vecPeers[unPeerIndex].m_NextDial)) {
						Dial(unPeerIndex);
					}
				}

				SteamRelayNetworkStatus_t RelayStatus {};
				m_pUtilities->GetRelayNetworkStatus(&RelayStatus);

				if ((RelayStatus.m_eAvail == k_ESteamNetworkingAvailability_Current) &&
					std::any_of(m_mapConnections.begin(), m_mapConnections.end(), [](std::pair<const HSteamNetConnection, Connection> const& Entry) -> bool {
						return Entry.second.m_bReady;
					})) {
					return true;
				}

				Waiter.Wait();
			}

			strErrorOut = g_bStopping.load() ? "Internet activation cancelled" : "The Steam gateway did not become ready. Start its Internet server profile first; Internet routes were not enabled.";

			return false;
		}

		int Run(unsigned unRunSeconds, std::function<bool()> const& fnIsHealthy) {
			constexpr int kRelayRefreshSeconds = 2;

			if (!m_bStarted) {
				Log("Start the Steam bridge before running it");
				return 1;
			}

			Clock::time_point const StartTime = Clock::now();
			Clock::time_point NextStatisticsAt = StartTime + std::chrono::seconds(m_Configuration.m_nStatisticsSeconds);
			Clock::time_point NextRelayStatusAt = StartTime;
			PollWait Waiter;

			while (!g_bStopping.load()) {
				if (fnIsHealthy && !fnIsHealthy()) {
					Log("A tunnel component stopped unexpectedly; stopping the tunnel for recovery");
					return 1;
				}

				SteamAPI_RunCallbacks();
				RefreshPersonas();
				ReceiveSteam();
				m_Fragments.Tick();
				ProcessHandshakes();
				ReceiveLocal();
				FlushClosures();

				Clock::time_point const CurrentTime = Clock::now();
				for (std::size_t unPeerIndex = 0; unPeerIndex < m_vecPeers.size(); ++unPeerIndex) {
					if (!m_vecPeers[unPeerIndex].m_hConnection && m_vecPeers[unPeerIndex].m_Configuration.m_bDial && (CurrentTime >= m_vecPeers[unPeerIndex].m_NextDial)) {
						Dial(unPeerIndex);
					}
				}

				if ((m_Configuration.m_nStatisticsSeconds > 0) && (CurrentTime >= NextStatisticsAt)) {
					LogStatistics();
					NextStatisticsAt = CurrentTime + std::chrono::seconds(m_Configuration.m_nStatisticsSeconds);
				}

				if (CurrentTime >= NextRelayStatusAt) {
					SteamRelayNetworkStatus_t RelayStatus {};
					m_pUtilities->GetRelayNetworkStatus(&RelayStatus);

					if (m_nLastRelayStatus != static_cast<int>(RelayStatus.m_eAvail)) {
						m_nLastRelayStatus = static_cast<int>(RelayStatus.m_eAvail);
						Log("SDR status " + std::to_string(m_nLastRelayStatus) + ": " + RelayStatus.m_debugMsg);
					}

					NextRelayStatusAt = CurrentTime + std::chrono::seconds(kRelayRefreshSeconds);
				}

				if (unRunSeconds && ((CurrentTime - StartTime) >= std::chrono::seconds(unRunSeconds))) {
					break;
				}

				Waiter.Wait();
			}

			LogStatistics();
			Log("Stopping WireSteam transport");

			return 0;
		}

		Config m_Configuration;
		std::string m_strLocalPublicKey;
		Key m_arrLocalKey;
		bool m_bVerbose;
		bool m_bWSAInitialized;
		bool m_bSteamInitialized;
		bool m_bConsoleInstalled;
		bool m_bStarted;
		bool m_bPrepared;
		std::uint16_t m_unWireGuardPort;
		ISteamNetworkingSockets* m_pSockets;
		ISteamNetworkingUtils* m_pUtilities;
		HSteamListenSocket m_hListener;
		HSteamNetPollGroup m_hPollGroup;
		CCallbackManual<Implementation, SteamNetConnectionStatusChangedCallback_t> m_ConnectionCallback;
		CCallbackManual<Implementation, PersonaStateChange_t> m_PersonaCallback;
		std::uint64_t m_unLocalSteamID;
		std::map<std::uint64_t, std::string> m_mapPersonaNames;
		Clock::time_point m_NextPersonaRefresh;
		std::vector<Peer> m_vecPeers;
		std::map<std::uint64_t, std::size_t> m_mapPeerBySteamID;
		std::map<HSteamNetConnection, Connection> m_mapConnections;
		std::map<HSteamNetConnection, std::string> m_mapClosingConnections;
		st::UdpFragments m_Fragments;
		std::size_t m_unNextSocketIndex;
		int m_nLastRelayStatus;
	};

	void RequestStop() {
		g_bStopping = true;
	}

	bool IsStopRequested() {
		return g_bStopping.load();
	}

	SteamBridge::SteamBridge(Config const& Configuration, std::string const& strLocalPublicKey, bool bVerbose) :
		m_pImplementation(std::make_unique<Implementation>(Configuration, strLocalPublicKey, bVerbose))
	{
	}

	SteamBridge::~SteamBridge() = default;

	bool SteamBridge::Start(std::string& strErrorOut) {
		return m_pImplementation->Start(strErrorOut);
	}

	bool SteamBridge::PrepareSockets(std::string& strErrorOut) {
		return m_pImplementation->PrepareSockets(strErrorOut);
	}

	bool SteamBridge::WaitForGateway(unsigned unTimeoutSeconds, std::string& strErrorOut) {
		return m_pImplementation->WaitForGateway(unTimeoutSeconds, strErrorOut);
	}

	std::uint16_t SteamBridge::WireGuardPort() const {
		return m_pImplementation->m_unWireGuardPort;
	}

	std::vector<std::uint16_t> SteamBridge::PeerPorts() const {
		std::vector<std::uint16_t> vecPorts;
		for (auto const& CurrentPeer : m_pImplementation->m_vecPeers) {
			vecPorts.push_back(CurrentPeer.m_unPort);
		}

		return vecPorts;
	}

	int SteamBridge::Run(unsigned unRunSeconds, std::function<bool()> const& fnIsHealthy) {
		return m_pImplementation->Run(unRunSeconds, fnIsHealthy);
	}
} // namespace ws
