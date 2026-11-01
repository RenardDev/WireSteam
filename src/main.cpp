
// General
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

// C++
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>

// STL
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

// Project
#include "config.hpp"
#include "internet_legacy_routes.hpp"
#include "internet_routing.hpp"
#include "internet_server.hpp"
#include "internet_worker.hpp"
#include "platform.hpp"
#include "port_forward.hpp"
#include "preflight.hpp"
#include "steam_bridge.hpp"
#include "steam_identity.hpp"
#include "ui_control.hpp"

namespace {
	using Arguments = std::vector<std::wstring>;
	namespace FileSystem = std::filesystem;

	constexpr std::size_t kTunnelPrefixCharacters = sizeof("WireSteam-") - 1;

	class SensitiveStringWiper {
	public:
		explicit SensitiveStringWiper(std::string& strValue) :
			m_strValue(strValue)
		{
		}

		~SensitiveStringWiper() {
			if (!m_strValue.empty()) {
				SecureZeroMemory(m_strValue.data(), m_strValue.size());
			}
		}

		SensitiveStringWiper(SensitiveStringWiper const&) = delete;
		SensitiveStringWiper& operator=(SensitiveStringWiper const&) = delete;

	private:
		std::string& m_strValue;
	};

	class SensitiveMemoryWiper {
	public:
		SensitiveMemoryWiper(void* pMemory, std::size_t unBytes) :
			m_pMemory(pMemory),
			m_unBytes(unBytes)
		{
		}

		~SensitiveMemoryWiper() {
			SecureZeroMemory(m_pMemory, m_unBytes);
		}

		SensitiveMemoryWiper(SensitiveMemoryWiper const&) = delete;
		SensitiveMemoryWiper& operator=(SensitiveMemoryWiper const&) = delete;

	private:
		void* m_pMemory;
		std::size_t m_unBytes;
	};

	class SensitiveConfigurationWiper {
	public:
		explicit SensitiveConfigurationWiper(ws::Config& Configuration) :
			m_Configuration(Configuration)
		{
		}

		~SensitiveConfigurationWiper() {
			SecureZeroMemory(m_Configuration.m_strPrivateKey.data(), m_Configuration.m_strPrivateKey.size());
			for (auto& Peer : m_Configuration.m_vecPeers) {
				SecureZeroMemory(Peer.m_strPresharedKey.data(), Peer.m_strPresharedKey.size());
			}
		}

		SensitiveConfigurationWiper(SensitiveConfigurationWiper const&) = delete;
		SensitiveConfigurationWiper& operator=(SensitiveConfigurationWiper const&) = delete;

	private:
		ws::Config& m_Configuration;
	};

	static_assert(std::is_nothrow_move_constructible_v<ws::Config>);

	struct TaskMemoryDeleter {
		void operator()(void* pMemory) const noexcept {
			CoTaskMemFree(pMemory);
		}
	};

	std::string EncodeUTF8(std::wstring const& wstrValue) {
		if (wstrValue.empty()) {
			return {};
		}

		if (wstrValue.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
			throw std::runtime_error("Invalid Unicode argument");
		}

		const int nCount = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wstrValue.data(), static_cast<int>(wstrValue.size()), nullptr, 0, nullptr, nullptr);
		if (!nCount) {
			throw std::runtime_error("Invalid Unicode argument");
		}

