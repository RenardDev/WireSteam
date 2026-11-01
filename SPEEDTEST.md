# Measuring WireSteam throughput

`WireSteamSpeedServer.exe` and `WireSteamSpeedClient.exe` measure application data delivered between two computers. They work with WireSteam VPN addresses, an ordinary LAN, or localhost; the benchmark programs do not require Steam, WireGuard, or administrator rights themselves.

Use the **Speed** programs for sustained upload/download measurements. `WireSteamTestServer.exe` and `WireSteamTestClient.exe` check payload integrity and request/response behavior. Benchmarks are separate command-line tools, outside the desktop app.

## Test over the VPN

Start WireSteam on the participating computers and wait for `WIRESTEAM READY`. Confirm that the WireGuard peers have exchanged a handshake. These examples use computer A at `10.77.0.1`, computer B at `10.77.0.2`, and an optional second client C at `10.77.0.3`.

On A, start the speed server:

```powershell
.\WireSteamSpeedServer.exe --listen 10.77.0.1:5201
```

On B, run a 15-second TCP test in each direction with four parallel streams:

```powershell
.\WireSteamSpeedClient.exe --target 10.77.0.1:5201 --protocol tcp --direction both --seconds 15 --streams 4 --json tcp-results.json
```

Upload means **B sends to A**. Download means **A sends to B**. Tests in opposite directions run sequentially, so the complete command takes longer than 15 seconds. A direction's reported rate is the aggregate across its TCP streams.

Run a combined TCP/UDP comparison:

```powershell
.\WireSteamSpeedClient.exe --target 10.77.0.1:5201 --protocol both --direction both --seconds 15 --streams 4 --udp-mbps 24 --udp-size 1100 --json combined-results.json
```

This runs TCP upload, TCP download, UDP upload and UDP download. UDP uses a fixed offered rate; `--streams` controls TCP parallelism. Run `--help` on either executable for the complete option list. `--quiet` suppresses interval output while retaining the final results.

For a quick local check, bind the server to `127.0.0.1:5201` and target that same address from the client. This checks the benchmark programs and local networking; it does not measure Steam, SDR, or the VPN.

## Concurrent clients

One server supports 16 active control clients by default; `--max-clients N` accepts 1–64. Run the usual client command on B and C together, with separate JSON filenames. Output identifies each client and phase, and each result includes only that client's data. Phases within one client remain sequential. Excess clients receive `Server busy` and can retry later.

Concurrent tests share CPU, network and Steam transport capacity. Test clients separately for individual path capacity, or together for competition under load. Aggregate throughput requires delivered bytes over the same overlapping time window; adding rates from different periods is misleading. The tools report individual results and do not calculate a synchronized aggregate.

## Firewall and test ports

The server first accepts a TCP control connection on its configured port. Each measurement negotiates additional ephemeral TCP or UDP data ports. **UDP measurements also require the TCP control connection.** Allowing only UDP port 5201, or only TCP port 5201, is insufficient for a complete test.

Bind the server to its VPN address and allow its program through Windows Firewall, scoped to the intended VPN peers. If necessary, this example creates a rule in an administrator PowerShell on A for B and C; adjust the addresses to match your configuration:

```powershell
$speedServer = (Resolve-Path .\WireSteamSpeedServer.exe).Path
New-NetFirewallRule -DisplayName 'WireSteam speed test from VPN clients' -Direction Inbound -Action Allow -Program $speedServer -Protocol Any -LocalAddress 10.77.0.1 -RemoteAddress 10.77.0.2,10.77.0.3 -Profile Any
```

The benchmark does not create this rule automatically. To remove that example rule afterwards:

```powershell
Remove-NetFirewallRule -DisplayName 'WireSteam speed test from VPN clients'
```

## Read the results

