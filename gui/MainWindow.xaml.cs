using System;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Threading;
using Microsoft.Win32;

namespace WireSteam.UI;

public partial class MainWindow : Window {
	public MainViewModel Model {
		get;
	}

	private readonly bool preview;
	private HwndSource? windowSource;
	private bool updatingMonitorMinimum;
	private bool monitorRefreshPending;
	private readonly double preferredMinimumWidth;
	private readonly double preferredMinimumHeight;
	public MainWindow(MainViewModel model, bool preview = false) {
		this.preview = preview;
		Model = model;
		InitializeComponent();
		DataContext = model;
		preferredMinimumWidth = MinWidth;
		preferredMinimumHeight = MinHeight;

		if (!preview) {
			model.ImportRequested += async (_, _) => await ImportAsync();
			model.NewRequested += async (_, _) => await NewAsync();
			model.EditRequested += async (_, _) => await EditAsync();
			model.PortForwardsRequested += async (_, _) => await ManageForwardsAsync();
			model.ExportRequested += async (_, _) => await ExportAsync();
			model.RemoveRequested += async (_, _) => await RemoveAsync();
		}

		Closing += OnClosing;
	}

	protected override void OnSourceInitialized(EventArgs args) {
		base.OnSourceInitialized(args);
		windowSource = HwndSource.FromHwnd(new WindowInteropHelper(this).Handle);
		windowSource?.AddHook(WindowMessage);

		if (windowSource != null && TryGetWorkingBounds(windowSource.Handle, out var bounds, out _)) {
			UpdateMonitorMinimum(bounds, GetDpiForWindow(windowSource.Handle));
		}
	}

	protected override void OnClosed(EventArgs args) {
		windowSource?.RemoveHook(WindowMessage);
		windowSource = null;
		base.OnClosed(args);
	}

	private IntPtr WindowMessage(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled) {
		if (message == 0x0024 && lParam != IntPtr.Zero && TryGetWorkingBounds(hwnd, out var bounds, out var monitor)) { // WM_GETMINMAXINFO
			UpdateMonitorMinimum(bounds, GetDpiForWindow(hwnd));
			var limits = Marshal.PtrToStructure<NativeMinMaxInformation>(lParam);
			limits.MaxPosition = new NativePoint(bounds.Left - monitor.Left, bounds.Top - monitor.Top);
			limits.MaxSize = new NativePoint(bounds.Width, bounds.Height);
			Marshal.StructureToPtr(limits, lParam, false);
			// Let Window process the corrected structure too: its native size cache
			// controls WPF measure, and it still enforces our adaptive minimums.
		} else if (message is 0x001A or 0x007E or 0x02E0) { // settings, display, per-monitor DPI
			QueueMonitorRefresh();
		}

		return IntPtr.Zero;
	}

	private void UpdateMonitorMinimum(PixelRect bounds, uint dpi) {
		if (updatingMonitorMinimum) {
			return;
		}

		var minimum = CalculateMonitorMinimum(bounds, dpi, new Size(preferredMinimumWidth, preferredMinimumHeight));
		try {
			updatingMonitorMinimum = true;

			if (Math.Abs(MinWidth - minimum.Width) > 0.01) {
				SetCurrentValue(MinWidthProperty, minimum.Width);
			}

			if (Math.Abs(MinHeight - minimum.Height) > 0.01) {
				SetCurrentValue(MinHeightProperty, minimum.Height);
			}
		} finally {
			updatingMonitorMinimum = false;
		}
	}

