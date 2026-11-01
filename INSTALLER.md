# Installation and updates

The MSI installs WireSteam for all users on Windows 10 22H2 (build 19045) and Windows 11 x64. The desktop app includes its .NET runtime.

## Install

1. Install [official WireGuard for Windows](https://www.wireguard.com/install/) in its standard location.
2. Run `WireSteam-<version>-x64.msi` and approve administrator access.
3. Open **WireSteam** from the Start menu and sign in to Steam.

Setup checks the registered WireGuard installation and its `wireguard.exe` and `wg.exe` files. If they are missing, install or repair WireGuard and run setup again. Steam and WireGuard are installed separately.

The default folder is `C:\Program Files\WireSteam`. The installer offers optional desktop shortcuts, bandwidth tools and TCP/UDP test tools. Private profiles and keys are not part of the installer.

## Update, reinstall and remove

Run the MSI for the desired version. Other registered WireSteam versions are removed as part of the replacement, including when installing an older version. Running the same MSI opens maintenance options; quit WireSteam first for maintenance or uninstall.

Before replacing files, setup closes installed WireSteam UI, engines and test tools, and stops and removes verified WireSteam tunnel services. Active WireSteam connections will be interrupted. The privileged helper gets time to restore its routing and NAT changes. If it cannot finish, setup fails before replacing files and reports the cleanup problem.

Setup targets WireSteam components only. It does not stop Steam, the WireGuard manager or unrelated VPN tunnels. Its tunnel-service cleanup also covers verified WireSteam tunnels started by a portable copy. Do not start WireSteam again while setup is running.

To remove WireSteam, use **Installed apps → WireSteam → Uninstall**. Your profiles, catalog and recovery files under `%LOCALAPPDATA%\WireSteam` are preserved. WireGuard and Steam remain installed.

If setup reports a leftover tunnel, run the installed engine with its actual local tunnel name:

```powershell
& 'C:\Program Files\WireSteam\WireSteam.exe' down --name host
```

The local name may instead be `ui-<profile-id-prefix>`; check the tunnel log. Approve the cleanup prompt, wait for it to finish, then retry setup. See [Internet-mode recovery](INTERNET.md#stop-and-recover) for routing or gateway cleanup.

## Unattended setup

Run from an administrator terminal, replacing the example filename with your MSI:

```powershell
msiexec.exe /i .\WireSteam-0.6.1-x64.msi /qn /norestart /L*v .\WireSteam-install.log
```

Use `ADDLOCAL=ALL` to include every optional feature, or `ADDLOCAL=MainFeature,SpeedTestFeature,TestAppsFeature` for the app and tools. Set `INSTALLFOLDER="C:\Program Files\WireSteam"` to choose the destination.

Silent setup performs the same prerequisite and cleanup checks. Inspect the MSI log if it fails.

## Portable package

Extract `WireSteam-win64.zip` and start `WireSteam.UI.exe`. Keep its adjacent engine, helper and Steam DLL together. The MSI only replaces registered installations; portable copies are managed separately.

The MSI is currently unsigned. Windows may show an unknown-publisher prompt.

For source builds and installer validation, see CONTRIBUTING.md and PUBLISHING.md in the source repository.
