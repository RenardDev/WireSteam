#pragma once

// STL
#include <memory>
#include <string>

namespace ws {

	// Elevated, session-owned IPv4 exit gateway. Start only after the named
	// WireGuard adapter exists; Stop before removing that adapter. Forwarding is
	// changed only on that owned adapter; the current uplink and its routes are
	// observed without reconfiguring another VPN or physical interface.
	// Helper and recovery instances are stack-owned; lifecycle and query calls
	// stay on the thread that acquires the gateway's mutex.
	class InternetServerGateway {
	public:
		InternetServerGateway();
		~InternetServerGateway();
		InternetServerGateway(InternetServerGateway const&) = delete;
		InternetServerGateway& operator=(InternetServerGateway const&) = delete;

		bool Start(std::string const& strAdapterName, std::string const& strSubnet, std::string& strErrorOut);
		// Records cleanup failures; failure to release a confirmed mutex acquisition terminates.
		void Stop() noexcept;

		// Returns an owned diagnostic copy and can allocate.
		std::string CleanupError() const;

		// After a failed Stop, let a fresh elevated down invocation retry only
		// when the complete protected durable record can recover every change.
		bool HandOffRecovery(std::string& strErrorOut);
		// Elevated explicit recovery for a crashed worker. Missing records are a
		// no-op; malformed/unprotected records and live gateway owners are refused.
		static bool Recover(std::string const& strAdapterName, std::string& strErrorOut);
		// Read-only existence check; inaccessible records still require elevated
		// recovery rather than being mistaken for an absent record.
		static bool HasRecovery(std::string const& strAdapterName);

	private:
		struct Implementation;
		std::unique_ptr<Implementation> m_pImplementation;
	};

	// Pure validation/command-builder tests. Does not create NATs, change
	// forwarding, run PowerShell, request UAC, or touch network interfaces.
	void RunInternetServerTests();
} // namespace ws
