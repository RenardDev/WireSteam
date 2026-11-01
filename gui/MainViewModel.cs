using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Globalization;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Data;
using WireSteam.UI.Services;

namespace WireSteam.UI;

public sealed class MainViewModel : Observable {
	public IBackend Backend {
		get;
	}

	public ObservableCollection<Profile> Profiles {
		get;
	} = new();

	public ICollectionView ProfilesView {
		get;
	}

	private ICollectionView filteredLogs;
	public ICollectionView FilteredLogs {
		get => filteredLogs;
		private set => Set(ref filteredLogs, value);
	}

	private Profile? selected;
	private Profile? pendingProfile;
	private bool pendingStop, lifecycleMessage;
	private string page = "Dashboard", engineStatus = "Checking engine…", statusMessage = "Ready", profileFilter = "", logFilter = "";
	private bool isQuitting, autoScrollLogs = true;
	public bool AutoScrollLogs {
		get => autoScrollLogs;
		set => Set(ref autoScrollLogs, value);
	}

	public bool IsQuitting {
		get => isQuitting;
		set {
			if (Set(ref isQuitting, value)) {
				RefreshCommands();
				PresentationChanged?.Invoke(this, EventArgs.Empty);
			}
		}
	}

	public Profile? Selected {
		get => selected;
		set {
			if (Set(ref selected, value)) {
				FilteredLogs = CollectionViewSource.GetDefaultView(value?.Logs ?? new ObservableCollection<LogEntry>());
				FilteredLogs.Filter = FilterLog;
				RefreshCommands();
			}
		}
	}

	public string EngineStatus {
		get => engineStatus;
		set => Set(ref engineStatus, value);
	}

	public string StatusMessage {
		get => statusMessage;
		set {
			if (Set(ref statusMessage, value)) {
				lifecycleMessage = false;
				Changed(nameof(StatusDisplay));
			}
		}
	}

	public string StatusDisplay => lifecycleMessage ? statusMessage : statusMessage.ToUpperInvariant();

	public string ProfileFilter {
		get => profileFilter;
		set {
			if (Set(ref profileFilter, value)) {
				ProfilesView.Refresh();
			}
		}
	}

	public string LogFilter {
		get => logFilter;
		set {
			if (Set(ref logFilter, value)) {
				FilteredLogs.Refresh();
			}
		}
	}

	public string ActiveCountText => Profiles.Count(profile => profile.Running).ToString(CultureInfo.InvariantCulture) + " ACTIVE";
	public string ProfileCountText => Profiles.Count.ToString(CultureInfo.InvariantCulture) + " TUNNELS";
	public bool IsDashboardVisible => page == "Dashboard";
	public bool IsLogsVisible => page == "Logs";
	public bool IsSettingsVisible => page == "Settings";

	public event EventHandler? PresentationChanged;
	public event EventHandler? NewRequested;
	public event EventHandler? ImportRequested;
	public event EventHandler? EditRequested;
	public event EventHandler? PortForwardsRequested;
	public event EventHandler? ExportRequested;
	public event EventHandler? RemoveRequested;
	public RelayCommand ImportCommand {
		get;
	}

	public RelayCommand NewCommand {
		get;
	}

	public RelayCommand EditCommand {
		get;
	}

	public RelayCommand ManageForwardsCommand {
		get;
	}

	public RelayCommand ExportCommand {
		get;
	}

	public RelayCommand RemoveCommand {
		get;
	}

	public RelayCommand ToggleCommand {
		get;
	}

	public RelayCommand ConnectCommand {
		get;
	}

	public RelayCommand DisconnectCommand {
		get;
	}

	public RelayCommand ShowDashboardCommand {
		get;
	}

	public RelayCommand ShowLogsCommand {
		get;
	}

	public RelayCommand ShowSettingsCommand {
		get;
	}

	public RelayCommand CopyLogCommand {
		get;
	}

	public RelayCommand ClearLogsCommand {
		get;
	}

