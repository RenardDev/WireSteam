#pragma once

// General
// These standalone-header Windows defaults are public; preserve caller-defined values.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <ws2tcpip.h>

// C++
#include <cstdlib>
#include <cstring>

// STL
#include <array>
#include <string>

// Project
#include "tunnel.hpp"

namespace st::net {
	// Owns a socket; keep Winsock initialized until it is closed or transferred.
	// get borrows, release transfers, and reset takes a distinct owned socket or INVALID_SOCKET.
	// Synchronize access and lifetime externally.
	class Socket {
	public:
		Socket() {
			m_hSocket = INVALID_SOCKET;
		}

		explicit Socket(SOCKET hSocket) :
			m_hSocket(hSocket)
		{
		}

		~Socket() {
			reset();
		}

		Socket(Socket const&) = delete;
		Socket& operator=(Socket const&) = delete;

		Socket(Socket&& Other) noexcept :
			m_hSocket(Other.release())
		{
		}

		Socket& operator=(Socket&& Other) noexcept {
			if (this != &Other) {
				reset(Other.release());
			}

			return *this;
		}

		SOCKET get() const {
			return m_hSocket;
		}

		explicit operator bool() const {
			return m_hSocket != INVALID_SOCKET;
		}

		SOCKET release() {
			const SOCKET hSocket = m_hSocket;
			m_hSocket = INVALID_SOCKET;

			return hSocket;
		}

		void reset(SOCKET hSocket = INVALID_SOCKET) {
			if (m_hSocket != INVALID_SOCKET) {
				closesocket(m_hSocket);
			}

			m_hSocket = hSocket;
		}

	private:
		SOCKET m_hSocket;
	};

	struct Address {
		sockaddr_storage m_Storage {};
		int m_nSize = sizeof(sockaddr_storage);

		sockaddr* ptr() {
			return reinterpret_cast<sockaddr*>(&m_Storage);
		}

		sockaddr const* ptr() const {
			return reinterpret_cast<sockaddr const*>(&m_Storage);
		}

		int family() const {
			return m_Storage.ss_family;
		}
	};

	// Free helpers borrow inputs/handles for the call; synchronize shared sockets and outputs externally.
	// Allocation failures propagate. The default error code must be captured before intervening work.
	inline std::string Error(int nCode = WSAGetLastError()) {
		return "Winsock error " + std::to_string(nCode);
	}

	inline bool WouldBlock(int nCode) {
		return (nCode == WSAEWOULDBLOCK) || (nCode == WSAEINPROGRESS) || (nCode == WSAEALREADY);
	}

	inline bool Nonblocking(SOCKET hSocket) {
		u_long unEnabled = 1;

		return ioctlsocket(hSocket, FIONBIO, &unEnabled) == 0;
	}

	// Synchronous DNS resolution may block; AddressOut commits only on success.
	inline bool Resolve(Endpoint const& EndpointValue, int nType, Address& AddressOut, std::string& strErrorOut, bool bPassive = false) {
		addrinfo Hints {};
		Hints.ai_family = AF_UNSPEC;
		Hints.ai_socktype = nType;
		Hints.ai_protocol = (nType == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP;
		Hints.ai_flags = bPassive ? AI_PASSIVE : 0;
		addrinfo* pAddresses = nullptr;
		std::string const strPort = std::to_string(EndpointValue.m_unPort);
		const int nCode = getaddrinfo(EndpointValue.m_strHost.c_str(), strPort.c_str(), &Hints, &pAddresses);
		if (nCode || !pAddresses) {
			strErrorOut = "Cannot resolve " + EndpointValue.ToString() + ": " + std::to_string(nCode);
			return false;
		}

		// The SDK list spans only nonthrowing bounds/copy work; release before allocating diagnostics.
		if (pAddresses->ai_addrlen > sizeof(AddressOut.m_Storage)) {
			freeaddrinfo(pAddresses);
			strErrorOut = "Address too large";
			return false;
		}

		std::memcpy(&AddressOut.m_Storage, pAddresses->ai_addr, pAddresses->ai_addrlen);
		AddressOut.m_nSize = static_cast<int>(pAddresses->ai_addrlen);
		freeaddrinfo(pAddresses);

		return true;
	}

	inline Endpoint Describe(Address const& AddressValue) {
		std::array<char, NI_MAXHOST> szHost {};
		std::array<char, NI_MAXSERV> szService {};
		if (getnameinfo(AddressValue.ptr(), AddressValue.m_nSize, szHost.data(), static_cast<DWORD>(szHost.size()), szService.data(), static_cast<DWORD>(szService.size()), NI_NUMERICHOST | NI_NUMERICSERV)) {
			return { "?", 0 };
		}

		return { szHost.data(), static_cast<std::uint16_t>(std::strtoul(szService.data(), nullptr, 10)) };
	}

	inline Endpoint Bound(SOCKET hSocket) {
		Address AddressValue;
		if (getsockname(hSocket, AddressValue.ptr(), &AddressValue.m_nSize)) {
			return { "?", 0 };
		}

		return Describe(AddressValue);
	}

	inline std::string Key(Address const& AddressValue) {
		return Describe(AddressValue).ToString();
	}

	// Resolves and binds synchronously; the returned Socket owns the handle, failure sets strErrorOut.
	inline Socket Bind(Endpoint const& EndpointValue, int nType, std::string& strErrorOut) {
		Address AddressValue;
		if (!Resolve(EndpointValue, nType, AddressValue, strErrorOut, true)) {
			return {};
		}

		Socket Result(::socket(AddressValue.family(), nType, (nType == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP));
		if (!Result) {
			strErrorOut = Error();
			return {};
		}

		BOOL nExclusive = TRUE;
		if (setsockopt(Result.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&nExclusive), sizeof(nExclusive))) {
			strErrorOut = Error();
			return {};
		}

		if (!Nonblocking(Result.get()) || bind(Result.get(), AddressValue.ptr(), AddressValue.m_nSize)) {
			const int nError = WSAGetLastError();
			strErrorOut = "Cannot bind " + EndpointValue.ToString() + ": " + Error(nError);
			return {};
		}

		return Result;
	}

	inline void ConfigureTcp(SOCKET hSocket) {
		BOOL nEnabled = TRUE;
		// These optional performance/keepalive options do not gate socket use.
		setsockopt(hSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&nEnabled), sizeof(nEnabled));
		setsockopt(hSocket, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<char const*>(&nEnabled), sizeof(nEnabled));
	}

	inline void ConfigureUdp(SOCKET hSocket) {
		constexpr int kReceiveBufferBytes = 1024 * 1024;

		BOOL nDisabled = FALSE;
		DWORD unReturned = 0;
		WSAIoctl(hSocket, SIO_UDP_CONNRESET, &nDisabled, sizeof(nDisabled), nullptr, 0, &unReturned, nullptr, nullptr);

		// Platforms may retain their receive-buffer default when tuning fails.
		setsockopt(hSocket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&kReceiveBufferBytes), sizeof(kReceiveBufferBytes));
	}
} // namespace st::net
