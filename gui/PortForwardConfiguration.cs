using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;

namespace WireSteam.UI;

public sealed record PortForwardRule(string Listen, string Target, string Protocol = "TCP") {
	internal int SourceIndex {
		get;
		init;
	} = -1;

	public string DisplayText => Protocol.ToUpperInvariant() + "  " + Listen + "  →  " + Target;
}

// Keep private configuration text outside the view model. Editing forwarding rules
// rewrites only their three public fields, retaining keys, comments and advanced limits.
internal sealed class PortForwardConfiguration {
	internal const int MaximumRules = 32;
	private sealed record Section(string Name, string Text);
	private readonly List<Section> sections = new();
	private readonly string newline;
	private int wireGuardListenPort = 51820;
	private readonly HashSet<int> peerBridgePorts = new();

	internal IReadOnlyList<PortForwardRule> Rules {
		get;
	}

	internal IReadOnlyList<string> VpnAddresses {
		get;
	}

	internal bool AllowsForwards {
		get;
	} = true;

	internal PortForwardConfiguration(string text) {
		newline = text.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
		var headers = Regex.Matches(text, @"(?m)^(?:\A\uFEFF)?[\t ]*\[([^\]\r\n]+)\][\t ]*(?:[#;][^\r\n]*)?(?:\r?$)");
		int start = 0;
		string name = "";
		foreach (Match header in headers) {
			if (header.Index > start) {
				sections.Add(new Section(name, text[start..header.Index]));
			}

			start = header.Index;
			name = header.Groups[1].Value.Trim();
		}

		if (start < text.Length) {
			sections.Add(new Section(name, text[start..]));
		}

		var rules = new List<PortForwardRule>();
		var addresses = new List<string>();
		for (int index = 0; index < sections.Count; ++index) {
			var section = sections[index];
			var values = Values(section.Text);
			if (section.Name.Equals("PortForward", StringComparison.OrdinalIgnoreCase)) {
				rules.Add(new PortForwardRule(values.GetValueOrDefault("Listen", ""), values.GetValueOrDefault("Target", ""), values.GetValueOrDefault("Protocol", "TCP")) { SourceIndex = index });
			} else if (section.Name.Equals("Interface", StringComparison.OrdinalIgnoreCase) && values.TryGetValue("Address", out string? addressList)) {
				foreach (var cidr in addressList.Split(',')) {
					var raw = cidr.Trim().Split('/')[0];
					if (!raw.Contains('%') && IPAddress.TryParse(raw, out var address) && !addresses.Contains(address.ToString(), StringComparer.Ordinal)) {
						addresses.Add(address.ToString());
					}
				}
			}

			if (section.Name.Equals("WireSteam", StringComparison.OrdinalIgnoreCase) && values.GetValueOrDefault("InternetMode", "off").Equals("client", StringComparison.OrdinalIgnoreCase)) {
				AllowsForwards = false;
			}

			if (section.Name.Equals("Interface", StringComparison.OrdinalIgnoreCase) && values.TryGetValue("ListenPort", out string? wireGuardPort) && int.TryParse(wireGuardPort, NumberStyles.None, CultureInfo.InvariantCulture, out int parsedPort)) {
				wireGuardListenPort = parsedPort;
			}

			if (section.Name.Equals("Peer", StringComparison.OrdinalIgnoreCase) && values.TryGetValue("BridgePort", out string? bridgePort) && int.TryParse(bridgePort, NumberStyles.None, CultureInfo.InvariantCulture, out int parsedBridgePort) && parsedBridgePort > 0) {
				peerBridgePorts.Add(parsedBridgePort);
			}
		}

		Rules = rules;
		VpnAddresses = addresses;
	}

