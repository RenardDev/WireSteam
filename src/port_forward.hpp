#pragma once

// STL
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Project
#include "config.hpp"

namespace ws {

	// Owns only its explicitly bound TCP/UDP listeners and relay sockets. It never
	// changes Windows routes, firewall rules, or unrelated network interfaces.
	class PortForwarder {
	public:
		// Logger runs on the worker and may query IsRunning; it must not call Start or Stop.
		using Logger = std::function<void(std::string const& strText)>;
		explicit PortForwarder(Logger fnLog = {});
		~PortForwarder() noexcept;
		PortForwarder(PortForwarder const&) = delete;
		PortForwarder& operator=(PortForwarder const&) = delete;

		// All listeners are ready on success; any startup failure rolls them back.
		// Cancellation runs synchronously under the lifecycle lock and must not reenter Start or Stop.
		bool Start(std::vector<PortForwardConfig> const& vecRules, std::string& strErrorOut, std::function<bool()> const& fnIsCancelled = {});
		// Stops all forwarding and joins the worker before the VPN adapter is removed.
		void Stop() noexcept;
		bool IsRunning() const noexcept;

	private:
		struct Implementation;
		std::unique_ptr<Implementation> m_pImplementation;
	};

} // namespace ws
