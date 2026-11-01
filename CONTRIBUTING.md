# Contributing to WireSteam

Read [CODE_STYLE_GUIDE_MINI.md](CODE_STYLE_GUIDE_MINI.md) and [STYLE_ADOPTION.md](STYLE_ADOPTION.md) before editing. Keep changes focused and preserve configuration, CLI, Steam and WireGuard contracts unless the change deliberately updates them.

## Development environment

Use Windows x64, Visual Studio C++ tools, Windows SDK 10.0.26100.0 or later, PowerShell and the .NET 10 SDK. Use CMake 3.21+ with Visual Studio 2022, or CMake 4.2+ with Visual Studio 2026. `global.json` selects a stable .NET 10 feature band. Obtain Steamworks SDK 1.65 separately and place its SDK directory in `sdk/`; the directory must contain `public/steam/steam_api.h` and `redistributable_bin/win64/steam_api64.lib` plus `steam_api64.dll`. Download it from [Steamworks](https://partner.steamgames.com/doc/sdk). Install official WireGuard before running native integration tests.

The SDK, private profiles and local dependencies are excluded from Git. Use `examples/*.conf.example` as templates and generate your own keys locally. Never commit working profiles, diagnostic runtime configurations, private keys, crash dumps or unredacted identity logs.

## Build and test

The complete local build, automated tests and portable packaging are:

```powershell
.\build.ps1
```

To configure and run the Debug native suite:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

For a focused desktop change, with the native engine already built:

```powershell
dotnet build .\gui\WireSteam.UI.csproj --configuration Release -warnaserror
.\gui\bin\Release\net10.0-windows\win-x64\WireSteam.UI.exe --self-test --engine .\build\Release\WireSteam.exe
```

Use the matching Debug engine and UI configuration when checking Debug behavior.

The GitHub Actions workflow builds the desktop application in Debug and Release and checks its whitespace without requiring the Steamworks SDK. It does not run native integration tests, activate a VPN or validate an MSI. Full local checks and two-computer network tests remain separate requirements.

For an installer change, run `.\build.ps1 -Installer`. The installer checks inspect the payload, MSI tables, extracted files and native OS guard without installing the product or changing networking.

Full ICE validation has narrowly scoped authoring exceptions: ICE20 requires the FilesInUse/ListBox UI deliberately omitted so deferred shutdown can run first; ICE57 does not account for ALLUSERS=1 on the two machine-wide shortcut components using HKMU markers; ICE61 warns about the unrestricted replacement range needed for downgrades. Review only those exact findings as expected; resolve any additional error. The package-table tests enforce shutdown sequencing, machine scope, shortcut targets and registry ownership.

## Review and reporting

- Run checks appropriate to the affected surface and report the commands and results in the pull request. Describe tests that could not be run.
- Review the full diff and run `git diff --check`. Preserve each native file's established line endings; `.gitattributes` disables automatic conversion.
- Keep generated files, local tool caches and release binaries out of source commits. Update their owning source or build script instead.
- Use synthetic SteamIDs and placeholder keys in examples, previews and tests. Redact personal identities and paths before attaching logs.
- For suspected security problems, ask the maintainer for a private reporting channel before posting exploit details, private profiles or sensitive logs in a public issue.

Dependency provenance is recorded in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Repository and binary release preparation are described in [PUBLISHING.md](PUBLISHING.md).