	public MainViewModel(IBackend backend) {
		Backend = backend;
		ProfilesView = CollectionViewSource.GetDefaultView(Profiles);
		ProfilesView.Filter = item => item is Profile profile && (string.IsNullOrWhiteSpace(ProfileFilter) || profile.Name.Contains(ProfileFilter, StringComparison.OrdinalIgnoreCase) || profile.Addresses.Contains(ProfileFilter, StringComparison.OrdinalIgnoreCase));
		filteredLogs = CollectionViewSource.GetDefaultView(new ObservableCollection<LogEntry>());

		ImportCommand = new(_ => ImportRequested?.Invoke(this, EventArgs.Empty), _ => !IsQuitting);
		NewCommand = new(_ => NewRequested?.Invoke(this, EventArgs.Empty), _ => !IsQuitting);
		EditCommand = new(value => RequestProfileAction(value, EditRequested, false), value => CanModify(CommandProfile(value)));
		ManageForwardsCommand = new(value => RequestProfileAction(value, PortForwardsRequested, false), value => CanModify(CommandProfile(value)));
		ExportCommand = new(value => RequestProfileAction(value, ExportRequested, true), value => CommandProfile(value) != null);
		RemoveCommand = new(value => RequestProfileAction(value, RemoveRequested, false), value => CanModify(CommandProfile(value)));
		ToggleCommand = new(async _ => await ToggleAsync(), _ => !IsQuitting && Selected != null && (!Selected.Busy || (Selected.Running && Selected.State != "Stopping")));
		ConnectCommand = new(async value => {
			var profile = value as Profile ?? Selected;
			if (CanConnect(profile)) {
				await ToggleAsync(profile);
			}
		}, value => CanConnect(value as Profile ?? Selected));

		DisconnectCommand = new(async value => {
			var profile = value as Profile ?? Selected;
			if (CanDisconnect(profile)) {
				await ToggleAsync(profile);
			}
		}, value => CanDisconnect(value as Profile ?? Selected));

		ShowDashboardCommand = new(_ => Show("Dashboard"));
		ShowLogsCommand = new(_ => Show("Logs"));
		ShowSettingsCommand = new(_ => Show("Settings"));

		CopyLogCommand = new(_ => CopyLogs(), _ => Selected?.Logs.Count > 0);
		ClearLogsCommand = new(_ => Selected?.Logs.Clear(), _ => Selected?.Logs.Count > 0);

		Backend.StatusChanged += (_, _) => Refresh();
	}

	private bool FilterLog(object item) => item is LogEntry entry && (string.IsNullOrWhiteSpace(LogFilter) || (entry.Level + " " + entry.Message).Contains(LogFilter, StringComparison.OrdinalIgnoreCase));

	public async Task InitializeAsync() {
		try {
			bool available = await Backend.CheckPrerequisitesAsync();
			EngineStatus = available ? "ENGINE READY" : "ENGINE SETUP REQUIRED";

			foreach (var profile in await Backend.LoadProfilesAsync()) {
				AddProfile(profile);
			}

			Selected ??= Profiles.FirstOrDefault();
			StatusMessage = Profiles.Count > 0 ? "Select a tunnel to inspect or activate it." : "Import a WireSteam configuration to get started.";
		} catch (Exception error) {
			StatusMessage = error.Message;
			EngineStatus = "ENGINE UNAVAILABLE";
		}

		Refresh();
	}

	public void AddProfile(Profile profile) {
		if (Profiles.Any(candidate => candidate.Id == profile.Id)) {
			Selected = Profiles.First(candidate => candidate.Id == profile.Id);
			return;
		}

		profile.PropertyChanged += (_, eventArguments) => {
			if (pendingProfile == profile && eventArguments.PropertyName is nameof(Profile.State) or nameof(Profile.Detail) or nameof(Profile.Running) or nameof(Profile.Busy)) {
				UpdateLifecycleStatus(profile);
			}

			Refresh();
		};
		profile.Logs.CollectionChanged += (_, _) => RefreshCommands();
		Profiles.Add(profile);
		Selected = profile;
		Refresh();
	}

	public void Refresh() {
		Changed(nameof(ActiveCountText));
		Changed(nameof(ProfileCountText));
		RefreshCommands();
		PresentationChanged?.Invoke(this, EventArgs.Empty);
	}

	private void RefreshCommands() {
		ImportCommand?.Refresh();
		NewCommand?.Refresh();
		EditCommand?.Refresh();
		ManageForwardsCommand?.Refresh();
		ExportCommand?.Refresh();
		RemoveCommand?.Refresh();
		ToggleCommand?.Refresh();
		ConnectCommand?.Refresh();
		DisconnectCommand?.Refresh();
		CopyLogCommand?.Refresh();
		ClearLogsCommand?.Refresh();
	}

	public void Show(string destination) {
		page = destination;
		Changed(nameof(IsDashboardVisible));
		Changed(nameof(IsLogsVisible));
		Changed(nameof(IsSettingsVisible));
	}

	public async Task ToggleAsync(Profile? profile = null) {
		profile ??= Selected;
		if (IsQuitting || profile == null || (profile.Busy && (!profile.Running || profile.State == "Stopping"))) {
			return;
		}

		try {
			pendingProfile = profile;
			pendingStop = profile.Running || profile.State == "Cleanup required";
			SetLifecycleStatus((pendingStop ? "STOPPING " : "STARTING ") + profile.Name + "…");

			if (pendingStop) {
				await Backend.StopAsync(profile);
			} else {
				await Backend.StartAsync(profile);
			}

			UpdateLifecycleStatus(profile);
		} catch (Exception error) {
			SetLifecycleStatus("FAILED · " + profile.Name + " · " + error.Message.ToUpperInvariant());
		}

		Refresh();
	}

