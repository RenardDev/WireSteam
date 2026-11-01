#include "ui_control.hpp"

// General
#include <windows.h>
#include <objbase.h>

// C++
#include <utility>

// STL
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

// Project
#include "steam_bridge.hpp"

namespace ws {
	namespace {
		constexpr std::wstring_view kStopEventPrefix = L"Local\\WireSteamStop-";
		constexpr std::size_t kCompactGUIDCharacters = 32;
		constexpr std::size_t kHyphenatedGUIDCharacters = 36;

		struct Handle {
			Handle() {
				m_hHandle = nullptr;
			}

			explicit Handle(HANDLE hHandle) :
				m_hHandle(hHandle)
			{
			}

			~Handle() {
				if (m_hHandle) {
					CloseHandle(m_hHandle);
				}
			}

			Handle(Handle const&) = delete;
			Handle& operator=(Handle const&) = delete;

			HANDLE m_hHandle;
		};

		void Check(bool bCondition, char const* pszMessage) {
			if (!bCondition) {
				throw std::runtime_error(pszMessage);
			}
		}

		std::wstring GenerateTestEventName() {
			constexpr int kGUIDBufferCharacters = 40;
			constexpr std::size_t kGUIDTokenOffset = 0x01;

			GUID Identifier {};
			Check(SUCCEEDED(CoCreateGuid(&Identifier)), "Cannot generate a stop-event test identifier");

			wchar_t szText[kGUIDBufferCharacters] {};
			Check(StringFromGUID2(Identifier, szText, kGUIDBufferCharacters) != 0, "Cannot format a stop-event test identifier");

			return std::wstring(kStopEventPrefix) + std::wstring(szText + kGUIDTokenOffset, kHyphenatedGUIDCharacters);
		}
	} // namespace

	bool IsValidStopEventName(std::wstring_view strvName) noexcept {
		constexpr std::size_t kMaximumNameCharacters = 120;
		constexpr wchar_t kFirstPrintableASCII = 0x20;
		constexpr wchar_t kLastPrintableASCII = 0x7E;
		constexpr std::size_t kFirstGUIDHyphenOffset = 0x08;
		constexpr std::size_t kSecondGUIDHyphenOffset = 0x0D;
		constexpr std::size_t kThirdGUIDHyphenOffset = 0x12;
		constexpr std::size_t kFourthGUIDHyphenOffset = 0x17;

		if ((strvName.size() > kMaximumNameCharacters) || !strvName.starts_with(kStopEventPrefix)) {
			return false;
		}

		for (auto const& wchCharacter : strvName) {
			if ((wchCharacter < kFirstPrintableASCII) || (wchCharacter > kLastPrintableASCII)) {
				return false;
			}
		}

		std::wstring_view const strvToken = strvName.substr(kStopEventPrefix.size());
		if ((strvToken.size() != kCompactGUIDCharacters) && (strvToken.size() != kHyphenatedGUIDCharacters)) {
			return false;
		}

		for (std::size_t unCharacterIndex = 0; unCharacterIndex < strvToken.size(); ++unCharacterIndex) {
			const wchar_t wchCharacter = strvToken[unCharacterIndex];
			if ((strvToken.size() == kHyphenatedGUIDCharacters) && ((unCharacterIndex == kFirstGUIDHyphenOffset) || (unCharacterIndex == kSecondGUIDHyphenOffset) || (unCharacterIndex == kThirdGUIDHyphenOffset) || (unCharacterIndex == kFourthGUIDHyphenOffset))) {
				if (wchCharacter != L'-') {
					return false;
				}
			} else if (!(((wchCharacter >= L'0') && (wchCharacter <= L'9')) || ((wchCharacter >= L'a') && (wchCharacter <= L'f')) || ((wchCharacter >= L'A') && (wchCharacter <= L'F')))) {
				return false;
			}
		}

		return true;
	}

	struct StopWatcher::Implementation {
		Implementation(std::wstring const& wstrEventName, std::function<void()> fnOnStop) :
			m_OnStop(std::move(fnOnStop))
		{
			Check(IsValidStopEventName(wstrEventName), "Invalid GUI stop-event name; use Local\\WireSteamStop- followed by a GUID");

			m_StopEvent.m_hHandle = OpenEventW(SYNCHRONIZE, FALSE, wstrEventName.c_str());
			Check(m_StopEvent.m_hHandle != nullptr, "Cannot open the GUI stop event; the desktop app must create it first");

			m_QuitEvent.m_hHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			Check(m_QuitEvent.m_hHandle != nullptr, "Cannot create the stop-watcher shutdown event");

			if (!m_OnStop) {
				m_OnStop = []() -> void {
					RequestStop();
				};
			}

			// C++20 initializes the atomic to false before this object is published to the worker.
			m_Worker = std::thread([this]() -> void {
				constexpr DWORD kWaitHandleCount = 2;

				HANDLE const arrHandles[kWaitHandleCount] = { m_StopEvent.m_hHandle, m_QuitEvent.m_hHandle };
				if (WaitForMultipleObjects(kWaitHandleCount, arrHandles, FALSE, INFINITE) == WAIT_OBJECT_0) {
					// Publish the stop flag before invoking the callback; IsSignaled reads it with acquire.
					m_bObserved.store(true, std::memory_order_release);

					try {
						m_OnStop();
					} catch (...) {
						// Callback exceptions must not escape the worker thread.
					}
				}
			});
		}

		~Implementation() {
			if (m_QuitEvent.m_hHandle) {
				SetEvent(m_QuitEvent.m_hHandle);
			}

			if (m_Worker.joinable()) {
				m_Worker.join();
			}
		}

		bool IsSignaled() const noexcept {
			return m_bObserved.load(std::memory_order_acquire) || (WaitForSingleObject(m_StopEvent.m_hHandle, 0) == WAIT_OBJECT_0);
		}

