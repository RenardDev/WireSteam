# Third-party dependencies and notices

This file records WireSteam's dependency boundaries and official upstream sources. A license for WireSteam's own code does not replace the licenses or agreements covering these dependencies.

## Steamworks SDK and Steam

WireSteam's native build uses Valve's Steamworks SDK **1.65**, including its headers, `steam_api64.lib`, and the `steam_api64.dll` runtime. Obtain the SDK from the official [Steamworks SDK page](https://partner.steamgames.com/doc/sdk) and extract it locally into `sdk/`, or select an external SDK through CMake's `STEAMWORKS_SDK_ROOT` option. The source repository excludes `sdk/`; it does not vendor or relicense Valve's SDK.

The packaging scripts copy the runtime DLL from that local SDK. Valve documents this runtime integration in the [Steamworks API overview](https://partner.steamgames.com/doc/sdk/api). The applicable Steamworks SDK and Steam agreements remain separate from this repository's license; consult the terms accompanying your SDK and Steamworks account before distributing a binary build.

AppID **480** belongs to Valve's [SpaceWar example](https://partner.steamgames.com/doc/sdk/api/example). WireSteam currently uses it for development. Its presence in the examples does not provide an independent production AppID or a guarantee of relay service availability.

## WireGuard for Windows

WireSteam invokes the separately installed official WireGuard for Windows tools. Its source repository and release payload do not include the WireGuard installer, driver, `wireguard.exe`, or `wg.exe`.

Install WireGuard from its [official download page](https://www.wireguard.com/install/). The upstream [WireGuard for Windows repository and copyright/license file](https://git.zx2c4.com/wireguard-windows/tree/COPYING) describe that project's terms. Consult the notices supplied with the specific WireGuard version you install.

## .NET runtime, WPF, and Windows Forms

The desktop application targets **.NET 10** and publishes as a self-contained Windows x64 executable. Self-contained publishing [includes the runtime](https://learn.microsoft.com/en-us/dotnet/core/deploying/). The current locally verified runtime is **10.0.14**; another SDK or rebuild can select a different patch version.

The upstream .NET runtime, WPF, and Windows Forms repositories use the MIT license and carry additional component notices:

- [.NET runtime license](https://github.com/dotnet/runtime/blob/v10.0.14/LICENSE.TXT) and [third-party notices](https://github.com/dotnet/runtime/blob/v10.0.14/THIRD-PARTY-NOTICES.TXT).
- [WPF license](https://github.com/dotnet/wpf/blob/v10.0.14/LICENSE.TXT).
- [Windows Forms license](https://github.com/dotnet/winforms/blob/v10.0.14/LICENSE.TXT).

When distributing a self-contained binary, retain the license and component notices applicable to the exact runtime and desktop components included in that build. These upstream links identify the verified version; they are not a complete replacement for the notices accompanying a binary distribution.

## Build tools

The MSI build restores **WiX Toolset 4.0.6** and its UI extension into the ignored `build-installer/` cache. This version's upstream [license file](https://github.com/wixtoolset/wix/blob/v4.0.6/LICENSE.TXT) specifies the Microsoft Reciprocal License (MS-RL). WiX executables and downloaded extension packages are not source-repository content.

MSVC, the Windows SDK, CMake, the .NET SDK, PowerShell, LLVM, and PSScriptAnalyzer are development tools or local prerequisites. Their local installations and caches are excluded from the source repository. Obtain them from their respective official publishers and retain their accompanying terms. The source repository contains build instructions in CONTRIBUTING.md and toolchain details in STYLE_ADOPTION.md.

## Artwork

The WireSteam application artwork is project artwork. The source repository records its generation provenance in gui/Assets/WireSteam.generated.prompt.txt; gui/GenerateIcon.ps1 produces the icon variants.