		std::string strResult = std::string(static_cast<std::size_t>(nCount), '\0');
		if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wstrValue.data(), static_cast<int>(wstrValue.size()), strResult.data(), nCount, nullptr, nullptr) != nCount) {
			throw std::runtime_error("Invalid Unicode argument");
		}

		return strResult;
	}

	std::string FormatPath(FileSystem::path const& Path) {
		return EncodeUTF8(Path.wstring());
	}

	void Require(bool bCondition, std::string const& strMessage) {
		if (!bCondition) {
			throw std::runtime_error(strMessage);
		}
	}

	unsigned int ParseUnsignedNumber(std::wstring const& wstrText, unsigned int unMaximum) {
		std::string const strNarrow = EncodeUTF8(wstrText);
		unsigned int unValue = 0;
		std::from_chars_result const ParsedNumber = std::from_chars(strNarrow.data(), strNarrow.data() + strNarrow.size(), unValue);
		Require(!strNarrow.empty() && (ParsedNumber.ec == std::errc {}) && (ParsedNumber.ptr == (strNarrow.data() + strNarrow.size())) && (unValue <= unMaximum), "Invalid numeric argument");

		return unValue;
	}

	std::string ParseTunnelName(std::wstring const& wstrText) {
		constexpr std::size_t kMaximumNameCharacters = 20;

		std::string const strName = EncodeUTF8(wstrText);
		Require(!strName.empty() && (strName.size() <= kMaximumNameCharacters), "Tunnel name must contain 1-20 ASCII letters, digits, '_' or '-'");
		for (auto const& chCharacter : strName) {
			Require(((chCharacter >= 'a') && (chCharacter <= 'z')) || ((chCharacter >= 'A') && (chCharacter <= 'Z')) || ((chCharacter >= '0') && (chCharacter <= '9')) || (chCharacter == '_') || (chCharacter == '-'), "Invalid tunnel name");
		}

		return "WireSteam-" + strName;
	}

	FileSystem::path GetRuntimePath(std::string const& strName) {
		PWSTR pszFolder = nullptr;
		const HRESULT nStatus = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &pszFolder);
		std::unique_ptr<wchar_t, TaskMemoryDeleter> pFolder(pszFolder);
		Require(SUCCEEDED(nStatus) && pFolder, "Cannot locate the local application data directory");
		FileSystem::path const RootPath = pFolder.get();
		pFolder.reset();

		return RootPath / L"WireSteam" / L"runtime" / (strName + ".conf");
	}

	ws::Config LoadConfiguration(FileSystem::path const& Path) {
		std::string strText;
		SensitiveStringWiper TextWiper(strText);
		std::string strError;
		SensitiveStringWiper ErrorWiper(strError);

		Require(ws::ReadConfigFile(FileSystem::absolute(Path), strText, strError), strError);

		ws::Config Configuration;
		SensitiveConfigurationWiper ConfigurationWiper(Configuration);
		Require(ws::ParseConfig(strText, Configuration, strError), strError);

		// Construct a distinct result before the armed guard cleans the moved-from source.
		return ws::Config(std::move(Configuration));
	}

	std::string ReadStandardConfiguration() {
		constexpr std::size_t kMaximumInput = 1024 * 1024;
		constexpr std::size_t kBufferCapacity = 4096;

		static_assert(kMaximumInput <= static_cast<std::size_t>(std::numeric_limits<int>::max()));
		static_assert(kBufferCapacity <= std::numeric_limits<DWORD>::max());

		const HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
		Require(hInput && (hInput != INVALID_HANDLE_VALUE), "save-config requires configuration text on standard input");
		std::string strContents;
		SensitiveStringWiper ContentsWiper(strContents);
		// Reserve before reading secrets: bounded appends cannot reallocate or enter SSO.
		strContents.reserve(kMaximumInput);
		std::array<char, kBufferCapacity> arrBuffer {};
		SensitiveMemoryWiper BufferWiper = SensitiveMemoryWiper(arrBuffer.data(), arrBuffer.size());
		for (;;) {
			DWORD unCount = 0;
			if (!ReadFile(hInput, arrBuffer.data(), static_cast<DWORD>(arrBuffer.size()), &unCount, nullptr)) {
				const DWORD unError = GetLastError();
				if (unError == ERROR_BROKEN_PIPE) {
					break;
				}

				throw std::runtime_error("Cannot read the configuration from standard input");
			}

			if (!unCount) {
				break;
			}

			Require((unCount <= arrBuffer.size()) && (strContents.size() <= kMaximumInput) && (unCount <= (kMaximumInput - strContents.size())), "Configuration input exceeds 1 MiB");
			strContents.append(arrBuffer.data(), unCount);
		}

		Require(!strContents.empty(), "Configuration input is empty");
		Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, strContents.data(), static_cast<int>(strContents.size()), nullptr, 0) > 0, "Configuration input must be valid UTF-8");

		// The distinct result takes this pre-reserved heap buffer; the armed source guard sees empty state.
		return std::string(std::move(strContents));
	}

	std::string SanitizeConfigurationError(std::string const& strError) {
		// Borrow the original diagnostic; never copy then shrink private editor-derived text.
		const std::size_t unSettingOffset = strError.find("Unsupported setting '");
		const std::size_t unSectionOffset = strError.find("Unknown or unsupported section [");
		if ((unSettingOffset != std::string::npos) && ((unSectionOffset == std::string::npos) || (unSettingOffset < unSectionOffset))) {
			return strError.substr(0, unSettingOffset) + "Unsupported configuration setting.";
		}

		if (unSectionOffset != std::string::npos) {
			return strError.substr(0, unSectionOffset) + "Unsupported configuration section.";
		}

		return strError;
	}

	class NameLock {
	public:
		explicit NameLock(std::string const& strName) {
			m_hMutex = nullptr;
			try {
				std::wstring const wstrMutexName = L"Local\\" + std::wstring(strName.begin(), strName.end());
				m_hMutex = CreateMutexW(nullptr, FALSE, wstrMutexName.c_str());
				Require(m_hMutex != nullptr, "Cannot create tunnel ownership lock");
				const DWORD unResult = WaitForSingleObject(m_hMutex, 0);
				if ((unResult != WAIT_OBJECT_0) && (unResult != WAIT_ABANDONED)) {
					CloseHandle(m_hMutex);
					m_hMutex = nullptr;
					throw std::runtime_error("Another WireSteam process is managing this tunnel name");
				}
			} catch (...) {
				// No throwing work follows a successful acquisition; this is only an unowned handle.
				if (m_hMutex) {
					CloseHandle(m_hMutex);
					m_hMutex = nullptr;
				}

				throw;
			}
		}

		~NameLock() {
			if (m_hMutex) {
				// Both instances are stack-owned within the same synchronous CLI invocation/thread.
				if (!ReleaseMutex(m_hMutex)) {
					OutputDebugStringA("WireSteam: cannot release the tunnel ownership mutex.\n");
					std::terminate();
				}

				// Matching terminal handle release after the confirmed mutex release.
				CloseHandle(m_hMutex);
			}
		}

		NameLock(NameLock const&) = delete;
		NameLock& operator=(NameLock const&) = delete;

	private:
		HANDLE m_hMutex;
	};

	void RemoveRuntime(FileSystem::path const& Path) {
		std::error_code Error;
		FileSystem::remove(Path, Error);

		if (Error) {
			std::cerr << "Could not remove the private runtime configuration: " << Error.message() << '\n';
		}
	}

	struct Session {
		explicit Session(std::string strName, FileSystem::path RuntimePath) :
			m_strName(std::move(strName)),
			m_RuntimePath(std::move(RuntimePath))
		{
			m_bFileOwned = false;
			m_bInstalled = false;
			m_bCleanupAttempted = false;
		}

		~Session() {
			if (m_bInstalled || m_bFileOwned || m_pInternet || m_pForwards) {
				try {
					Cleanup();
				} catch (...) {
					OutputDebugStringA("WireSteam: session cleanup failed; owned recovery state may remain.\n");
				}
			}
		}

		Session(Session const&) = delete;
		Session& operator=(Session const&) = delete;

		bool Cleanup() {
			if (m_pForwards) {
				m_pForwards->Stop();
			}

			if (m_bCleanupAttempted) {
				return !m_bInstalled && !m_bFileOwned;
			}

			m_bCleanupAttempted = true;

			if (m_pInternet) {
				std::string strError;
				if (!m_pInternet->Stop(strError)) {
					std::cerr << "Adapter cleanup failed: " << strError << "\nRun: WireSteam.exe down --name " << m_strName.substr(kTunnelPrefixCharacters) << '\n';
					return false;
				}

				m_bInstalled = false;
			}

			if (m_bInstalled) {
				std::string strError;
				std::cout << "Removing the WireGuard adapter (administrator approval may be required)...\n";

				if (!ws::ServiceAction("remove", {}, m_strName, strError)) {
					std::cerr << "Adapter cleanup failed: " << strError << "\nRun: WireSteam.exe down --name " << m_strName.substr(kTunnelPrefixCharacters) << '\n';
					return false;
				}

				m_bInstalled = false;
			}

			if (m_bFileOwned && !ws::ServiceExists(m_strName)) {
				RemoveRuntime(m_RuntimePath);
				m_bFileOwned = false;
			}

			return true;
		}

		std::string m_strName;
		FileSystem::path m_RuntimePath;
		bool m_bFileOwned;
		bool m_bInstalled;
		std::unique_ptr<ws::InternetWorker> m_pInternet;
		std::unique_ptr<ws::PortForwarder> m_pForwards;
		bool m_bCleanupAttempted;
	};

	char const* GetHelpText() {
		return R"(WireSteam 0.4.0 - WireGuard over Steam P2P / SDR

Usage:
  WireSteam.exe                             Interactive launcher
  WireSteam.exe init <config.conf> [--address 10.77.0.1/32]
  WireSteam.exe keygen <private-key-file>
  WireSteam.exe check <config.conf>
  WireSteam.exe preflight <config.conf>
  WireSteam.exe save-config <destination.conf> [--overwrite]
  WireSteam.exe up <config.conf> [--name default] [--verbose] [--run-seconds N]
  WireSteam.exe down [--name default]
  WireSteam.exe bridge <config.conf> [--write-config <file>] [--run-seconds N]
  WireSteam.exe status [--name default]
  WireSteam.exe self-test [--steam] [--appid 480]

init creates a private configuration template and prints its public key.
keygen saves only the private key to a new restricted file and prints the public key.
save-config validates UTF-8 configuration text from standard input (up to 1 MiB)
and saves it with private file permissions. --overwrite permits replacement.
Add a [Peer] with PublicKey, SteamID and AllowedIPs before running up.
Each peer also needs your public key, SteamID and corresponding VPN routes.
preflight validates a profile and checks local addresses, UDP and TCP listeners without
starting Steam, changing adapters, or requesting administrator approval.

Install official WireGuard for Windows and sign in to Steam first.
Run WireSteam as your normal user. Adapter and Internet routing setup requests UAC.
Press Ctrl+C to stop gracefully; approve adapter removal when requested.
bridge runs Steam transport without creating an adapter, for diagnostics/manual setup.
up and bridge accept --stats-seconds 1..3600 to override live statistics only.
The desktop app uses --stop-event Local\WireSteamStop-<GUID> for graceful stopping.
status prints official wg status; its command may require administrator rights.

Private mode supports RFC1918 IPv4 and ULA IPv6 routes.
InternetMode=client/server enables an IPv4 Internet exit over SDR; see INTERNET.md.
Optional [PortForward] rules publish local loopback TCP/UDP services on this profile's
VPN address while up is running; see FORWARDING.md. They are not used by bridge.
Internet clients support Windows 10/11 with a physical IPv4 uplink.
AppID 480 is a shared development default; use your own Steamworks AppID for release.
)";
	}

	struct Options {
		Options() {
			m_wstrName = L"default";
			m_wstrAddress = L"10.77.0.1/32";
			m_unSeconds = 0;
			m_unAppID = 480;
			m_unStatisticsSeconds = 0;
			m_bVerbose = false;
			m_bSteam = false;
			m_bOverwrite = false;
		}

		FileSystem::path m_ConfigurationPath;
		FileSystem::path m_OutputPath;
		std::wstring m_wstrName;
		std::wstring m_wstrAddress;
		std::wstring m_wstrStopEvent;
		unsigned int m_unSeconds;
		unsigned int m_unAppID;
		unsigned int m_unStatisticsSeconds;
		bool m_bVerbose;
		bool m_bSteam;
		bool m_bOverwrite;
	};

	Options ParseOptions(Arguments const& vecArguments) {
		constexpr unsigned int kMaximumRunSeconds = 86400 * 30;
		constexpr unsigned int kMaximumStatisticsSeconds = 3600;

		// Run validates a nonempty argument vector; element zero is the command.
		Options ParsedOptions;
		std::vector<std::wstring> vecSeenOptions;
		for (std::size_t unArgumentIndex = 1; unArgumentIndex < vecArguments.size(); ++unArgumentIndex) {
			std::wstring const& wstrFlag = vecArguments[unArgumentIndex];
			if (!wstrFlag.starts_with(L"--")) {
				Require(ParsedOptions.m_ConfigurationPath.empty(), "Only one configuration path is accepted");
				ParsedOptions.m_ConfigurationPath = wstrFlag;
				continue;
			}

			Require(std::find(vecSeenOptions.begin(), vecSeenOptions.end(), wstrFlag) == vecSeenOptions.end(), "Duplicate option: " + EncodeUTF8(wstrFlag));
			vecSeenOptions.push_back(wstrFlag);
			std::wstring const& wstrCommand = vecArguments[0];
			if ((wstrFlag == L"--verbose") && ((wstrCommand == L"up") || (wstrCommand == L"bridge"))) {
				ParsedOptions.m_bVerbose = true;
			} else if ((wstrFlag == L"--steam") && (wstrCommand == L"self-test")) {
				ParsedOptions.m_bSteam = true;
			} else if ((wstrFlag == L"--overwrite") && (wstrCommand == L"save-config")) {
				ParsedOptions.m_bOverwrite = true;
			} else {
				Require((unArgumentIndex + 1) < vecArguments.size(), "Missing value for " + EncodeUTF8(wstrFlag));
				std::wstring const& wstrValue = vecArguments[++unArgumentIndex];
				if ((wstrFlag == L"--name") && ((wstrCommand == L"up") || (wstrCommand == L"down") || (wstrCommand == L"status"))) {
					ParsedOptions.m_wstrName = wstrValue;
				} else if ((wstrFlag == L"--address") && (wstrCommand == L"init")) {
					ParsedOptions.m_wstrAddress = wstrValue;
				} else if ((wstrFlag == L"--write-config") && (wstrCommand == L"bridge")) {
					ParsedOptions.m_OutputPath = wstrValue;
				} else if ((wstrFlag == L"--run-seconds") && ((wstrCommand == L"up") || (wstrCommand == L"bridge"))) {
					ParsedOptions.m_unSeconds = ParseUnsignedNumber(wstrValue, kMaximumRunSeconds);
				} else if ((wstrFlag == L"--stats-seconds") && ((wstrCommand == L"up") || (wstrCommand == L"bridge"))) {
					ParsedOptions.m_unStatisticsSeconds = ParseUnsignedNumber(wstrValue, kMaximumStatisticsSeconds);
					Require(ParsedOptions.m_unStatisticsSeconds >= 1, "Statistics interval must be from 1 to 3600 seconds");
				} else if ((wstrFlag == L"--stop-event") && ((wstrCommand == L"up") || (wstrCommand == L"bridge"))) {
					Require(ws::IsValidStopEventName(wstrValue), "Invalid GUI stop-event name; use Local\\WireSteamStop- followed by a GUID");
					ParsedOptions.m_wstrStopEvent = wstrValue;
				} else if ((wstrFlag == L"--appid") && (wstrCommand == L"self-test")) {
					ParsedOptions.m_unAppID = ParseUnsignedNumber(wstrValue, UINT32_MAX);
					Require(ParsedOptions.m_unAppID != 0, "AppID must be nonzero");
				} else {
					throw std::runtime_error("Unknown option for this command: " + EncodeUTF8(wstrFlag));
				}
			}
		}

		return ParsedOptions;
	}

	int RunCommand(Arguments const& vecArguments) {
		constexpr unsigned int kGatewayWaitSeconds = 45;

		Require(!vecArguments.empty(), "No command provided");
		std::wstring const& wstrCommand = vecArguments[0];
		if ((wstrCommand == L"--help") || (wstrCommand == L"help") || (wstrCommand == L"-h")) {
			Require(vecArguments.size() == 1, "Help does not accept options");
			std::cout << GetHelpText();
			return 0;
		}

		if (wstrCommand == L"--version") {
			Require(vecArguments.size() == 1, "Version does not accept options");
			std::cout << "WireSteam 0.4.0 (Windows x64; Steamworks SDK 1.65)\n";
			return 0;
		}

		Require((wstrCommand == L"init") || (wstrCommand == L"keygen") || (wstrCommand == L"check") || (wstrCommand == L"preflight") || (wstrCommand == L"save-config") || (wstrCommand == L"up") || (wstrCommand == L"down") || (wstrCommand == L"bridge") || (wstrCommand == L"status") || (wstrCommand == L"self-test"), "Unknown command; use --help");
		Options const ParsedOptions = ParseOptions(vecArguments);

		if (wstrCommand == L"self-test") {
			Require(ParsedOptions.m_ConfigurationPath.empty(), "self-test does not accept a configuration path");
			ws::RunUiControlTests();
			ws::RunSteamIdentityTests();
			ws::RunInternetWorkerTests();
			ws::RunInternetRoutingTests();
			ws::RunInternetServerTests();
			ws::RunConfigTests();
			std::cout << "PASS configuration: keys, Steam IDs, private routes, compiled endpoints and malformed input\n";
			ws::RunPreflightTests();
			ws::RunPlatformTests();
			return ws::RunTransportTests(ParsedOptions.m_bSteam, ParsedOptions.m_unAppID);
		}

		if ((wstrCommand == L"down") || (wstrCommand == L"status")) {
			Require(ParsedOptions.m_ConfigurationPath.empty(), "This command does not accept a configuration path");
			std::string const strName = ParseTunnelName(ParsedOptions.m_wstrName);

			if (wstrCommand == L"status") {
				Require(!ws::FindWg().empty(), "Install official WireGuard for Windows first");
				ws::ProcessResult const Result = ws::RunProcess(ws::FindWg(), L"show " + ws::QuoteArgument(std::wstring(strName.begin(), strName.end())));
				std::cout << Result.m_strOutput;

				if (Result.m_nExitCode) {
					std::cerr << "Cannot read WireGuard status. Run status from an elevated terminal if the tunnel exists. " << Result.m_strError << '\n';
				}

				return (Result.m_nExitCode == 0) ? 0 : 1;
			}

			NameLock NameOwnership(strName);
			std::string strError;
			if (ws::ServiceExists(strName) || ws::InternetServerGateway::HasRecovery(strName) || ws::LegacyInternetRoutes::HasRecovery()) {
				Require(ws::ServiceAction("remove", {}, strName, strError), strError);
			}

			RemoveRuntime(GetRuntimePath(strName));
			std::cout << strName << " is down.\n";
			return 0;
		}

		Require(!ParsedOptions.m_ConfigurationPath.empty(), "A configuration/file path is required; use --help");

		if (wstrCommand == L"save-config") {
			std::string strContents = ReadStandardConfiguration();
			SensitiveStringWiper ContentsWiper(strContents);
			ws::Config ValidatedConfiguration;
			SensitiveConfigurationWiper ValidatedWiper(ValidatedConfiguration);
			std::string strError;
			SensitiveStringWiper ErrorWiper(strError);

			if (!ws::ParseConfig(strContents, ValidatedConfiguration, strError)) {
				throw std::runtime_error("Configuration validation failed: " + SanitizeConfigurationError(strError));
			}

			Require(ws::WritePrivateFile(FileSystem::absolute(ParsedOptions.m_ConfigurationPath), strContents, ParsedOptions.m_bOverwrite, strError), strError);
			std::cout << "Saved private configuration: " << FormatPath(FileSystem::absolute(ParsedOptions.m_ConfigurationPath)) << '\n';
			return 0;
		}

		if ((wstrCommand == L"keygen") || (wstrCommand == L"init")) {
			std::string strPrivateKey;
			SensitiveStringWiper PrivateKeyWiper(strPrivateKey);
			std::string strPublicKey;
			std::string strError;
			SensitiveStringWiper ErrorWiper(strError);
			std::string strContents;
			SensitiveStringWiper ContentsWiper(strContents);
			Require(ws::GenerateKeyPair(strPrivateKey, strPublicKey, strError), strError);

			if (wstrCommand == L"keygen") {
				strContents = strPrivateKey + "\n";
			} else {
				// Keep private template output in its guarded string owner; avoid an additional plaintext stream buffer.
				// Validate even a template's address using the real parser with a temporary dummy peer.
				ws::Key arrDummyKey {};
				arrDummyKey[0] = 0x01;
				strContents = "[Interface]\nPrivateKey = " + strPrivateKey + "\nAddress = " + EncodeUTF8(ParsedOptions.m_wstrAddress) + "\nListenPort = 51820\nMTU = 1280\n\n[WireSteam]\nAppID = 480\nVirtualPort = 0\nRelayOnly = true\n\n";
				std::string const strSample = "[Peer]\nPublicKey = " + ws::EncodeKey(arrDummyKey) + "\nSteamID = 76561198000000000\nAllowedIPs = 10.77.0.2/32\n";
				ws::Config TemporaryConfiguration;
				SensitiveConfigurationWiper TemporaryWiper(TemporaryConfiguration);
				std::string strValidation = strContents + strSample;
				SensitiveStringWiper ValidationWiper(strValidation);
				Require(ws::ParseConfig(strValidation, TemporaryConfiguration, strError), strError);
				strContents += "# Add one or more peers after exchanging public keys and Steam IDs.\n# [Peer]\n# PublicKey = <REMOTE_WIREGUARD_PUBLIC_KEY>\n# SteamID = <REMOTE_STEAM_ID64>\n# AllowedIPs = 10.77.0.2/32\n# PersistentKeepalive = 25\n# SteamPort = 0\n# Dial = true\n";
			}

			Require(ws::WritePrivateFile(FileSystem::absolute(ParsedOptions.m_ConfigurationPath), strContents, false, strError), strError);
			std::cout << "Created private " << (wstrCommand == L"init" ? "configuration template: " : "key file: ") << FormatPath(FileSystem::absolute(ParsedOptions.m_ConfigurationPath)) << "\nPublicKey = " << strPublicKey << '\n';

			if (wstrCommand == L"init") {
				std::cout << "Add a [Peer] before starting the VPN. Exchange only public keys.\n";
			}

			return 0;
		}

		ws::Config Configuration = LoadConfiguration(ParsedOptions.m_ConfigurationPath);
		SensitiveConfigurationWiper ConfigurationWiper(Configuration);

		if (ParsedOptions.m_unStatisticsSeconds) {
			Configuration.m_nStatisticsSeconds = static_cast<int>(ParsedOptions.m_unStatisticsSeconds);
		}

		if (wstrCommand == L"preflight") {
			std::string strError;
			Require(ws::CheckAdapterConflicts(Configuration, strError), strError);

			if (Configuration.m_InternetMode == ws::InternetMode::Client) {
				Require(ws::InternetClientRouting::CheckPreflight(strError), strError);
			}

			std::cout << "Local adapter preflight passed. No VPN address, fixed UDP ListenPort or TCP/UDP forwarding listener conflicts were found.\n";
			return 0;
		}

		if (wstrCommand == L"check") {
			std::cout << "Configuration is valid.\n" << ws::RedactedSummary(Configuration);

			std::string strPublicKey;
			std::string strError;
			if (ws::DerivePublicKey(Configuration.m_strPrivateKey, strPublicKey, strError)) {
				std::cout << "Local PublicKey = " << strPublicKey << '\n';
			} else {
				std::cout << "Public-key derivation unavailable: " << strError << '\n';
			}

			return 0;
		}

		if (wstrCommand == L"bridge") {
			Require(Configuration.m_InternetMode == ws::InternetMode::Off, "Internet profiles require up so their routing and gateway helper is managed safely");
			Require(Configuration.m_vecPortForwards.empty(), "PortForward rules require up so their VPN listener and lifetime are managed with the adapter");
		}

		std::string strPublicKey;
		std::string strError;
		Require(ws::DerivePublicKey(Configuration.m_strPrivateKey, strPublicKey, strError), strError);
		for (auto const& Peer : Configuration.m_vecPeers) {
			Require(Peer.m_strPublicKey != strPublicKey, "A peer public key equals the local public key");
		}

		if (wstrCommand == L"bridge") {
			ws::SteamBridge Bridge = ws::SteamBridge(Configuration, strPublicKey, ParsedOptions.m_bVerbose);
			Require(Bridge.Start(strError), strError);
			std::unique_ptr<ws::StopWatcher> pStopWatcher = ParsedOptions.m_wstrStopEvent.empty() ? std::unique_ptr<ws::StopWatcher> {} : std::make_unique<ws::StopWatcher>(ParsedOptions.m_wstrStopEvent);
			Require(!pStopWatcher || !pStopWatcher->IsSignaled(), "Start cancelled by the desktop app");
			std::string strCompiled = ws::CompileConfig(Configuration, Bridge.WireGuardPort(), Bridge.PeerPorts());
			SensitiveStringWiper CompiledWiper(strCompiled);
			if (!ParsedOptions.m_OutputPath.empty()) {
				Require(ws::WritePrivateFile(FileSystem::absolute(ParsedOptions.m_OutputPath), strCompiled, false, strError), strError);
				std::cout << "Private manual WireGuard configuration saved: " << FormatPath(FileSystem::absolute(ParsedOptions.m_OutputPath)) << '\n';
			}

			std::cout << "Transport only: no WireGuard adapter was installed.\n";
			return Bridge.Run(ParsedOptions.m_unSeconds);
		}

		std::string const strName = ParseTunnelName(ParsedOptions.m_wstrName);
		NameLock NameOwnership(strName);
		Require(!ws::FindWireGuard().empty(), "Install official WireGuard for Windows first");
		Require(!ws::ServiceExists(strName), "Tunnel service already exists. Run down with the same --name before starting");
		Require(ws::CheckAdapterConflicts(Configuration, strError), strError);

		if (Configuration.m_InternetMode == ws::InternetMode::Client) {
			Require(ws::InternetClientRouting::CheckPreflight(strError), strError);
		}

		Session ActiveSession { strName, GetRuntimePath(strName) };
		Require(!FileSystem::exists(ActiveSession.m_RuntimePath), "A private runtime configuration already exists. Run down with the same --name to clean up a previous session");
		ws::SteamBridge Bridge = ws::SteamBridge(Configuration, strPublicKey, ParsedOptions.m_bVerbose);
		Require(Bridge.PrepareSockets(strError), strError);
		std::unique_ptr<ws::StopWatcher> pStopWatcher = ParsedOptions.m_wstrStopEvent.empty() ? std::unique_ptr<ws::StopWatcher> {} : std::make_unique<ws::StopWatcher>(ParsedOptions.m_wstrStopEvent);
		Require(!pStopWatcher || !pStopWatcher->IsSignaled(), "Start cancelled before adapter installation");
		std::string strCompiled = ws::CompileConfig(Configuration, Bridge.WireGuardPort(), Bridge.PeerPorts());
		SensitiveStringWiper CompiledWiper(strCompiled);
		Require(ws::ValidateCompiledConfig(strCompiled, strError, Configuration.m_InternetMode, Configuration.m_strInternetSubnet), strError);
		Require(ws::WritePrivateFile(ActiveSession.m_RuntimePath, strCompiled, false, strError), strError);
		ActiveSession.m_bFileOwned = true;
		Require(!pStopWatcher || !pStopWatcher->IsSignaled(), "Start cancelled before adapter installation");
		std::cout << "Installing " << strName << " (administrator approval may be required)...\n";

		if (Configuration.m_InternetMode != ws::InternetMode::Off) {
			ActiveSession.m_pInternet = std::make_unique<ws::InternetWorker>();
			std::cout << "Preparing Internet " << ws::InternetModeName(Configuration.m_InternetMode) << " helper (administrator approval may be required)..." << std::endl;
			Require(
				ActiveSession.m_pInternet->Prepare(ActiveSession.m_RuntimePath, strName, Configuration.m_InternetMode, Configuration.m_strInternetSubnet, strError, []() -> bool {
					return ws::IsStopRequested();
				}),
				strError);
			std::cout << ActiveSession.m_pInternet->Message() << std::endl;
			Require(!ws::IsStopRequested(), "Internet activation cancelled before Steam startup");
			Require(Bridge.Start(strError), strError);

			if (Configuration.m_InternetMode == ws::InternetMode::Client) {
				Require(Bridge.WaitForGateway(kGatewayWaitSeconds, strError), strError);
			}

			Require(!ws::IsStopRequested(), "Internet activation cancelled before adapter installation");
			Require(
				ActiveSession.m_pInternet->Install(strError, []() -> bool {
					return ws::IsStopRequested();
				}),
				strError);
			std::cout << ActiveSession.m_pInternet->Message() << std::endl;
		} else {
			Require(Bridge.Start(strError), strError);
			Require(!ws::IsStopRequested(), "Start cancelled before adapter installation");
			if (!ws::ServiceAction("install", ActiveSession.m_RuntimePath, strName, strError)) {
				// A helper already inside the official installer can finish after our
				// deadline even if the service has not appeared yet. Preserve its file
				// on every ambiguous failure; down performs an explicit recovery.
				ActiveSession.m_bFileOwned = false;
				std::cerr << "Installation did not complete successfully. Its private runtime configuration was preserved.\n" << "Run WireSteam.exe down --name " << strName.substr(kTunnelPrefixCharacters) << " after the helper has exited.\n";
				throw std::runtime_error(strError);
			}
		}

		ActiveSession.m_bInstalled = true;

		if (!Configuration.m_vecPortForwards.empty()) {
			ActiveSession.m_pForwards = std::make_unique<ws::PortForwarder>([](std::string const& strMessage) -> void {
				std::cout << (strMessage + "\n") << std::flush;
			});
			Require(
				ActiveSession.m_pForwards->Start(Configuration.m_vecPortForwards, strError, [&pStopWatcher]() -> bool {
					return ws::IsStopRequested() || (pStopWatcher && pStopWatcher->IsSignaled());
				}),
				strError);
		}

		Require(!ws::IsStopRequested() && (!pStopWatcher || !pStopWatcher->IsSignaled()), "Start cancelled before tunnel readiness");
		std::cout << "VPN adapter is running. Ctrl+C stops the transport and removes the adapter.\n";
		const int nResult = Bridge.Run(ParsedOptions.m_unSeconds, [&ActiveSession]() -> bool {
			if (ActiveSession.m_pForwards && !ActiveSession.m_pForwards->IsRunning()) {
				std::cerr << "Error: Port forwarding stopped unexpectedly. Stopping the tunnel.\n";
				return false;
			}

			return !ActiveSession.m_pInternet || ActiveSession.m_pInternet->IsAlive();
		});
		const bool bCleaned = ActiveSession.Cleanup();
		// Do not request UAC a second time after a rejected cleanup attempt.
		if (!bCleaned) {
			ActiveSession.m_bInstalled = false;
			ActiveSession.m_bFileOwned = false;
		}

		return nResult ? nResult : (bCleaned ? 0 : 1);
	}

	std::wstring Prompt(wchar_t const* pszTitle, wchar_t const* pszFallback = L"") {
		std::wcout << pszTitle;

		if (*pszFallback) {
			std::wcout << L" [" << pszFallback << L"]";
		}

		std::wcout << L": " << std::flush;
		std::wstring wstrLine;
		if (!std::getline(std::wcin, wstrLine)) {
			return {};
		}

		if ((wstrLine.size() >= 2) && (wstrLine.front() == L'"') && (wstrLine.back() == L'"')) {
			wstrLine = wstrLine.substr(1, wstrLine.size() - 2);
		}

		return wstrLine.empty() ? pszFallback : wstrLine;
	}

	int RunLauncher() {
		while (std::wcin) {
			std::cout << "\nWireSteam - WireGuard over Steam\n1) Create configuration template\n2) Check configuration\n3) Start VPN\n4) Stop leftover VPN adapter\n5) Run transport only\n6) Show help\n0) Exit\n";
			std::wstring const wstrChoice = Prompt(L"Choose", L"3");
			if (!std::wcin || (wstrChoice == L"0")) {
				return 0;
			}

			Arguments vecArguments;
			if (wstrChoice == L"1") {
				vecArguments = { L"init", Prompt(L"New configuration path", L"wiresteam.conf"), L"--address", Prompt(L"Local VPN address", L"10.77.0.1/32") };
			} else if (wstrChoice == L"2") {
				vecArguments = { L"check", Prompt(L"Configuration path", L"wiresteam.conf") };
			} else if (wstrChoice == L"3") {
				vecArguments = { L"up", Prompt(L"Configuration path", L"wiresteam.conf"), L"--name", Prompt(L"Tunnel name", L"default") };
			} else if (wstrChoice == L"4") {
				vecArguments = { L"down", L"--name", Prompt(L"Tunnel name", L"default") };
			} else if (wstrChoice == L"5") {
				vecArguments = { L"bridge", Prompt(L"Configuration path", L"wiresteam.conf") };
			} else if (wstrChoice == L"6") {
				vecArguments = { L"help" };
			} else {
				std::cout << "Choose a number from 0 to 6.\n";
				continue;
			}

			if (!std::wcin) {
				return 0;
			}

			// Commands report their result; this interactive menu continues after each invocation.
			try {
				RunCommand(vecArguments);
			} catch (std::exception const& Exception) {
				std::cerr << "Error: " << Exception.what() << '\n';
			}
		}

		return 0;
	}
} // namespace

int wmain(int nArgumentCount, wchar_t** ppszArguments) {
	// Console presentation is best effort; redirected UTF-8 records do not depend on this setting.
	if (!SetConsoleOutputCP(CP_UTF8)) {
		OutputDebugStringA("WireSteam: cannot select UTF-8 console output.\n");
	}

	try {
		if (nArgumentCount == 1) {
			return RunLauncher();
		}

		return RunCommand(Arguments(ppszArguments + 1, ppszArguments + nArgumentCount));
	} catch (std::exception const& Exception) {
		try {
			std::cerr << "Error: " << Exception.what() << '\n';
		} catch (...) {
			OutputDebugStringA("WireSteam: cannot report command-line failure.\n");
		}

		return 1;
	} catch (...) {
		OutputDebugStringA("WireSteam: command-line entry failed.\n");
		return 1;
	}
}
