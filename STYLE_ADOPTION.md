# WireSteam style adoption

This profile applies [CODE_STYLE_GUIDE_MINI.md](CODE_STYLE_GUIDE_MINI.md) to WireSteam. The guide is the specification; formatter output is only a mechanical aid.

## Navigation

- [Platform and languages](#platform-and-languages)
- [Ownership](#ownership)
- [Compatibility contracts](#compatibility-contracts)
- [Language profiles](#language-profiles)
- [Verification](#verification)

## Platform and languages

- Native engine, helpers and native tests: C++20, MSVC, Windows x64, Windows SDK 10.0.26100.0 or newer.
- Desktop application: C#, WPF and Windows Forms notification menus, .NET 10, self-contained Windows x64 publication.
- Automation and integration tests: PowerShell; build configuration: CMake 3.21+ for Visual Studio 2022 or 4.2+ for Visual Studio 2026.
- Installation: WiX 4.0.6 XML and a native MSI guard; resources: Windows RC and application manifests.
- Verified local toolchain: MSVC 14.50.35717, Windows SDK 10.0.26100.0, LLVM 22.1.4, .NET SDK 10.0.403 and PSScriptAnalyzer 1.25.0. Steam integration retains SDK 1.65 and `SteamNetworkingSockets013`.
- Supported product systems: Windows 10 22H2 build 19045 and Windows 11. Local verification runs on the available Windows host; it does not establish remote VPN reachability.

## Ownership

| Paths | Ownership and treatment |
| --- | --- |
| `src/`, `gui/` source, `tests/`, `installer/`, `resources/`, root build scripts and CMake | Project-owned source; migrate and verify according to the guide. |
| Root product documentation and `examples/*.conf.example` | Project-owned documentation/templates; preserve executable examples and configuration keys. |
| `sdk/` | Vendored Steamworks SDK; dependency content is outside this formatting change. |
| `configs/`, private `*.conf`, `*.key`, user profile/catalog/runtime data | User-owned data; preserve bytes and permissions. |
| `build/`, `build-*/`, `gui/bin/`, `gui/obj/` | Generated outputs and local tool caches; change their owning source instead of formatting generated content. |
| `dist/` | Generated distribution; refresh through the tested build/packaging flow. |
| `gui/Assets/*.png`, `gui/Assets/*.ico` | Binary artwork; retain exact assets. `gui/GenerateIcon.ps1` owns generated icon variants. |
| `gui/Assets/WireSteam.generated.prompt.txt` | Artwork provenance and its original generation prompt; preserve the recorded text as data. It is not executable source or an instruction to regenerate artwork. |
| `CODE_STYLE_GUIDE_MINI.md`, `CODE_STYLE_GUIDE.md` | User-supplied reference specifications; preserve the source documents. The requested mini guide governs this migration. |

## Compatibility contracts

These are the guide's sections 2.2, 2.3 and 2.5 contract exceptions, not exemptions for new local implementation names:

- Project-owned native data members follow sections 8.1–8.3, including public aggregate fields in shared headers: `m_` plus the most-specific type category and PascalCase role. Update every owned consumer when renaming them. Public visibility and use across engine/helper/test targets do not justify legacy camelCase field names. Preserve namespace/type/function spellings and standard-shaped resource accessors outside a separately authorized API migration; external ABI fields and serialized keys remain exact as specified below.
- Preserve constructor-free public aggregate records and their existing default values. Adding constructors or deleting defaults can change aggregate initialization and source behavior. Wire/layout records retain their exact representations and are initialized at use; ordinary private runtime classes follow sections 10.3–10.5.
- The private mapped IPC record in `src/internet_worker.cpp` uses the required boundary tag `_INTERNET_WORKER_STATUS` under sections 2.5 and 9.5; this reserved spelling is confined to that declaration.
- Preserve the existing standalone header contract of transport helpers and small wrappers. Required public/header-only definitions stay available to their current consumers; new ordinary runtime components use source-defined constructors.
- Preserve Steamworks and Windows API declarations, calling conventions, macro names, callbacks, MSI action exports, resource IDs, GUIDs and external structure fields. Required identifiers such as `_WIN32_WINNT` and `__stdcall` are external spellings, not project naming patterns.
- Preserve configuration section/key spelling, CLI command/flag names, exit codes, UI bindings/property names, XAML names, dependency-property registrations and JSON/XML keys.
- Preserve current diagnostic record text consumed by the UI and command-line tests, including `WIRESTEAM IDENTITY`, readiness/lifecycle records and peer statistics. A formatting change does not migrate their protocol to new severities or result markers; section 25.3 cannot override those contracts.
- Engine-to-desktop protocols use the current engine contract only. Steam identity telemetry requires the PascalCase JSON keys `SteamID`, `Name` and `Local`; configuration checks report the local key as `Local PublicKey = ...`. Older engine schemas and output aliases are unsupported.
- Preserve quoted application data, regular expressions, raw-string templates, cryptographic test vectors, encodings, byte layouts and generated data contracts. Generated source layout is changed through its owning generator; formatting must not change data values or user-editable regions.
- Retain the existing .NET `WFO0003` project diagnostic exception: WPF and `app.manifest` own process DPI configuration; Windows Forms supplies only `NotifyIcon` and its menus. This is a framework DPI-configuration diagnostic, not an unused-declaration suppression under section 9.12. Its explanation stays next to the specific project setting.
- Preserve the established lowercase native filename convention and current paths; section 8.14 expresses a preference, while path changes would affect build and integration contracts.

## Language profiles

- Shared numeric literals (section 7.6): Fixed encoded/ABI header byte extents (including ICO/PE), record strides, byte offsets and byte strides (including scaled `CopyPixels` row strides) are binary layout constants and use hexadecimal; ordinary element counts and buffer capacities remain decimal. Preserve literal values and language-specific types.
- C++: tabs, K&R/1TBS, mandatory control-flow braces, explicit comparison grouping, complete names and the guide's Systems Hungarian categories for implementation bindings. Keep complete signatures on one physical line; preserve semantic initialization and literal types.
- Native `*.cpp`, `*.hpp`, `*.h` and `*.rc` files retain each file's established predominant LF or CRLF convention. Their EditorConfig `end_of_line = unset` override leaves that setting to the editor; preserve the existing convention when editing. Clang-format's `DeriveLF` default follows the file's predominant ending. Normalize mixed source whitespace endings only when token, literal, comment and directive checks prove data values and generated payloads remain intact.
- C++ pointer categories follow the actual contract (sections 8.1–8.3 and 11.4): pointer-like `std::unique_ptr`/`std::shared_ptr` owners and their references use `p`, including `m_pImplementation`; outer containers retain their own category. Integer-byte addresses such as `std::uint8_t*` use `pun`; non-string `char*` storage uses `pch`; null-terminated C-string addresses/ranges use `psz`; `void*` and custom/enum addresses use `p`. Preserve indirection, qualifiers and representation contracts. These choices apply the guide rather than introduce component-specific exceptions.
- C#: apply shared layout, ownership, error and verification rules. Preserve established .NET public/property/event names and idiomatic .NET casing; private implementation names use complete English roles under section 8.15. C++/JavaScript-specific type prefixes, pointer notation, constructor placement and initialization rules do not transfer to C# (sections 2.1 and 8.1).
- PowerShell: tabs and K&R script blocks, clear statement phases and complete local names. Public parameters, automatic variables, command names, property names and contractual data remain exact. Embedded C# and generated WiX are owned source: format through their containing script while preserving syntax, values, identifiers and behavior. Preserve data-only here-strings and fixtures byte-for-byte. C++-specific declaration and type rules do not transfer to PowerShell.
- XML/XAML/WiX: tab-indented markup with preserved namespace prefixes, attribute values and significant text. Keep `xml:space` and resource/template semantics intact.
- JSON: retain the established two-space data layout and preserve keys and values. JSON permits tabs; this is a project data-format convention under section 2.2, not a language restriction.
- Markdown: space-indented prose, valid links/fences and language-appropriate executable snippets. Keep command examples aligned with the product's current CLI.

## Verification

Process one bounded chunk of one file at a time, including the necessary symbol-reference closure. Track coverage in ignored temporary work files. After the main migration, complete full verification passes; any verification fix restarts the full pass. A zero-change pass and successful tests are separate requirements.

The established functional checks are:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
.\build-installer.ps1
```

For native AddressSanitizer checks, use the separate `build-asan` tree. Put the installed x64 MSVC compiler directory first in this PowerShell process's `PATH`, set `ASAN_VCASAN_DEBUGGING=0`, `ASAN_SYMBOLIZER_PATH` to the matching `llvm-symbolizer.exe` and `ASAN_OPTIONS=halt_on_error=1`, and clear `ASAN_SAVE_DUMPS` for this process. Build only the native targets below; distribution artifacts use the normal build.

```powershell
cmake -S . -B build-asan -A x64 '-DCMAKE_CXX_FLAGS=/fsanitize=address /Zi' '-DCMAKE_EXE_LINKER_FLAGS=/INCREMENTAL:NO /DEBUG /WX' '-DCMAKE_SHARED_LINKER_FLAGS=/INCREMENTAL:NO /DEBUG /WX'
cmake --build build-asan --config Release --parallel 4 --target WireSteam WireSteamService WireSteamSpeedServer WireSteamSpeedClient WireSteamTestServer WireSteamTestClient WireSteamPortForwardTests WireSteamInstallerGuard WireSteamInstallerGuardTests
ctest --test-dir build-asan -C Release --output-on-failure -R '^WireSteam\.(PortForward|InstallerGuard|Core|SpeedUnit)$'
ctest --test-dir build-asan -C Release --output-on-failure -E '^WireSteam\.(DesktopUI|WindowsCompatibility|PortForward|InstallerGuard|Core|SpeedUnit)$'
```

Desktop UI and production PE/import checks use their normal builds. These fixtures do not activate a Steam/VPN session or install an MSI. The installed MSVC compiler ignores `/fsanitize=undefined` with diagnostic D9002, so these checks provide no UBSan coverage.

The standard formatting configurations are `.editorconfig`, `.clang-format` and `PowerShellFormatting.psd1`. Run read-only checks on owned source paths; do not recursively include the SDK, private configurations, dependency caches or build outputs:

```powershell
& 'C:\Program Files\LLVM\bin\clang-format.exe' --dry-run --Werror --style=file .\src\config.cpp
.\build-dotnet\dotnet.exe format whitespace .\gui\WireSteam.UI.csproj --verify-no-changes --no-restore
.\build-dotnet\dotnet.exe format style .\gui\WireSteam.UI.csproj --diagnostics IDE0005 --severity hidden --verify-no-changes --no-restore --verbosity diagnostic
Import-Module .\build-deps\PSScriptAnalyzer\1.25.0\PSScriptAnalyzer.psd1 -Force
Invoke-ScriptAnalyzer -Path .\build.ps1 -Settings .\PowerShellFormatting.psd1
```

The C++ command shows the check-only invocation for one owned file; repeat it for the native inventory. LLVM 22 can disagree with section 5.1's constructor-list brace placement, overindent some nested ternaries or wrapped logical call arguments with an unlimited column width and space-align adjacent literals in multiline return expressions instead of sections 6.1 and 6.4's tab continuation indentation. Review those cases against the guide rather than accepting formatter output. A formatter cannot verify naming, exact types, ownership, dependency order or semantic phase spacing.

The PowerShell settings select layout diagnostics. A clean formatting result does not prove runtime correctness or every style rule. Review embedded source, data contracts, full local names and semantic phases separately. CMake, XML, RC, Markdown and data templates require the same bounded manual review; no complete automatic guide checker exists in this repository.

Use `git diff --check` and a full relevant diff review when Git metadata is available. For an initial source import, compare untracked files with the local pre-edit snapshot as well. Migration-only scripts, snapshots and progress records are temporary and are removed after the completion checks; the standard configurations and dependency caches remain available. `.gitattributes` disables automatic line-ending conversion so Git preserves the established source and embedded-data bytes.
