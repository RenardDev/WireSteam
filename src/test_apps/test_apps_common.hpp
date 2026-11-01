#pragma once

// C++
#include <cstddef>
#include <cstdint>

// STL
#include <chrono>
#include <string>
#include <vector>

// Project
#include "net.hpp"

namespace st::testapp {
	using Clock = std::chrono::steady_clock;
	using Bytes = std::vector<std::uint8_t>;

	struct ServerOptions {
		Endpoint m_Listen { "127.0.0.1", 27015 };
		bool m_bTCP = true;
		bool m_bUDP = true;
		bool m_bVerbose = false;
		bool m_bHelp = false;
		std::size_t m_unMaximumClients = 64;
		int m_nIdleSeconds = 60;
		int m_nRunSeconds = 0;
	};

	struct ClientOptions {
		Endpoint m_Target { "127.0.0.1", 27016 };
		bool m_bTCP = true;
		bool m_bUDP = true;
		bool m_bHelp = false;
		bool m_bHalfClose = false;
		bool m_bAllowUDPLoss = false;
		bool m_bHasMessage = false;
		std::string m_strMessage;
		std::size_t m_unTCPSize = 65536;
		std::size_t m_unUDPSize = 1024;
		unsigned m_unCount = 10;
		unsigned m_unConnections = 1;
		int m_nTimeoutMilliseconds = 3000;
		int m_nIntervalMilliseconds = 100;
	};

	// Borrow nArgumentCount argument pointers for this call; slots 1..nArgumentCount-1 are nonnull C strings.
	// Counts 0/1 do not read ppszArguments; negative counts or required null pointers report an error.
	bool ParseServer(int nArgumentCount, char** ppszArguments, ServerOptions& OptionsOut, std::string& strErrorOut);
	bool ParseClient(int nArgumentCount, char** ppszArguments, ClientOptions& OptionsOut, std::string& strErrorOut);
	std::string ServerHelp();
	std::string ClientHelp();

	// Owns one Winsock reference; console-handler registration is best effort and process-wide.
	class Session {
	public:
		Session();
		~Session();
		Session(Session const&) = delete;
		Session& operator=(Session const&) = delete;

		explicit operator bool() const {
			return m_bInitialized;
		}

	private:
		bool m_bInitialized;
	};

	// These process-wide atomic operations may run concurrently; Stop is not reset by a new Session.
	bool Stopping();
	void Stop();
	// Serializes complete stdout lines; the stream sink must not reenter Log.
	void Log(std::string const& strMessage);
	void Pause(int nMilliseconds);
	// Waits for socket events until a deadline, checking Ctrl+C at least every 50ms.
	// POLLERR/HUP are reported as ready so callers can read EOF / SO_ERROR.
	bool Wait(SOCKET hSocket, short nEvents, Clock::time_point Deadline, std::string& strErrorOut);
	Bytes Payload(ClientOptions const& Options, bool bTCP, unsigned unConnection, unsigned unSequence);
	bool PayloadSequence(Bytes const& vecBytes, bool bTCP, unsigned unConnection, unsigned& unSequenceOut);
	// Borrow unBytes bytes for this call; null is valid only for an empty range.
	// Reads at most 48 bytes and returns escaped text; allocation or invalid input may throw.
	std::string Preview(void const* pData, std::size_t unBytes);
} // namespace st::testapp