	private static Dictionary<string, string> Values(string block) {
		var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
		foreach (string original in block.Split('\n')) {
			int comment = original.IndexOfAny(new[] { '#', ';' });
			string line = (comment >= 0 ? original[..comment] : original).Trim();
			int equals = line.IndexOf('=');
			if (equals > 0) {
				values[line[..equals].Trim()] = line[(equals + 1)..].Trim();
			}
		}

		return values;
	}

	internal static string Endpoint(string address, int port) => address.Contains(':') ? "[" + address + "]:" + port.ToString(CultureInfo.InvariantCulture) : address + ":" + port.ToString(CultureInfo.InvariantCulture);

	internal static IPEndPoint ParseEndpoint(string endpoint) {
		string addressText, portText;
		if (endpoint.StartsWith('[')) {
			int close = endpoint.IndexOf(']');
			if (close < 0 || close + 1 >= endpoint.Length || endpoint[close + 1] != ':') {
				throw new ArgumentException("Use an IP address and port, for example 127.0.0.1:25565 or [::1]:25565.");
			}

			addressText = endpoint[1..close];
			portText = endpoint[(close + 2)..];

			if (!addressText.Contains(':')) {
				throw new ArgumentException("Use brackets only for IPv6 addresses.");
			}
		} else {
			int colon = endpoint.LastIndexOf(':');
			if (colon <= 0 || endpoint[..colon].Contains(':')) {
				throw new ArgumentException("Use an IP address and port. Enclose IPv6 addresses in brackets.");
			}

			addressText = endpoint[..colon];
			portText = endpoint[(colon + 1)..];
		}

		if (addressText.Contains('%') || !IPAddress.TryParse(addressText, out var address) || !int.TryParse(portText, NumberStyles.None, CultureInfo.InvariantCulture, out int port) || port is < 1 or > 65535) {
			throw new ArgumentException("Use a numeric IP address and a port between 1 and 65535.");
		}

		if (address.AddressFamily == AddressFamily.InterNetwork && addressText != address.ToString()) {
			throw new ArgumentException("Write IPv4 addresses as four decimal octets, for example 127.0.0.1.");
		}

		return new IPEndPoint(address, port);
	}

	internal void Validate(IReadOnlyList<PortForwardRule> rules) {
		if (rules.Count > MaximumRules) {
			throw new ArgumentException($"A profile can contain at most {MaximumRules} port forwards.");
		}

		if (rules.Count > 0 && !AllowsForwards) {
			throw new ArgumentException("Port forwarding requires InternetMode = off or server. Internet-client profiles cannot host forwards.");
		}

		var listeners = new HashSet<string>(StringComparer.Ordinal);
		foreach (var rule in rules) {
			if (!rule.Protocol.Equals("TCP", StringComparison.OrdinalIgnoreCase) && !rule.Protocol.Equals("UDP", StringComparison.OrdinalIgnoreCase)) {
				throw new ArgumentException("Port forwarding supports TCP or UDP.");
			}

			var listen = ParseEndpoint(rule.Listen);
			var target = ParseEndpoint(rule.Target);

			if (rule.Protocol.Equals("UDP", StringComparison.OrdinalIgnoreCase)) {
				if (wireGuardListenPort != 0 && (listen.Port == wireGuardListenPort || target.Port == wireGuardListenPort)) {
					throw new ArgumentException("This UDP listen or target port is reserved by the WireGuard interface. Choose another forwarding port or set Interface ListenPort = 0.");
				}

				if (target.Address.Equals(IPAddress.Loopback) && peerBridgePorts.Contains(target.Port)) {
					throw new ArgumentException("This UDP target at 127.0.0.1 is reserved by a peer BridgePort. Choose another target port or change that peer's BridgePort.");
				}
			}

			if (!VpnAddresses.Contains(listen.Address.ToString(), StringComparer.Ordinal)) {
				throw new ArgumentException("The listening address must be an address assigned to this VPN profile.");
			}

			bool loopback = target.Address.AddressFamily == AddressFamily.InterNetwork ? target.Address.GetAddressBytes()[0x00] == 0x7F : target.Address.Equals(IPAddress.IPv6Loopback);
			if (!loopback) {
				throw new ArgumentException("The target must be a local loopback address: 127.x.x.x or ::1.");
			}

			if (!listeners.Add(rule.Protocol.ToUpperInvariant() + " " + Endpoint(listen.Address.ToString(), listen.Port))) {
				throw new ArgumentException("Each protocol, listening address and port must be unique. TCP and UDP can share a port.");
			}
		}
	}

