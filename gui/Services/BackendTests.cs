using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace WireSteam.UI.Services;

public sealed partial class Backend {
	/// <summary>Exercises owned temporary files and parser state without Steam, adapters, UAC or network connections.</summary>
	public static async Task<string> RunTestsAsync(string? enginePath = null) {
		static void Assert(bool condition, string message) {
			if (!condition) {
				throw new InvalidOperationException(message);
			}
		}

		const int drainLineLength = 4096;
		const int drainLineCount = 1024;
		const string committedProfileName = "Committed observer test profile";
		const string committedObserverMessage = "Owned post-commit observer failure";

		BoundedLogCollectionTests.Run();
		PortForwardConfigurationTests.Run();
		await BoundedLineReader.RunTestsAsync().ConfigureAwait(false);

		// A failed presentation callback must not stop draining an owned child's pipe.
		var drainText = string.Concat(Enumerable.Repeat(new string('x', drainLineLength) + "\n", drainLineCount));
		using (var drainData = new MemoryStream(Utf8.GetBytes(drainText))) {
			using var drainReader = new StreamReader(drainData, Utf8, false, drainLineLength, true);
			var expectedFailure = new InvalidOperationException("Owned pipe callback failure");
			Exception? observedFailure = null;
			int callbackCount = 0;
			try {
				await PumpAsync(drainReader, null, _ => {
					callbackCount++;

					return Task.FromException(expectedFailure);
				}).ConfigureAwait(false);
			} catch (Exception error) {
				observedFailure = error;
			}

			Assert(ReferenceEquals(observedFailure, expectedFailure) && callbackCount == 1 && drainData.Position == drainData.Length, "A failed pipe callback lost its first exception or stopped draining before EOF.");
		}

		var failedReader = new TaskCompletionSource();
		var pendingReader = new TaskCompletionSource();
		var expectedReaderFailure = new IOException("Owned reader failure");
		var readerCompletion = WaitForReadersAsync(failedReader.Task, pendingReader.Task);
		failedReader.SetException(expectedReaderFailure);
		Assert(!readerCompletion.IsCompleted, "Reader cleanup finished while another created reader still borrowed the job.");

		pendingReader.SetResult();
		Assert(ReferenceEquals(await readerCompletion.ConfigureAwait(false), expectedReaderFailure), "Joining both readers lost the original read failure.");

		var root = Path.Combine(Path.GetTempPath(), "WireSteam-ui-tests-" + Guid.NewGuid().ToString("N"));
		var profiles = Path.Combine(root, "profiles");
		var runtime = Path.Combine(root, "runtime");
		using var backend = new Backend(enginePath, profiles, false, runtime);
		var peerKeyFile = Path.Combine(root, "peer.key");
		var exported = Path.Combine(root, "exported.conf");
		Profile? created = null, imported = null;
		Exception? testFailure = null;
		try {
			ManagedFiles.EnsureDirectory(profiles);
			var template = await backend.CreateTemplateAsync().ConfigureAwait(false);
			var secret = Regex.Match(template, @"PrivateKey\s*=\s*([A-Za-z0-9+/]{43}=)").Groups[1].Value;
			Assert(secret.Length == 44, "The private template did not contain a generated key.");

			var peerResult = await backend.CoreAsync(new[] { "keygen", peerKeyFile }).ConfigureAwait(false);
			RequireSuccess(peerResult);
			var publicKey = Regex.Match(peerResult.Output, @"PublicKey\s*=\s*([A-Za-z0-9+/]{43}=)").Groups[1].Value;
			Assert(publicKey.Length == 44, "Cannot obtain a public test peer key.");

			Assert(ParseLocalPublicKey("Configuration is valid.\nLocal PublicKey = " + publicKey + "\n") == publicKey &&
				ParseLocalPublicKey("Configuration is valid.\r\nLocal PublicKey = " + publicKey + "\r\n") == publicKey, "Current engine public-key records must parse with native or normalized line endings.");

			foreach (var invalidKeyRecord in new[] {
				"PublicKey = " + publicKey,
				"Peer PublicKey = " + publicKey,
				"Unrelated Local PublicKey = " + publicKey,
				" Local PublicKey = " + publicKey,
				"Local PublicKey = " + publicKey + " extra",
				"Local PublicKey = " + publicKey[..^1],
				"Local PublicKey = " + publicKey + "="
			}) {
				Assert(ParseLocalPublicKey(invalidKeyRecord) == "UNAVAILABLE", "A noncurrent or malformed public-key record was accepted as the local identity.");
			}

			var content = template + "\n[Peer]\nPublicKey = " + publicKey + "\nSteamID = 76561198000000001\nAllowedIPs = 10.77.0.2/32\n";
			created = await backend.CreateAsync("Backend test profile", content).ConfigureAwait(false);
			Assert(created.Addresses == "10.77.0.1/32" && created.Peers.Count == 1, "Public configuration metadata was not parsed.");

			Assert(created.PublicKey.Length == 44 && created.PublicKey != secret, "The profile's public key is incorrect.");
			Assert(await backend.ReadConfigAsync(created).ConfigureAwait(false) == content, "Managed configuration did not round-trip.");

			int commitNotifications = 0;
			EventHandler failCommittedNotification = (_, _) => {
				if (++commitNotifications == 2) {
					throw new InvalidOperationException(committedObserverMessage);
				}
			};
			bool committedNotificationRejected = false;
			backend.StatusChanged += failCommittedNotification;
			try {
				await backend.CreateAsync(committedProfileName, content).ConfigureAwait(false);
			} catch (InvalidOperationException error) when (error.Message == committedObserverMessage) {
				committedNotificationRejected = true;
			} finally {
				backend.StatusChanged -= failCommittedNotification;
			}

			Assert(committedNotificationRejected && commitNotifications == 2, "The committed-catalog notification failure fixture did not reach its post-commit boundary.");
			var recoveredCommittedProfile = (await backend.LoadProfilesAsync().ConfigureAwait(false)).Single(profile => profile.Name == committedProfileName);
			Assert(File.Exists(recoveredCommittedProfile.ConfigPath) && await backend.ReadConfigAsync(recoveredCommittedProfile).ConfigureAwait(false) == content, "A post-commit observer failure removed the catalog's committed private configuration.");

			await backend.RemoveAsync(recoveredCommittedProfile).ConfigureAwait(false);

			var forwardText = content + "\n# forwarding integration fixture\n[PortForward]\nProtocol = TCP\nListen = 10.77.0.1:25565\nTarget = 127.0.0.1:25565\nMaxConnections = 23\nConnectTimeoutSeconds = 8\nIdleTimeoutSeconds = 75\n\n[PortForward]\nProtocol = UDP\nListen = 10.77.0.1:25565\nTarget = 127.0.0.1:25565\nMaxConnections = 17\nIdleTimeoutSeconds = 40\n";
			await backend.SaveAsync(created, forwardText).ConfigureAwait(false);
			Assert(created.PortForwards.Count == 2 && created.PortForwards[0].Listen == "10.77.0.1:25565" && created.PortForwards[1].Protocol == "UDP" && created.PortForwardSummary == "1 TCP FORWARD · 1 UDP FORWARD", "Mixed TCP/UDP forward metadata did not refresh after native validation and private save.");

			Assert(await backend.ReadConfigAsync(created).ConfigureAwait(false) == forwardText, "Saving forward rules changed unrelated private configuration or advanced settings.");

			created.Busy = true;
			bool busyEditRejected = false;
			try {
				await backend.SaveAsync(created, content).ConfigureAwait(false);
			} catch (InvalidOperationException) {
				busyEditRejected = true;
			} finally {
				created.Busy = false;
			}

			Assert(busyEditRejected && await backend.ReadConfigAsync(created).ConfigureAwait(false) == forwardText, "A staged forwarding edit must not save while activation or cleanup is pending.");

			var modified = content.Replace("MTU = 1280", "MTU = 1360", StringComparison.Ordinal);
			created.Name = "Renamed backend test profile";
			await backend.SaveAsync(created, modified).ConfigureAwait(false);
			Assert(created.Mtu == 1360, "Saved metadata was not refreshed.");

			Assert(created.PortForwards.Count == 0 && created.PortForwardSummary == "NO PORT FORWARDS", "Removing forwarding sections left stale public metadata.");

			bool rejected = false;
			try {
				await backend.SaveAsync(created, modified.Replace("10.77.0.2/32", "0.0.0.0/0", StringComparison.Ordinal)).ConfigureAwait(false);
			} catch (InvalidOperationException error) {
				rejected = true;
				Assert(!error.Message.Contains(secret, StringComparison.Ordinal), "An invalid-save diagnostic exposed a private key.");
			}

			Assert(rejected && await backend.ReadConfigAsync(created).ConfigureAwait(false) == modified, "An invalid edit replaced a working configuration.");

			// A duplicate port must fail inside GUI preflight, before a long
			// running engine, Steam session or elevation can be started.
			using (var occupied = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp)) {
				occupied.ExclusiveAddressUse = true;
				occupied.Bind(new IPEndPoint(IPAddress.Any, 0));
				int port = ((IPEndPoint)occupied.LocalEndPoint!).Port;
				var addresses = NetworkInterface.GetAllNetworkInterfaces()
					.SelectMany(networkInterface => networkInterface.GetIPProperties().UnicastAddresses).Select(unicastAddress => unicastAddress.Address.ToString()).ToHashSet();
				string unused = Enumerable.Range(1, 254).Select(octet => "10.254.241." + octet).First(address => !addresses.Contains(address));
				string conflict = modified.Replace("ListenPort = 51820", "ListenPort = " + port, StringComparison.Ordinal)
					.Replace("10.77.0.1/32", unused + "/32", StringComparison.Ordinal);
				await backend.SaveAsync(created, conflict).ConfigureAwait(false);
				rejected = false;
				try {
					try {
						await backend.StartAsync(created).ConfigureAwait(false);
					} catch (InvalidOperationException error) {
						rejected = error.Message.Contains("ListenPort", StringComparison.Ordinal);
					}

					Assert(rejected && backend.jobs.IsEmpty && !created.Running && created.State == "Error", "An occupied port did not stop GUI activation before engine launch.");

					Assert(created.Logs.Any(logEntry => logEntry.Level == "ERROR" && logEntry.Message.Contains(port.ToString(), StringComparison.Ordinal)), "GUI preflight did not expose the port conflict.");
				} finally {
					await backend.StopAllAsync().ConfigureAwait(false);
					await backend.SaveAsync(created, modified).ConfigureAwait(false);
				}
			}

			await backend.ExportAsync(created, exported).ConfigureAwait(false);
			var exportedContents = await ManagedFiles.ReadAsync(exported).ConfigureAwait(false);
			var exportedTime = File.GetLastWriteTimeUtc(exported);
			rejected = false;
			try {
				await backend.ExportAsync(created, exported).ConfigureAwait(false);
			} catch (InvalidOperationException) {
				rejected = true;
			}

			Assert(rejected && await ManagedFiles.ReadAsync(exported).ConfigureAwait(false) == modified, "Export overwrote an existing file.");

			imported = await backend.ImportAsync(exported).ConfigureAwait(false);
			Assert(imported.ConfigPath != exported && imported.Id != created.Id, "Import did not create an independent managed copy.");

			Assert(File.GetLastWriteTimeUtc(exported) == exportedTime && await ManagedFiles.ReadAsync(exported).ConfigureAwait(false) == exportedContents, "Import modified its source.");

			var metadata = await ManagedFiles.ReadAsync(backend.CatalogPath).ConfigureAwait(false);
			Assert(!metadata.Contains(secret, StringComparison.Ordinal) && !metadata.Contains("PrivateKey", StringComparison.OrdinalIgnoreCase), "Profile metadata contains private configuration material.");

			var saved = await backend.LoadProfilesAsync().ConfigureAwait(false);
			Assert(saved.Count == 2 && saved.All(loadedProfile => loadedProfile.Peers.Count == 1), "Managed profile catalog did not reload independently.");

			Assert(saved.Single(loadedProfile => loadedProfile.Id == created.Id).Name == created.Name, "Renaming a profile was not persisted in the catalog.");

			var validCatalog = await ManagedFiles.ReadAsync(backend.CatalogPath).ConfigureAwait(false);
			var catalogWithNullEntries = "[null,{\"Id\":null,\"Name\":null,\"FileName\":null}," + validCatalog.TrimStart()[1..];
			await ManagedFiles.WritePrivateAsync(backend.CatalogPath, catalogWithNullEntries, true).ConfigureAwait(false);
			Assert((await backend.LoadProfilesAsync().ConfigureAwait(false)).Count == 2, "Null metadata entries prevented valid profiles from loading.");

			var acl = new FileInfo(created.ConfigPath).GetAccessControl();
			Assert(acl.AreAccessRulesProtected, "A managed configuration inherits public access.");

			Assert(!PublicText("PrivateKey = " + secret).Contains(secret, StringComparison.Ordinal), "Private-key log redaction failed.");
			// Synthetic public log lines exercise real backend state transitions;
			// no engine process or adapter is started for this section.
			var telemetry = new Profile { Name = "Telemetry test" };
			ParseMetadata(telemetry, "\uFEFF[Interface]\nAddress = 10.77.0.1/32\n[PortForward]\nListen = 10.77.0.1:25565\nTarget = 127.0.0.1:25565\n");
			Assert(telemetry.Addresses == "10.77.0.1/32" && telemetry.PortForwards.Count == 1, "An initial UTF-8 BOM must not hide the profile's public address or forwarding metadata.");

			ParseMetadata(telemetry, "[ Interface ]\nAddress = 10.77.0.1/32 ; address comment\nMTU = 1360\n[ WireSteam ]\nAppID = 481\nVirtualPort = 4\nRelayOnly = 0\nInternetMode = server ; configured gateway role\nInternetSubnet = 10.77.0.0/24 # gateway subnet\n[ Peer ]\nSteamID = 76561198000000001 ; peer comment\nPublicKey = " + publicKey + "\nAllowedIPs = 10.77.0.2/32\n");
			Assert(telemetry.AppId == 481 && telemetry.Mtu == 1360 && telemetry.VirtualPort == 4 && !telemetry.RelayOnly && telemetry.Peers[0].SteamId == "76561198000000001", "Public metadata does not follow the native INI grammar.");

			Assert(telemetry.InternetMode == "server" && telemetry.InternetSubnet == "10.77.0.0/24" && telemetry.InternetModeLabel == "INTERNET GATEWAY", "Gateway role/subnet metadata was not parsed.");
			var metadataNotifications = new HashSet<string>();
			telemetry.PropertyChanged += (_, eventArguments) => {
				if (eventArguments.PropertyName != null) {
					metadataNotifications.Add(eventArguments.PropertyName);
				}
			};
			telemetry.RefreshMetadata();
			Assert(new[] { nameof(Profile.InternetMode), nameof(Profile.InternetSubnet), nameof(Profile.InternetModeLabel), nameof(Profile.InternetSubnetText) }.All(metadataNotifications.Contains), "Internet metadata changes did not notify UI bindings.");

			ParseMetadata(telemetry, "[WireSteam]\nInternetMode = client\n");
			Assert(telemetry.InternetMode == "client" && telemetry.InternetSubnet == "" && telemetry.InternetModeLabel == "INTERNET CLIENT", "Client metadata retained the previous gateway subnet.");

			ParseMetadata(telemetry, "[Interface]\nAddress = 10.77.0.1/32\n");
			Assert(telemetry.AppId == 480 && telemetry.Mtu == 1280 && telemetry.VirtualPort == 0 && telemetry.RelayOnly && telemetry.InternetMode == "off" && telemetry.InternetSubnet == "" && telemetry.InternetModeLabel == "PRIVATE NETWORK", "Removing optional configuration keys left stale metadata.");

			ParseMetadata(telemetry, modified);
			using var process = new Process();
			using var stop = new EventWaitHandle(false, EventResetMode.ManualReset);
			var job = new TunnelJob { Profile = telemetry, RegistrationKey = telemetry.Id, Process = process, StopEvent = stop };
			backend.HandleLine(job, "Setting breakpad minidump AppID = 480", true);
			backend.HandleLine(job, "SteamInternal_SetMinidumpSteamID:  Caching Steam ID:  76561198000000001 [API loaded no]", true);
			Assert(telemetry.Logs.All(logEntry => logEntry.Level == "INFO") && job.LastError.Length == 0, "Normal Steam startup diagnostics were treated as tunnel errors.");

			backend.HandleLine(job, "Error: WireSteam service helper failed (exit 1)", true);
			Assert(telemetry.Logs.Last().Level == "ERROR" && job.LastError == "Error: WireSteam service helper failed (exit 1)", "A real stderr error lost its severity or diagnostic.");

			backend.HandleLine(job, "[00:00:00Z] Setting breakpad minidump AppID = 480", true);
			backend.HandleLine(job, "SteamInternal_SetMinidumpSteamID:  Caching Steam ID:  76561198000000001 [API loaded yes]", true);
			Assert(telemetry.Logs.TakeLast(2).All(logEntry => logEntry.Level == "INFO") && job.LastError == "Error: WireSteam service helper failed (exit 1)", "A benign Steam diagnostic replaced the actual tunnel error.");

			backend.HandleLine(job, "SteamInternal_SetMinidumpSteamID: failed to cache Steam ID", true);
			backend.HandleLine(job, "Setting breakpad minidump AppID = 480 failed", true);
			Assert(telemetry.Logs.TakeLast(2).All(logEntry => logEntry.Level == "ERROR") && job.LastError == "Setting breakpad minidump AppID = 480 failed", "The Steam diagnostic whitelist masked an unexpected SDK error.");

			backend.HandleLine(job, "Service exists after installation failed", true);
			Assert(!job.CleanupRequired && telemetry.Logs.Last().Level == "ERROR", "An unsupported engine diagnostic must retain its error severity without selecting the adapter recovery state.");

			var peerNotifications = new HashSet<string>();
			telemetry.Peers[0].PropertyChanged += (_, eventArguments) => {
				if (eventArguments.PropertyName != null) {
					peerNotifications.Add(eventArguments.PropertyName);
				}
			};
			metadataNotifications.Clear();
			const string localName = "Local operator 🎮";
			const string remoteName = "MiXeD \"Night\" \\ Ω";
			backend.HandleLine(job, "[00:00:00Z] WIRESTEAM IDENTITY " + JsonSerializer.Serialize(new {
				SteamID = "76561198000000002",
				Name = localName,
				Local = true
			}), false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY " + JsonSerializer.Serialize(new {
				SteamID = "76561198000000001",
				Name = remoteName,
				Local = false
			}), false);
			Assert(telemetry.LocalSteamId == "76561198000000002" && telemetry.LocalSteamName == localName && telemetry.LocalIdentityText == localName + " · 76561198000000002", "The PascalCase local Steam identity did not preserve the UTF-8 persona name and ID.");

			Assert(telemetry.Peers[0].SteamName == remoteName && telemetry.Peers[0].DisplayName == remoteName, "The PascalCase peer Steam persona name was not decoded verbatim.");
			Assert(new[] { nameof(Profile.LocalSteamId), nameof(Profile.LocalSteamName), nameof(Profile.LocalIdentityText) }.All(metadataNotifications.Contains) && new[] { nameof(Peer.SteamName), nameof(Peer.DisplayName) }.All(peerNotifications.Contains), "Live identity fields did not notify their UI bindings.");

			backend.HandleLine(job, "WIRESTEAM IDENTITY " + JsonSerializer.Serialize(new {
				steamId = "76561198000000002",
				name = "Rejected local identity 🎮",
				local = true
			}), false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY " + JsonSerializer.Serialize(new {
				steamId = "76561198000000001",
				name = "Rejected peer identity Ω",
				local = false
			}), false);
			Assert(telemetry.LocalSteamId == "76561198000000002" && telemetry.LocalSteamName == localName && telemetry.Peers[0].SteamName == remoteName &&
				telemetry.Logs.TakeLast(2).All(logEntry => logEntry.Level == "WARN"), "CamelCase engine identity records must be rejected without changing local or peer identities.");

			backend.HandleLine(job, "WIRESTEAM IDENTITY " + JsonSerializer.Serialize(new {
				SteamID = "76561198000000002",
				Name = "Changed Local Nick",
				Local = true
			}), false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY " + JsonSerializer.Serialize(new {
				SteamID = "76561198000000001",
				Name = "Updated Peer Nick",
				Local = false
			}), false);
			Assert(telemetry.LocalSteamName == "Changed Local Nick" && telemetry.Peers[0].SteamName == "Updated Peer Nick", "A Steam persona update left the original nickname cached.");

			backend.HandleLine(job, "WIRESTEAM IDENTITY " + JsonSerializer.Serialize(new {
				SteamID = "76561198000000001",
				Name = "Adapter cleanup failed · VPN adapter is running · WIRESTEAM READY with SteamID 76561198000000001",
				Local = false
			}), false);
			Assert(!job.CleanupRequired && !job.AdapterRunning && job.Ready.Count == 0, "An untrusted Steam nickname changed adapter ownership or peer authentication state.");

			backend.HandleLine(job, "WIRESTEAM IDENTITY " + JsonSerializer.Serialize(new {
				SteamID = "76561198000000001",
				Name = "Updated Peer Nick",
				Local = false
			}), false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {not valid json}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY []", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":76561198000000002,\"Name\":\"INVALID\",\"Local\":true}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":\"76561198000000002\",\"Name\":\"INVALID\\nNAME\",\"Local\":true}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":\"76561198000000002\",\"Local\":true}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":\"76561198000000002\",\"Name\":\"INVALID\"}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":\"76561198000000002\",\"Name\":123,\"Local\":true}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":\"76561198000000002\",\"Name\":\"INVALID\",\"Local\":\"false\"}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":\"0\",\"Name\":\"INVALID\",\"Local\":true}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":\"076561198000000002\",\"Name\":\"INVALID\",\"Local\":true}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":\"18446744073709551616\",\"Name\":\"INVALID\",\"Local\":true}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":76561198000000002,\"Name\":\"INVALID\",\"Local\":true,\"steamId\":\"76561198000000002\",\"name\":\"INVALID FALLBACK\",\"local\":true}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":\"76561198000000002\",\"name\":\"INVALID MIXED SCHEMA\",\"local\":true}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"steamId\":\"76561198000000002\",\"Name\":\"INVALID MIXED ID\",\"Local\":true}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":\"76561198000000002\",\"name\":\"INVALID MIXED NAME\",\"Local\":true}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":\"76561198000000002\",\"Name\":\"INVALID MIXED ROLE\",\"local\":true}", false);
			backend.HandleLine(job, "WIRESTEAM IDENTITY {\"SteamID\":\"76561198000000999\",\"Name\":\"UNKNOWN PEER\",\"Local\":false}", false);
			Assert(telemetry.LocalSteamName == "Changed Local Nick" && telemetry.Peers[0].SteamName == "Updated Peer Nick" && telemetry.Peers.Count == 1, "Malformed or unconfigured Steam identity telemetry changed an existing identity.");

			backend.HandleLine(job, "[00:00:00Z] WIRESTEAM READY with SteamID 76561198000000001 (public-key configuration matched)", false);
			Assert(telemetry.State == "Starting", "Peer readiness incorrectly implied an active adapter.");

			backend.HandleLine(job, "VPN adapter is running. Ctrl+C stops the transport and removes the adapter.", false);
			Assert(telemetry.State == "Active", "Adapter and peer readiness did not activate the profile.");

			job.Samples["76561198000000001"] = (0, 0, Stopwatch.GetTimestamp() - Stopwatch.Frequency);
			backend.HandleLine(job, "[00:00:01Z] Peer 76561198000000001 tx=125000B rx=250000B dropped=0 route=SDR via test ping=17ms pending=0B", false);
			Assert(telemetry.ReceiveMbps > 1.5 && telemetry.SendMbps > 0.75 && telemetry.Peers[0].PingMs == 17, "Public traffic counters or ping did not reach the model.");

			var firstPeer = telemetry.Peers[0];
			Assert(firstPeer.ReceiveMbps == telemetry.ReceiveMbps && firstPeer.SendMbps == telemetry.SendMbps && firstPeer.IsReceiving && firstPeer.IsSending && firstPeer.RxBytes == 250000 && firstPeer.TxBytes == 125000 && firstPeer.ReceiveTotalText == "244.14 KiB" && firstPeer.SendTotalText == "122.07 KiB", "Per-peer traffic indicators or cumulative counters did not follow the actual payload statistics.");

			Assert(firstPeer.ReceiveActivityAt > 0 && firstPeer.SendActivityAt > 0 && telemetry.ReceiveActivityAt == firstPeer.ReceiveActivityAt && telemetry.SendActivityAt == firstPeer.SendActivityAt, "Observed RX/TX byte deltas must update independent peer and aggregate LED timestamps.");
			Assert(new[] { nameof(Peer.ReceiveMbps), nameof(Peer.SendMbps), nameof(Peer.ReceiveText), nameof(Peer.SendText), nameof(Peer.IsReceiving), nameof(Peer.IsSending), nameof(Peer.RxBytes), nameof(Peer.TxBytes), nameof(Peer.ReceiveTotalText), nameof(Peer.SendTotalText) }.All(peerNotifications.Contains), "A peer traffic update did not notify its rate, activity or total bindings.");

			telemetry.Peers.Add(new Peer { SteamId = "76561198000000002" });
			job.Samples[firstPeer.SteamId] = (125000, 250000, Stopwatch.GetTimestamp() - Stopwatch.Frequency);
			backend.HandleLine(job, "Peer 76561198000000001 tx=125000B rx=250000B dropped=0", false);
			Assert(firstPeer.ReceiveMbps == 0 && firstPeer.SendMbps == 0 && !firstPeer.IsReceiving && !firstPeer.IsSending && firstPeer.RxBytes == 250000, "An idle peer retained traffic indicators or lost its accumulated payload counters.");

			Assert(firstPeer.ReceiveActivityAt == 0 && firstPeer.SendActivityAt == 0 && telemetry.ReceiveActivityAt == 0 && telemetry.SendActivityAt == 0, "Unchanged byte counters must turn activity LEDs off without an idle heartbeat.");
			backend.HandleLine(job, "Peer 76561198000000001 tx=125000B rx=250001B dropped=0", false);
			Assert(firstPeer.ReceiveActivityAt > 0 && firstPeer.SendActivityAt == 0, "Receive-only data must not illuminate the send LED.");

			job.Samples[firstPeer.SteamId] = (125000, 250000, Stopwatch.GetTimestamp() - Stopwatch.Frequency);
			backend.HandleLine(job, "Peer 76561198000000001 tx=0B rx=0B dropped=0", false);
			Assert(firstPeer.ReceiveMbps == 0 && firstPeer.SendMbps == 0, "A reset native counter underflowed into a fictitious traffic spike.");

			Assert(firstPeer.ReceiveActivityAt == 0 && firstPeer.SendActivityAt == 0, "Counter resets must not simulate packet activity.");
			backend.HandleLine(job, "Peer 76561198000000001 tx=999999999999999999999999999B rx=0B dropped=0", false);
			Assert(firstPeer.TxBytes == 0, "An overflowing counter changed the peer telemetry.");

			job.Rates[firstPeer.SteamId] = (1, 2);
			job.Rates["76561198000000002"] = (3, 4);
			job.Samples["76561198000000002"] = (0, 0, Stopwatch.GetTimestamp() - 4 * Stopwatch.Frequency);
			firstPeer.ReceiveActivityAt = Stopwatch.GetTimestamp();
			telemetry.Peers[1].SendActivityAt = Stopwatch.GetTimestamp() - 4 * Stopwatch.Frequency;
			UpdateTrafficRates(job);
			backend.HandleLine(job, "[00:00:02Z] Disconnect SteamID 76561198000000001: test disconnect", false);
			Assert(telemetry.State == "Waiting" && telemetry.Peers[0].PingMs == -1, "Disconnect left a stale active state.");

			Assert(firstPeer.ReceiveMbps == 0 && firstPeer.SendMbps == 0 && telemetry.ReceiveMbps == 3 && telemetry.SendMbps == 4, "Disconnect left the departed peer's rates in the aggregate counters.");
			Assert(firstPeer.ReceiveActivityAt == 0 && firstPeer.SendActivityAt == 0, "Disconnected peers must clear their LED activity immediately.");

			Assert(ExpireTrafficRates(job, Stopwatch.GetTimestamp()) && telemetry.ReceiveMbps == 0 && telemetry.SendMbps == 0 && telemetry.Peers.All(trafficPeer => !trafficPeer.IsReceiving && !trafficPeer.IsSending), "A peer without fresh engine statistics retained a stale traffic indicator.");
			Assert(telemetry.ReceiveActivityAt == 0 && telemetry.SendActivityAt == 0 && telemetry.Peers.All(peer => peer.ReceiveActivityAt == 0 && peer.SendActivityAt == 0), "Stale telemetry must clear peer and aggregate LED timestamps.");

			backend.HandleLine(job, "PrivateKey = " + secret, true);
			Assert(telemetry.Logs.All(logEntry => !logEntry.Message.Contains(secret, StringComparison.Ordinal)), "The UI log collection contains a private key.");

			backend.HandleLine(job, "Installation did not complete successfully. Its private runtime configuration was preserved.", true);
			Assert(job.CleanupRequired, "An ambiguous adapter installation lost its recovery state.");

			telemetry.Running = telemetry.Busy = true;
			telemetry.State = "Stopping";
			backend.FinishJob(job, 1);
			Assert(!telemetry.Running && !telemetry.Busy && telemetry.State == "Cleanup required" && telemetry.ActionText == "Clean up adapter", "A failed adapter cleanup retained a busy stopping state or lost its explicit recovery action.");

			job.CleanupRequired = false;
			job.StopRequested = true;
			job.LastError = "";
			telemetry.Running = telemetry.Busy = true;
			telemetry.State = "Stopping";
			backend.FinishJob(job, 0);
			Assert(!telemetry.Running && !telemetry.Busy && telemetry.State == "Inactive" && telemetry.Detail == "Tunnel stopped" && telemetry.ActionText == "Activate tunnel", "A successful stop did not finish the profile's lifecycle.");

			job.StopRequested = false;
			job.LastError = "Error: Administrator approval was cancelled";
			telemetry.Running = true;
			telemetry.State = "Starting";
			backend.FinishJob(job, 1);
			Assert(!telemetry.Running && !telemetry.Busy && telemetry.State == "Inactive" && telemetry.Detail == "Activation cancelled", "Dismissing UAC left an activation in progress.");

			job.LastError = "Error: Invalid test configuration";
			telemetry.Running = true;
			telemetry.State = "Starting";
			backend.FinishJob(job, 1);
			Assert(!telemetry.Running && !telemetry.Busy && telemetry.State == "Error" && telemetry.Detail == job.LastError, "An engine startup failure retained a starting state or dropped its diagnostic.");

			firstPeer.TxBytes = 4096;
			firstPeer.RxBytes = 8192;
			firstPeer.ReceiveMbps = 5;
			firstPeer.SendMbps = 6;
			ResetTraffic(telemetry, true);
			Assert(telemetry.ReceiveMbps == 0 && telemetry.SendMbps == 0 && telemetry.Peers.All(trafficPeer => trafficPeer.RxBytes == 0 && trafficPeer.TxBytes == 0 && !trafficPeer.IsReceiving && !trafficPeer.IsSending), "A new session did not reset per-peer totals and activity.");

			var ownedProfile = new Profile { Name = "Ownership transition", Running = true, Busy = true, State = "Stopping" };
			var oldJob = new TunnelJob { Profile = ownedProfile, RegistrationKey = ownedProfile.Id, Process = process, StopEvent = stop };
			var nextJob = new TunnelJob { Profile = ownedProfile, RegistrationKey = ownedProfile.Id, Process = process, StopEvent = stop };
			Assert(backend.jobs.TryAdd(ownedProfile.Id, oldJob), "Cannot register the synthetic owned session.");

			bool idlePublishedWhileOwned = false, replacedAtIdle = false;
			ownedProfile.PropertyChanged += (_, eventArguments) => {
				if (eventArguments.PropertyName != nameof(Profile.State) || ownedProfile.State != "Inactive") {
					return;
				}

				idlePublishedWhileOwned = backend.jobs.ContainsKey(ownedProfile.Id);
				replacedAtIdle = backend.jobs.TryAdd(ownedProfile.Id, nextJob);
			};
			backend.FinishJob(oldJob, 0);
			Assert(!idlePublishedWhileOwned && replacedAtIdle, "A terminal profile state enabled reconnect before releasing its exited session's ownership.");

			backend.ReleaseJob(oldJob);
			Assert(backend.jobs.TryGetValue(ownedProfile.Id, out var currentJob) && ReferenceEquals(currentJob, nextJob), "A completed monitor's final cleanup removed a newer session's ownership entry.");

			backend.ReleaseJob(nextJob);
			Assert(!backend.jobs.ContainsKey(ownedProfile.Id), "Synthetic session ownership was not removed after the race regression test.");

			// Public model identifiers can change; owned cleanup still follows the job/reference.
			using (var firstStop = new EventWaitHandle(false, EventResetMode.ManualReset)) {
				using var secondStop = new EventWaitHandle(false, EventResetMode.ManualReset);
				var firstProfile = new Profile { Name = "First stable registration" };
				var secondProfile = new Profile { Name = "Second stable registration" };
				var firstJob = new TunnelJob { Profile = firstProfile, RegistrationKey = firstProfile.Id, Process = process, StopEvent = firstStop };
				var secondJob = new TunnelJob { Profile = secondProfile, RegistrationKey = secondProfile.Id, Process = process, StopEvent = secondStop };
				firstJob.CompletionSource.TrySetResult();
				secondJob.CompletionSource.TrySetResult();
				try {
					Assert(backend.jobs.TryAdd(firstJob.RegistrationKey, firstJob) && backend.jobs.TryAdd(secondJob.RegistrationKey, secondJob), "Cannot register synthetic stable-key sessions.");
					var collisionId = Guid.NewGuid().ToString("N");
					firstProfile.Id = collisionId;
					secondProfile.Id = collisionId;
					Assert(ReferenceEquals(backend.FindJob(firstProfile), firstJob) && ReferenceEquals(backend.FindJob(secondProfile), secondJob), "Mutable public identifiers redirected an owned profile to another session.");

					await backend.StopAllAsync().ConfigureAwait(false);
					Assert(firstStop.WaitOne(0) && secondStop.WaitOne(0), "Shutdown collapsed distinct owned profile references after their public identifiers collided.");

					backend.ReleaseJob(firstJob);
					backend.ReleaseJob(secondJob);
					Assert(!backend.jobs.ContainsKey(firstJob.RegistrationKey) && !backend.jobs.ContainsKey(secondJob.RegistrationKey), "Cleanup missed a captured registration key after the public model identifier changed.");
				} finally {
					backend.jobs.TryRemove(new KeyValuePair<string, TunnelJob>(firstJob.RegistrationKey, firstJob));
					backend.jobs.TryRemove(new KeyValuePair<string, TunnelJob>(secondJob.RegistrationKey, secondJob));
				}
			}

			telemetry.Logs.Clear();
			for (var index = 0; index <= BoundedLogCollection.MaximumEntries; ++index) {
				AddLog(telemetry, "Bounded log test " + index);
			}

			Assert(telemetry.Logs.Count == 2 && telemetry.Logs[0].Message == BoundedLogCollection.AutomaticClearMessage && telemetry.Logs[^1].Message == "Bounded log test " + BoundedLogCollection.MaximumEntries, "The backend log must reset at the shared limit and retain its incoming diagnostic.");

			AddLog(telemetry, "PrivateKey = " + secret + "\nPresharedKey = " + secret);
			Assert(!telemetry.Logs[^1].Message.Contains(secret, StringComparison.Ordinal) && telemetry.Logs[^1].Message.Contains("[REDACTED]", StringComparison.Ordinal), "Log rotation must not bypass private-key redaction.");

			// Reproduce an activation queued just before shutdown. The gate
			// must reject it before any Steam or adapter work can be spawned.
			await backend.lifecycleLock.WaitAsync().ConfigureAwait(false);
			var pendingStart = backend.StartAsync(created);
			var pendingShutdown = backend.StopAllAsync();
			backend.lifecycleLock.Release();
			rejected = false;
			try {
				await pendingStart.ConfigureAwait(false);
			} catch (InvalidOperationException error) {
				rejected = error.Message.Contains("shutdown", StringComparison.OrdinalIgnoreCase);
			}

			await pendingShutdown.ConfigureAwait(false);
			Assert(rejected && backend.jobs.IsEmpty && !created.Running, "A pending activation escaped the shutdown barrier.");

			// Inject an owned temporary marker, never a real user's runtime.
			// Restart recovery must protect the key/profile until cleanup.
			var marker = backend.RuntimePath(created);
			Assert(Path.GetFileName(marker) == "WireSteam-" + created.TunnelName + ".conf", "The runtime recovery name differs from the native core.");

			await ManagedFiles.WritePrivateAsync(marker, "owned test marker", false).ConfigureAwait(false);
			await backend.ReloadAsync(created).ConfigureAwait(false);
			Assert(created.State == "Cleanup required", "A leftover owned runtime was not detected.");

			rejected = false;
			try {
				await backend.SaveAsync(created, modified).ConfigureAwait(false);
			} catch (InvalidOperationException) {
				rejected = true;
			}

			Assert(rejected, "A profile with an uncleared runtime could be edited.");

			ManagedFiles.DeleteOwned(marker);
			created.State = "Inactive"; // No real adapter was created by this test.
			await backend.RemoveAsync(created).ConfigureAwait(false);
			await backend.RemoveAsync(imported).ConfigureAwait(false);
			created = imported = null;
			Assert(await ManagedFiles.ReadAsync(exported).ConfigureAwait(false) == exportedContents, "Removing an imported profile removed its source.");

			return "PASS GUI backend: private profile/template/import/export/save/catalog, rejected edits, source preservation, public-key metadata, redacted bounded logs, Steam identity updates, per-peer payload rates/totals/idle indicators and terminal activation/stop/cancellation states. No Steam, services, adapters, UAC or network connections were used.";
		} catch (Exception error) {
			testFailure = error;
			throw;
		} finally {
			// This uniquely created root contains only this test's explicit
			// configuration/key/catalog files; no recursive removal is used.
			try {
				foreach (var file in new[] { created?.ConfigPath, imported?.ConfigPath, peerKeyFile, exported, backend.CatalogPath }) {
					if (file != null) {
						ManagedFiles.DeleteOwned(file);
					}
				}

				// A failed create can leave its new GUID file before returning
				// a Profile. This directory belongs solely to this test.
				if (Directory.Exists(profiles)) {
					foreach (var file in Directory.EnumerateFiles(profiles, "*.conf")) {
						if (IdPattern.IsMatch(Path.GetFileNameWithoutExtension(file))) {
							ManagedFiles.DeleteOwned(file);
						}
					}
				}

				var drafts = Path.Combine(profiles, "drafts");
				if (Directory.Exists(drafts)) {
					Directory.Delete(drafts);
				}

				if (Directory.Exists(runtime)) {
					foreach (var file in Directory.EnumerateFiles(runtime, "WireSteam-ui-*.conf")) {
						ManagedFiles.DeleteOwned(file);
					}

					Directory.Delete(runtime);
				}

				if (Directory.Exists(profiles)) {
					Directory.Delete(profiles);
				}

				if (Directory.Exists(root)) {
					Directory.Delete(root);
				}
			} catch when (testFailure != null) {
				/* Preserve the original test failure instead of masking its stack. */
			}
		}
	}
}
