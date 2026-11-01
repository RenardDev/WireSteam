# Configuration and CLI reference

WireSteam uses a restricted WireGuard-style format. Generate local keys with `WireSteam.exe init`; the placeholders below are not working keys.

```ini
[Interface]
PrivateKey = <LOCAL_PRIVATE_KEY>
Address = 10.77.0.1/32, fd77::1/128
ListenPort = 51820
MTU = 1280
# DNS = 10.77.0.53

[WireSteam]
AppID = 480
VirtualPort = 0
RelayOnly = true
InternetMode = off
ReconnectSeconds = 3
SendRate = 4194304
StatsSeconds = 10

[Peer]
PublicKey = <REMOTE_PUBLIC_KEY>
# PresharedKey = <OPTIONAL_SHARED_WIREGUARD_KEY>
SteamID = <REMOTE_STEAM_ID64>
AllowedIPs = 10.77.0.2/32, fd77::2/128
PersistentKeepalive = 25
# SteamPort = 0
BridgePort = 0
Dial = true
```

| Setting | Meaning |
| --- | --- |
| `Address` | Local IPv4/IPv6 VPN addresses, comma separated. RFC1918 IPv4 and ULA IPv6 only. Internet modes require exactly one private IPv4 address. Host prefixes `/32` and `/128` avoid unnecessary connected subnet routes. |
| `ListenPort` | Local WireGuard UDP port. Default 51820; 0 selects a port before compiling the runtime config. This port never needs Internet forwarding. |
| `MTU` | VPN interface MTU, 1280–1420; default 1280. |
| `DNS` | Optional private IPv4/ULA IPv6 nameservers in private-network/server mode. Internet-client mode requires numeric IPv4 unicast nameservers, such as `1.1.1.1, 9.9.9.9`; those may be public addresses. |
| `VirtualPort` | Local Steam virtual port, 0–999. Choose a different value for concurrent instances in the same Steam account. |
| `RelayOnly` | Default true disables Steam ICE direct transport and uses SDR. False enables Steam's automatic direct/relay selection in private-network mode. Internet clients require true. |
| `InternetMode` | `off` by default. `client` enables the managed IPv4 Internet-client session; `server` enables the exit host's managed IPv4 forwarding and NAT. See [INTERNET.md](INTERNET.md). |
| `InternetSubnet` | Required only for `InternetMode = server`: a canonical private IPv4 NAT prefix such as `10.77.0.0/24`. Its server address and authorized client `/32` addresses must belong to this prefix. |
| `ReconnectSeconds` | Delay before redialing a disconnected configured peer, 1–300. |
| `SendRate` | Per-connection Steam send-rate configuration in bytes/second, 65536–134217728. Default 4 MiB/s; this is a configured limit, not a throughput guarantee. |
| `StatsSeconds` | Statistics interval, 0 disables periodic output. Final statistics are still printed. |
| `SteamID` | Public-universe individual desktop account's numeric SteamID64. Exactly one configured peer per SteamID. |
| `AllowedIPs` | Routes and WireGuard source authorization for that peer. Private-network mode accepts RFC1918 IPv4/ULA IPv6. An Internet client requires exactly `0.0.0.0/0` on one gateway peer; an Internet server requires contained private IPv4 client `/32` addresses. Host bits are normalized. Overlapping routes across peers are rejected. |
| `SteamPort` | Remote peer's `VirtualPort`; omitted means inherit the local `VirtualPort`. Set it explicitly when the two ends differ. |
| `BridgePort` | Stable local loopback endpoint for that peer during the session. Default 0 allocates an exclusive ephemeral UDP port. Must differ from `ListenPort`. |
| `PersistentKeepalive` | Standard WireGuard keepalive interval, 0–65535 seconds. Default 25; it also supplies traffic after a reconnect. |
| `Dial` | Default true. False accepts incoming connections without initiating. |

Multiple `[Peer]` sections are supported (maximum 128). Keys are validated as canonical nonzero 32-byte Base64 values. Unknown options, duplicate keys/SteamIDs, ordinary `Endpoint`, and execution hooks such as `PreUp`/`PostUp` are rejected. This is a restricted WireGuard-style configuration format; arbitrary `wg-quick` directives are not accepted.

## Routing

Private-network mode (`InternetMode = off`) routes private IPv4 and ULA IPv6 addresses. It does not configure NAT or forwarding. Subnets behind a peer need separate forwarding, firewall and return-route setup. Ethernet bridging and broadcast discovery are not supported.

Choose addresses that do not overlap your LAN or another VPN. Direct Steam connections (`RelayOnly = false`) can use the LAN, so routing that same subnet through WireGuard can capture the transport. SDR-only is the default.

For an IPv4 Internet exit, follow [Internet gateway setup](INTERNET.md). Internet clients and gateways have additional requirements and recovery rules.

## Commands and diagnostics

```powershell
.\WireSteam.exe                                 # English interactive launcher

.\WireSteam.exe --help

.\WireSteam.exe keygen my-private.key           # new restricted file; public key to stdout

.\WireSteam.exe check a.conf                    # validates and prints a redacted summary

.\WireSteam.exe up a.conf --name home --verbose

.\WireSteam.exe up a.conf --name home --run-seconds 60

.\WireSteam.exe status --name home              # wg show; may need an elevated terminal

.\WireSteam.exe down --name home

.\WireSteam.exe bridge a.conf --run-seconds 10  # Steam transport only, no adapter/UAC

.\WireSteam.exe self-test

.\WireSteam.exe self-test --steam
```

In private-network mode, `bridge` also accepts `--write-config <new-file>` to save its generated standard WireGuard configuration privately for manual integration. It contains the actual ports selected for that session. Keep the bridge running while using it; ephemeral ports may change on the next launch. The diagnostic configuration is not removed automatically. Never share it. Use managed `up` or UI activation for Internet mode; a manual WireGuard import lacks the required live Internet-routing session.

`status` uses standard `wg show`, which hides private and preshared keys. It reports WireGuard handshake/transfer state independently of Steam's ready log. Do not use `wg showconf` to collect a public diagnostic report.

## Keys and connection state

SteamID identifies the outer transport peer; WireGuard keys authorize the VPN. A Steam connection or `WIRESTEAM READY` message is not a WireGuard handshake. Check `status` for handshake and transfer state; peers need VPN traffic or a keepalive to complete a handshake.

`init` and `keygen` create new protected files and never overwrite an existing file. Protect source configurations and exports yourself. Generated runtime configurations are private and removed after successful adapter cleanup; failed installation can retain them for recovery with `down`.

Traffic is dropped while a peer is disconnected or the transport is backpressured. Applications and WireGuard handle their own retries. The transport does not maintain an offline packet queue.

## Test connectivity

```powershell
# Computer A
.\WireSteamTestServer.exe --listen 10.77.0.1:27015

# Computer B
.\WireSteamTestClient.exe --target 10.77.0.1:27015 --count 20 --tcp-size 262144 --udp-size 1200 --half-close
```

The test checks both TCP and UDP payload integrity. Windows Firewall must allow the server on the VPN interface. For sustained throughput measurements, use the [standalone bandwidth tools](SPEEDTEST.md).
