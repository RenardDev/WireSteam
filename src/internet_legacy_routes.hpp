#pragma once

// C++
#include <cstdint>

// STL
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ws {
	// Windows 10 transport routing: learn only authenticated executable identities
	// from WFP connection events, then keep short-lived exact /32 routes outside
	// the VPN. The InternetClientRouting WFP guard remains mandatory: other apps
	// must never inherit these destination-based transport exceptions.
	// Lifecycle calls are caller-confined. Health/readiness readers synchronize
	// with the internal worker/callbacks; Stop blocks for both and must not run
	// on either internal thread. Use a fresh instance for another activation.
	// Stop terminates if callback unsubscription or worker joining fails.
	// PrepareAdapter/Stop must not hold the borrowed filter mutex while waiting.
	// The engine and activation mutex are borrowed through completion of Stop;
	// each opaque sublayer key points to a complete GUID record.
	class LegacyInternetRoutes {
	public:
		LegacyInternetRoutes();
		~LegacyInternetRoutes();
		LegacyInternetRoutes(LegacyInternetRoutes const&) = delete;
		LegacyInternetRoutes& operator=(LegacyInternetRoutes const&) = delete;

		static bool IsSupported();
		// Only missing registry paths report false; other open errors conservatively report possible recovery.
		static bool HasRecovery();
		// A live route lease is left alone. Otherwise restore only the protected
		// recorded telemetry settings still matching our intended values.
		static bool RecoverSettings(std::string& strErrorOut);
		// Administrator-only bounded connection-event diagnostic. Observes exact apps
		// and blocks UDP only for the diagnostic executable itself; never starts
		// the route worker or changes adapters/NAT/routes. Virtual uplinks allowed.
		static bool ProbeTelemetry(std::vector<std::filesystem::path> const& vecApplications, unsigned unSeconds, std::string& strReportOut, std::string& strErrorOut);
		// Stage the real endpoint filter builder in the caller's always-aborted
		// diagnostic transaction. Does not create routes, telemetry, or a worker.
		static bool ProbeEndpointPermits(void* pFilterEngine, void const* pSublayerKey, std::filesystem::path const& Application, std::uint64_t unUplinkLUID, std::uint32_t unSourceAddress, std::string& strErrorOut);
		bool Start(void* pFilterEngine, std::vector<std::filesystem::path> const& vecApplications, std::uint64_t unUplinkLUID, std::uint32_t unSourceAddress, std::uint32_t unNextHop, std::uint32_t unOverlayAddress, std::string& strErrorOut);
		// Begin applying routes only after the parent has committed the matching
		// UDP-learning sublayer. The shared mutex serializes all session updates.
		bool ActivateBootstrap(void const* pSublayerKey, std::mutex& FilterMutex, std::string& strErrorOut);
		// Before default routes appear, require a real observed SDR UDP flow and
		// finish every queued route. Otherwise startup fails with normal routing.
		bool PrepareAdapter(std::string& strErrorOut);
		bool Healthy(std::string& strErrorOut) const;
		void Stop() noexcept;

	private:
		struct Implementation;
		std::unique_ptr<Implementation> m_pImplementation;
	};

	void RunLegacyInternetRoutesTests();
} // namespace ws
