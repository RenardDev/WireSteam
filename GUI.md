# WireSteam desktop app

Run `WireSteam.UI.exe` from the installed folder or the extracted distribution. The app includes its .NET runtime. Steam must be running under the correct account, and official WireGuard for Windows must be installed. Keep the app in your normal Windows user session; its helper requests administrator approval when needed.

## Profiles and activation

Use **IMPORT** to add WireSteam `.conf` files. The app stores private copies in `%LOCALAPPDATA%\WireSteam\profiles`; editing or deleting a profile leaves the original file unchanged. On first launch from a project/distribution folder, it also discovers the adjacent `configs` directory.

- **NEW TUNNEL** creates a local private key and opens the configuration editor.
- **EDIT PROFILE** and **MANAGE FORWARDS** require a stopped profile with no pending cleanup.
- **EXPORT** saves the complete private configuration, including its keys. Share only with someone who should possess those keys.
- Right-click a profile to connect, disconnect, edit, delete or export that profile.

Select a profile and choose **ACTIVATE TUNNEL**. Use **CANCEL ACTIVATION** during startup or **DEACTIVATE TUNNEL** to stop it. The footer reports the final result, including failed activation or denied administrator approval. Closing the window leaves tunnels running in the tray.

Do not run the same configuration through the app and CLI simultaneously. Activation checks for conflicting interface addresses and fixed WireGuard UDP ListenPorts. Existing adapters are never stopped automatically; changing only a ListenPort does not fix a duplicate VPN address. If an old CLI bridge has ended but left its adapter behind, remove that specific adapter, for example:

```powershell
.\WireSteam.exe down --name host
```

The dashboard shows local adapter and Steam transport readiness, peer routes, relay latency and traffic counters. **ACTIVE** means the local adapter is running and at least one authenticated Steam peer is ready. Verify an actual WireGuard handshake and ping a peer's VPN address or connect to its service to check the full path.

**PEER LINKS** shows separate receive/send rates and accumulated encrypted WireGuard payload bytes carried by Steam, including protocol traffic. Activity lights reflect recent sampled counter changes. Steam nicknames are display names; the configured SteamID64 remains the peer identity.

Use **MANAGE FORWARDS** or **SETTINGS → PORT FORWARDING** to publish loopback-only services to VPN peers. Rules start and stop with the adapter. The app supports up to 32 TCP/UDP rules, exact profile VPN listener addresses and loopback-only targets. Internet-client profiles cannot host forwards. See [port forwarding](FORWARDING.md) for configuration and firewall examples. Throughput benchmarks are separate [command-line tools](SPEEDTEST.md).

## IPv4 Internet profiles

Import matching gateway and client profiles, then start the gateway before the client. Stop any private profile that shares their address or keys first. Edit the role in the stopped profile's configuration editor. Dashboard/settings labels show **PRIVATE NETWORK**, **INTERNET CLIENT** or **INTERNET GATEWAY** for the configured role; they do not prove a handshake or Internet access. See [Internet setup and recovery](INTERNET.md) for the complete requirements and commands.

- Clients require Windows 10/11 x64 and a physical IPv4 Internet connection. Disable WARP and other active client VPNs before activation.
- A Windows gateway requires a functioning WinNAT provider and no existing NAT. WireSteam refuses to replace another NAT and does not enable ICS or install Windows features.
- IPv4 application traffic and configured IPv4 DNS use the tunnel; non-loopback IPv6 is blocked during the client session.
- Steam and `WireSteam.exe` need a physical-uplink exception. On newer Windows, this covers all non-loopback IPv4 traffic from the detected Steam executable paths, including unrelated Steam activity.
- When per-application routing is unavailable, guarded endpoint routing learns public Steam UDP destinations. The first attempt to a new destination is blocked while its physical route is authorized, so retries may be needed. Private/LAN and multicast UDP stay blocked; destinations shared with Steam can also be blocked for other applications. Startup requires an observed/routed UDP attempt and an authenticated gateway before activating the adapter.
- The egress guard lasts for the live helper session. It does not remain after forced helper termination or reboot.

Check the gateway's public IPv4 address from a normal client browser, the WireGuard handshake, and DNS/IPv6 behavior on the actual computers.

## Tray and cleanup

Closing or minimizing hides the window to the tray. Left-click the tray icon, choose **OPEN WIRESTEAM**, or launch the app again to reopen it. The tray offers per-profile controls and **DISCONNECT ALL**. To start hidden:

```powershell
.\WireSteam.UI.exe --tray
```

**QUIT WIRESTEAM** waits for managed tunnels and adapter cleanup. If cleanup fails or administrator approval is denied, the window reopens with the error. The app controls only tunnels it started; profiles reopen inactive after a normal restart.

After an interrupted session, **CLEANUP REQUIRED** means the private runtime configuration remains. Choose **CLEAN UP ADAPTER** before editing, deleting or reactivating the profile. A process still managing that tunnel name must finish first.

Internet-gateway cleanup can recover acknowledged owned NAT and forwarding changes from a protected durable record. Incomplete or modified records are refused. If manual recovery is needed, use the local `ui-...` name shown in the event log, for example:

```powershell
.\WireSteam.exe down --name ui-30c76bbda69e
```

See [stop and recover](INTERNET.md#stop-and-recover) before removing recovery files or adapters manually.

## Logs

Select text in **EVENT STREAM** or **EVENT LOG** and press **Ctrl+C** or choose **COPY**. **COPY LOG** copies the selected profile's complete current log. The log page also supports search and clear. Uncheck **AUTO-SCROLL** to inspect older messages; the setting is shared by both views.

Logs are session-only. Each profile keeps up to 2,000 entries and 1 MiB of UTF-16 text; reaching either limit clears the previous log and records a warning. Individual messages are limited to 8,192 characters. Copy needed diagnostics before quitting or before the buffer clears. Private/preshared key assignments are redacted, but review logs before sharing them.

## Other VPN clients

Private-network activation adds its own WireGuard adapter, addresses and peer routes. Internet-gateway mode changes forwarding only on the exact owned WireSteam adapter and preserves the existing default exit route. Recovery can restore external forwarding changes recorded by an older session.

Adapter creation and WinNAT still generate Windows network notifications that can affect another VPN client. Compatibility with a particular VPN requires live testing; absence of reconnects is not guaranteed. Internet clients should be used with other active VPNs disabled.
