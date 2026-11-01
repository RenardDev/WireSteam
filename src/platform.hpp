#pragma once

// STL
#include <filesystem>
#include <string>

// Project
#include "config.hpp"

namespace ws {
	struct ProcessResult {
		int m_nExitCode = -1;
		std::string m_strOutput;
		std::string m_strError;
	};

	bool IsElevated();
	std::filesystem::path FindWireGuard();
	std::filesystem::path FindWg();
	ProcessResult RunProcess(std::filesystem::path const& ExecutablePath, std::wstring const& wstrArguments, std::string const& strInput = {}, unsigned unTimeoutMilliseconds = 15000, unsigned unMaximumOutputBytes = 1024 * 1024);
	bool DerivePublicKey(std::string const& strPrivateKey, std::string& strPublicKeyOut, std::string& strErrorOut);
	bool GenerateKeyPair(std::string& strPrivateKeyOut, std::string& strPublicKeyOut, std::string& strErrorOut);
	bool WritePrivateFile(std::filesystem::path const& Path, std::string const& strContents, bool bOverwrite, std::string& strErrorOut);
	bool ReadConfigFile(std::filesystem::path const& Path, std::string& strContentsOut, std::string& strErrorOut);
	std::wstring QuoteArgument(std::wstring const& wstrValue);
	bool ServiceExists(std::string const& strName);
	bool ServiceMatchesConfig(std::string const& strName, std::filesystem::path const& ConfigurationPath, std::string& strErrorOut);

	// Invokes WireSteamService.exe with UAC only when needed. It never elevates Steam.
	bool ServiceAction(std::string const& strAction, std::filesystem::path const& ConfigurationPath, std::string const& strTunnelName, std::string& strErrorOut, InternetMode ModeValue = InternetMode::Off, std::string const& strSubnet = {});
	int ServiceHelperMain(int nArgumentCount, wchar_t** ppszArguments);
	void RunPlatformTests();
} // namespace ws
