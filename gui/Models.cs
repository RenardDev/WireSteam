using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows.Input;

namespace WireSteam.UI;

public abstract class Observable : INotifyPropertyChanged {
	public event PropertyChangedEventHandler? PropertyChanged;

	protected bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null) {
		if (EqualityComparer<T>.Default.Equals(field, value)) {
			return false;
		}

		field = value;
		Changed(name);

		return true;
	}

	protected void Changed([CallerMemberName] string? name = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

public sealed class RelayCommand : ICommand {
	private readonly Action<object?> execute;
	private readonly Predicate<object?>? canExecute;

	public RelayCommand(Action<object?> execute, Predicate<object?>? canExecute = null) {
		this.execute = execute;
		this.canExecute = canExecute;
	}

	public bool CanExecute(object? parameter) => canExecute?.Invoke(parameter) ?? true;
	public void Execute(object? parameter) => execute(parameter);

	public event EventHandler? CanExecuteChanged;

	public void Refresh() => CanExecuteChanged?.Invoke(this, EventArgs.Empty);
}

public sealed class Profile : Observable {
	public string Id {
		get;
		set;
	} = Guid.NewGuid().ToString("N");
	public string Name {
		get;
		set;
	} = "Untitled tunnel";
	public string ConfigPath {
		get;
		set;
	} = "";
	public string TunnelName => "ui-" + Id[..12];
	public string Addresses {
		get;
		set;
	} = "NO ADDRESS";
	public string PublicKey {
		get;
		set;
	} = "UNAVAILABLE";
	public uint AppId {
		get;
		set;
	} = 480;
	public int VirtualPort {
		get;
		set;
	}

	public int Mtu {
		get;
		set;
	} = 1280;
	public bool RelayOnly {
		get;
		set;
	} = true;
	public string InternetMode {
		get;
		set;
	} = "off";
	public string InternetSubnet {
		get;
		set;
	} = "";
	public string InternetModeLabel => InternetMode switch { "off" => "PRIVATE NETWORK", "client" => "INTERNET CLIENT", "server" => "INTERNET GATEWAY", _ => "UNKNOWN INTERNET MODE" };
	public string InternetSubnetText => string.IsNullOrWhiteSpace(InternetSubnet) ? "—" : InternetSubnet;
	public ObservableCollection<Peer> Peers {
		get;
	} = new();
	public ObservableCollection<PortForwardRule> PortForwards {
		get;
	} = new();
	public string PortForwardSummary {
		get {
			int tcp = 0, udp = 0;
			foreach (var rule in PortForwards) {
				if (rule.Protocol.Equals("UDP", StringComparison.OrdinalIgnoreCase)) {
					++udp;
				} else {
					++tcp;
				}
			}

			static string Count(int count, string protocol) => count.ToString(System.Globalization.CultureInfo.InvariantCulture) + " " + protocol + (count == 1 ? " FORWARD" : " FORWARDS");

			return tcp > 0 && udp > 0 ? Count(tcp, "TCP") + " · " + Count(udp, "UDP") : tcp > 0 ? Count(tcp, "TCP") : udp > 0 ? Count(udp, "UDP") : "NO PORT FORWARDS";
		}
	}

	public BoundedLogCollection Logs {
		get;
	} = new();
	public ObservableCollection<TrafficPoint> Traffic {
		get;
	} = new();

	private string state = "Inactive", detail = "Ready to activate", localSteamId = "—", localSteamName = "";
	private bool running, busy;
	private double receiveMbps, sendMbps;
	private long receiveActivityAt, sendActivityAt;

	public long ReceiveActivityAt {
		get => receiveActivityAt;
		internal set => Set(ref receiveActivityAt, value);
	}

	public long SendActivityAt {
		get => sendActivityAt;
		internal set => Set(ref sendActivityAt, value);
	}

	public string State {
		get => state;
		set {
			if (Set(ref state, value)) {
				Changed(nameof(IsConnected));
				Changed(nameof(StateColor));
				Changed(nameof(ActionText));
			}
		}
	}

	public string Detail {
		get => detail;
		set => Set(ref detail, value);
	}

	public string LocalSteamId {
		get => localSteamId;
		set {
			if (Set(ref localSteamId, value)) {
				Changed(nameof(LocalIdentityText));
			}
		}
	}

	public string LocalSteamName {
		get => localSteamName;
		set {
			if (Set(ref localSteamName, value)) {
				Changed(nameof(LocalIdentityText));
			}
		}
	}

	public string LocalIdentityText => string.IsNullOrEmpty(LocalSteamName) ? LocalSteamId : LocalSteamName + " · " + LocalSteamId;
	public bool Running {
		get => running;
		set {
			if (Set(ref running, value)) {
				Changed(nameof(ActionText));
			}
		}
	}

	public bool Busy {
		get => busy;
		set {
			if (Set(ref busy, value)) {
				Changed(nameof(ActionText));
			}
		}
	}

	public bool IsConnected => State == "Active";
	public string StateColor => State switch { "Active" => "#6FFFC7", "Inactive" => "#718398", "Error" or "Cleanup required" => "#FF768F", _ => "#F8C46D" };
	public string ActionText => State == "Cleanup required" ? "Clean up adapter" : State == "Stopping" ? "Disconnecting…" : Running && State == "Starting" ? "Cancel activation" : Busy ? "Starting…" : Running ? "Deactivate tunnel" : "Activate tunnel";
	public double ReceiveMbps {
		get => receiveMbps;
		set {
			if (Set(ref receiveMbps, value)) {
				Changed(nameof(ReceiveText));
			}
		}
	}

	public double SendMbps {
		get => sendMbps;
		set {
			if (Set(ref sendMbps, value)) {
				Changed(nameof(SendText));
			}
		}
	}

	public string ReceiveText => ReceiveMbps.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture);
	public string SendText => SendMbps.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture);
	public string LatencyText {
		get {
			long sum = 0;
			int count = 0;
			foreach (var peer in Peers) {
				if (peer.PingMs >= 0) {
					sum += peer.PingMs;
					++count;
				}
			}

			return count > 0 ? (sum / count).ToString() : "—";
		}
	}

	public void RefreshPeerSummary() => Changed(nameof(LatencyText));

	public void RefreshMetadata() {
		Changed(nameof(Name));
		Changed(nameof(Addresses));
		Changed(nameof(PublicKey));
		Changed(nameof(AppId));
		Changed(nameof(VirtualPort));
		Changed(nameof(Mtu));
		Changed(nameof(RelayOnly));
		Changed(nameof(InternetMode));
		Changed(nameof(InternetSubnet));
		Changed(nameof(InternetModeLabel));
		Changed(nameof(InternetSubnetText));
		Changed(nameof(PortForwardSummary));
	}
}

