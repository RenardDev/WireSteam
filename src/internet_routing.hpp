#pragma once

// STL
#include <filesystem>
#include <memory>
#include <string>

namespace ws {
	// Owns a dynamic WFP session and optional legacy endpoint worker. Lifecycle
	// and query calls are caller-confined. Keep it alive for the complete
	// Internet-client adapter lifetime and start it before default routes.
	class InternetClientRouting {
	public:
		InternetClientRouting();
		~InternetClientRouting();
		InternetClientRouting(InternetClientRouting const&) = delete;
		InternetClientRouting& operator=(InternetClientRouting const&) = delete;

		bool Start(std::filesystem::path const& CoreExecutable, std::string const& strOverlayIPv4, std::string& strErrorOut, bool bDeferGuard = false);
		// Deferred startup permits the Steam SDK to resolve/bootstrap before VPN
		// routes exist. Enable the guard before installing those default routes.
		bool EnableGuard(std::string& strErrorOut);
		// Returns true with bBoundOut=false while the adapter/address is absent.
		// Call until bBoundOut=true after the official WireGuard service starts.
		// The initial guard already blocks untunneled application traffic.
		bool TryBindAdapter(std::wstring const& wstrTunnelName, bool& bBoundOut, std::string& strErrorOut);
		// Periodic health check for the Windows 10 endpoint-route worker. Failure
		// keeps the guard in place until the official VPN adapter is removed.
		bool Healthy(std::string& strErrorOut) const;
		// Stops legacy callbacks/worker before closing the session. A failed
		// callback unsubscribe, worker join or cleanup lock terminates.
		void Stop() noexcept;
		// Returns an owned UTF-8 description; copying it can allocate.
		std::string UplinkDescription() const;
		static bool IsSupported(std::string& strReasonOut);
		static bool CheckPreflight(std::string& strReasonOut);
		// Administrator-only diagnostics: validates OS registration inside an
		// always-aborted transaction. It never enables or commits any policy.
		// The explicit physical interface avoids an unrelated active VPN default.
		static bool ProbeRegistration(std::filesystem::path const& DiagnosticExecutable, unsigned unPhysicalInterfaceIndex, std::string& strErrorOut);

	private:
		struct Implementation;
		std::unique_ptr<Implementation> m_pImplementation;
	};

	// Pure validation and read-only OS capability checks; never opens a WFP
	// session or changes route policies, adapters, firewall rules, or services.
	void RunInternetRoutingTests();
} // namespace ws
