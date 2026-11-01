#pragma once

// STL
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

// Project
#include "config.hpp"

namespace ws {
	// A normal-user controller for a session-scoped elevated networking helper.
	// The helper owns WFP/NAT and the official WireGuard service together.
	// Serialize controller calls externally. Prepare/Install/Stop block; cancellation
	// callbacks run synchronously on that calling thread. Callback exceptions signal this
	// attempt's stop event before propagating. Message returns an allocating copy.
	class InternetWorker {
	public:
		InternetWorker();
		~InternetWorker();
		InternetWorker(InternetWorker const&) = delete;
		InternetWorker& operator=(InternetWorker const&) = delete;

		bool Prepare(std::filesystem::path const& CompiledConfiguration, std::string const& strName, InternetMode Mode, std::string const& strSubnet, std::string& strErrorOut, std::function<bool()> const& fnCancelled = {});
		bool Install(std::string& strErrorOut, std::function<bool()> const& fnCancelled = {});
		bool Stop(std::string& strErrorOut);

		bool IsAlive() const;
		std::string Message() const;

	private:
		struct Implementation;
		std::unique_ptr<Implementation> m_pImplementation;
	};

	int InternetWorkerHelperMain(int nArgumentCount, wchar_t** ppszArguments);
	int InternetIpcTestHelperMain(int nArgumentCount, wchar_t** ppszArguments);
	void RunInternetWorkerTests();
} // namespace ws
