#include "steam_identity.hpp"

// C++
#include <cstddef>

// STL
#include <stdexcept>

namespace ws {
	namespace {
		constexpr std::size_t kMaximumNameBytes = 1024;
		constexpr unsigned char kUTF8ContinuationMask = 0xC0;
		constexpr unsigned char kUTF8ContinuationPrefix = 0x80;
		constexpr unsigned char kFirstPrintableCharacter = 0x20;
		constexpr unsigned kHexNibbleBits = 4;
		constexpr unsigned char kHexNibbleMask = 0x0F;
		constexpr char kHexDigits[] = "0123456789abcdef";
	} // namespace

	std::string SteamIdentityLine(std::uint64_t unSteamID, std::string_view strvName, bool bLocal) {
		// A single-line JSON record avoids ambiguous delimiters and log injection
		// from Steam names. Keep the API's UTF-8 spelling unchanged.
		if (strvName.size() > kMaximumNameBytes) {
			std::size_t unEnd = kMaximumNameBytes;
			while (unEnd && ((static_cast<unsigned char>(strvName[unEnd]) & kUTF8ContinuationMask) == kUTF8ContinuationPrefix)) {
				--unEnd;
			}

			strvName = strvName.substr(0, unEnd);
		}

		std::string strEscaped;
		for (auto const& chCharacter : strvName) {
			const unsigned char unCharacter = static_cast<unsigned char>(chCharacter);
			if ((unCharacter == '"') || (unCharacter == '\\')) {
				strEscaped += '\\';
				strEscaped += static_cast<char>(unCharacter);

			} else if (unCharacter < kFirstPrintableCharacter) {
				strEscaped += "\\u00";
				strEscaped += kHexDigits[unCharacter >> kHexNibbleBits];
				strEscaped += kHexDigits[unCharacter & kHexNibbleMask];

			} else {
				strEscaped += static_cast<char>(unCharacter);
			}
		}

		return "WIRESTEAM IDENTITY {\"SteamID\":\"" + std::to_string(unSteamID) + "\",\"Name\":\"" + strEscaped + "\",\"Local\":" + (bLocal ? "true}" : "false}");
	}

	void RunSteamIdentityTests() {
		std::string const strLine = SteamIdentityLine(76561198000000001, "MiXeD \"name\"\\\n\t", true);
		if ((strLine != "WIRESTEAM IDENTITY {\"SteamID\":\"76561198000000001\",\"Name\":\"MiXeD \\\"name\\\"\\\\\\u000a\\u0009\",\"Local\":true}") || (strLine.find('\n') != std::string::npos) || (strLine.find('\t') != std::string::npos)) {
			throw std::runtime_error("Steam persona telemetry escaping failed");
		}

		if ((SteamIdentityLine(1, "Test \xD0\xA2\xD0\xB5\xD1\x81\xD1\x82", false) != "WIRESTEAM IDENTITY {\"SteamID\":\"1\",\"Name\":\"Test \xD0\xA2\xD0\xB5\xD1\x81\xD1\x82\",\"Local\":false}") || (SteamIdentityLine(1, std::string(kMaximumNameBytes, 'a') + "\xD0\xA0", false).find("\xD0") != std::string::npos)) {
			throw std::runtime_error("Steam persona telemetry UTF-8 preservation/bounds failed");
		}
	}
} // namespace ws