public sealed class Peer : Observable {
	public string SteamId {
		get;
		set;
	} = "";
	public string PublicKey {
		get;
		set;
	} = "";
	public string AllowedIps {
		get;
		set;
	} = "";

	private string state = "Waiting", route = "No route yet", steamName = "";
	private int pingMs = -1;
	private ulong txBytes, rxBytes;
	private double receiveMbps, sendMbps;
	private long receiveActivityAt, sendActivityAt;

	public long ReceiveActivityAt {
		get => receiveActivityAt;
		internal set => Set(ref receiveActivityAt, value);
	}

	public long SendActivityAt {
		get => sendActivityAt;
		internal set => Set(ref sendActivityAt, value);
	}

	public string State {
		get => state;
		set => Set(ref state, value);
	}

	public string Route {
		get => route;
		set => Set(ref route, value);
	}

	public int PingMs {
		get => pingMs;
		set {
			if (Set(ref pingMs, value)) {
				Changed(nameof(PingText));
			}
		}
	}

	public string PingText => PingMs >= 0 ? PingMs + " ms" : "—";
	public string SteamName {
		get => steamName;
		set {
			if (Set(ref steamName, value)) {
				Changed(nameof(DisplayName));
			}
		}
	}

	public string DisplayName => string.IsNullOrEmpty(SteamName) ? SteamId : SteamName;
	public ulong TxBytes {
		get => txBytes;
		set {
			if (Set(ref txBytes, value)) {
				Changed(nameof(SendTotalText));
			}
		}
	}

	public ulong RxBytes {
		get => rxBytes;
		set {
			if (Set(ref rxBytes, value)) {
				Changed(nameof(ReceiveTotalText));
			}
		}
	}

	public double ReceiveMbps {
		get => receiveMbps;
		set {
			if (Set(ref receiveMbps, value)) {
				Changed(nameof(ReceiveText));
				Changed(nameof(IsReceiving));
			}
		}
	}

	public double SendMbps {
		get => sendMbps;
		set {
			if (Set(ref sendMbps, value)) {
				Changed(nameof(SendText));
				Changed(nameof(IsSending));
			}
		}
	}

	public bool IsReceiving => ReceiveMbps > 0;
	public bool IsSending => SendMbps > 0;
	public string ReceiveText => ReceiveMbps.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture);
	public string SendText => SendMbps.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture);
	public string ReceiveTotalText => FormatBytes(RxBytes);
	public string SendTotalText => FormatBytes(TxBytes);

	private static string FormatBytes(ulong value) {
		if (value < 1024) {
			return value.ToString(System.Globalization.CultureInfo.InvariantCulture) + " B";
		}

		string[] units = { "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
		double count = value;
		int unit = -1;
		do {
			count /= 1024;
			++unit;
		} while (count >= 1024 && unit < units.Length - 1);

		return count.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture) + " " + units[unit];
	}
}

public sealed record TrafficPoint(double ReceiveMbps, double SendMbps);

public sealed record LogEntry(string Time, string Level, string Message);
