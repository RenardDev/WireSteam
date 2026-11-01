using System;
using System.Linq;

namespace WireSteam.UI;

internal static class PortForwardConfigurationTests {
	internal static void Run() {
		static void Require(bool condition, string message) {
			if (!condition) {
				throw new InvalidOperationException(message);
			}
		}

		const string other = "# keep private profile\r\n[Interface]\r\nPrivateKey = exact-Private+Key/value= # keep secret comment\r\nAddress = 10.77.0.1/32, fd77::1/128\r\n\r\n[WireSteam]\r\nInternetMode = server\r\nInternetSubnet = 10.77.0.0/24\r\n\r\n";
		const string forward = "[ PortForward ] # service comment\r\n  Protocol = TCP\r\nListen\t= 10.77.0.1:25565  # listen note\r\nTarget = 127.0.0.1:25565 ; target note\r\nMaxConnections = 23\r\nConnectTimeoutSeconds = 8\r\nIdleTimeoutSeconds = 75\r\n# keep the peer description\r\n\r\n";
		const string peer = "[Peer]\r\nPublicKey = exact-Peer+Key/value=\r\nPresharedKey = exact-Preshared/Key=\r\nSteamID = 76561198000000001\r\nAllowedIPs = 10.77.0.2/32\r\n";

		string source = other + forward + peer;
		var configuration = new PortForwardConfiguration(source);
		Require(configuration.Rules.Count == 1 && configuration.VpnAddresses.SequenceEqual(new[] { "10.77.0.1", "fd77::1" }), "Forward editor must read the native section grammar and IPv4/IPv6 interface addresses.");
		Require(configuration.Apply(configuration.Rules) == source, "Opening and saving forwards without changes must retain exact configuration bytes.");

		string bomSource = "\uFEFF" + source["# keep private profile\r\n".Length..];
		var bomConfiguration = new PortForwardConfiguration(bomSource);
		Require(bomConfiguration.VpnAddresses.SequenceEqual(new[] { "10.77.0.1", "fd77::1" }) && bomConfiguration.Apply(bomConfiguration.Rules) == bomSource, "An initial UTF-8 BOM must not hide the Interface section or change any saved configuration bytes.");
		string bomAdded = bomConfiguration.Apply(bomConfiguration.Rules.Append(new PortForwardRule("10.77.0.1:25567", "127.0.0.1:25565")).ToArray());
		Require(bomAdded.StartsWith(bomSource, StringComparison.Ordinal) && bomAdded[0] == '\uFEFF' && new PortForwardConfiguration(bomAdded).Rules.Count == 2, "Adding a forward to a BOM-prefixed profile must preserve its entire original configuration, BOM and existing rule.");

		string optionalProtocol = source.Replace("  Protocol = TCP\r\n", "", StringComparison.Ordinal);
		var defaultProtocol = new PortForwardConfiguration(optionalProtocol);
		Require(defaultProtocol.Apply(defaultProtocol.Rules) == optionalProtocol, "An unchanged rule using the native default TCP protocol must stay byte-identical.");

		var edited = configuration.Rules[0] with {
			Listen = "10.77.0.1:25566",
			Target = "[::1]:25565"
		};
		string changed = configuration.Apply(new[] { edited });
		Require(changed == source.Replace("Listen\t= 10.77.0.1:25565  #", "Listen\t= 10.77.0.1:25566  #", StringComparison.Ordinal).Replace("Target = 127.0.0.1:25565 ;", "Target = [::1]:25565 ;", StringComparison.Ordinal), "Editing a forward must change only its endpoint values, preserving comments, private fields, advanced limits, section order and CRLF.");

		var ipv6 = new PortForwardRule("[fd77::1]:25565", "127.1.2.3:25566");
		string appended = configuration.Apply(new[] { edited, ipv6 });
		var reparsed = new PortForwardConfiguration(appended);
		Require(reparsed.Rules.Count == 2 && reparsed.Rules[1].Listen == ipv6.Listen && appended.Contains("MaxConnections = 23", StringComparison.Ordinal), "Adding a forward must preserve existing advanced settings and use a bracketed IPv6 listener.");

		var udp = new PortForwardRule("10.77.0.1:25565", "127.0.0.1:25565", "UDP");
		string mixed = configuration.Apply(configuration.Rules.Append(udp).ToArray());
		var mixedConfiguration = new PortForwardConfiguration(mixed);
		Require(mixedConfiguration.Rules.Count == 2 && mixedConfiguration.Rules[0].Protocol == "TCP" && mixedConfiguration.Rules[1].Protocol == "UDP" && mixedConfiguration.Rules.All(rule => rule.Listen == "10.77.0.1:25565") && mixed.StartsWith(source, StringComparison.Ordinal), "TCP and UDP forwards must share a VPN address/port, preserve prior configuration and serialize the selected protocol.");
		Require(mixedConfiguration.Apply(mixedConfiguration.Rules) == mixed, "Unchanged mixed-protocol rules must retain exact configuration bytes.");

		var changedProtocol = configuration.Rules[0] with {
			Protocol = "UDP"
		};
		Require(configuration.Apply(new[] { changedProtocol }) == source.Replace("  Protocol = TCP\r\n", "  Protocol = UDP\r\n", StringComparison.Ordinal), "Changing a rule's protocol must retain endpoints, comments and advanced settings.");

		var summary = new Profile();
		summary.PortForwards.Add(configuration.Rules[0]);
		summary.PortForwards.Add(udp);
		Require(summary.PortForwardSummary == "1 TCP FORWARD · 1 UDP FORWARD", "Mixed-protocol summaries must report each protocol.");

		string removed = configuration.Apply(Array.Empty<PortForwardRule>());
		Require(removed.StartsWith(other, StringComparison.Ordinal) && removed.EndsWith(peer, StringComparison.Ordinal) && removed.Contains("# keep the peer description", StringComparison.Ordinal) && !removed.Contains("[ PortForward ]", StringComparison.Ordinal), "Removing forwards must retain unrelated sections and standalone comments.");
		Require(new PortForwardConfiguration(source.Replace("\r\n", "\n", StringComparison.Ordinal)).Apply(new PortForwardConfiguration(source.Replace("\r\n", "\n", StringComparison.Ordinal)).Rules) == source.Replace("\r\n", "\n", StringComparison.Ordinal), "LF-only configurations must remain LF-only.");

		void Reject(PortForwardRule[] rules) {
			bool rejected = false;
			try {
				configuration.Apply(rules);
			} catch (ArgumentException) {
				rejected = true;
			}

			Require(rejected, "The forward editor accepted an invalid listener, target, protocol, duplicate endpoint or rule count.");
		}

		Reject(new[] { new PortForwardRule("0.0.0.0:25565", "127.0.0.1:25565") });
		Reject(new[] { new PortForwardRule("10.77.0.2:25565", "127.0.0.1:25565") });
		Reject(new[] { new PortForwardRule("10.77.0.1:0", "127.0.0.1:25565") });
		Reject(new[] { new PortForwardRule("10.77.0.1:25565", "192.168.1.1:25565") });
		Reject(new[] { new PortForwardRule("10.77.0.1:25565", "localhost:25565") });
		Reject(new[] { new PortForwardRule("10.77.0.1:25565", "127.1:25565") });
		Reject(new[] { new PortForwardRule("10.77.0.1:25565", "[127.0.0.1]:25565") });
		Reject(new[] { new PortForwardRule("10.77.0.1:25565", "[::ffff:127.0.0.1]:25565") });
		Reject(new[] { new PortForwardRule("10.77.0.1:25565", "127.0.0.1:25565", "ICMP") });
		Reject(new[] { udp, udp with { Protocol = "udp", Target = "127.0.0.1:25566" } });
		Reject(new[] { new PortForwardRule("10.77.0.1:51820", "127.0.0.1:25565", "UDP") });
		Reject(new[] { new PortForwardRule("10.77.0.1:25565", "127.0.0.1:51820", "UDP") });
		Reject(new[] { new PortForwardRule("10.77.0.1:25565", "[::1]:51820", "UDP") });

		Require(configuration.Apply(new[] { new PortForwardRule("10.77.0.1:51820", "127.0.0.1:25565", "TCP") }).Contains("Listen = 10.77.0.1:51820", StringComparison.Ordinal), "TCP forwarding may share the numeric WireGuard UDP interface port.");

		var automaticListen = new PortForwardConfiguration("[Interface]\nAddress = 10.77.0.1/32\nListenPort = 0\n");
		Require(automaticListen.Apply(new[] { new PortForwardRule("10.77.0.1:51820", "127.0.0.1:25565", "UDP") }).Contains("Protocol = UDP", StringComparison.Ordinal), "Automatic WireGuard ListenPort must permit a UDP forward on another otherwise valid port.");

		var reservedBridge = new PortForwardConfiguration("[Interface]\nAddress = 10.77.0.1/32\nListenPort = 0\n[Peer]\nBridgePort = 52001\n");
		bool bridgeTargetRejected = false;
		try {
			reservedBridge.Apply(new[] { new PortForwardRule("10.77.0.1:25565", "127.0.0.1:52001", "UDP") });
		} catch (ArgumentException) {
			bridgeTargetRejected = true;
		}

		Require(bridgeTargetRejected, "An explicit peer BridgePort must not be exposed as a loopback UDP target.");

		foreach (string target in new[] { "127.0.0.2:52001", "[::1]:52001" }) {
			Require(reservedBridge.Apply(new[] { new PortForwardRule("10.77.0.1:25565", target, "UDP") }).Contains("Target = " + target, StringComparison.Ordinal), "A loopback target outside the bridge's exact IPv4 address may reuse its port.");
		}

		Reject(new[] { new PortForwardRule("[fd77::1]:25565", "127.0.0.1:25565"), new PortForwardRule("[fd77:0:0:0:0:0:0:1]:25565", "[::1]:25566") });
		var maximumRules = Enumerable.Range(0, 32).Select(index => new PortForwardRule("10.77.0.1:" + (24000 + index / 2), "127.0.0.1:25565", index % 2 == 0 ? "TCP" : "UDP")).ToArray();
		string maximumText = configuration.Apply(maximumRules);
		var maximumConfiguration = new PortForwardConfiguration(maximumText);
		Require(maximumConfiguration.Rules.Count == 32 && maximumConfiguration.Rules.Count(rule => rule.Protocol == "TCP") == 16 && maximumConfiguration.Rules.Count(rule => rule.Protocol == "UDP") == 16 && maximumConfiguration.Apply(maximumConfiguration.Rules) == maximumText, "All 32 TCP/UDP slots must survive saving and reopening, including separate protocols sharing each listening port.");
		Reject(maximumRules.Append(new PortForwardRule("10.77.0.1:24016", "127.0.0.1:25565")).ToArray());
		Require(configuration.Apply(maximumRules) == maximumText, "Rejecting the 33rd rule must leave the configuration reusable and unchanged.");
		Reject(new[] { new PortForwardRule("10.77.0.1:25565", "127.0.0.1:25565", "TCP+UDP") });
		Reject(new[] { new PortForwardRule("10.77.0.1:25565", "127.0.0.1:25565", "TCP + UDP") });

		var client = new PortForwardConfiguration("[Interface]\nAddress = 10.77.0.2/32\n[WireSteam]\nInternetMode = client\n");
		bool clientRejected = false;
		try {
			client.Apply(new[] { new PortForwardRule("10.77.0.2:25565", "127.0.0.1:25565") });
		} catch (ArgumentException) {
			clientRejected = true;
		}

		Require(!client.AllowsForwards && clientRejected && !client.Apply(Array.Empty<PortForwardRule>()).Contains("[PortForward]", StringComparison.Ordinal), "Internet-client profiles must explain their unsupported host role and allow removing rules.");

		Console.WriteLine("PASS forward editor: exact private configuration/BOM round-trip, comments/advanced limits, add/edit/delete, mixed TCP/UDP sharing, IPv4/IPv6 validation, duplicate/limit/client restrictions.");
	}
}
