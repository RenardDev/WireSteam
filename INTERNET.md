# Internet access through a WireSteam exit host

Internet mode sends ordinary client IPv4 traffic through WireGuard, Steam Datagram Relay and a Windows exit host's NAT. Websites should see the exit host's public IPv4 address. `InternetMode = off` keeps private-network behavior; Internet access requires explicit client and server profiles. Use `up` or desktop activation; `bridge` is a transport diagnostic.

## Requirements

- Both computers need official WireGuard for Windows, the WireSteam release files and Steam running under different signed-in accounts. Run WireSteam as your normal user; its helper requests administrator approval.
- Clients support Windows 10/11 x64, including Windows 10 22H2 build 19045, and require a physical IPv4 Internet uplink. Disconnect WARP and other active VPNs. Virtual default uplinks and other WireSteam adapters are refused.
- The gateway needs a functioning Windows NetNat provider and the built-in Windows PowerShell NetNat module. WireSteam refuses any existing NAT, including Docker, Hyper-V, HNS or another WireSteam session. [WinNAT supports one internal NAT subnet prefix per host](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/setup-nat-network).
- WireSteam does not install Windows features, enable Hyper-V or ICS, reboot, or change container networks. If NetNat is unavailable, configure the required Windows capabilities separately or use another exit host.
- Choose a VPN subnet separate from the physical LAN and other adapters. The examples use `10.77.0.0/24`; server setup refuses it if another adapter has an address inside it. Do not activate private and Internet-profile copies with the same address and keys simultaneously.