- **Receiver throughput** is the useful delivered rate. Sender throughput alone can overstate UDP delivery when packets are lost.
- **Mbit/s** means decimal megabits per second: `received bytes * 8 / seconds / 1,000,000`. Divide by eight to convert Mbit/s to decimal MB/s.
- **TCP** adjusts its rate to congestion and loss. Several streams can fill a path more effectively than one. Compare `--streams 1`, `4` and `8`; more streams do not guarantee a higher rate.
- **UDP offered rate** is the requested transmission load. Compare it with delivered throughput and packet loss. Duplicate, reordered, invalid and locally dropped packets help explain discrepancies.
- A **peak interval** is a short observation, not a guarantee of the path's physical maximum. Prefer repeatable longer-run receiver results.

JSON exports contain the target, units, control-channel RTT statistics and one result per measured protocol/direction. Keep separate runs when comparing settings. RTT statistics are not a continuous latency-under-load measurement; throughput and latency can change while the path is busy.

The reported rate measures application data. Steam messages, fragmentation, WireGuard encryption, IP/UDP/TCP headers, acknowledgements and relay overhead also consume network capacity. A VPN payload rate is therefore lower than the underlying link's raw bit rate.

TCP receiver timing spans the first payload byte through all data-stream EOFs, including queued data drained after the sender stops. UDP allows up to two seconds for in-flight packets after the sender report arrives and uses the longer of the sender's active interval and the receiver's arrival span. Missing packets after that drain window count as lost. Initial TCP ramp-up is included; longer runs reduce its effect. Jitter measures variation in successive packet timing without requiring synchronized clocks, not one-way latency.

## Find a useful UDP rate

There is no automatic maximum-rate search. Start below your observed TCP rate, then increase the offered rate, for example through 8, 16, 24 and 32 Mbit/s:

```powershell
.\WireSteamSpeedClient.exe --target 10.77.0.1:5201 --protocol udp --direction upload --seconds 15 --udp-mbps 8
```

Repeat with `--direction download`. A useful operating rate delivers most of the offered traffic with acceptable loss and stable performance across repeated runs. Increasing the offered rate after delivered throughput stops growing usually increases loss rather than useful bandwidth.

The default `--udp-size 1100` sends 1100 useful bytes per datagram plus a 48-byte benchmark header, before UDP/IP and VPN overhead. Smaller payloads increase packet-processing overhead; larger payloads can increase fragmentation overhead. Keep the size constant when comparing paths or settings.

## Account for the WireSteam send limit

WireSteam's default `[WireSteam] SendRate = 4194304` is **4 MiB/s**, approximately **33.55 Mbit/s**, per Steam connection. It limits the outer transport; useful benchmark throughput will be lower after overhead. A result near that level may reflect the configured limit rather than the network's capacity.

To investigate a faster path, choose a higher `SendRate` in the relevant configurations on both computers and restart WireSteam before repeating the tests. For example, `SendRate = 16777216` configures 16 MiB/s, approximately 134.22 Mbit/s, within the supported range. The benchmark does not modify VPN configurations, and raising this limit does not guarantee Steam or SDR will deliver that rate.

Keep `RelayOnly = true` when measuring SDR. To compare Steam's automatic direct/relay selection, deliberately configure `RelayOnly = false` on both peers, restart them, and record the route shown by WireSteam. Do not route the physical LAN subnet through WireGuard when Steam uses that LAN for a direct connection. Record which route each result used; different routes can have different limits.

Use 15–30-second runs, repeat each configuration several times, and keep other traffic quiet during comparison. Test upload and download separately because upstream capacity, CPU load, packet loss and Steam routing can differ by direction.

## Server lifetime

The server normally accepts concurrent and consecutive clients. `--once` serves the first control connection, closes the listener and exits after that client completes; orderly success returns exit code zero. It cannot be combined with `--max-clients` greater than one. `--run-seconds N` bounds the run window; active phases may finish before shutdown. Ctrl+C stops active measurements and releases network resources.

If the listener expires during a phase, the server delivers its result and allows a fixed two-second control-finish window. It accepts the client's final `Finish` and refuses new test phases during that window. This lets a completed measurement save its JSON before the connection closes.
