using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.ExceptionServices;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;

namespace WireSteam.UI.Services;

public sealed partial class Backend : IBackend {
	private sealed record CatalogEntry(string Id, string Name, string FileName, string? SourcePath = null);
	private sealed record CommandResult(int ExitCode, string Output);
	private sealed class TunnelJob {
		public required Profile Profile;
		public required string RegistrationKey;
		public required Process Process;
		public required EventWaitHandle StopEvent;
		public readonly TaskCompletionSource CompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);
		public readonly TaskCompletionSource<bool> StartupSource = new(TaskCreationOptions.RunContinuationsAsynchronously);
		public Task Completion => CompletionSource.Task;
		public bool AdapterRunning, StopRequested, CleanupRequired;
		public string LastError = "";
		public readonly HashSet<string> Ready = new(StringComparer.Ordinal);
		public readonly Dictionary<string, (ulong Tx, ulong Rx, long At)> Samples = new();
		public readonly Dictionary<string, (double Rx, double Tx)> Rates = new();
		public long TrafficAt;
	}

	private const int ProcessExitPollMilliseconds = 250;

	// Lifecycle transitions acquire lifecycleLock before ownershipLock.
	// Dispose retains ownershipLock while requesting owned tunnel shutdown; catalogLock is independent.
	// Catalog writes retain catalogLock across I/O to commit one consistent snapshot.
	private readonly SemaphoreSlim catalogLock = new(1, 1), lifecycleLock = new(1, 1);
	private readonly ConcurrentDictionary<string, TunnelJob> jobs = new(StringComparer.Ordinal);
	private readonly ConcurrentDictionary<string, Profile> knownProfiles = new(StringComparer.Ordinal);
	private readonly Dictionary<string, CatalogEntry> catalog = new(StringComparer.Ordinal);
	private readonly object ownershipLock = new();
	private bool disposed;
	private int stoppingAll;
	private readonly bool discoverExistingProfiles;
	private readonly string runtimeDirectory;
	private static readonly Regex IdPattern = new("^[a-f0-9]{32}$", RegexOptions.CultureInvariant);
	private static readonly Regex SecretPattern = new(@"(?i)\b(PrivateKey|PresharedKey)\s*=\s*[^\s#;]+", RegexOptions.CultureInvariant);
	private static readonly Regex PrefixPattern = new(@"^\[[^\]]+\]\s*", RegexOptions.CultureInvariant);
	private static readonly Regex SteamStartupDiagnosticPattern = new(@"^(?:Setting breakpad minidump AppID\s*=\s*[0-9]+|SteamInternal_SetMinidumpSteamID:\s+Caching Steam ID:\s+[0-9]+\s+\[API loaded (?:no|yes)\])\s*$", RegexOptions.CultureInvariant);
	private static readonly UTF8Encoding Utf8 = new(false, true);

	public string EnginePath {
		get;
	}

	public string ProfilesDirectory {
		get;
	}

	// Notifications run synchronously on the application dispatcher when present,
	// otherwise on the calling thread. Observer exceptions propagate to the task.
	public event EventHandler? StatusChanged;
	private string CatalogPath => Path.Combine(ProfilesDirectory, "catalog.json");
	private string BaseDirectory => Path.GetDirectoryName(EnginePath)!;

	public Backend(string? enginePath = null, string? profilesDirectory = null, bool discoverExistingProfiles = true, string? runtimeDirectory = null) {
		EnginePath = Path.GetFullPath(enginePath ?? Path.Combine(AppContext.BaseDirectory, "WireSteam.exe"));
		ProfilesDirectory = Path.GetFullPath(profilesDirectory ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WireSteam", "profiles"));
		this.discoverExistingProfiles = discoverExistingProfiles;
		this.runtimeDirectory = Path.GetFullPath(runtimeDirectory ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WireSteam", "runtime"));
	}

	private static Task UiAsync(Action action) {
		var application = Application.Current;
		var dispatcher = application?.Dispatcher;
		if (dispatcher == null || dispatcher.CheckAccess()) {
			action();
			return Task.CompletedTask;
		}

		if (dispatcher.HasShutdownStarted) {
			return Task.CompletedTask;
		}

		return dispatcher.InvokeAsync(action).Task;
	}

	private void Changed() => StatusChanged?.Invoke(this, EventArgs.Empty);
	private static string PublicText(string text) => SecretPattern.Replace(text, "$1 = [REDACTED]");
	private static string ErrorText(string text) {
		text = PublicText(text).Trim();

		return text.Length <= 1600 ? text : text[..1600] + "…";
	}

	private static void AddLog(Profile profile, string text, string level = "INFO") {
		text = PublicText(text);
		profile.Logs.Add(new LogEntry(DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture), level, text));
	}

	private void RequireEngine() {
		ObjectDisposedException.ThrowIf(disposed, this);

		if (!File.Exists(EnginePath)) {
			throw new FileNotFoundException("Keep WireSteam.exe beside WireSteam.UI.exe.", EnginePath);
		}

		ManagedFiles.CheckPath(EnginePath);
	}

	private static string DisplayName(string name) {
		name = name.Trim();
		if (name.Length is < 1 or > 100 || name.Any(char.IsControl)) {
			throw new ArgumentException("Profile names must contain 1–100 printable characters.");
		}

		return name;
	}

	private string ManagedPath(Profile profile) {
		if (!IdPattern.IsMatch(profile.Id)) {
			throw new InvalidOperationException("Invalid managed profile identifier.");
		}

		var expected = Path.Combine(ProfilesDirectory, profile.Id + ".conf");
		if (!string.Equals(Path.GetFullPath(profile.ConfigPath), expected, StringComparison.OrdinalIgnoreCase)) {
			throw new InvalidOperationException("Only managed configuration copies can be edited or removed.");
		}

		return expected;
	}

	private string RuntimePath(Profile profile) => Path.Combine(runtimeDirectory, "WireSteam-" + profile.TunnelName + ".conf");

	private static Process CreateProcess(string executable, IEnumerable<string> arguments) {
		var start = new ProcessStartInfo(executable) {
			UseShellExecute = false,
			CreateNoWindow = true,
			RedirectStandardInput = true,
			RedirectStandardOutput = true,
			RedirectStandardError = true,
			StandardOutputEncoding = Utf8,
			StandardErrorEncoding = Utf8,
			StandardInputEncoding = Utf8,
			WorkingDirectory = Path.GetDirectoryName(executable)!
		};
		foreach (var argument in arguments) {
			start.ArgumentList.Add(argument);
		}

		return new Process { StartInfo = start };
	}

	private static async Task PumpAsync(StreamReader reader, StringBuilder? capture, Func<string, Task>? lineCallback) {
		ExceptionDispatchInfo? callbackFailure = null;
		try {
			await foreach (string line in BoundedLineReader.ReadLinesAsync(reader).ConfigureAwait(false)) {
				if (capture != null) {
					lock (capture) {
						if (capture.Length < ManagedFiles.MaximumBytes) {
							capture.AppendLine(line[..Math.Min(line.Length, ManagedFiles.MaximumBytes - capture.Length)]);
						}
					}
				}

				if (lineCallback != null && callbackFailure == null) {
					try {
						await lineCallback(line).ConfigureAwait(false);
					} catch (Exception error) {
						// Presentation failures must not leave a live child blocked on its pipe.
						callbackFailure = ExceptionDispatchInfo.Capture(error);
					}
				}
			}
		} catch (Exception) when (callbackFailure != null) {
			callbackFailure.Throw();
			throw;
		}

		callbackFailure?.Throw();
	}

	private static async Task<Exception?> WaitForReadersAsync(Task stdout, Task stderr) {
		Exception? failure = null;
		try {
			await stdout.ConfigureAwait(false);
		} catch (Exception error) {
			failure = error;
		}

		try {
			await stderr.ConfigureAwait(false);
		} catch (Exception error) {
			failure ??= error;
		}

		return failure;
	}

	private static async Task<CommandResult> ExecuteAsync(string executable, IEnumerable<string> arguments, string? input = null, int timeoutMs = 15000) {
		if (input != null && Utf8.GetByteCount(input) > ManagedFiles.MaximumBytes) {
			throw new IOException("Configuration input must be at most 1 MiB.");
		}

		using var process = CreateProcess(executable, arguments);
		var output = new StringBuilder();
		using var deadline = new CancellationTokenSource();
		deadline.CancelAfter(timeoutMs);
		StreamReader? stdoutReader = null, stderrReader = null;
		StreamWriter? stdinWriter = null;
		Task stdout = Task.CompletedTask, stderr = Task.CompletedTask;
		bool processStarted = false;
		ExceptionDispatchInfo? failure = null;
		CommandResult? result = null;
		try {
			if (!process.Start()) {
				throw new IOException("Cannot start the WireSteam component.");
			}

			processStarted = true;
			stdoutReader = process.StandardOutput;
			stdout = PumpAsync(stdoutReader, output, null);
			stderrReader = process.StandardError;
			stderr = PumpAsync(stderrReader, output, null);

			stdinWriter = process.StandardInput;
			if (input != null) {
				await stdinWriter.WriteAsync(input.AsMemory(), deadline.Token).ConfigureAwait(false);
			}

			stdinWriter.Close();
			await process.WaitForExitAsync(deadline.Token).ConfigureAwait(false);
			var readerFailure = await WaitForReadersAsync(stdout, stderr).ConfigureAwait(false);
			if (readerFailure != null) {
				ExceptionDispatchInfo.Capture(readerFailure).Throw();
			}

			result = new CommandResult(process.ExitCode, output.ToString());
		} catch (OperationCanceledException) {
			failure = ExceptionDispatchInfo.Capture(new TimeoutException("The WireSteam command exceeded its time limit."));
		} catch (Exception error) {
			failure = ExceptionDispatchInfo.Capture(error);
		} finally {
			if (processStarted) {
				try {
					// Only these owned short commands are force-stop eligible.
					// VPN sessions retain their separate graceful event/monitor lifecycle.
					try {
						if (!process.HasExited) {
							process.Kill();
						}
					} catch (Exception error) {
						failure ??= ExceptionDispatchInfo.Capture(error);
					}

					try {
						await process.WaitForExitAsync().ConfigureAwait(false);
					} catch (Exception error) {
						failure ??= ExceptionDispatchInfo.Capture(error);

						while (!process.HasExited) {
							await Task.Delay(ProcessExitPollMilliseconds).ConfigureAwait(false);
						}
					}

					var readerFailure = await WaitForReadersAsync(stdout, stderr).ConfigureAwait(false);
					if (readerFailure != null) {
						failure ??= ExceptionDispatchInfo.Capture(readerFailure);
					}
				} finally {
					// Process leaves accessed streams to the caller. All I/O has settled.
					try {
						stdinWriter?.Dispose();
					} catch (Exception error) {
						failure ??= ExceptionDispatchInfo.Capture(error);
					} finally {
						stdoutReader?.Dispose();
						stderrReader?.Dispose();
					}
				}
			}
		}

		failure?.Throw();

		return result!;
	}

	private async Task<CommandResult> CoreAsync(IEnumerable<string> arguments, string? input = null, int timeoutMs = 15000) {
		RequireEngine();

		return await ExecuteAsync(EnginePath, arguments, input, timeoutMs).ConfigureAwait(false);
	}

	private static void RequireSuccess(CommandResult result) {
		if (result.ExitCode != 0) {
			throw new InvalidOperationException(ErrorText(result.Output.Length > 0 ? result.Output : "The WireSteam command failed."));
		}
	}

	private async Task SaveCatalogLockedAsync() {
		var json = JsonSerializer.Serialize(catalog.Values.OrderBy(entry => entry.Name).ToArray(), new JsonSerializerOptions { WriteIndented = true });
		await ManagedFiles.WritePrivateAsync(CatalogPath, json, true).ConfigureAwait(false);
	}

	private async Task SaveNativeAsync(string path, string text, bool overwrite) {
		var arguments = new List<string> { "save-config", path };
		if (overwrite) {
			arguments.Add("--overwrite");
		}

		RequireSuccess(await CoreAsync(arguments, text).ConfigureAwait(false));
	}

	public async Task<IReadOnlyList<Profile>> LoadProfilesAsync() {
		ManagedFiles.EnsureDirectory(ProfilesDirectory);
		var firstLaunch = !File.Exists(CatalogPath);
		await catalogLock.WaitAsync().ConfigureAwait(false);
		try {
			catalog.Clear();

			if (File.Exists(CatalogPath)) {
				try {
					var entries = JsonSerializer.Deserialize<CatalogEntry[]>(await ManagedFiles.ReadAsync(CatalogPath).ConfigureAwait(false)) ?? Array.Empty<CatalogEntry>();
					foreach (var entry in entries.Take(256)) {
						if (entry != null && !string.IsNullOrWhiteSpace(entry.Id) && !string.IsNullOrWhiteSpace(entry.Name) &&
							entry.Name.Length <= 100 && !entry.Name.Any(char.IsControl) && IdPattern.IsMatch(entry.Id) &&
							entry.FileName == entry.Id + ".conf" && File.Exists(Path.Combine(ProfilesDirectory, entry.FileName))) {
							catalog[entry.Id] = entry;
						}
					}
				} catch (JsonException) {
					/* Recover owned configuration copies below; never read keys into metadata. */
				}
			}

			foreach (var file in Directory.EnumerateFiles(ProfilesDirectory, "*.conf").Take(256)) {
				var id = Path.GetFileNameWithoutExtension(file);
				if (IdPattern.IsMatch(id) && !catalog.ContainsKey(id)) {
					catalog[id] = new CatalogEntry(id, "Recovered tunnel " + id[..6], id + ".conf");
				}
			}

			await SaveCatalogLockedAsync().ConfigureAwait(false);
		} finally {
			catalogLock.Release();
		}

		// Discover the repository's existing configs without changing originals.
		var sources = new[] { Path.Combine(BaseDirectory, "configs"), Path.Combine(Path.GetDirectoryName(BaseDirectory) ?? BaseDirectory, "configs") };
		foreach (var directory in sources.Distinct(StringComparer.OrdinalIgnoreCase)) {
			if (!discoverExistingProfiles || !firstLaunch) {
				break;
			}

			if (!Directory.Exists(directory)) {
				continue;
			}

			foreach (var source in Directory.EnumerateFiles(directory, "*.conf").Take(64)) {
				bool known;
				await catalogLock.WaitAsync().ConfigureAwait(false);
				try {
					known = catalog.Values.Any(entry => string.Equals(entry.SourcePath, Path.GetFullPath(source), StringComparison.OrdinalIgnoreCase));
				} finally {
					catalogLock.Release();
				}

				if (known) {
					continue;
				}

				try {
					await ImportAsync(source).ConfigureAwait(false);
				} catch (Exception exception) when (exception is IOException or InvalidOperationException or UnauthorizedAccessException) {
					/* Invalid source copies stay untouched. */
				}
			}
		}

		CatalogEntry[] snapshot;
		await catalogLock.WaitAsync().ConfigureAwait(false);
		try {
			snapshot = catalog.Values.OrderBy(entry => entry.Name).ToArray();
		} finally {
			catalogLock.Release();
		}

		var profiles = new List<Profile>();
		foreach (var entry in snapshot) {
			var profile = jobs.TryGetValue(entry.Id, out var running) ? running.Profile :
				new Profile { Id = entry.Id, Name = entry.Name, ConfigPath = Path.Combine(ProfilesDirectory, entry.FileName) };
			try {
				await ReloadAsync(profile).ConfigureAwait(false);
			} catch (Exception error) {
				await UiAsync(() => {
					profile.State = "Error";
					profile.Detail = ErrorText(error.Message);
					AddLog(profile, error.Message, "ERROR");
				}).ConfigureAwait(false);
			}

			profiles.Add(profile);
			knownProfiles[profile.Id] = profile;
		}

		return profiles;
	}

	public async Task<Profile> ImportAsync(string sourcePath) {
		sourcePath = Path.GetFullPath(sourcePath);
		var text = await ManagedFiles.ReadAsync(sourcePath).ConfigureAwait(false);

		return await CreateManagedAsync(Path.GetFileNameWithoutExtension(sourcePath), text, sourcePath).ConfigureAwait(false);
	}

	public Task<Profile> CreateAsync(string name, string content) => CreateManagedAsync(name, content, null);

	private async Task<Profile> CreateManagedAsync(string name, string content, string? source) {
		name = DisplayName(name);
		ManagedFiles.EnsureDirectory(ProfilesDirectory);
		var id = Guid.NewGuid().ToString("N");
		var profile = new Profile { Id = id, Name = name, ConfigPath = Path.Combine(ProfilesDirectory, id + ".conf") };
		await SaveNativeAsync(profile.ConfigPath, content, false).ConfigureAwait(false);
		try {
			await ReloadAsync(profile).ConfigureAwait(false);
			await catalogLock.WaitAsync().ConfigureAwait(false);
			try {
				catalog[id] = new CatalogEntry(id, name, id + ".conf", source);
				try {
					await SaveCatalogLockedAsync().ConfigureAwait(false);
				} catch {
					catalog.Remove(id);
					throw;
				}
			} finally {
				catalogLock.Release();
			}
		} catch {
			ManagedFiles.DeleteOwned(profile.ConfigPath);
			throw;
		}

		// A committed catalog entry must keep its configuration even if an observer fails.
		await UiAsync(Changed).ConfigureAwait(false);
		knownProfiles[profile.Id] = profile;

		return profile;
	}

	private static string ParseLocalPublicKey(string text) {
		var match = Regex.Match(text, @"(?m)^Local PublicKey = ([A-Za-z0-9+/]{43}=)\r?$");

		return match.Success ? match.Groups[1].Value : "UNAVAILABLE";
	}

	public async Task ReloadAsync(Profile profile) {
		if (profile.Running) {
			return;
		}

		var text = await ReadConfigAsync(profile).ConfigureAwait(false);
		var cleanupNeeded = File.Exists(RuntimePath(profile)) || profile.State == "Cleanup required";
		var check = await CoreAsync(new[] { "check", profile.ConfigPath }).ConfigureAwait(false);
		RequireSuccess(check);
		var publicKey = ParseLocalPublicKey(check.Output);
		await UiAsync(() => {
			ParseMetadata(profile, text);
			profile.PublicKey = publicKey;
			profile.RefreshMetadata();

			if (!profile.Running) {
				profile.State = cleanupNeeded ? "Cleanup required" : "Inactive";
				profile.Detail = cleanupNeeded ? "A previous runtime configuration remains · clean up this adapter before editing or activating" : "Validated configuration · ready to activate";
			}

			Changed();
		}).ConfigureAwait(false);
	}

	public async Task SaveAsync(Profile profile, string content) {
		if (profile.Running || profile.Busy || FindJob(profile) != null) {
			throw new InvalidOperationException("Deactivate this tunnel and wait for cleanup before editing its configuration.");
		}

		if (profile.State == "Cleanup required") {
			throw new InvalidOperationException("Recover this tunnel's adapter before editing its profile.");
		}

		var path = ManagedPath(profile);
		var name = DisplayName(profile.Name);
		await SaveNativeAsync(path, content, true).ConfigureAwait(false);
		await ReloadAsync(profile).ConfigureAwait(false);
		await catalogLock.WaitAsync().ConfigureAwait(false);
		try {
			if (catalog.TryGetValue(profile.Id, out var entry)) {
				catalog[profile.Id] = entry with {
					Name = name
				};
			} else {
				catalog[profile.Id] = new CatalogEntry(profile.Id, name, profile.Id + ".conf");
			}

			await SaveCatalogLockedAsync().ConfigureAwait(false);
		} finally {
			catalogLock.Release();
		}
	}

	public async Task<string> ReadConfigAsync(Profile profile) => await ManagedFiles.ReadAsync(ManagedPath(profile)).ConfigureAwait(false);

	public async Task ExportAsync(Profile profile, string destination, bool overwrite = false) {
		var text = await ReadConfigAsync(profile).ConfigureAwait(false);
		await SaveNativeAsync(Path.GetFullPath(destination), text, overwrite).ConfigureAwait(false);
	}

	public async Task RemoveAsync(Profile profile) {
		if (profile.Running || FindJob(profile) != null) {
			throw new InvalidOperationException("Deactivate this tunnel before removing its profile.");
		}

		if (profile.State == "Cleanup required") {
			throw new InvalidOperationException("Recover this tunnel's adapter before removing its profile.");
		}

		ManagedFiles.DeleteOwned(ManagedPath(profile));
		await catalogLock.WaitAsync().ConfigureAwait(false);
		try {
			catalog.Remove(profile.Id);
			await SaveCatalogLockedAsync().ConfigureAwait(false);
		} finally {
			catalogLock.Release();
		}

		knownProfiles.TryRemove(profile.Id, out _);
		await UiAsync(Changed).ConfigureAwait(false);
	}

	public async Task<string> CreateTemplateAsync() {
		var directory = Path.Combine(ProfilesDirectory, "drafts");
		ManagedFiles.EnsureDirectory(directory);
		var path = Path.Combine(directory, Guid.NewGuid().ToString("N") + ".conf");
		try {
			RequireSuccess(await CoreAsync(new[] { "init", path }).ConfigureAwait(false));

			return await ManagedFiles.ReadAsync(path).ConfigureAwait(false);
		} finally {
			ManagedFiles.DeleteOwned(path);
		}
	}

	private static void ParseMetadata(Profile profile, string text) {
		profile.Peers.Clear();
		profile.PortForwards.Clear();
		foreach (var rule in new PortForwardConfiguration(text).Rules) {
			profile.PortForwards.Add(rule);
		}

		profile.Addresses = "NO ADDRESS";
		profile.Mtu = 1280;
		profile.AppId = 480;
		profile.VirtualPort = 0;
		profile.RelayOnly = true;
		profile.InternetMode = "off";
		profile.InternetSubnet = "";
		var section = "";
		Peer? peer = null;
		foreach (var original in (text.StartsWith('\uFEFF') ? text[1..] : text).Split('\n')) {
			var comment = original.IndexOfAny(new[] { '#', ';' });
			var line = (comment >= 0 ? original[..comment] : original).Trim();
			if (line.Length == 0) {
				continue;
			}

			if (line.StartsWith('[') && line.EndsWith(']')) {
				section = line[1..^1].Trim();
				peer = section.Equals("Peer", StringComparison.OrdinalIgnoreCase) ? new Peer() : null;
				if (peer != null) {
					profile.Peers.Add(peer);
				}

				continue;
			}

			var split = line.IndexOf('=');
			if (split <= 0) {
				continue;
			}

			var key = line[..split].Trim().ToLowerInvariant();
			var value = line[(split + 1)..].Trim();
			if (peer != null) {
				if (key == "steamid") {
					peer.SteamId = ulong.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var id) ? id.ToString(CultureInfo.InvariantCulture) : value;
				} else if (key == "publickey") {
					peer.PublicKey = value;
				} else if (key == "allowedips") {
					peer.AllowedIps = value;
				}
			} else if (section.Equals("Interface", StringComparison.OrdinalIgnoreCase)) {
				if (key == "address") {
					profile.Addresses = value;
				} else if (key == "mtu" && int.TryParse(value, out var mtu)) {
					profile.Mtu = mtu;
				}
			} else if (section.Equals("WireSteam", StringComparison.OrdinalIgnoreCase)) {
				if (key == "appid" && uint.TryParse(value, out var appId)) {
					profile.AppId = appId;
				} else if (key == "virtualport" && int.TryParse(value, out var port)) {
					profile.VirtualPort = port;
				} else if (key == "internetmode") {
					profile.InternetMode = value;
				} else if (key == "internetsubnet") {
					profile.InternetSubnet = value;
				} else if (key == "relayonly") {
					if (value == "0") {
						profile.RelayOnly = false;
					} else if (value == "1") {
						profile.RelayOnly = true;
					} else if (bool.TryParse(value, out var relay)) {
						profile.RelayOnly = relay;
					}
				}
			}
		}
	}

	public async Task<bool> CheckPrerequisitesAsync() {
		bool result = await Task.Run(() => File.Exists(EnginePath) && File.Exists(Path.Combine(BaseDirectory, "steam_api64.dll")) &&
			File.Exists(Path.Combine(BaseDirectory, "WireSteamService.exe")) &&
			File.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "WireGuard", "wireguard.exe")) &&
			File.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "WireGuard", "wg.exe"))).ConfigureAwait(false);
		await UiAsync(Changed).ConfigureAwait(false);

		return result;
	}

	private TunnelJob StartTunnelJob(Profile profile) {
		// Starting under ownershipLock prevents Dispose from missing a new child.
		lock (ownershipLock) {
			ObjectDisposedException.ThrowIf(disposed, this);

			if (Volatile.Read(ref stoppingAll) > 0) {
				throw new InvalidOperationException("Tunnel activation was cancelled because shutdown began.");
			}

			var eventName = @"Local\WireSteamStop-" + Guid.NewGuid().ToString("N");
			var stop = new EventWaitHandle(false, EventResetMode.ManualReset, eventName, out var created);
			Process? process = null;
			TunnelJob? job = null;
			bool started = false, monitorCreated = false;
			try {
				if (!created) {
					throw new IOException("Cannot create a unique session stop event.");
				}

				// Register and allocate the waiting monitor before starting a child.
				// StopAsync may pre-signal this manual-reset event during startup.
				process = CreateProcess(EnginePath, new[] { "up", profile.ConfigPath, "--name", profile.TunnelName, "--stop-event", eventName, "--stats-seconds", "1" });
				job = new TunnelJob { Profile = profile, RegistrationKey = profile.Id, Process = process, StopEvent = stop };
				if (!jobs.TryAdd(job.RegistrationKey, job)) {
					throw new InvalidOperationException("Another activation was already registered.");
				}

				_ = MonitorAsync(job);
				monitorCreated = true;

				if (!process.Start()) {
					throw new IOException("Cannot start the tunnel engine.");
				}

				started = true;
				process.StandardInput.Close();
			} catch {
				if (started) {
					// A started child keeps its handles until the monitor confirms exit.
					try {
						job!.StopRequested = true;
						stop.Set();
					} finally {
						job!.StartupSource.TrySetResult(true);
					}
				} else {
					if (job != null) {
						ReleaseJob(job);
					}

					if (monitorCreated) {
						job!.StartupSource.TrySetResult(false);
					} else {
						stop.Dispose();
						process?.Dispose();
						job?.CompletionSource.TrySetResult();
					}
				}

				throw;
			}

			return job;
		}
	}

	public async Task StartAsync(Profile profile) {
		// Notifications are part of this serialized transition. Observers must not
		// synchronously wait for another lifecycle operation while this gate is held.
		await lifecycleLock.WaitAsync().ConfigureAwait(false);
		try {
			RequireEngine();

			if (Volatile.Read(ref stoppingAll) > 0) {
				throw new InvalidOperationException("Tunnel activation is unavailable while shutdown is in progress.");
			}

			if (FindJob(profile) != null) {
				throw new InvalidOperationException("This tunnel already has an owned session.");
			}

			if (profile.State == "Cleanup required") {
				throw new InvalidOperationException("Deactivate again to recover the previous adapter before activating.");
			}

			ManagedPath(profile);
			await UiAsync(() => {
				profile.Busy = true;
				profile.Detail = "Validating configuration and adapter availability";
				Changed();
			}).ConfigureAwait(false);

			if (!await CheckPrerequisitesAsync().ConfigureAwait(false)) {
				throw new InvalidOperationException("Keep the core files beside this application and install official WireGuard for Windows. Sign in to Steam before activating.");
			}

			await ReloadAsync(profile).ConfigureAwait(false);

			if (profile.State == "Cleanup required") {
				throw new InvalidOperationException("Recover this tunnel's adapter before activating its profile.");
			}

			var preflight = await CoreAsync(new[] { "preflight", profile.ConfigPath }).ConfigureAwait(false);
			if (preflight.ExitCode != 0) {
				var message = ErrorText(preflight.Output);
				await UiAsync(() => {
					profile.State = "Error";
					profile.Detail = message;
					AddLog(profile, message, "ERROR");
					Changed();
				}).ConfigureAwait(false);
				throw new InvalidOperationException(message);
			}

			var job = StartTunnelJob(profile);
			try {
				await UiAsync(() => {
					profile.Running = true;
					profile.Busy = false;
					profile.State = "Starting";
					profile.Detail = "Starting Steam transport · approve adapter setup when prompted";
					profile.LocalSteamId = "—";
					profile.LocalSteamName = "";
					ResetTraffic(profile, true);
					profile.Traffic.Clear();
					AddLog(profile, "Launching the normal-user tunnel engine. Adapter setup requests UAC separately.");
					Changed();
				}).ConfigureAwait(false);
				knownProfiles[profile.Id] = profile;
			} finally {
				job.StartupSource.TrySetResult(true);
			}
		} catch (Exception error) {
			// Failure before a session was created must also finish the UI's
			// activation state. A live owned session keeps its monitor/state.
			if (FindJob(profile) == null && !profile.Running) {
				await UiAsync(() => {
					profile.Busy = false;
					var message = ErrorText(error.Message);

					if (profile.State != "Cleanup required") {
						profile.State = "Error";
					}

					if (profile.Detail != message) {
						AddLog(profile, message, "ERROR");
					}

					profile.Detail = message;
					Changed();
				}).ConfigureAwait(false);
			}

			throw;
		} finally {
			lifecycleLock.Release();
		}
	}

	private static void ResetTraffic(Profile profile, bool clearTotals) {
		profile.ReceiveMbps = profile.SendMbps = 0;
		profile.ReceiveActivityAt = profile.SendActivityAt = 0;
		foreach (var peer in profile.Peers) {
			peer.ReceiveMbps = peer.SendMbps = 0;
			peer.ReceiveActivityAt = peer.SendActivityAt = 0;

			if (clearTotals) {
				peer.RxBytes = peer.TxBytes = 0;
			}

			peer.State = "Waiting";
			peer.PingMs = -1;
			peer.Route = "No route yet";
		}

		profile.RefreshPeerSummary();
	}

	private static void UpdateTrafficRates(TunnelJob job) {
		foreach (var peer in job.Profile.Peers) {
			var rate = job.Rates.TryGetValue(peer.SteamId, out var current) ? current : default;
			peer.ReceiveMbps = rate.Rx;
			peer.SendMbps = rate.Tx;

			if (!job.Samples.ContainsKey(peer.SteamId)) {
				peer.ReceiveActivityAt = peer.SendActivityAt = 0;
			}
		}

		job.Profile.ReceiveMbps = job.Profile.Peers.Sum(peer => peer.ReceiveMbps);
		job.Profile.SendMbps = job.Profile.Peers.Sum(peer => peer.SendMbps);
		job.Profile.ReceiveActivityAt = job.Profile.Peers.Select(peer => peer.ReceiveActivityAt).DefaultIfEmpty().Max();
		job.Profile.SendActivityAt = job.Profile.Peers.Select(peer => peer.SendActivityAt).DefaultIfEmpty().Max();
	}

	private static bool ExpireTrafficRates(TunnelJob job, long now) {
		var stale = job.Rates.Keys.Where(id => !job.Samples.TryGetValue(id, out var sample) ||
			(now - sample.At) / (double)Stopwatch.Frequency >= 3).ToArray();
		foreach (var id in stale) {
			job.Rates.Remove(id);
			var peer = job.Profile.Peers.FirstOrDefault(peer => peer.SteamId == id);
			if (peer != null) {
				peer.ReceiveActivityAt = peer.SendActivityAt = 0;
			}
		}

		if (stale.Length > 0) {
			UpdateTrafficRates(job);
		}

		return stale.Length > 0;
	}

	private async Task MonitorTrafficIdleAsync(TunnelJob job, CancellationToken cancellation) {
		try {
			while (true) {
				await Task.Delay(500, cancellation).ConfigureAwait(false);
				await UiAsync(() => {
					if (ExpireTrafficRates(job, Stopwatch.GetTimestamp())) {
						Changed();
					}
				}).ConfigureAwait(false);
			}
		} catch (OperationCanceledException) when (cancellation.IsCancellationRequested) {
		}
	}

	private void FinishJob(TunnelJob job, int code, string? monitorError = null) {
		var profile = job.Profile;
		// Release this exact exited session before publishing an idle state.
		// UI commands may start another session as soon as it becomes idle.
		ReleaseJob(job);
		var cancelled = !job.CleanupRequired && (
			job.LastError.Contains("Administrator approval was cancelled", StringComparison.OrdinalIgnoreCase) ||
			job.StopRequested && job.LastError.Contains("cancelled", StringComparison.OrdinalIgnoreCase));
		profile.Running = profile.Busy = false;
		job.Ready.Clear();
		job.Rates.Clear();
		job.Samples.Clear();
		ResetTraffic(profile, false);
		profile.State = job.CleanupRequired ? "Cleanup required" : code == 0 || cancelled ? "Inactive" : "Error";
		profile.Detail = job.CleanupRequired ? "Adapter cleanup needs approval · deactivate again to recover" :
			cancelled ? "Activation cancelled" : code == 0 ? "Tunnel stopped" :
			ErrorText(job.LastError.Length > 0 ? job.LastError : monitorError ?? "The tunnel engine exited with code " + code);
		AddLog(profile, "Tunnel engine exited with code " + code + ".", code == 0 || cancelled ? "INFO" : "ERROR");
		Changed();
	}

	private TunnelJob? FindJob(Profile profile) {
		if (jobs.TryGetValue(profile.Id, out var job) && ReferenceEquals(job.Profile, profile)) {
			return job;
		}

		return jobs.Values.FirstOrDefault(candidate => ReferenceEquals(candidate.Profile, profile)) ?? job;
	}

	private void ReleaseJob(TunnelJob job) => jobs.TryRemove(new KeyValuePair<string, TunnelJob>(job.RegistrationKey, job));
	private async Task MonitorAsync(TunnelJob job) {
		CancellationTokenSource? idleCancellation = null;
		StreamReader? stdoutReader = null, stderrReader = null;
		Task idle = Task.CompletedTask, stdout = Task.CompletedTask, stderr = Task.CompletedTask;
		bool started = false, errorReported = false;
		Exception? monitorError = null;
		ExceptionDispatchInfo? notificationFailure = null;
		try {
			started = await job.StartupSource.Task.ConfigureAwait(false);
			if (!started) {
				return;
			}

			try {
				idleCancellation = new CancellationTokenSource();
				idle = MonitorTrafficIdleAsync(job, idleCancellation.Token);
				stdoutReader = job.Process.StandardOutput;
				stdout = PumpAsync(stdoutReader, null, line => UiAsync(() => HandleLine(job, line, false)));
				stderrReader = job.Process.StandardError;
				stderr = PumpAsync(stderrReader, null, line => UiAsync(() => HandleLine(job, line, true)));
				await job.Process.WaitForExitAsync().ConfigureAwait(false);
			} catch (Exception error) {
				monitorError = error;
				try {
					await UiAsync(() => {
						AddLog(job.Profile, error.Message, "ERROR");
						errorReported = true;

						if (!job.Process.HasExited) {
							job.Profile.Busy = false;
							job.Profile.State = "Error";
							job.Profile.Detail = "Cannot read the owned tunnel engine's output: " + ErrorText(error.Message);
							Changed();
						}
					}).ConfigureAwait(false);
				} catch (Exception notificationError) {
					notificationFailure = ExceptionDispatchInfo.Capture(notificationError);
				}

				// Never release a live VPN after a setup/read/observer failure.
				while (!job.Process.HasExited) {
					await Task.Delay(ProcessExitPollMilliseconds).ConfigureAwait(false);
				}
			}
		} finally {
			try {
				idleCancellation?.Cancel();

				if (!started || job.Process.HasExited) {
					var readerFailure = await WaitForReadersAsync(stdout, stderr).ConfigureAwait(false);
					monitorError ??= readerFailure;
					try {
						await idle.ConfigureAwait(false);
					} catch (OperationCanceledException) {
						/* Dispatcher shutdown can cancel a pending rate refresh. */
					} catch (Exception idleError) {
						monitorError ??= idleError;
					}

					if (started) {
						try {
							await UiAsync(() => {
								if (monitorError != null && !errorReported) {
									AddLog(job.Profile, monitorError.Message, "ERROR");
								}

								// All pipe and idle callbacks have settled before idle publication.
								FinishJob(job, job.Process.ExitCode, monitorError?.Message);
							}).ConfigureAwait(false);
						} catch (Exception notificationError) {
							notificationFailure ??= ExceptionDispatchInfo.Capture(notificationError);
						}
					}
				}
			} finally {
				idleCancellation?.Dispose();

				if (!started || job.Process.HasExited) {
					// These synchronous Process readers own their pipe handles.
					stdoutReader?.Dispose();
					stderrReader?.Dispose();
					job.StopEvent.Dispose();
					job.Process.Dispose();
					ReleaseJob(job);
					job.CompletionSource.TrySetResult();
				}
			}
		}

		notificationFailure?.Throw();
	}

	private void HandleLine(TunnelJob job, string line, bool error) {
		var profile = job.Profile;
		var text = PrefixPattern.Replace(line, "");
		if (text.StartsWith("WIRESTEAM IDENTITY ", StringComparison.Ordinal)) {
			// Persona names are untrusted display data. Never let a nickname
			// containing an old diagnostic phrase drive lifecycle parsing.
			AddLog(profile, line, ApplySteamIdentity(profile, text) ? "INFO" : "WARN");
			Changed();
			return;
		}

		// Steam writes these normal startup diagnostics to stderr. Match their
		// complete known format so an unrelated SDK error retains its severity.
		var level = SteamStartupDiagnosticPattern.IsMatch(text) ? "INFO" : error ? "ERROR" : text.Contains("failed", StringComparison.OrdinalIgnoreCase) ? "WARN" : "INFO";
		AddLog(profile, line, level);

		if (level == "ERROR") {
			job.LastError = text;
		}

		if (text.Contains("Adapter cleanup failed", StringComparison.OrdinalIgnoreCase) || text.Contains("runtime configuration already exists", StringComparison.OrdinalIgnoreCase) || text.Contains("Tunnel service already exists", StringComparison.OrdinalIgnoreCase) || text.Contains("Installation did not complete successfully. Its private runtime configuration was preserved.", StringComparison.OrdinalIgnoreCase)) {
			job.CleanupRequired = true;
		}

		var own = Regex.Match(text, @"^SteamID (\d+) \|");
		if (own.Success) {
			profile.LocalSteamId = own.Groups[1].Value;
		}

		if (text.Contains("VPN adapter is running", StringComparison.Ordinal)) {
			job.AdapterRunning = true;
		}

		var ready = Regex.Match(text, @"WIRESTEAM READY with SteamID (\d+)");
		if (ready.Success) {
			var id = ready.Groups[1].Value;
			job.Ready.Add(id);
			var peer = profile.Peers.FirstOrDefault(candidate => candidate.SteamId == id);
			if (peer != null) {
				peer.State = "Ready";
			}
		}

		var connected = Regex.Match(text, @"^SteamID (\d+) connected; route=(.+)$");
		if (connected.Success) {
			var peer = profile.Peers.FirstOrDefault(candidate => candidate.SteamId == connected.Groups[1].Value);
			if (peer != null) {
				peer.Route = connected.Groups[2].Value;
				peer.State = "Authenticating";
			}
		}

		var disconnected = Regex.Match(text, @"^Disconnect SteamID (\d+):");
		if (disconnected.Success) {
			var id = disconnected.Groups[1].Value;
			job.Ready.Remove(id);
			job.Rates.Remove(id);
			job.Samples.Remove(id);
			var peer = profile.Peers.FirstOrDefault(candidate => candidate.SteamId == id);
			if (peer != null) {
				peer.State = "Waiting";
				peer.Route = "No route yet";
				peer.PingMs = -1;
			}

			UpdateTrafficRates(job);
			profile.RefreshPeerSummary();
		}

		var statistics = Regex.Match(text, @"^Peer (\d+) tx=(\d+)B rx=(\d+)B dropped=\d+(?: route=(.+?) ping=(-?\d+)ms pending=\d+B)?$");
		if (statistics.Success && ulong.TryParse(statistics.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var tx) &&
			ulong.TryParse(statistics.Groups[3].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var rx)) {
			var id = statistics.Groups[1].Value;
			var now = Stopwatch.GetTimestamp();
			if (job.Samples.TryGetValue(id, out var last)) {
				var elapsed = (now - last.At) / (double)Stopwatch.Frequency;
				if (elapsed > 0.1) {
					job.Rates[id] = ((rx >= last.Rx ? rx - last.Rx : 0) * 8.0 / elapsed / 1e6, (tx >= last.Tx ? tx - last.Tx : 0) * 8.0 / elapsed / 1e6);
				}
			}

			job.Samples[id] = (tx, rx, now);
			var peer = profile.Peers.FirstOrDefault(candidate => candidate.SteamId == id);
			if (peer != null) {
				peer.ReceiveActivityAt = rx > peer.RxBytes ? now : 0;
				peer.SendActivityAt = tx > peer.TxBytes ? now : 0;
				peer.TxBytes = tx;
				peer.RxBytes = rx;

				if (statistics.Groups[4].Success) {
					peer.Route = statistics.Groups[4].Value;
				}

				if (statistics.Groups[5].Success && int.TryParse(statistics.Groups[5].Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var ping)) {
					peer.PingMs = ping;
				}
			}

			UpdateTrafficRates(job);

			if ((now - job.TrafficAt) / (double)Stopwatch.Frequency >= 0.75) {
				profile.Traffic.Add(new TrafficPoint(profile.ReceiveMbps, profile.SendMbps));

				while (profile.Traffic.Count > 60) {
					profile.Traffic.RemoveAt(0);
				}

				job.TrafficAt = now;
			}

			profile.RefreshPeerSummary();
		}

		if (!job.StopRequested) {
			profile.State = job.AdapterRunning ? job.Ready.Count > 0 ? "Active" : "Waiting" : "Starting";
			profile.Detail = job.AdapterRunning ? job.Ready.Count > 0 ? "Steam transport ready · WireGuard handshake is negotiated separately" : "Adapter running · waiting for an authenticated Steam peer" : "Starting transport and adapter · administrator approval may be required";
		}

		Changed();
	}

	private static bool ApplySteamIdentity(Profile profile, string text) {
		const string prefix = "WIRESTEAM IDENTITY ";
		if (!text.StartsWith(prefix, StringComparison.Ordinal)) {
			return false;
		}

		try {
			using var json = JsonDocument.Parse(text[prefix.Length..]);
			var root = json.RootElement;
			if (root.ValueKind != JsonValueKind.Object) {
				return false;
			}

			if (!root.TryGetProperty("SteamID", out var idValue) || idValue.ValueKind != JsonValueKind.String ||
				!root.TryGetProperty("Name", out var nameValue) || nameValue.ValueKind != JsonValueKind.String ||
				!root.TryGetProperty("Local", out var localValue) ||
				localValue.ValueKind is not (JsonValueKind.True or JsonValueKind.False)) {
				return false;
			}

			var id = idValue.GetString()!;
			if (!ulong.TryParse(id, NumberStyles.None, CultureInfo.InvariantCulture, out var numericId) || numericId == 0 ||
				numericId.ToString(CultureInfo.InvariantCulture) != id) {
				return false;
			}

			var name = nameValue.GetString()!;
			if (name.Length > 256 || name.Any(char.IsControl)) {
				return false;
			}

			if (localValue.GetBoolean()) {
				profile.LocalSteamId = id;
				profile.LocalSteamName = name;
			} else {
				var peer = profile.Peers.FirstOrDefault(candidate => candidate.SteamId == id);
				if (peer != null) {
					peer.SteamName = name;
				}
			}

			return true;
		} catch (JsonException) {
			return false;
		}
	}

	public async Task StopAsync(Profile profile) {
		var job = FindJob(profile);
		if (job == null) {
			if (profile.State != "Cleanup required") {
				return;
			}

			await UiAsync(() => {
				profile.Busy = true;
				profile.Detail = "Recovering the owned adapter · administrator approval may be required";
				Changed();
			}).ConfigureAwait(false);
			try {
				RequireSuccess(await CoreAsync(new[] { "down", "--name", profile.TunnelName }, timeoutMs: 120000).ConfigureAwait(false));
				await UiAsync(() => {
					profile.State = "Inactive";
					profile.Detail = "Adapter cleanup completed";
					AddLog(profile, "Recovered the owned adapter.");
				}).ConfigureAwait(false);
			} catch (Exception error) {
				await UiAsync(() => {
					profile.Detail = ErrorText(error.Message);
					AddLog(profile, profile.Detail, "ERROR");
					Changed();
				}).ConfigureAwait(false);
				throw;
			} finally {
				await UiAsync(() => {
					profile.Busy = false;
					Changed();
				}).ConfigureAwait(false);
			}

			return;
		}

		job.StopRequested = true;
		try {
			try {
				await UiAsync(() => {
					profile.Busy = true;
					profile.State = "Stopping";
					profile.Detail = "Stopping safely · approve adapter removal when prompted";
					Changed();
				}).ConfigureAwait(false);
			} finally {
				try {
					job.StopEvent.Set();
				} catch (ObjectDisposedException) {
					/* The monitor already observed process exit. */
				}
			}

			await job.Completion.WaitAsync(TimeSpan.FromSeconds(105)).ConfigureAwait(false);
		} catch (TimeoutException) {
			await UiAsync(() => {
				profile.Busy = false;
				profile.Detail = "Still stopping. Resolve the administrator prompt and try again; the VPN process has not been killed.";
				Changed();
			}).ConfigureAwait(false);
			throw new TimeoutException("The tunnel is still stopping. Resolve its administrator prompt and try again. The adapter process remains owned and was not killed.");
		}

		if (profile.State == "Cleanup required") {
			throw new InvalidOperationException("Adapter cleanup requires another approval. Deactivate again to recover this owned adapter.");
		}
	}

	public async Task StopAllAsync() {
		Interlocked.Increment(ref stoppingAll);
		try {
			await lifecycleLock.WaitAsync().ConfigureAwait(false);
			Profile[] profiles;
			try {
				profiles = jobs.Values.Select(job => job.Profile).Concat(knownProfiles.Values.Where(profile => profile.State == "Cleanup required"))
					.DistinctBy(profile => profile, ReferenceEqualityComparer.Instance).ToArray();
			} finally {
				lifecycleLock.Release();
			}

			await Task.WhenAll(profiles.Select(StopAsync)).ConfigureAwait(false);
		} finally {
			Interlocked.Decrement(ref stoppingAll);
		}
	}

	public void Dispose() {
		lock (ownershipLock) {
			if (disposed) {
				return;
			}

			disposed = true;
			ExceptionDispatchInfo? failure = null;
			foreach (var job in jobs.Values) {
				try {
					job.StopRequested = true;
					job.StopEvent.Set();
				} catch (ObjectDisposedException) {
				} catch (Exception error) {
					failure ??= ExceptionDispatchInfo.Capture(error);
				}
			}

			failure?.Throw();
		}

		// Background monitors retain live VPN handles until normal cleanup.
		// There is deliberately no Process.Kill path for tunnel sessions.
	}
}
