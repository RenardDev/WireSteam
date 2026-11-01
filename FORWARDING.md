# Publish local TCP and UDP services through WireSteam

Port forwarding lets VPN peers reach a service bound to this computer's loopback address. For example, a peer connects to `10.77.0.1:25565`, and WireSteam forwards it to `127.0.0.1:25565`. Put the rule on the computer running the service. Peers need a working VPN route to that computer.

Forwarding works in private-network and Internet-gateway profiles. Internet-client profiles cannot host these listeners.

## Desktop UI

1. Select the stopped host profile and choose **MANAGE FORWARDS**, or **SETTINGS → PORT FORWARDING → MANAGE FORWARDS**.
2. Choose **ADD FORWARD**. Select the profile's VPN address, protocol (**TCP** or **UDP**), listening port, loopback target address and target port.
3. Add the rule, then choose **VALIDATE & SAVE**. Activate the tunnel and start the local service.
4. Connect from a peer using the host's VPN address and exposed port.

A profile supports 32 rules total. The same VPN address/port can have one TCP and one UDP rule. Edit rules only while the profile is stopped with no pending cleanup. The Settings list describes configured endpoints; it does not probe target availability.

## Configuration

Append a rule to an existing valid host configuration:

```ini
[PortForward]
Protocol = TCP
Listen = 10.77.0.1:25565
Target = 127.0.0.1:25565
```

Add another section for each rule. To publish UDP on the same port:

```ini
[PortForward]
Protocol = UDP
Listen = 10.77.0.1:25565
Target = 127.0.0.1:25565
```

`Protocol` is `TCP` or `UDP`, case-insensitive, and defaults to TCP if omitted. Forwarding does not translate between protocols: a UDP rule requires a UDP target service.

`Listen` must be an exact local IP from the profile's `[Interface] Address`, with a port from 1 to 65535. It cannot be a wildcard, a LAN/public address outside the profile, or another peer's VPN IP. `Target` must be numeric loopback IPv4 (`127.x.x.x`) or IPv6 (`::1`). Hostnames are not resolved. For IPv6, use bracketed endpoints:

```ini
[PortForward]
Protocol = TCP
Listen = [fd77::1]:25565
Target = [::1]:25565
```

The two sides can use different address families or ports. To expose the same local service at another VPN port, use `Listen = 10.77.0.1:25566` with `Target = 127.0.0.1:25565`.

Listening **protocol/address/port** combinations must be unique. Both the listening and target port of a UDP rule must differ from `[Interface] ListenPort`, because WireGuard reserves its UDP port on all addresses, including loopback. A UDP target at `127.0.0.1` must also avoid explicit peer `BridgePort` values. `ListenPort = 0` and omitted `BridgePort` values choose free transport ports automatically while avoiding these forwarding endpoints, even if the target service has not started yet. TCP rules do not have these UDP-port restrictions.

Optional limits:

```ini
MaxConnections = 64
ConnectTimeoutSeconds = 10
IdleTimeoutSeconds = 600
```

`MaxConnections` is per rule (1–256); for UDP it limits client address/port mappings. The engine caps active TCP connections and UDP mappings together at **256**. TCP uses bounded buffers and backpressure for slow readers. `ConnectTimeoutSeconds` accepts 1–300 seconds and applies only to TCP connection setup. `IdleTimeoutSeconds` accepts 0–86400 and expires idle connections or UDP mappings; zero disables idle expiration while capacity limits still apply. Leave the defaults unless the local application needs a different idle period.

UDP remains best effort. Datagrams up to **65,507 bytes**, including empty datagrams, retain their boundaries; oversized packets are discarded. Full socket buffers or exhausted mapping capacity cause packets to be dropped. The proxy does not add reliability or an unbounded datagram queue. Network MTU, fragmentation and Steam packet loss still affect delivery. Expired mappings are recreated by a later client packet; the target may then see a different local source port.

The normal CLI manages the same rules:

```powershell
.\WireSteam.exe check .\host.conf
.\WireSteam.exe preflight .\host.conf
.\WireSteam.exe up .\host.conf --name host
```

`check` lists forwarding protocols and endpoints without printing private keys. `preflight` checks existing TCP listeners and UDP bindings without binding the future VPN IP or changing adapters. Real listeners start after the adapter is installed and its address becomes available. All listeners must bind successfully before tunnel readiness is reported; a failed bind triggers adapter cleanup. Rules stop and active connections/mappings close before the adapter is removed. `bridge` does not manage an adapter and rejects configurations containing forwarding rules.

The target need not run at activation. An unavailable TCP target fails only that incoming connection; UDP packets can be lost until the service starts. Each TCP client gets a separate local connection, with bounded buffers, backpressure and half-close support. UDP uses a separate target socket per remote address/port so replies reach the correct client.

The local service sees a connection from the local proxy, typically `127.0.0.1` or `::1`, rather than the remote VPN client's address. No application-specific proxy headers are injected. Keep any application authentication enabled; address-based bans/rate limits at the target see the proxy address.

## Windows Firewall

WireSteam does not add forwarding firewall rules. If Windows blocks the listener, an administrator can add a scoped inbound rule. This example assumes CLI `--name host`, the installed engine path and authorized peers `10.77.0.2`/`10.77.0.3`; replace them with your actual values:

```powershell
New-NetFirewallRule -DisplayName 'WireSteam Minecraft TCP over VPN' `
	-Direction Inbound -Action Allow -Protocol TCP `
	-Program 'C:\Program Files\WireSteam\WireSteam.exe' `
	-LocalAddress 10.77.0.1 -LocalPort 25565 `
	-RemoteAddress 10.77.0.2,10.77.0.3 `
	-InterfaceAlias 'WireSteam-host' -Profile Any
```

For UDP, use `-Protocol UDP` and a distinct display name. Desktop adapter names use `WireSteam-ui-...`, shown in the activation log. `Profile Any` includes adapters classified as Public; the program, interface and address scopes still restrict the rule. WireGuard peers must also be authorized to route to the VPN address. See [Windows firewall parameters](https://learn.microsoft.com/en-us/powershell/module/netsecurity/new-netfirewallrule).

## Troubleshooting and verification

- Verify the original service locally at `127.0.0.1:25565`.
- Verify the peer's VPN route, then run `Test-NetConnection 10.77.0.1 -Port 25565` from the peer for TCP. That command does not test UDP; use the target application's UDP client or the included UDP echo test applications.
- A wildcard service at `0.0.0.0:25565` or `[::]:25565` can already own the VPN port. Bind the original service to loopback or choose another VPN listening port. Exact loopback and VPN-IP listeners can share a port.
- IPv6 wildcard listeners are checked conservatively because Windows TCP tables do not identify whether an existing socket also reserves IPv4. Use another exposed port when preflight reports that ambiguity.
- Listener startup errors appear in the event log. If the local target is unavailable, the incoming connection fails; check the target locally before investigating the VPN path. The engine does not inspect or log application payloads.

Verify the complete two-computer Steam/WireGuard and firewall path on your computers; local proxy checks alone do not establish remote reachability.
