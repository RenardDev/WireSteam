#pragma once

// C++
#include <cstdint>

// STL
#include <array>
#include <string>
#include <vector>

namespace ws {
	using Key = std::array<std::uint8_t, 0x20>;

	enum class InternetMode {
		Off = 0,
		Client,
		Server
	};

	enum class PortForwardProtocol {
		Tcp = 0,
		Udp
	};

	struct PortForwardConfig {
		PortForwardProtocol m_Protocol = PortForwardProtocol::Tcp;
		std::string m_strListenAddress;
		std::string m_strTargetAddress;
		std::uint16_t m_unListenPort = 0;
		std::uint16_t m_unTargetPort = 0;
		unsigned m_unMaximumConnections = 64;
		unsigned m_unConnectTimeoutSeconds = 10;
		unsigned m_unIdleTimeoutSeconds = 600;
	};

	struct PeerConfig {
		std::string m_strPublicKey;
		std::string m_strPresharedKey;
		std::vector<std::string> m_vecAllowedIPs;
		std::uint64_t m_unSteamID = 0;
		int m_nSteamPort = -1;			  // -1 inherits the local [WireSteam] VirtualPort.
		std::uint16_t m_unBridgePort = 0; // Ephemeral loopback port by default.
		int m_nKeepalive = 25;
		bool m_bDial = true;
	};

	struct Config {
		std::string m_strPrivateKey;
		std::vector<std::string> m_vecAddresses;
		std::vector<std::string> m_vecDNS;
		std::uint16_t m_unListenPort = 51820;
		int m_nMTU = 1280;
		std::vector<PeerConfig> m_vecPeers;
		std::vector<PortForwardConfig> m_vecPortForwards;
		std::uint32_t m_unAppID = 480;
		int m_nVirtualPort = 0;
		bool m_bRelayOnly = true;
		int m_nReconnectSeconds = 3;
		int m_nSendRate = 4194304;
		int m_nStatisticsSeconds = 10;
		InternetMode m_InternetMode = InternetMode::Off;
		std::string m_strInternetSubnet;
	};

	// Inputs are borrowed; decoded and parsed outputs commit only on success.
	// Returned and parsed owning strings remain caller-owned, including any key material.
	// Calls have no shared mutable state; synchronize access to shared input/output objects.
	bool DecodeKey(std::string const& strText, Key& KeyOut);
	std::string EncodeKey(Key const& KeyValue);
	bool ParseConfig(std::string const& strText, Config& ConfigurationOut, std::string& strErrorOut);
	std::string CompileConfig(Config const& Configuration, std::uint16_t unWireGuardPort, std::vector<std::uint16_t> const& vecPeerPorts);
	std::string RedactedSummary(Config const& Configuration);
	char const* InternetModeName(InternetMode Mode);
	char const* PortForwardProtocolName(PortForwardProtocol Protocol);
	bool PortForwardConflictsWithWireGuardPort(Config const& Configuration, std::uint16_t unPort);
	bool PortForwardConflictsWithBridgePort(Config const& Configuration, std::uint16_t unPort);
	bool ValidateCompiledConfig(std::string const& strText, std::string& strErrorOut, InternetMode Mode = InternetMode::Off, std::string const& strInternetSubnet = {});
	void RunConfigTests();
} // namespace ws
