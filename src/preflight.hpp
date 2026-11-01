#pragma once

// STL
#include <string>

// Project
#include "config.hpp"

namespace ws {
	// Checks local adapter addresses, WireGuard UDP port availability, and existing
	// TCP/UDP bindings overlapping configured VPN forwarding addresses. It never
	// initializes Steam, changes an adapter, or stops another tunnel.
	bool CheckAdapterConflicts(Config const& Configuration, std::string& strErrorOut);
	void RunPreflightTests();
} // namespace ws