	private bool CanConnect(Profile? profile) => !IsQuitting && profile is { Running: false, Busy: false } && profile.State != "Cleanup required";
	private bool CanDisconnect(Profile? profile) => !IsQuitting && profile != null && profile.State != "Stopping" && (profile.Running || (!profile.Busy && profile.State == "Cleanup required"));
	private Profile? CommandProfile(object? parameter) => parameter as Profile ?? Selected;
	private bool CanModify(Profile? profile) => !IsQuitting && profile is { Running: false, Busy: false } && profile.State != "Cleanup required";

	private void RequestProfileAction(object? parameter, EventHandler? request, bool allowRunning) {
		var profile = CommandProfile(parameter);
		if (profile == null || (!allowRunning && !CanModify(profile))) {
			return;
		}

		Selected = profile;
		request?.Invoke(this, EventArgs.Empty);
	}

	private void SetLifecycleStatus(string text) {
		StatusMessage = text;
		lifecycleMessage = true;
		Changed(nameof(StatusDisplay));
	}

	private void UpdateLifecycleStatus(Profile profile) {
		if (profile.Busy && !profile.Running && profile.State != "Stopping") {
			return;
		}

		string text = profile.State switch {
			"Starting" => "STARTING " + profile.Name + "…",
			"Stopping" => "STOPPING " + profile.Name + "…",
			"Error" => "FAILED · " + profile.Name + " · " + profile.Detail.ToUpperInvariant(),
			"Cleanup required" => "CLEANUP REQUIRED · " + profile.Name + " · " + profile.Detail.ToUpperInvariant(),
			_ when profile.Running && profile.State == "Active" => "CONNECTED · " + profile.Name,
			_ when profile.Running => "RUNNING · " + profile.Name,
			_ when profile.Detail.Contains("cancel", StringComparison.OrdinalIgnoreCase) => "ACTIVATION CANCELLED · " + profile.Name,
			_ => (pendingStop ? "DISCONNECTED · " : "STOPPED · ") + profile.Name
		};
		SetLifecycleStatus(text);
	}

	private void CopyLogs() {
		if (Selected == null) {
			return;
		}

		try {
			Clipboard.SetText(string.Join(Environment.NewLine, Selected.Logs.Select(entry => entry.Time + " " + entry.Level + " " + entry.Message)));
			StatusMessage = "Redacted diagnostics copied.";
		} catch (Exception error) {
			StatusMessage = "Cannot access clipboard: " + error.Message;
		}
	}

	public void LoadPreview() {
		EngineStatus = "DESIGN PREVIEW · SIMULATED DATA";
		var profile = new Profile { Name = "Home relay", Addresses = "10.77.0.1/32", AppId = 480, VirtualPort = 40, LocalSteamId = "76561198000000001", LocalSteamName = "Relay operator", PublicKey = "PREVIEW PUBLIC KEY — NO PRIVATE KEYS LOADED", Running = true, State = "Active", Detail = "Adapter running · Steam peer ready", ReceiveMbps = 21.84, SendMbps = 8.32 };
		profile.Peers.Add(new Peer { SteamId = "76561198000000002", SteamName = "Client workstation", AllowedIps = "10.77.0.2/32", State = "Ready", Route = "SDR · sto2", PingMs = 34, ReceiveMbps = 12.14, SendMbps = 5.2, RxBytes = 1785151488, TxBytes = 767557632 });
		profile.PortForwards.Add(new PortForwardRule("10.77.0.1:25565", "127.0.0.1:25565"));
		profile.Peers.Add(new Peer { SteamId = "76561198000000003", SteamName = "Client 02", AllowedIps = "10.77.0.3/32", State = "Ready", Route = "SDR · sto", PingMs = 41, ReceiveMbps = 9.7, SendMbps = 3.12, RxBytes = 944892805, TxBytes = 461373440 });

		for (int sampleIndex = 0; sampleIndex < 72; sampleIndex++) {
			profile.Traffic.Add(new TrafficPoint(16 + Math.Sin(sampleIndex * .19) * 5 + Math.Sin(sampleIndex * .61) * 2.3, 5 + Math.Sin(sampleIndex * .14) * 2 + Math.Cos(sampleIndex * .32)));
		}

		profile.Logs.Add(new LogEntry("12:48:02", "INFO", "Design preview: no Steam connection or VPN adapter is active."));
		profile.Logs.Add(new LogEntry("12:48:03", "INFO", "Preview transport · SDR via sto2 · peer authenticated"));
		AddProfile(profile);
		AddProfile(new Profile { Name = "Client workstation", Addresses = "10.77.0.2/32", VirtualPort = 40, Detail = "Ready to activate" });
		AddProfile(new Profile { Name = "Client 02", Addresses = "10.77.0.3/32", VirtualPort = 40, Detail = "Ready to activate" });
		Selected = profile;
		StatusMessage = "DESIGN PREVIEW — all traffic and connection values are simulated.";
		Refresh();
	}
}
