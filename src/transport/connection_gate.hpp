#pragma once

// General
#include <steam/steamnetworkingtypes.h>

// C++
#include <cstdint>

// STL
#include <string>

namespace ws::transport {
	enum class ConnectionGate {
		Pending = 0,
		Authenticated,
		Rejected,
		Ended
	};

	// Route negotiation and authentication are distinct. An authenticated message
	// can reach the SDK receive queue before its connection enters Connected.
	// Keep that queue outside the application poll group until this gate opens.
	// Borrows a stable SDK info record and writes an owned reason; SDK string contracts apply.
	// Independent output strings may be used concurrently; allocation failures propagate.
	inline ConnectionGate ClassifyConnection(SteamNetConnectionInfo_t const& ConnectionInformation, std::uint64_t unExpectedSteamID, std::string& strReasonOut) {
		strReasonOut.clear();

		switch (ConnectionInformation.m_eState) {
			case k_ESteamNetworkingConnectionState_Connecting:
				strReasonOut = "Steam connection is still connecting";
				return ConnectionGate::Pending;

			case k_ESteamNetworkingConnectionState_FindingRoute:
				strReasonOut = "Steam connection is still finding a route";
				return ConnectionGate::Pending;

			case k_ESteamNetworkingConnectionState_Connected:
				if (!unExpectedSteamID || (ConnectionInformation.m_identityRemote.GetSteamID64() != unExpectedSteamID)) {
					strReasonOut = "Remote Steam identity does not match the configured peer";
					return ConnectionGate::Rejected;
				}

				if (ConnectionInformation.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Unauthenticated) {
					strReasonOut = "Remote Steam connection is unauthenticated";
					return ConnectionGate::Rejected;
				}

				if (ConnectionInformation.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Unencrypted) {
					strReasonOut = "Remote Steam connection is unencrypted";
					return ConnectionGate::Rejected;
				}

				return ConnectionGate::Authenticated;

			case k_ESteamNetworkingConnectionState_ClosedByPeer:
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
				strReasonOut = "Steam connection ended (" + std::to_string(ConnectionInformation.m_eEndReason) + "): " + ConnectionInformation.m_szEndDebug;
				return ConnectionGate::Ended;

			default:
				strReasonOut = "Steam connection is not active (state " + std::to_string(static_cast<int>(ConnectionInformation.m_eState)) + ")";
				return ConnectionGate::Ended;
		}
	}
} // namespace ws::transport