	internal string Apply(IReadOnlyList<PortForwardRule> rules) {
		Validate(rules);

		var existing = rules.Where(rule => rule.SourceIndex >= 0).ToDictionary(rule => rule.SourceIndex);
		var builder = new StringBuilder();
		for (int index = 0; index < sections.Count; ++index) {
			var section = sections[index];
			if (!section.Name.Equals("PortForward", StringComparison.OrdinalIgnoreCase)) {
				builder.Append(section.Text);
				continue;
			}

			if (existing.TryGetValue(index, out var rule)) {
				builder.Append(UpdateSection(section.Text, rule));
			} else {
				// Deleting a rule removes its settings, while retaining standalone
				// comments (which may document the section that follows it).
				foreach (string line in Regex.Split(section.Text, "(?<=\\n)")) {
					if (string.IsNullOrWhiteSpace(line) || line.TrimStart().StartsWith('#') || line.TrimStart().StartsWith(';')) {
						builder.Append(line);
					}
				}
			}
		}

		foreach (var rule in rules.Where(rule => rule.SourceIndex < 0)) {
			if (builder.Length > 0 && builder[^1] != '\n') {
				builder.Append(newline);
			}

			if (builder.Length > 0) {
				builder.Append(newline);
			}

			builder
				.Append("[PortForward]").Append(newline)
				.Append("Protocol = ").Append(rule.Protocol.ToUpperInvariant()).Append(newline)
				.Append("Listen = ").Append(rule.Listen).Append(newline)
				.Append("Target = ").Append(rule.Target).Append(newline);
		}

		return builder.ToString();
	}

	private string UpdateSection(string block, PortForwardRule rule) {
		var originals = Values(block);
		if (originals.GetValueOrDefault("Protocol", "TCP") == rule.Protocol && originals.GetValueOrDefault("Listen", "") == rule.Listen && originals.GetValueOrDefault("Target", "") == rule.Target) {
			return block;
		}

		var fields = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) { ["Protocol"] = rule.Protocol, ["Listen"] = rule.Listen, ["Target"] = rule.Target };
		var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
		var builder = new StringBuilder();
		foreach (string line in Regex.Split(block, "(?<=\\n)")) {
			var match = Regex.Match(line, @"^([\t ]*)(Protocol|Listen|Target)([\t ]*=[\t ]*)([^#;\r\n]*)(.*)$", RegexOptions.IgnoreCase | RegexOptions.Singleline);
			if (!match.Success) {
				builder.Append(line);
				continue;
			}

			string key = match.Groups[2].Value;
			seen.Add(key);

			if (originals.TryGetValue(key, out string? value) && value == fields[key]) {
				builder.Append(line);
				continue;
			}

			string previous = match.Groups[4].Value;
			int tail = previous.Length;
			while (tail > 0 && char.IsWhiteSpace(previous[tail - 1])) {
				--tail;
			}

			builder.Append(match.Groups[1].Value).Append(key).Append(match.Groups[3].Value).Append(fields[key]).Append(previous[tail..]).Append(match.Groups[5].Value);
		}

		foreach (var field in fields) {
			if (!seen.Contains(field.Key)) {
				if (builder.Length > 0 && builder[^1] != '\n') {
					builder.Append(newline);
				}

				builder.Append(field.Key).Append(" = ").Append(field.Value).Append(newline);
			}
		}

		return builder.ToString();
	}
}
