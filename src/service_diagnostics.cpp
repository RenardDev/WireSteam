#include "service_diagnostics.hpp"

// General
#include <windows.h>

// STL
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ws {
	namespace {
		constexpr std::size_t kTimestampBytes = 19;

		void WipeSensitiveText(std::string& strText) noexcept {
			if (!strText.empty()) {
				SecureZeroMemory(strText.data(), strText.size());
			}
		}

		class SensitiveStringWiper {
		public:
			SensitiveStringWiper(std::string& strText) :
				m_strText(strText)
			{
			}

			~SensitiveStringWiper() {
				WipeSensitiveText(m_strText);
			}

			SensitiveStringWiper(SensitiveStringWiper const&) = delete;
			SensitiveStringWiper& operator=(SensitiveStringWiper const&) = delete;

		private:
			std::string& m_strText;
		};

		class SensitiveInputStreamWiper {
		public:
			SensitiveInputStreamWiper(std::istringstream& InputStream) :
				m_InputStream(InputStream)
			{
			}

			~SensitiveInputStreamWiper() {
				std::string_view const strvContents = m_InputStream.view();
				if (!strvContents.empty()) {
					// This nonconst owned input buffer stays alive through cleanup; view()
					// includes its full initialized sequence even after getline reaches EOF.
					SecureZeroMemory(const_cast<char*>(strvContents.data()), strvContents.size());
				}
			}

			SensitiveInputStreamWiper(SensitiveInputStreamWiper const&) = delete;
			SensitiveInputStreamWiper& operator=(SensitiveInputStreamWiper const&) = delete;

		private:
			std::istringstream& m_InputStream;
		};

		bool IsTimestamp(std::string const& strValue) {
			constexpr std::size_t kYearSeparatorIndex = 0x04;
			constexpr std::size_t kMonthSeparatorIndex = 0x07;
			constexpr std::size_t kDateSeparatorIndex = 0x0A;
			constexpr std::size_t kHourSeparatorIndex = 0x0D;
			constexpr std::size_t kMinuteSeparatorIndex = 0x10;

			if (strValue.size() != kTimestampBytes) {
				return false;
			}

			for (std::size_t unCharacterIndex = 0; unCharacterIndex < strValue.size(); ++unCharacterIndex) {
				const char chExpected = ((unCharacterIndex == kYearSeparatorIndex) || (unCharacterIndex == kMonthSeparatorIndex)) ? '-' : (unCharacterIndex == kDateSeparatorIndex) ? ' ' : ((unCharacterIndex == kHourSeparatorIndex) || (unCharacterIndex == kMinuteSeparatorIndex)) ? ':' : '\0';
				if (chExpected ? (strValue[unCharacterIndex] != chExpected) : ((strValue[unCharacterIndex] < '0') || (strValue[unCharacterIndex] > '9'))) {
					return false;
				}
			}

			return true;
		}

		void ConvertToLowerCase(std::string& strText) {
			std::transform(strText.begin(), strText.end(), strText.begin(), [](unsigned char unCharacter) -> char {
				return static_cast<char>(std::tolower(unCharacter));
			});
		}

		std::string BoundText(std::string strText) {
			constexpr std::size_t kMaximumMessageBytes = 600;
			constexpr unsigned char kUTF8ContinuationMask = 0xC0;
			constexpr unsigned char kUTF8ContinuationPrefix = 0x80;

			if (strText.size() <= kMaximumMessageBytes) {
				return strText;
			}

			std::size_t unEnd = kMaximumMessageBytes;
			while (unEnd && ((static_cast<unsigned char>(strText[unEnd]) & kUTF8ContinuationMask) == kUTF8ContinuationPrefix)) {
				--unEnd;
			}

			return strText.substr(0, unEnd) + "...";
		}
	} // namespace

	std::string SelectServiceDiagnostics(std::string const& strLog, std::string const& strTunnelName, std::string const& strSinceLocalTime) {
		constexpr std::size_t kMinimumLogLineBytes = 27;
		constexpr std::size_t kMaximumMarkerOffset = 0x1A;
		constexpr std::size_t kMaximumMessages = 3;
		constexpr unsigned char kFirstPrintableByte = 0x20;

		if (!IsTimestamp(strSinceLocalTime) || strTunnelName.empty()) {
			return {};
		}

		std::string const strMarker = ": [TUN] [" + strTunnelName + "] ";
		std::vector<std::string> vecMessages;

		std::istringstream InputStream(strLog);
		SensitiveInputStreamWiper InputWiper = SensitiveInputStreamWiper(InputStream);
		std::string strLine;
		SensitiveStringWiper LineWiper = SensitiveStringWiper(strLine);

		while (true) {
			// Wipe the previous live line before getline can replace it with a shorter
			// one. The outer owner also covers a failed or throwing partial extraction.
			WipeSensitiveText(strLine);

			if (!std::getline(InputStream, strLine)) {
				break;
			}

			if ((strLine.size() < kMinimumLogLineBytes) || !IsTimestamp(strLine.substr(0, kTimestampBytes)) || (strLine.compare(0, kTimestampBytes, strSinceLocalTime) < 0)) {
				continue;
			}

			const std::size_t unMarkerAt = strLine.find(strMarker, kTimestampBytes);
			if ((unMarkerAt == std::string::npos) || (unMarkerAt > kMaximumMarkerOffset)) {
				continue;
			}

			std::string strMessage = strLine.substr(unMarkerAt + strMarker.size());
			SensitiveStringWiper MessageWiper = SensitiveStringWiper(strMessage);
			if (!strMessage.empty() && (strMessage.back() == '\r')) {
				strMessage.pop_back();
			}

			std::string strLower = strMessage;
			SensitiveStringWiper LowercaseWiper = SensitiveStringWiper(strLower);
			ConvertToLowerCase(strLower);

			// Never forward configuration material, even if an upstream diagnostic
			// were to start quoting it. Routine peer/handshake chatter is omitted.
			if ((strLower.find("privatekey") != std::string::npos) || (strLower.find("presharedkey") != std::string::npos)) {
				continue;
			}

			if ((strLower.find("could not") == std::string::npos) && (strLower.find("unable to") == std::string::npos) && (strLower.find("failed") == std::string::npos) && (strLower.find("error:") == std::string::npos)) {
				continue;
			}

			if (std::any_of(strMessage.begin(), strMessage.end(), [](unsigned char unCharacter) -> bool {
				return (unCharacter < kFirstPrintableByte) && (unCharacter != '\t');
			})) {
				continue;
			}

			vecMessages.push_back(BoundText(strMessage));

			if (vecMessages.size() > kMaximumMessages) {
				vecMessages.erase(vecMessages.begin());
			}
		}

		std::string strResult;
		for (auto const& strMessage : vecMessages) {
			strResult += "\nWireGuard: " + strMessage;
		}

		return strResult;
	}

	void RunServiceDiagnosticTests() {
		constexpr std::size_t kTestLineCount = 12;
		constexpr std::size_t kTestMessageBytes = 1200;
		constexpr std::size_t kMaximumDiagnosticBytes = 1900;

		std::string const strName = "WireSteam-ui-test";
		std::string const strHeader = "2026-10-30 04:03:15.420571: [TUN] [" + strName + "] ";
		std::string const strSelected = SelectServiceDiagnostics(
			"2026-10-30 04:03:14.123456: [TUN] [" + strName + "] Unable to old error\n" +
				"2026-10-30 04:03:15.123456: [TUN] [OtherTunnel] Unable to unrelated error\n" +
				strHeader + "Setting interface configuration\n" +
				strHeader + "Could not bind socket to 0.0.0.0:51820 (0xc0000043)\n" +
				strHeader + "Unable to bring up adapter: address already in use\n" +
				strHeader + "Error: PrivateKey = secret\n",
			strName, "2026-10-30 04:03:15");
		if ((strSelected.find("51820") == std::string::npos) || (strSelected.find("address already in use") == std::string::npos) || (strSelected.find("old error") != std::string::npos) || (strSelected.find("unrelated") != std::string::npos) || (strSelected.find("secret") != std::string::npos) || (strSelected.find("Setting interface") != std::string::npos)) {
			throw std::runtime_error("Service diagnostics failed to isolate current tunnel errors");
		}

		std::string strLarge;
		for (std::size_t unLineIndex = 0; unLineIndex < kTestLineCount; ++unLineIndex) {
			strLarge += strHeader + "Error: " + std::string(kTestMessageBytes, 'x') + '\n';
		}

		if ((SelectServiceDiagnostics(strLarge, strName, "2026-10-30 04:03:15").size() > kMaximumDiagnosticBytes) || !SelectServiceDiagnostics(strLarge, strName, "bad timestamp").empty()) {
			throw std::runtime_error("Service diagnostics exceeded its public output bounds");
		}

		std::cout << "PASS service diagnostics: current tunnel/time isolation, startup errors, secret filtering and bounded output\n";
	}
} // namespace ws
