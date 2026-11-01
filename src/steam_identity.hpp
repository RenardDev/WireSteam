#pragma once

// C++
#include <cstdint>

// STL
#include <string>
#include <string_view>

namespace ws {
	// Borrows the UTF-8 name for this call and returns an owned single-line identity record.
	// JSON fields are SteamID (decimal string), Name (UTF-8 string) and Local (boolean).
	// Clips the name to at most 1024 input bytes without splitting a valid UTF-8 character;
	// escapes JSON controls and appends no newline. Concurrent calls share no mutable state.
	std::string SteamIdentityLine(std::uint64_t unSteamID, std::string_view strvName, bool bLocal);
	void RunSteamIdentityTests();
} // namespace ws