		Handle m_StopEvent;
		Handle m_QuitEvent;
		std::function<void()> m_OnStop;
		std::atomic<bool> m_bObserved;
		std::thread m_Worker;
	};

	StopWatcher::StopWatcher(std::wstring const& wstrEventName, std::function<void()> fnOnStop) :
		m_pImplementation(std::make_unique<Implementation>(wstrEventName, std::move(fnOnStop)))
	{
	}

	StopWatcher::~StopWatcher() = default;

	bool StopWatcher::IsSignaled() const noexcept {
		return m_pImplementation->IsSignaled();
	}

	void RunUiControlTests() {
		constexpr DWORD kCallbackTimeoutMilliseconds = 1000;
		constexpr int kJoinTimeoutSeconds = 2;
		constexpr int kCancellationTimeoutSeconds = 1;
		constexpr std::size_t kInvalidNameCharacters = 121;

		std::wstring const wstrSampleName = GenerateTestEventName();
		Check(IsValidStopEventName(wstrSampleName), "Valid stop-event GUID name was rejected");
		Check(IsValidStopEventName(std::wstring(kStopEventPrefix) + L"0123456789abcdef0123456789ABCDEF"), "Compact stop-event GUID name was rejected");

		for (auto const& wstrInvalid : { std::wstring {}, std::wstring(L"Global\\WireSteamStop-0123456789abcdef0123456789abcdef"), std::wstring(kStopEventPrefix) + L"short", std::wstring(kStopEventPrefix) + std::wstring(kInvalidNameCharacters, L'a'), std::wstring(kStopEventPrefix) + L"01234567/9abcdef0123456789abcdef0", std::wstring(kStopEventPrefix) + L"01234567-89ab-cdef-0123-456789abcdeg" }) {
			Check(!IsValidStopEventName(wstrInvalid), "Malformed stop-event name was accepted");
		}

		std::atomic<unsigned> unCallbackCount = 0;
		bool bRejected = false;
		try {
			StopWatcher MissingWatcher(wstrSampleName, [&]() -> void {
				++unCallbackCount;
			});
		} catch (std::runtime_error const&) {
			bRejected = true;
		}

		Check(bRejected && (unCallbackCount.load() == 0), "Missing UI stop event did not fail without invoking the callback");
		Handle MissingEvent(OpenEventW(SYNCHRONIZE, FALSE, wstrSampleName.c_str()));
		Check(!MissingEvent.m_hHandle, "Stop watcher unexpectedly created the GUI event");

		Handle Notification(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		Check(Notification.m_hHandle != nullptr, "Cannot create callback test notification");

		std::wstring const wstrNormalName = GenerateTestEventName();
		Handle UIStopEvent(CreateEventW(nullptr, TRUE, FALSE, wstrNormalName.c_str()));
		Check(UIStopEvent.m_hHandle != nullptr, "Cannot create a UI-owned test stop event");

		std::chrono::steady_clock::time_point const StartTime = std::chrono::steady_clock::now();
		{
			StopWatcher Watcher(wstrNormalName, [&]() -> void {
				++unCallbackCount;
				SetEvent(Notification.m_hHandle);
			});
			Check(!Watcher.IsSignaled(), "Unsignaled UI stop event was reported as signaled");
			Check(SetEvent(UIStopEvent.m_hHandle) != FALSE, "Cannot signal the UI-owned test event");
			Check(WaitForSingleObject(Notification.m_hHandle, kCallbackTimeoutMilliseconds) == WAIT_OBJECT_0, "Stop callback was not delivered promptly");
			Check(Watcher.IsSignaled() && (unCallbackCount.load() == 1), "Stop callback was not delivered exactly once");
		}

		Check((std::chrono::steady_clock::now() - StartTime) < std::chrono::seconds(kJoinTimeoutSeconds), "Stop watcher did not join promptly");

		ResetEvent(Notification.m_hHandle);
		std::wstring const wstrSetName = GenerateTestEventName();
		Handle AlreadySignaledEvent(CreateEventW(nullptr, TRUE, TRUE, wstrSetName.c_str()));
		Check(AlreadySignaledEvent.m_hHandle != nullptr, "Cannot create an already-signaled UI test event");

		{
			StopWatcher Watcher(wstrSetName, [&]() -> void {
				++unCallbackCount;
				SetEvent(Notification.m_hHandle);
			});
			Check(Watcher.IsSignaled(), "An already-signaled stop event was missed during startup");
			Check((WaitForSingleObject(Notification.m_hHandle, kCallbackTimeoutMilliseconds) == WAIT_OBJECT_0) && (unCallbackCount.load() == 2), "Already-signaled stop event did not invoke its callback");
		}

		std::wstring const wstrCancellationName = GenerateTestEventName();
		Handle CancellationEvent(CreateEventW(nullptr, TRUE, FALSE, wstrCancellationName.c_str()));
		Check(CancellationEvent.m_hHandle != nullptr, "Cannot create watcher cancellation test event");

		std::chrono::steady_clock::time_point const CancellationStartTime = std::chrono::steady_clock::now();
		{
			StopWatcher Watcher(wstrCancellationName, [&]() -> void {
				++unCallbackCount;
			});
		}

		Check((std::chrono::steady_clock::now() - CancellationStartTime) < std::chrono::seconds(kCancellationTimeoutSeconds), "Unsignaled watcher destructor failed to cancel its wait");
		SetEvent(CancellationEvent.m_hHandle);
		Check(unCallbackCount.load() == 2, "Destroyed stop watcher invoked a callback");

		std::cout << "PASS UI control: restricted event names, open-only access, signaled startup, stop callback and joined cancellation\n";
	}
} // namespace ws
