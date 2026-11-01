#pragma once

// STL
#include <string>

namespace ws {
	// Select only this action's startup errors from the official WireGuard log.
	std::string SelectServiceDiagnostics(std::string const& strLog, std::string const& strTunnelName, std::string const& strSinceLocalTime);
	void RunServiceDiagnosticTests();
} // namespace ws
