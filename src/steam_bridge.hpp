#pragma once

// C++
#include <cstdint>

// STL
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Project
#include "config.hpp"

namespace ws {
	// Thread-safe stop state; these calls do not access bridge-owned storage.
	void RequestStop();
	bool IsStopRequested();

	// Keep one bridge active per process. Operations and destruction stay on one thread;
	// do not pump Steam callbacks concurrently. After PrepareSockets or Start fails,
	// destroy this instance before retrying so its partial resources are released.
	class SteamBridge {
	public:
		SteamBridge(Config const& Configuration, std::string const& strLocalPublicKey, bool bVerbose = false);
		~SteamBridge();
		SteamBridge(SteamBridge const&) = delete;
		SteamBridge& operator=(SteamBridge const&) = delete;
		bool Start(std::string& strErrorOut);
		// Reserves local ports without initializing Steam; Internet mode uses it
		// to configure process routing before Steam creates its network sockets.
		bool PrepareSockets(std::string& strErrorOut);
		bool WaitForGateway(unsigned unTimeoutSeconds, std::string& strErrorOut);
		std::uint16_t WireGuardPort() const;
		std::vector<std::uint16_t> PeerPorts() const;
		// Calls the optional health callback on the owner thread without an internal lock.
		// Keep the bridge alive and do not reenter it from this callback.
		// False stops with result 1; callback exceptions propagate to the caller.
		int Run(unsigned unRunSeconds = 0, std::function<bool()> const& fnIsHealthy = {});

	private:
		struct Implementation;
		std::unique_ptr<Implementation> m_pImplementation;
	};

	// Offline codec/fragment/socket tests; optional real Steam SDK network loopback.
	int RunTransportTests(bool bUseSteam, std::uint32_t unAppID = 480);
} // namespace ws
