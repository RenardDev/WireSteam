#pragma once

// STL
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ws {
	// Accepts only the local stop-event prefix followed by a compact or hyphenated ASCII GUID.
	bool IsValidStopEventName(std::wstring_view strvName) noexcept;

	// The desktop UI owns and creates the manual-reset stop event. The child only
	// opens it with SYNCHRONIZE access and requests the normal transport shutdown.
	// A supplied callback runs at most once on an owned worker, possibly before construction returns.
	// An empty callback requests normal transport shutdown; callback exceptions are contained.
	// Callback borrows must survive until destruction, which signals private shutdown and joins the worker.
	// Destroy outside that worker and synchronize destruction with all IsSignaled calls.
	// Construction can throw for invalid names, unavailable events, allocation or thread creation failure.
	class StopWatcher {
	public:
		explicit StopWatcher(std::wstring const& wstrEventName, std::function<void()> fnOnStop = {});
		~StopWatcher();
		StopWatcher(StopWatcher const&) = delete;
		StopWatcher& operator=(StopWatcher const&) = delete;

		// Concurrent readers are safe while alive; this does not wait for callback completion.
		bool IsSignaled() const noexcept;

	private:
		struct Implementation;
		std::unique_ptr<Implementation> m_pImplementation;
	};

	void RunUiControlTests();
} // namespace ws