	private void QueueMonitorRefresh() {
		if (monitorRefreshPending) {
			return;
		}

		monitorRefreshPending = true;
		Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() => {
			monitorRefreshPending = false;

			if (windowSource is not { IsDisposed: false } source || !TryGetWorkingBounds(source.Handle, out var bounds, out _)) {
				return;
			}

			UpdateMonitorMinimum(bounds, GetDpiForWindow(source.Handle));

			if (WindowState == WindowState.Maximized) {
				SetWindowPos(source.Handle, IntPtr.Zero, bounds.Left, bounds.Top, bounds.Width, bounds.Height, 0x0014); // NOACTIVATE | NOZORDER
			}
		}));
	}

	internal static Size CalculateMonitorMinimum(PixelRect bounds, uint dpi, Size preferred) {
		double scale = (dpi == 0 ? 96 : dpi) / 96.0;

		return new Size(Math.Min(preferred.Width, bounds.Width / scale), Math.Min(preferred.Height, bounds.Height / scale));
	}

	internal static PixelRect CalculateWorkingBounds(PixelRect monitor, PixelRect work, int autoHideEdges) {
		// Win32 rectangles are already physical pixels in our PerMonitorV2 process.
		// Auto-hide appbars need only their activation strip, not their shown height.
		var bounds = new PixelRect(Math.Max(monitor.Left, work.Left), Math.Max(monitor.Top, work.Top), Math.Min(monitor.Right, work.Right), Math.Min(monitor.Bottom, work.Bottom));
		if (bounds.Width <= 0 || bounds.Height <= 0) {
			bounds = monitor;
		}

		const int activationStrip = 2;
		if ((autoHideEdges & 0x01) != 0 && bounds.Left == monitor.Left) {
			bounds.Left += activationStrip;
		}

		if ((autoHideEdges & 0x02) != 0 && bounds.Top == monitor.Top) {
			bounds.Top += activationStrip;
		}

		if ((autoHideEdges & 0x04) != 0 && bounds.Right == monitor.Right) {
			bounds.Right -= activationStrip;
		}

		if ((autoHideEdges & 0x08) != 0 && bounds.Bottom == monitor.Bottom) {
			bounds.Bottom -= activationStrip;
		}

		return bounds;
	}

	internal static bool TryGetWorkingBounds(IntPtr hwnd, out PixelRect bounds, out PixelRect monitor) {
		var monitorInformation = new NativeMonitorInformation { Size = Marshal.SizeOf<NativeMonitorInformation>() };
		IntPtr handle = MonitorFromWindow(hwnd, 0x02); // MONITOR_DEFAULTTONEAREST
		if (handle == IntPtr.Zero || !GetMonitorInfo(handle, ref monitorInformation)) {
			bounds = monitor = default;
			return false;
		}

		int edges = 0;
		for (uint edge = 0; edge < 4; ++edge) {
			var bar = new NativeAppBarData { Size = (uint)Marshal.SizeOf<NativeAppBarData>(), Edge = edge, Rectangle = monitorInformation.Monitor };
			if (SHAppBarMessage(0x0000000B, ref bar) != UIntPtr.Zero) {
				edges |= 0x01 << (int)edge; // ABM_GETAUTOHIDEBAREX
			}
		}

		monitor = monitorInformation.Monitor;
		bounds = CalculateWorkingBounds(monitorInformation.Monitor, monitorInformation.Work, edges);

		return true;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct PixelRect(int left, int top, int right, int bottom) {
		public int Left = left, Top = top, Right = right, Bottom = bottom;
		public readonly int Width => Right - Left;
		public readonly int Height => Bottom - Top;
	}

	[StructLayout(LayoutKind.Sequential)]
	private struct NativePoint(int x, int y) {
		public int X = x, Y = y;
	}

	[StructLayout(LayoutKind.Sequential)]
	private struct NativeMinMaxInformation {
		public NativePoint Reserved, MaxSize, MaxPosition, MinTrackSize, MaxTrackSize;
	}

	[StructLayout(LayoutKind.Sequential)]
	private struct NativeMonitorInformation {
		public int Size;
		public PixelRect Monitor, Work;
		public uint Flags;
	}

	[StructLayout(LayoutKind.Sequential)]
	private struct NativeAppBarData {
		public uint Size;
		public IntPtr Window;
		public uint Callback, Edge;
		public PixelRect Rectangle;
		public IntPtr Parameter;
	}

	[DllImport("user32.dll")]
	private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);
	[DllImport("user32.dll", EntryPoint = "GetMonitorInfoW")]
	[return: MarshalAs(UnmanagedType.Bool)]
	private static extern bool GetMonitorInfo(IntPtr monitor, ref NativeMonitorInformation monitorInformation);
	[DllImport("user32.dll")]
	private static extern uint GetDpiForWindow(IntPtr hwnd);
	[DllImport("shell32.dll")]
	private static extern UIntPtr SHAppBarMessage(uint message, ref NativeAppBarData data);
	[DllImport("user32.dll")]
	[return: MarshalAs(UnmanagedType.Bool)]
	private static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int width, int height, uint flags);

	private void OnClosing(object? sender, CancelEventArgs eventArguments) {
		if (preview || ((App)Application.Current).AllowClose) {
			return;
		}

		eventArguments.Cancel = true;
		((App)Application.Current).HideToTray();
	}

	private void Window_Close(object sender, RoutedEventArgs eventArguments) => Close();

	private void Window_Minimize(object sender, RoutedEventArgs eventArguments) {
		if (preview) {
			return;
		}

		((App)Application.Current).HideToTray();
	}

	private void Window_Maximize(object sender, RoutedEventArgs eventArguments) => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

	private void Profile_RightClick(object sender, MouseButtonEventArgs eventArguments) {
		if (ItemsControl.ContainerFromElement(ProfilesList, eventArguments.OriginalSource as DependencyObject) is ListBoxItem item && item.DataContext is Profile profile) {
			ProfilesList.SelectedItem = profile;
			item.Focus();
		} else {
			eventArguments.Handled = true;
		}
	}

	private void Profile_MenuOpening(object sender, ContextMenuEventArgs eventArguments) {
		if (Model.Selected is not Profile profile) {
			eventArguments.Handled = true;
			return;
		}

		ProfilesList.ContextMenu = CreateProfileMenu(profile);
	}

	internal ContextMenu CreateProfileMenu(Profile profile) {
		Model.Selected = profile;
		var menu = new ContextMenu { PlacementTarget = ProfilesList };
		foreach (var action in new[] {
			("CONNECT", Model.ConnectCommand),
			("DISCONNECT", Model.DisconnectCommand),
			("EDIT", Model.EditCommand),
			("DELETE", Model.RemoveCommand),
			("EXPORT", Model.ExportCommand)
		}) {
			menu.Items.Add(new MenuItem { Header = action.Item1, Command = action.Item2, CommandParameter = profile });
		}

		return menu;
	}

	public async Task ImportAsync() {
		var dialog = new OpenFileDialog { Filter = "WIRESTEAM CONFIGURATION (*.conf)|*.conf|ALL FILES (*.*)|*.*", Multiselect = true, Title = "IMPORT WIRESTEAM TUNNELS" };
		if (dialog.ShowDialog(this) != true) {
			return;
		}

		foreach (string file in dialog.FileNames) {
			await GuardAsync(async () => Model.AddProfile(await Model.Backend.ImportAsync(file)));
		}
	}

	private async Task NewAsync() {
		await GuardAsync(async () => {
			string template = await Model.Backend.CreateTemplateAsync();
			var dialog = new ConfigDialog(this, "CREATE TUNNEL", "New tunnel", template);
			while (dialog.ShowDialog() == true) {
				try {
					Model.AddProfile(await Model.Backend.CreateAsync(dialog.ProfileName, dialog.Configuration));
					Model.StatusMessage = "Tunnel created.";
					return;
				} catch (Exception error) {
					MessageBox.Show(this, error.Message, "CONFIGURATION NEEDS ATTENTION", MessageBoxButton.OK, MessageBoxImage.Information);
					dialog = new ConfigDialog(this, "CREATE TUNNEL", dialog.ProfileName, dialog.Configuration);
				}
			}
		});
	}

	private async Task EditAsync() {
		var profile = Model.Selected;
		if (profile == null || profile.Running || profile.State == "Cleanup required") {
			return;
		}

		await GuardAsync(async () => {
			var dialog = new ConfigDialog(this, "EDIT PROFILE", profile.Name, await Model.Backend.ReadConfigAsync(profile));
			while (dialog.ShowDialog() == true) {
				string previousName = profile.Name;
				try {
					profile.Name = dialog.ProfileName;
					await Model.Backend.SaveAsync(profile, dialog.Configuration);
					profile.RefreshMetadata();
					Model.ProfilesView.Refresh();
					Model.StatusMessage = "Configuration validated and saved.";
					return;
				} catch (Exception error) {
					profile.Name = previousName;
					profile.RefreshMetadata();
					MessageBox.Show(this, error.Message, "CONFIGURATION NEEDS ATTENTION", MessageBoxButton.OK, MessageBoxImage.Information);
					dialog = new ConfigDialog(this, "EDIT PROFILE", dialog.ProfileName, dialog.Configuration);
				}
			}
		});
	}

	private async Task ManageForwardsAsync() {
		var profile = Model.Selected;
		if (profile == null || profile.Running || profile.Busy || profile.State == "Cleanup required") {
			return;
		}

		await GuardAsync(async () => {
			string content = await Model.Backend.ReadConfigAsync(profile);
			while (true) {
				var dialog = new PortForwardDialog(this, profile.Name, content);
				if (dialog.ShowDialog() != true) {
					return;
				}

				content = dialog.Configuration;
				try {
					await Model.Backend.SaveAsync(profile, content);
					Model.StatusMessage = "Port forwards validated and saved. Activate this tunnel to make them available.";
					return;
				} catch (Exception error) {
					MessageBox.Show(this, error.Message, "PORT FORWARDING NEEDS ATTENTION", MessageBoxButton.OK, MessageBoxImage.Information);
				}
			}
		});
	}

	private async Task ExportAsync() {
		var profile = Model.Selected;
		if (profile == null) {
			return;
		}

		string fileName = new(profile.Name.Select(character => Array.IndexOf(Path.GetInvalidFileNameChars(), character) >= 0 ? '_' : character).ToArray());
		var dialog = new SaveFileDialog { Filter = "WIRESTEAM CONFIGURATION (*.conf)|*.conf", FileName = fileName + ".conf", Title = "EXPORT PRIVATE CONFIGURATION", OverwritePrompt = true };
		if (dialog.ShowDialog(this) != true) {
			return;
		}

		await GuardAsync(async () => {
			await Model.Backend.ExportAsync(profile, dialog.FileName, File.Exists(dialog.FileName));
			Model.StatusMessage = "Private configuration exported.";
		});
	}

	private async Task RemoveAsync() {
		var profile = Model.Selected;
		if (profile == null || profile.Running || profile.State == "Cleanup required") {
			return;
		}

		if (MessageBox.Show(this, "REMOVE “" + profile.Name + "” FROM THIS APP? THE ORIGINALLY IMPORTED FILE IS KEPT.", "REMOVE TUNNEL", MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No) != MessageBoxResult.Yes) {
			return;
		}

		await GuardAsync(async () => {
			await Model.Backend.RemoveAsync(profile);
			Model.Profiles.Remove(profile);
			Model.Selected = Model.Profiles.Count > 0 ? Model.Profiles[0] : null;
			Model.Refresh();
		});
	}

	private async Task GuardAsync(Func<Task> action) {
		try {
			await action();
		} catch (Exception error) {
			Model.StatusMessage = error.Message;
			MessageBox.Show(this, error.Message, "WIRESTEAM", MessageBoxButton.OK, MessageBoxImage.Information);
		}
	}
}