Windows selects per-application routing when available and guarded endpoint routing otherwise. The fallback requires usable Windows Filtering Platform DROP-event telemetry and an observed/routed transport UDP attempt before adapter activation. Selection is automatic. See [routing behavior](#transport-bypass-dns-and-ipv6) and [current limits](#current-limits).

## Example host and clients

Start with [the server example](examples/internet-server.conf.example) and [the client example](examples/internet-client.conf.example). Replace all key and SteamID64 placeholders. `WireSteam.exe init <config.conf>` creates a private template with a fresh key and prints its public key; exchange public keys and SteamID64 values only. Keep each private configuration on its assigned computer.

| Computer | VPN address | Example filename |
| --- | --- | --- |
| Exit host | `10.77.0.1` | `host-internet.conf` |
| Client | `10.77.0.2` | `client-internet.conf` |
| Additional client | `10.77.0.3` | `client2-internet.conf` |

Every client needs its own Steam account, key pair, address and server `[Peer]` entry. Multiple authorized clients may use one gateway concurrently. These examples use `AppID = 480`, `VirtualPort = 40` and `RelayOnly = true` on both sides.

## Start the server, then the client

From the release folder on the exit host, with its private profile saved there:

```powershell
.\WireSteam.exe check .\host-internet.conf
.\WireSteam.exe up .\host-internet.conf --name host-internet
```

Approve UAC and wait for the adapter to run. The server creates its own NAT and enables IPv4 forwarding only on its exact owned WireGuard adapter. It preserves the existing default exit route; forwarding through another VPN depends on that VPN accepting the traffic.

On the client computer, with that client's profile next to its release files:

```powershell
.\WireSteam.exe check .\client-internet.conf
.\WireSteam.exe up .\client-internet.conf --name client-internet
```

For another client, use its own filename and tunnel name. The client waits for an authenticated Steam gateway before installing the Internet adapter. `WIRESTEAM READY` describes the outer Steam connection and key-configuration checks; verify the WireGuard handshake and Internet path separately.

In the desktop app, stop the old private profile, import the matching Internet profile and activate it. Imports are private managed copies: later edits to the original file do not update the imported profile. Closing the window leaves the session in the tray. Use **DEACTIVATE TUNNEL**, **DISCONNECT ALL** or **QUIT WIRESTEAM** to stop it.

## Configuration rules

Add to the server's complete configuration:

```ini
[WireSteam]
AppID = 480
VirtualPort = 40
RelayOnly = true
InternetMode = server
InternetSubnet = 10.77.0.0/24
```

The server has one private IPv4 interface address, such as `10.77.0.1/32`. Each peer's `AllowedIPs` contains its unique client IPv4 `/32` inside `InternetSubnet`. Clients cannot use the server, subnet network or broadcast address. Use `Dial = false` on passive server peers when clients initiate.

A client profile:

```ini
[Interface]
PrivateKey = <CLIENT_PRIVATE_KEY>
Address = 10.77.0.2/32
DNS = 1.1.1.1, 9.9.9.9
ListenPort = 51820
MTU = 1280

[WireSteam]
AppID = 480
VirtualPort = 40
RelayOnly = true
InternetMode = client

[Peer]
PublicKey = <SERVER_PUBLIC_KEY>
SteamID = <SERVER_STEAM_ID64>
AllowedIPs = 0.0.0.0/0
PersistentKeepalive = 25
Dial = true
```

Clients require exactly one gateway peer, one private IPv4 interface address and source-profile `AllowedIPs = 0.0.0.0/0`. Numeric IPv4 DNS servers are required; IPv6, loopback, link-local, multicast and unspecified DNS addresses are refused. Omit `InternetSubnet` on clients. Internet clients require SDR-only transport; keep `RelayOnly = true` on both sides for these examples.

The generated runtime configuration uses two IPv4 half-default routes and depends on WireSteam's live loopback endpoints and privileged routing session. Do not import it into the original WireGuard app.

## Transport bypass, DNS, and IPv6

Steam's encrypted transport must reach the physical Internet without entering its own VPN. Both routing paths identify the exact `WireSteam.exe` path and detected running `steam.exe` paths; the client logs the selected path. Restart the tunnel after changing Steam's installation path or the physical uplink.

On systems with [Windows per-connection routing](https://learn.microsoft.com/en-us/windows/win32/api/fwpmu/nf-fwpmu-fwpmconnectionpolicyadd0), WireSteam applies executable routing policies. All non-loopback IPv4 traffic from the detected Steam paths has the physical-uplink exception, including unrelated Steam activity. Activation enables IPv4/IPv6 route-policy evaluation flags; these remain enabled until reboot so other applications' policies are not disabled. WireSteam's own policies and filters end with its dynamic session.

When that API is absent, including Windows 10, guarded endpoint routing blocks the first UDP attempt to each unlearned public Steam destination. A matching DROP event authorizes a physical `/32` route and a permit scoped to the transport executable, destination, physical source address and interface. Steam can retry; the learning guard remains active for new destinations and does not require ALLOW events. A separate block prevents transport from entering the VPN interface.

Fallback routes last 45 seconds and renew every 10 seconds while the helper is healthy. Startup requires an observed/routed UDP attempt and an authenticated gateway before applying the Internet adapter's routes. New destinations may require connection retries. Private/LAN and multicast UDP are not learned and remain blocked, which can affect Steam LAN features. Unrelated Steam activity can use the physical uplink in either path.

Because fallback `/32` routes are system routes, the egress guard blocks other applications targeting those same Steam destinations. Shared destinations may be unavailable during the session. The fallback also temporarily changes global connection-telemetry settings. A protected recovery record lets normal cleanup or a later elevated `down` restore settings still owned by WireSteam while preserving conflicting external changes.

The client guard permits ordinary IPv4 traffic through the bound VPN interface and blocks other non-loopback IPv4 egress, except the transport, local loopback and physical-interface IPv4 DHCP needed to operate. Configured IPv4 DNS uses the tunnel. Local LAN services and LAN-only DNS may be unavailable.

Internet exit is IPv4-only: non-loopback IPv6 is blocked, so IPv6-only destinations and local IPv6 services may be unavailable. Applications may need to reconnect after activation.

The guard requires the live elevated helper. Forced termination, helper failure or reboot removes the dynamic filters and can restore ordinary networking. Route lifetimes and telemetry recovery do not provide a persistent always-on kill switch.

## Verify the exit path

Compare the gateway's public IPv4 address with the address shown by the same trusted IP-check website in a normal client browser. Do not issue this check from `steam.exe`, which has the transport exception.

On the client:

```powershell
ping 10.77.0.1
Resolve-DnsName example.com -Server 1.1.1.1
.\WireSteam.exe status --name client-internet
```

`status` uses official `wg show` and may need an elevated terminal. Check for a recent handshake and increasing transfer counters, then test the public exit address, DNS and IPv6 behavior. UI readiness and outer-transport traffic do not establish those results.

The [speed tools](SPEEDTEST.md) measure the Steam/WireGuard path to the gateway, not its full Internet bandwidth. Internet throughput also depends on the gateway uplink, client downlink, relay conditions and configured send-rate limit.

## Stop and recover

Stop clients before the gateway. Ctrl+C or desktop deactivation asks the already approved helper to clean up its session. Server cleanup removes only its owned NAT and restores forwarding flags it changed, rechecking saved interface identities and preserving forwarding already enabled before activation. Older recovery records can also restore external-interface changes acknowledged by an older session.

After abnormal shutdown, run the matching name on each respective computer:

```powershell
.\WireSteam.exe down --name client-internet
```

```powershell
.\WireSteam.exe down --name host-internet
```

Desktop profiles use the local `ui-...` identifier from their event log, for example `down --name ui-30c76bbda69e` for adapter `WireSteam-ui-30c76bbda69e`.

For fallback clients, `down` attempts recovery of recorded global telemetry settings. Endpoint routes are removed normally or expire when renewal stops. Recovery refuses untrusted or incomplete records and preserves settings changed by another application.

Gateway recovery is stored under `HKLM\SOFTWARE\WireSteam\InternetGateways\<full-adapter-name>` after NAT creation is acknowledged. Elevated `down` can remove the exact recorded NAT prefix and restore recorded forwarding even if the adapter service is gone. Incomplete, modified or unprotected records are refused; pre-existing NATs are never adopted. A crash before the acknowledgment/record is saved may need manual inspection.

If cleanup fails after a complete valid gateway record was saved, the helper can leave the adapter and runtime file for a later elevated `down`. Without a valid record it retains ownership and retries instead of guessing which settings to remove.

For read-only diagnosis in an administrator terminal:

```powershell
Get-NetNat | Format-Table Name, InternalIPInterfaceAddressPrefix
Get-NetIPInterface -AddressFamily IPv4 | Format-Table InterfaceAlias, Forwarding
```

Keep the exact refusal/error message. Identify an existing NAT's owner before resolving a conflict. Do not delete all NATs, container switches or recovery registry records as routine troubleshooting.

## Current limits

- IPv4 exit only; no IPv6 exit, Ethernet bridging or LAN broadcast discovery.
- One gateway peer per client; multiple distinct clients per gateway subnet.
- No automatic uplink migration, gateway failover, NAT sharing, ICS fallback or boot-time Internet connection.
- Fallback routing allows one Internet-client session at a time, requires usable connection telemetry, may need retries and can block ordinary applications sharing Steam destinations.
- Other VPN clients can react to adapter/WinNAT notifications; coexistence requires live testing.
- AppID 480 is a shared development identity. Steam controls relay availability; a released product needs a dedicated Steamworks AppID and suitable Valve access.

A Windows 10 VM and a live two-computer Internet session have not been verified. Complete Internet, reconnect, DNS and IPv6 checks on the target computers before relying on this mode.
