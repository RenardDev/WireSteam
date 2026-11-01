using System;
using System.Collections.Specialized;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using WireSteam.UI.Controls;
using WireSteam.UI.Services;

namespace WireSteam.UI;

internal static class UiTests {
	internal static async Task RunAsync() {
		TestLatencyAverage();

		using var backend = new PreviewBackend();
		var model = new MainViewModel(backend);
		await model.InitializeAsync();
		Require(model.Profiles.Count == 0 && model.Selected == null, "An empty catalog must stay empty.");
		Require(!model.ToggleCommand.CanExecute(null), "Activation without a profile must be disabled.");

		model.LoadPreview();
		Require(model.EngineStatus.Contains("SIMULATED"), "Preview data must be labeled.");
		Require(model.Profiles.Count == 3 && model.Selected?.Peers.Count == 2, "Preview profiles did not load.");

		var profile = model.Selected!;
		model.ProfileFilter = "does-not-exist";
		Require(model.ProfilesView.IsEmpty, "Tunnel search did not filter.");

		model.ProfileFilter = "";
		Require(!model.ProfilesView.IsEmpty, "Tunnel search did not reset.");

		model.Show("Logs");
		Require(model.IsLogsVisible && !model.IsDashboardVisible, "Log navigation failed.");

		model.Show("Settings");
		Require(model.IsSettingsVisible && !model.IsLogsVisible, "Settings navigation failed.");

		profile.State = "Starting";
		profile.Running = true;
		profile.Busy = true;
		Require(model.ToggleCommand.CanExecute(null) && profile.ActionText == "Cancel activation", "Starting tunnels must be cancellable.");

		profile.State = "Stopping";
		Require(!model.ToggleCommand.CanExecute(null), "A pending disconnect must not be started twice.");

		profile.State = "Cleanup required";
		profile.Running = false;
		profile.Busy = false;
		Require(profile.ActionText == "Clean up adapter", "Cleanup action is missing.");
		Require(!model.EditCommand.CanExecute(null) && !model.RemoveCommand.CanExecute(null), "A profile requiring cleanup must retain its recovery reference.");
		Require(!model.ManageForwardsCommand.CanExecute(null), "A profile requiring cleanup must not expose editable port forwards.");

		model.IsQuitting = true;
		Require(!model.ToggleCommand.CanExecute(null), "Activation must be disabled during shutdown.");

		model.IsQuitting = false;
		TrayController.RunTests(model);
		TechIcon.RunTests();
		TrafficPulse.RunTests();
		TestTrafficChartTypography();
		TestMonitorWorkingBounds();
		TestCompactSidebar(model);
		TestForwardActionClipping(model);
		await TestMaximizedWorkingAreaAsync(model);
		await TestLifecycleFooterAsync();
		await TestLogFollowingAsync(model);
		Console.WriteLine("PASS UI logs: continuous multi-line selection/COPY, preserved selection on append/trim, exact diagnostic case, both auto-scroll checkboxes, filtered/profile-swapped logs, disable/read/resume and clear.");

		foreach (string page in new[] { "Dashboard", "Logs", "Settings" }) {
			model.Show(page);
			var window = new MainWindow(model, true);
			try {
				var content = (FrameworkElement)window.Content;
				content.Measure(new Size(1280, 820));
				content.Arrange(new Rect(0, 0, 1280, 820));
				content.UpdateLayout();
				Require(content.ActualWidth > 0, page + " did not lay out.");
			} finally {
				window.Close();
			}
		}
	}

	private static void TestLatencyAverage() {
		var profile = new Profile();
		Require(profile.LatencyText == "—", "An empty peer set must have unknown latency.");

		profile.Peers.Add(new Peer { PingMs = -1 });
		Require(profile.LatencyText == "—", "Unknown pings must not enter the latency average.");

		var first = new Peer { PingMs = int.MaxValue };
		var second = new Peer { PingMs = int.MaxValue };
		profile.Peers.Add(first);
		profile.Peers.Add(second);
		Require(profile.LatencyText == int.MaxValue.ToString(), "Two maximum pings must average without overflowing the accumulator.");

		first.PingMs = 34;
		second.PingMs = 41;
		Require(profile.LatencyText == "37", "Known pings must retain integer averaging while unknown pings remain excluded.");

		first.PingMs = second.PingMs = 0;
		Require(profile.LatencyText == "0", "Zero latency is a known ping and must remain in the average.");
	}

	private static void TestMonitorWorkingBounds() {
		foreach (var origin in new[] { new Point(0, 0), new Point(-1920, 0), new Point(0, -1080), new Point(2560, 1440) }) {
			int x = (int)origin.X, y = (int)origin.Y;
			var monitor = new MainWindow.PixelRect(x, y, x + 1920, y + 1080);
			foreach (var work in new[] {
				new MainWindow.PixelRect(x, y, x + 1920, y + 1032),
				new MainWindow.PixelRect(x + 64, y, x + 1920, y + 1080),
				new MainWindow.PixelRect(x, y + 40, x + 1920, y + 1080),
				new MainWindow.PixelRect(x, y, x + 1860, y + 1080)
			}) {
				var bounds = MainWindow.CalculateWorkingBounds(monitor, work, 0);
				Require(bounds.Left == work.Left && bounds.Top == work.Top && bounds.Width == work.Width && bounds.Height == work.Height,
					"Maximize bounds must use the current monitor's physical work rectangle, including negative monitor origins and side taskbars.");
			}

			for (int edge = 0; edge < 4; ++edge) {
				var bounds = MainWindow.CalculateWorkingBounds(monitor, monitor, 0x01 << edge);
				Require(bounds.Left == x + (edge == 0 ? 2 : 0) && bounds.Top == y + (edge == 1 ? 2 : 0) &&
					bounds.Right == x + 1920 - (edge == 2 ? 2 : 0) && bounds.Bottom == y + 1080 - (edge == 3 ? 2 : 0),
					"An auto-hide appbar must retain only its activation edge on the correct monitor.");
				var alreadyInset = MainWindow.CalculateWorkingBounds(monitor, bounds, 0x01 << edge);
				Require(alreadyInset.Left == bounds.Left && alreadyInset.Top == bounds.Top && alreadyInset.Right == bounds.Right && alreadyInset.Bottom == bounds.Bottom,
					"An already reserved auto-hide activation edge must not be subtracted twice.");
			}
		}

		foreach (uint dpi in new uint[] { 96, 120, 144, 192 }) {
			var work = new MainWindow.PixelRect(1920, 0, 3286, 720);
			var preferred = new Size(1000, 680);
			var minimum = MainWindow.CalculateMonitorMinimum(work, dpi, preferred);
			Require(minimum.Width <= work.Width * 96.0 / dpi && minimum.Height <= work.Height * 96.0 / dpi,
				"The WPF minimum size must fit the monitor work area at " + dpi + " DPI.");
			minimum = MainWindow.CalculateMonitorMinimum(new MainWindow.PixelRect(0, 0, 3840, 2160), dpi, preferred);
			Require(minimum == preferred, "Moving to a larger monitor must restore the preferred minimum size.");
		}

		Console.WriteLine("PASS monitor bounds: physical work areas, negative/secondary monitor origins, four taskbar/auto-hide edges and adaptive 100/125/150/200% minimums.");
	}

	private static void TestCompactSidebar(MainViewModel model) {
		foreach (var size in new[] { new Size(1280, 820), new Size(1093, 574), new Size(960, 480), new Size(800, 400) }) {
			var window = new MainWindow(model, true);
			try {
				var content = (FrameworkElement)window.Content;
				content.Measure(size);
				content.Arrange(new Rect(new Point(), size));
				content.UpdateLayout();
				var footer = (FrameworkElement)window.FindName("RailStatus");
				var bounds = footer.TransformToAncestor(content).TransformBounds(new Rect(footer.RenderSize));
				Require(bounds.Top >= 48 && bounds.Bottom <= size.Height - 15,
					"The active-tunnel status must remain inside the sidebar work area at " + size + "; footer=" + bounds + ".");
				var rail = (ScrollViewer)window.FindName("RailScroll");
				Require(size.Height >= 574 ? rail.ScrollableHeight < 0.1 : rail.ScrollableHeight > 0,
					"The upper rail must preserve its normal layout and scroll when the work area is short.");
				foreach (var button in VisualDescendants(rail).OfType<Button>().Where(button => button.Command == model.ImportCommand || button.Command == model.NewCommand)) {
					var label = VisualDescendants(button).OfType<TextBlock>().Single();
					var labelBounds = label.TransformToAncestor(button).TransformBounds(new Rect(label.RenderSize));
					Require(labelBounds.Left >= 0 && labelBounds.Right <= button.ActualWidth,
						"Sidebar actions must stay inside their buttons when compact-layout scrolling appears.");
					var ink = VisualTreeHelper.GetDrawing(label)?.Bounds ?? Rect.Empty;
					Require(!ink.IsEmpty, "Sidebar action text must render visible glyphs.");
					// An oversized StackPanel can fit inside the button's outer bounds
					// while its own layout clip silently removes the final glyphs.
					for (DependencyObject? ancestor = label; ancestor != rail; ancestor = VisualTreeHelper.GetParent(ancestor)) {
						Require(ancestor is Visual, "Sidebar action text must belong to its rail viewport.");

						var visual = (Visual)ancestor;
						if (VisualTreeHelper.GetClip(visual) is Geometry clip) {
							var clippedInk = label.TransformToAncestor(visual).TransformBounds(ink);
							Require(clippedInk.Left >= clip.Bounds.Left - 0.5 && clippedInk.Right <= clip.Bounds.Right + 0.5,
								label.Text + " glyphs must fit every ancestor layout clip at " + size + "; ancestor=" + visual.GetType().Name + ".");
						}
					}

					var viewport = (ScrollContentPresenter)rail.Template.FindName("PART_ScrollContentPresenter", rail);
					var viewportInk = label.TransformToAncestor(viewport).TransformBounds(ink);
					Require(viewportInk.Left >= 2 && viewportInk.Right <= viewport.ActualWidth - 2,
						"Sidebar action glyphs must retain a clear gap from the real scrollbar viewport edge.");
				}
			} finally {
				window.Close();
			}
		}
	}

	private static void TestForwardActionClipping(MainViewModel model) {
		string previousPage = model.IsLogsVisible ? "Logs" : model.IsSettingsVisible ? "Settings" : "Dashboard";
		model.Show("Dashboard");
		try {
			foreach (double dpi in new[] { 96.0, 120.0, 144.0 }) {
				foreach (var size in new[] { new Size(1280, 820), new Size(960, 480), new Size(800, 400) }) {
					var window = new MainWindow(model, true);
					try {
						var content = (FrameworkElement)window.Content;
						content.Measure(size);
						content.Arrange(new Rect(new Point(), size));
						content.UpdateLayout();
						var action = (Button)window.FindName("DashboardForwardAction");
						var label = VisualDescendants(action).OfType<TextBlock>().Single();
						var ink = VisibleInk(label, dpi);
						Require(!ink.IsEmpty && label.Text == "MANAGE FORWARDS", "Dashboard forwarding action must render its complete label at " + dpi + " DPI.");

						var buttonInk = label.TransformToAncestor(action).TransformBounds(ink);
						Require(buttonInk.Left >= 0 && buttonInk.Right <= action.ActualWidth + .5 && buttonInk.Top >= 0 && buttonInk.Bottom <= action.ActualHeight + .5, "Forwarding action glyphs must fit their actual button bounds at " + size + "/" + dpi + " DPI.");

						var viewport = Ancestors(action).OfType<ScrollViewer>().First();
						for (DependencyObject? ancestor = label; ancestor != viewport; ancestor = VisualTreeHelper.GetParent(ancestor)) {
							Require(ancestor is Visual, "Forwarding action must remain within its dashboard viewport.");

							var visual = (Visual)ancestor;
							if (VisualTreeHelper.GetClip(visual) is Geometry clip) {
								var bounds = label.TransformToAncestor(visual).TransformBounds(ink);
								Require(bounds.Left >= clip.Bounds.Left - .5 && bounds.Right <= clip.Bounds.Right + .5 && bounds.Top >= clip.Bounds.Top - .5 && bounds.Bottom <= clip.Bounds.Bottom + .5, "Forwarding glyphs must fit every ancestor clip at " + size + "/" + dpi + " DPI; ancestor=" + visual.GetType().Name + ".");
							}
						}

						var presenter = (ScrollContentPresenter)viewport.Template.FindName("PART_ScrollContentPresenter", viewport);
						var viewportInk = label.TransformToAncestor(presenter).TransformBounds(ink);
						Require(viewportInk.Left >= 2 && viewportInk.Right <= presenter.ActualWidth - 2 && viewportInk.Top >= 0 && viewportInk.Bottom <= presenter.ActualHeight + .5, "Dashboard forwarding action must fit its actual scrolling viewport at " + size + "/" + dpi + " DPI.");
					} finally {
						window.Close();
					}
				}
			}
		} finally {
			model.Show(previousPage);
		}

		Console.WriteLine("PASS forwarding action clipping: real button glyphs, all ancestor clips and compact dashboard viewports at 100/125/150% rendering.");
	}

	private static System.Collections.Generic.IEnumerable<DependencyObject> Ancestors(DependencyObject element) {
		for (var parent = VisualTreeHelper.GetParent(element); parent != null; parent = VisualTreeHelper.GetParent(parent)) {
			yield return parent;
		}
	}

	private static async Task TestMaximizedWorkingAreaAsync(MainViewModel model) {
		var window = new MainWindow(model, true) { Opacity = 0, ShowActivated = false, ShowInTaskbar = false };
		try {
			var helper = new WindowInteropHelper(window);
			IntPtr hwnd = helper.EnsureHandle();
			// The fixture owns this invisible HWND. Keep even its maximize operation
			// from activating it or changing focus in another application.
			SetWindowLongPtr(hwnd, -20, new IntPtr(GetWindowLongPtr(hwnd, -20).ToInt64() | 0x08000000));
			Require((GetWindowLongPtr(hwnd, -20).ToInt64() & 0x08000000) != 0, "The owned maximize fixture must reject activation before showing its HWND.");
			window.Show();
			await SettleAsync(window);
			var restore = window.RestoreBounds;

			window.WindowState = WindowState.Maximized;
			await SettleAsync(window);
			Require(MainWindow.TryGetWorkingBounds(hwnd, out var work, out _), "The test window's current monitor must be available.");

			var dpi = VisualTreeHelper.GetDpi(window);
			Require(window.MinWidth <= work.Width / dpi.DpiScaleX && window.MinHeight <= work.Height / dpi.DpiScaleY,
				"The actual window's WPF minimums must fit the current monitor's DPI-scaled work area.");
			SendMessage(hwnd, 0x001A, IntPtr.Zero, IntPtr.Zero);
			SendMessage(hwnd, 0x001A, IntPtr.Zero, IntPtr.Zero);
			await SettleAsync(window);
			Require(GetWindowRect(hwnd, out var actual) && Math.Abs(actual.Left - work.Left) <= 1 && Math.Abs(actual.Top - work.Top) <= 1 &&
				Math.Abs(actual.Right - work.Right) <= 1 && Math.Abs(actual.Bottom - work.Bottom) <= 1,
				"The maximized borderless HWND must fit its current monitor work area without an oversized hidden frame.");
			var footer = (FrameworkElement)window.FindName("RailStatus");
			var content = (FrameworkElement)window.Content;
			var footerBounds = footer.TransformToAncestor(content).TransformBounds(new Rect(footer.RenderSize));
			Require(footerBounds.Bottom <= content.ActualHeight - 15, "The maximized window's sidebar footer must stay above the taskbar.");

			window.WindowState = WindowState.Normal;
			await SettleAsync(window);
			var restored = window.RestoreBounds;
			Require(Math.Abs(restore.X - restored.X) <= 1 && Math.Abs(restore.Y - restored.Y) <= 1 &&
				Math.Abs(restore.Width - restored.Width) <= 1 && Math.Abs(restore.Height - restored.Height) <= 1,
				"Maximize and restore must preserve the original window bounds.");
		} finally {
			window.Close();
		}

		Console.WriteLine("PASS maximize/restore: owned non-activating HWND fits the real monitor work area and keeps its sidebar footer visible.");
	}

	[DllImport("user32.dll")]
	[return: MarshalAs(UnmanagedType.Bool)]
	private static extern bool GetWindowRect(IntPtr hwnd, out MainWindow.PixelRect rectangle);
	[DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
	private static extern IntPtr GetWindowLongPtr(IntPtr hwnd, int index);
	[DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
	private static extern IntPtr SetWindowLongPtr(IntPtr hwnd, int index, IntPtr value);
	[DllImport("user32.dll", EntryPoint = "SendMessageW")]
	private static extern IntPtr SendMessage(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam);

	private static ScrollViewer? FindViewer(DependencyObject root) {
		for (int childIndex = 0; childIndex < VisualTreeHelper.GetChildrenCount(root); ++childIndex) {
			var child = VisualTreeHelper.GetChild(root, childIndex);
			if (child is ScrollViewer viewer) {
				return viewer;
			}

			var descendant = FindViewer(child);
			if (descendant != null) {
				return descendant;
			}
		}

		return null;
	}

	private static async Task TestLogFollowingAsync(MainViewModel model) {
		Require(model.AutoScrollLogs, "Log following must be enabled by default.");

		var original = model.Selected!;
		original.Logs.Clear();
		for (int entryIndex = 0; entryIndex < 140; ++entryIndex) {
			original.Logs.Add(new LogEntry("12:00:00", "INFO", (entryIndex % 2 == 0 ? "match " : "other ") + entryIndex));
		}

		model.Show("Logs");
		var window = new MainWindow(model, true) { Opacity = 0, ShowActivated = false, ShowInTaskbar = false };
		try {
			window.Show();
			await SettleAsync(window);
			var generatedLogo = ((Image)window.FindName("BrandLogo")).Source as System.Windows.Media.Imaging.BitmapSource;
			Require(generatedLogo != null && generatedLogo.PixelWidth >= 512 && generatedLogo.PixelHeight >= 512,
				"The generated brand artwork must be packaged and decoded at its original resolution.");
			var logoPixels = new System.Windows.Media.Imaging.FormatConvertedBitmap(generatedLogo, PixelFormats.Bgra32, null, 0);
			var corner = new byte[0x04];
			logoPixels.CopyPixels(new Int32Rect(0, 0, 1, 1), corner, 0x04, 0x00);
			Require(corner[0x03] == 0, "The generated icon must retain its transparent background.");

			await TestActionLabelsAsync(window, original);
			await TestStatusTypographyAsync(window, model, original);
			await TestInternetModeLabelsAsync(window, model, original);
			TestConfigurationDialogChrome(window);
			TestForwardDialogChrome(window, model);
			TestProfileMenu(window, model);
			TestMenuChrome(window, model);
			await TestLivePeerBindingsAsync(window, model, original);
			foreach (string label in new[] { "EDIT PROFILE", "EXPORT", "MANAGE FORWARDS", "COPY LOG", "CLEAR" }) {
				Require(HasButtonLabel(window, label), "The actual product action must use uppercase: " + label + ".");
			}

			Require(window.FindName("PublicKeyValue") == null && window.FindName("SettingsDetail") == null,
				"Settings must not retain the redundant profile-identity card.");
			Require(!Descendants(window).OfType<TextBlock>().Any(text => text.Text is "PROFILE IDENTITY" or "SETTINGS & IDENTITY"),
				"The removed identity section must not remain in Settings chrome.");
			AssertStaticChromeUppercase(window);
			var eventLog = (LogViewer)window.FindName("EventLog");
			var eventStream = (LogViewer)window.FindName("EventStream");
			var logToggle = (CheckBox)window.FindName("LogAutoScroll");
			var streamToggle = (CheckBox)window.FindName("StreamAutoScroll");
			Require(logToggle.Content is "AUTO-SCROLL" && streamToggle.Content is "AUTO-SCROLL", "Both auto-scroll controls must use uppercase.");

			var logScroll = FindViewer(eventLog)!;
			Require(logScroll != null && logScroll.ScrollableHeight > 0, "The event log must have a real scrollable viewport.");
			Require(AtBottom(logScroll!), "The event log did not initially follow its newest message.");
			Require(eventLog.Selection.IsEmpty, "Auto-scroll must not select or change log messages.");

			logToggle.SetCurrentValue(ToggleButton.IsCheckedProperty, false);
			await SettleAsync(window);
			Require(!model.AutoScrollLogs && streamToggle.IsChecked == false && !eventLog.FollowTail && !eventStream.FollowTail, "The event-log checkbox did not disable both followers.");

			logScroll!.ScrollToTop();
			await SettleAsync(window);
			original.Logs.Add(new LogEntry("12:00:01", "INFO", "Disabled following must preserve the reading position."));
			await SettleAsync(window);
			Require(logScroll.VerticalOffset < 0.1, "A disabled follower moved away from the earlier messages.");

			logToggle.SetCurrentValue(ToggleButton.IsCheckedProperty, true);
			await SettleAsync(window);
			Require(AtBottom(logScroll), "Re-enabling auto-scroll did not return to the newest message.");

			original.Logs.Add(new LogEntry("12:00:02", "INFO", "match new message"));
			await SettleAsync(window);
			Require(AtBottom(logScroll), "An appended message was not followed.");

			await TestSelectableLogAsync(window, eventLog, original);
			model.LogFilter = "match";
			await SettleAsync(window);
			Require(eventLog.EntryCount > 50 && AtBottom(logScroll), "The filtered view did not follow its newest visible message.");

			original.Logs.Add(new LogEntry("12:00:03", "INFO", "match filtered append"));
			await SettleAsync(window);
			Require(AtBottom(logScroll), "Appending to a filtered view did not stay at the bottom.");

			var next = new Profile { Name = "CaseSensitive Profile" };
			for (int entryIndex = 0; entryIndex < 100; ++entryIndex) {
				next.Logs.Add(new LogEntry("12:01:00", "INFO", "match replacement " + entryIndex));
			}

			model.AddProfile(next);
			await SettleAsync(window);
			Require(eventLog.EntryCount == 100 && AtBottom(logScroll), "Switching profiles did not follow the replacement log collection.");

			model.LogFilter = "";
			model.Show("Dashboard");
			await SettleAsync(window);
			var streamScroll = FindViewer(eventStream)!;
			Require(streamScroll != null && streamScroll.ScrollableHeight > 0 && AtBottom(streamScroll), "The event stream did not follow when its page became visible.");

			streamToggle.SetCurrentValue(ToggleButton.IsCheckedProperty, false);
			await SettleAsync(window);
			Require(!model.AutoScrollLogs && logToggle.IsChecked == false, "The event-stream checkbox did not update the shared setting.");

			streamScroll!.ScrollToTop();
			await SettleAsync(window);
			const string exactMessage = @"MixedCase C:\\WireSteam\\host.conf PublicKey=aBcD= SteamID=76561198000000001";
			next.Logs.Add(new LogEntry("12:01:01", "INFO", exactMessage));
			await SettleAsync(window);
			Require(streamScroll.VerticalOffset < 0.1, "Disabled event-stream following changed the reading position.");
			Require(next.Logs[next.Logs.Count - 1].Message == exactMessage && next.Name == "CaseSensitive Profile", "UI typography changed user data casing.");

			streamToggle.SetCurrentValue(ToggleButton.IsCheckedProperty, true);
			await SettleAsync(window);
			Require(AtBottom(streamScroll), "The event stream did not resume following.");
			Require(eventStream.PlainText.Contains(exactMessage, StringComparison.Ordinal), "Rendered diagnostic messages must preserve mixed-case paths and keys.");

			next.Logs.Clear();
			await SettleAsync(window);
			next.Logs.Add(new LogEntry("12:01:02", "INFO", "After clearing the log."));
			await SettleAsync(window);
			Require(eventStream.EntryCount == 1 && AtBottom(streamScroll), "Clearing and resuming the stream failed.");

			await TestAutomaticLogRetentionAsync(window, model, eventStream, eventLog);
			model.Selected = original;
			model.Profiles.Remove(next);
			model.Refresh();
		} finally {
			window.Close();
			model.LogFilter = "";
			model.Selected = original;
		}
	}

	private static async Task TestAutomaticLogRetentionAsync(MainWindow window, MainViewModel model, LogViewer stream, LogViewer log) {
		var previous = model.Selected;
		var bytes = new Profile { Name = "Byte retention fixture" };
		var count = new Profile { Name = "Count retention fixture" };
		model.LogFilter = "";
		model.AutoScrollLogs = true;
		model.Show("Dashboard");
		try {
			model.AddProfile(bytes);
			bytes.Logs.Add(new LogEntry("12:02:00", "INFO", "Old byte-limit selected text."));
			bytes.Logs.Add(new LogEntry("12:02:01", "INFO", "Second old selected line."));
			await SettleAsync(window);
			stream.SelectAll();
			log.SelectAll();
			Require(!stream.Selection.IsEmpty && !log.Selection.IsEmpty, "Both viewers must retain their own selection before a log reset.");

			var streamDocument = stream.Document;
			var logDocument = log.Document;
			var current = OverflowLogText(bytes, "byte-limit incoming");
			await SettleAsync(window);
			AssertResetViewers(stream, log, streamDocument, logDocument, current, 2);
			Require(AtBottom(FindViewer(stream)!), "Automatic clearing must keep the event stream following its current incoming message.");

			// A filtered view must forget the old document too, even when its
			// cleanup notice is excluded by the user's existing filter.
			model.LogFilter = "filter-match";
			model.Show("Logs");
			bytes.Logs.Clear();
			bytes.Logs.Add(new LogEntry("12:03:00", "INFO", "filter-match old selected text"));
			await SettleAsync(window);
			log.SelectAll();
			streamDocument = stream.Document;
			logDocument = log.Document;
			current = OverflowLogText(bytes, "filter-match incoming");
			await SettleAsync(window);
			AssertResetViewers(stream, log, streamDocument, logDocument, current, 1);
			Require(AtBottom(FindViewer(log)!), "The filtered event log must resume following after its text-budget reset.");

			// Both pages stay loaded while another page or the tray hides them.
			// The reset must release their old text without waiting for a reopen.
			model.Show("Settings");
			await SettleAsync(window);
			Require(!stream.IsVisible && !log.IsVisible, "The hidden-viewer reset fixture must hide both log pages.");

			streamDocument = stream.Document;
			logDocument = log.Document;
			current = OverflowLogText(bytes, "filter-match hidden incoming");
			await SettleAsync(window);
			AssertResetViewers(stream, log, streamDocument, logDocument, current, 1);
			window.Hide();
			current = OverflowLogText(bytes, "filter-match tray incoming");
			await SettleAsync(window);
			window.Show();
			model.Show("Logs");
			await SettleAsync(window);
			Require(log.EntryCount == 1 && log.PlainText.Contains(current.Message, StringComparison.Ordinal),
				"A hidden and reopened window must show only the retained current filtered log.");
			model.LogFilter = "";
			model.Show("Dashboard");
			await SettleAsync(window);
			Require(stream.EntryCount == 2 && stream.PlainText.Contains(current.Message, StringComparison.Ordinal),
				"The reopened event stream must share the same bounded profile log.");

			// Prefill while unbound, then bind and overflow synchronously before
			// layout. This covers 2000 entries without 2000 rendered UI updates.
			for (int entryIndex = 0; entryIndex < BoundedLogCollection.MaximumEntries; ++entryIndex) {
				count.Logs.Add(new LogEntry("12:04:00", "INFO", "old count-limit text " + entryIndex));
			}

			model.AddProfile(count);
			stream.SelectAll();
			log.SelectAll();
			streamDocument = stream.Document;
			logDocument = log.Document;
			count.Logs.Add(new LogEntry("12:04:01", "INFO", "Count-limit incoming message must survive."));
			current = count.Logs[^1];
			await SettleAsync(window);
			AssertResetViewers(stream, log, streamDocument, logDocument, current, 2);
			Require(bytes.Logs.Count == 2 && bytes.Logs[^1].Message.Contains("tray incoming", StringComparison.Ordinal),
				"Automatic clearing must affect only the profile that reached its own limit.");

			streamDocument = stream.Document;
			logDocument = log.Document;
			model.ClearLogsCommand.Execute(null);
			await SettleAsync(window);
			Require(stream.EntryCount == 0 && log.EntryCount == 0 && !ReferenceEquals(stream.Document, streamDocument) && !ReferenceEquals(log.Document, logDocument),
				"Manual clear must release both documents and all rendered entry references.");
			count.Logs.Add(new LogEntry("12:04:02", "INFO", "After manual clear, current messages remain selectable."));
			await SettleAsync(window);
			Require(stream.EntryCount == 1 && log.EntryCount == 1 && stream.PlainText.Contains(count.Logs[0].Message, StringComparison.Ordinal),
				"Both viewers must resume after a manual clear.");
			stream.SelectAll();
			Require(ApplicationCommands.Copy.CanExecute(null, stream), "Reset documents must still support native selection and COPY.");

			streamDocument = stream.Document;
			logDocument = log.Document;
			model.Selected = bytes;
			await SettleAsync(window);
			Require(!ReferenceEquals(stream.Document, streamDocument) && !ReferenceEquals(log.Document, logDocument) &&
				!stream.PlainText.Contains("After manual clear", StringComparison.Ordinal),
				"Switching profiles must release the previous profile's document and selection references.");
		} finally {
			model.Selected = previous;
			model.LogFilter = "";
			model.Show("Dashboard");
			model.Profiles.Remove(bytes);
			model.Profiles.Remove(count);
			model.Refresh();
		}

		Console.WriteLine("PASS automatic log retention: shared count/text-budget resets, retained incoming message and cleanup notice, fresh documents/selections, filtered/hidden/tray-reopened viewers, manual clear and profile swaps.");
	}

	private static LogEntry OverflowLogText(Profile profile, string marker) {
		if (profile.Logs.Count == 0) {
			profile.Logs.Add(new LogEntry("12:05:00", "INFO", "Old text-budget anchor."));
		}

		int attempts = BoundedLogCollection.MaximumTextBytes / (BoundedLogCollection.MaximumMessageCharacters * 2) + 2;
		bool reset = false;
		NotifyCollectionChangedEventHandler observeReset = (_, eventArguments) => reset |= eventArguments.Action == NotifyCollectionChangedAction.Reset;
		profile.Logs.CollectionChanged += observeReset;
		try {
			for (int attemptIndex = 0; attemptIndex < attempts; ++attemptIndex) {
				string prefix = marker + " " + attemptIndex + " ";
				profile.Logs.Add(new LogEntry("12:05:00", "INFO", prefix + new string('x', BoundedLogCollection.MaximumMessageCharacters - prefix.Length)));

				if (reset) {
					Require(profile.Logs.Count == 2 && profile.Logs[0].Level == "WARN" && profile.Logs[^1].Message.StartsWith(marker, StringComparison.Ordinal),
						"A text-budget reset must retain one cleanup notice and the incoming message.");
					return profile.Logs[^1];
				}
			}

			throw new InvalidOperationException("The bounded log did not reset when its text budget was exhausted.");
		} finally {
			profile.Logs.CollectionChanged -= observeReset;
		}
	}

	private static void AssertResetViewers(LogViewer stream, LogViewer log, FlowDocument oldStream, FlowDocument oldLog, LogEntry current, int filteredCount) {
		Require(!ReferenceEquals(stream.Document, oldStream) && !ReferenceEquals(log.Document, oldLog),
			"A full reset must replace both WPF documents, including a hidden or filtered viewer.");
		Require(stream.EntryCount == 2 && log.EntryCount == filteredCount && stream.PlainText.Contains(current.Message, StringComparison.Ordinal) && log.PlainText.Contains(current.Message, StringComparison.Ordinal),
			"Both viewers must render only the bounded log's retained incoming message and applicable cleanup notice.");
		Require(stream.Selection.IsEmpty && log.Selection.IsEmpty && !stream.IsUndoEnabled && !log.IsUndoEnabled && !stream.CanUndo && !log.CanUndo,
			"A full log reset must discard obsolete selection/undo references.");
	}

	private static async Task TestSelectableLogAsync(Window window, LogViewer viewer, Profile profile) {
		var paragraphs = viewer.Document.Blocks.OfType<Paragraph>().ToArray();
		viewer.Selection.Select(paragraphs[2].ContentStart, paragraphs[3].ContentEnd);
		string selected = viewer.Selection.Text;
		Require(selected.Contains(profile.Logs[2].Message, StringComparison.Ordinal) && selected.Contains(profile.Logs[3].Message, StringComparison.Ordinal),
			"The log must support a continuous text selection across message boundaries.");
		Require(ApplicationCommands.Copy.CanExecute(null, viewer), "The native COPY / Ctrl+C command must be available for selected log text.");

		string? copied = null;
		DataObjectCopyingEventHandler copying = (_, eventArguments) => {
			copied = eventArguments.DataObject.GetData(DataFormats.UnicodeText) as string;
			eventArguments.CancelCommand(); // Exercise the real command without changing the user's clipboard.
		};
		DataObject.AddCopyingHandler(viewer, copying);
		try {
			ApplicationCommands.Copy.Execute(null, viewer);
		} finally {
			DataObject.RemoveCopyingHandler(viewer, copying);
		}

		Require(copied == selected, "COPY must use exactly the selected text rather than the entire log.");
		Require(viewer.ContextMenu?.Items.OfType<MenuItem>().Any(item => item.Header is "COPY" && item.Command == ApplicationCommands.Copy) == true,
			"The log context menu must offer COPY.");
		profile.Logs.Add(new LogEntry("12:00:02", "WARN", @"NewLine C:\WireSteam\MiXeD.conf PublicKey=aBcD="));
		await SettleAsync(window);
		Require(viewer.Selection.Text == selected, "Appending a log line must preserve an existing multi-line selection.");

		profile.Logs.RemoveAt(0);
		await SettleAsync(window);
		Require(viewer.Selection.Text == selected, "Trimming an older log line must preserve a selection in surviving messages.");
		Require(viewer.IsReadOnly && !viewer.IsUndoEnabled, "The log document must remain read-only without accumulating edit history.");

		viewer.Selection.Select(viewer.Document.ContentStart, viewer.Document.ContentStart);
	}

	private static void TestProfileMenu(MainWindow window, MainViewModel model) {
		var original = model.Selected;
		var target = new Profile { Name = "MiXeD context target" };
		var other = new Profile { Name = "Different selected tunnel", Running = true, State = "Active" };
		try {
			model.AddProfile(target);
			model.AddProfile(other);
			Require(window.FindName("ProfilesList") is ListBox, "The profile list must expose a right-click target surface.");

			foreach (var state in new[] {
				(State: "Inactive", Running: false, Busy: false, Enabled: new[] { true, false, true, true, true }),
				(State: "Active", Running: true, Busy: false, Enabled: new[] { false, true, false, false, true }),
				(State: "Starting", Running: true, Busy: true, Enabled: new[] { false, true, false, false, true }),
				(State: "Starting", Running: false, Busy: true, Enabled: new[] { false, false, false, false, true }),
				(State: "Stopping", Running: true, Busy: true, Enabled: new[] { false, false, false, false, true }),
				(State: "Cleanup required", Running: false, Busy: false, Enabled: new[] { false, true, false, false, true }),
				(State: "Error", Running: false, Busy: false, Enabled: new[] { true, false, true, true, true }),
			}) {
				target.State = state.State;
				target.Running = state.Running;
				target.Busy = state.Busy;
				model.Selected = original;
				var menu = window.CreateProfileMenu(target);
				Require(model.Selected == target, "Opening a profile menu must select the clicked profile rather than act on the previous selection.");

				var actions = menu.Items.OfType<MenuItem>().ToArray();
				Require(actions.Select(action => action.Header as string).SequenceEqual(new[] { "CONNECT", "DISCONNECT", "EDIT", "DELETE", "EXPORT" }),
					"The profile menu must keep Connect / Disconnect / Edit / Delete / Export in the requested order.");
				Require(actions.Select(action => action.Command).SequenceEqual(new[] { model.ConnectCommand, model.DisconnectCommand, model.EditCommand, model.RemoveCommand, model.ExportCommand }),
					"The profile menu must use the actual view-model commands.");
				for (int actionIndex = 0; actionIndex < actions.Length; ++actionIndex) {
					Require(actions[actionIndex].Command!.CanExecute(actions[actionIndex].CommandParameter) == state.Enabled[actionIndex],
						"The " + actions[actionIndex].Header + " menu command has the wrong availability for " + state.State + ".");
				}
			}

			target.State = "Inactive";
			target.Running = target.Busy = false;
			var retainedMenu = window.CreateProfileMenu(target);
			var retainedActions = retainedMenu.Items.OfType<MenuItem>().ToArray();
			Profile? edited = null, removed = null, exported = null;
			EventHandler edit = (_, _) => edited = model.Selected;
			EventHandler remove = (_, _) => removed = model.Selected;
			EventHandler export = (_, _) => exported = model.Selected;
			try {
				model.EditRequested += edit;
				model.RemoveRequested += remove;
				model.ExportRequested += export;
				foreach (int index in new[] { 2, 3, 4 }) {
					// An asynchronous import/selection can finish after the
					// context menu opened. Its captured target must survive.
					model.Selected = other;
					var action = retainedActions[index];
					Require(action.Command!.CanExecute(action.CommandParameter), "A profile-menu action evaluated the newly selected tunnel instead of its captured target.");

					action.Command.Execute(action.CommandParameter);
				}

				Require(edited == target && removed == target && exported == target,
					"Edit, Delete and Export must keep the right-clicked profile even if selection changes while its menu remains open.");
				other.Running = false;
				other.State = "Inactive";
				target.Running = true;
				target.State = "Active";
				model.Selected = other;
				Require(!retainedActions[2].Command!.CanExecute(target) && !retainedActions[3].Command!.CanExecute(target),
					"A menu action allowed editing/deleting a running captured profile because the selected profile was inactive.");
			} finally {
				model.EditRequested -= edit;
				model.RemoveRequested -= remove;
				model.ExportRequested -= export;
			}
		} finally {
			model.Profiles.Remove(target);
			model.Profiles.Remove(other);
			model.Selected = original;
			model.Refresh();
		}

		Console.WriteLine("PASS profile menu: captured right-click target survives later selection changes, Connect/Disconnect/Edit/Delete/Export order and inactive/active/starting/stopping/cleanup/error availability.");
	}

	private static async Task TestLifecycleFooterAsync() {
		using (var offline = new PreviewBackend()) {
			var failed = new MainViewModel(offline);
			var profile = new Profile { Name = "MiXeD Failed" };
			failed.AddProfile(profile);
			await failed.ToggleAsync();
			Require(failed.StatusDisplay.StartsWith("FAILED · MiXeD Failed", StringComparison.Ordinal) &&
				!failed.StatusDisplay.StartsWith("STARTING", StringComparison.Ordinal),
				"A synchronous activation failure must replace the starting footer and preserve the tunnel-name case.");
		}

		using var backend = new LifecycleBackend();
		var model = new MainViewModel(backend);
		var tunnel = new Profile { Name = "MiXeD Host" };
		model.AddProfile(tunnel);
		await model.ToggleAsync();
		Require(model.StatusDisplay == "STARTING MiXeD Host…", "Starting feedback must preserve the original tunnel-name case.");

		tunnel.State = "Active";
		tunnel.Busy = false;
		Require(model.StatusDisplay == "CONNECTED · MiXeD Host", "A successful activation must replace the starting footer.");

		await model.ToggleAsync();
		Require(model.StatusDisplay == "STOPPING MiXeD Host…", "Stopping feedback is missing.");

		tunnel.Running = false;
		tunnel.Busy = false;
		tunnel.Detail = "Stopped";
		tunnel.State = "Inactive";
		Require(model.StatusDisplay == "DISCONNECTED · MiXeD Host", "A completed disconnect must replace the stopping footer.");

		await model.ToggleAsync();
		tunnel.Detail = "Administrator approval was cancelled.";
		tunnel.State = "Error";
		tunnel.Running = false;
		tunnel.Busy = false;
		Require(model.StatusDisplay == "FAILED · MiXeD Host · ADMINISTRATOR APPROVAL WAS CANCELLED.", "A delayed administrator-approval failure must replace the starting footer.");

		tunnel.State = "Inactive";
		tunnel.Detail = "Ready";
		await model.ToggleAsync();
		tunnel.Detail = "Activation cancelled";
		tunnel.State = "Inactive";
		tunnel.Running = false;
		tunnel.Busy = false;
		Require(model.StatusDisplay == "ACTIVATION CANCELLED · MiXeD Host", "A cancelled activation must have a terminal footer.");

		tunnel.State = "Inactive";
		tunnel.Detail = "Ready";
		await model.ToggleAsync();
		tunnel.Detail = "Owned adapter needs cleanup";
		tunnel.State = "Cleanup required";
		tunnel.Running = false;
		tunnel.Busy = false;
		Require(model.StatusDisplay.StartsWith("CLEANUP REQUIRED · MiXeD Host", StringComparison.Ordinal), "An adapter cleanup failure must replace the starting footer with its recovery state.");

		Console.WriteLine("PASS lifecycle footer: connected, disconnected, immediate/delayed activation failures, cancelled activation and cleanup-required states; tunnel names retain original case.");
	}

	private static async Task TestLivePeerBindingsAsync(MainWindow window, MainViewModel model, Profile profile) {
		var peer = profile.Peers[0];
		var previousLocal = (profile.LocalSteamId, profile.LocalSteamName, profile.Running, profile.State, profile.ReceiveActivityAt, profile.SendActivityAt);
		var previousPeer = (peer.SteamName, peer.ReceiveMbps, peer.SendMbps, peer.RxBytes, peer.TxBytes, peer.ReceiveActivityAt, peer.SendActivityAt);
		try {
			profile.LocalSteamId = "76561198000000001";
			profile.LocalSteamName = "Local MiXeD 🎮";
			profile.Running = true;
			profile.State = "Active";
			peer.SteamName = "Peer MiXeD Ω";
			peer.ReceiveMbps = 3.25;
			peer.SendMbps = 1.5;
			peer.RxBytes = 2048;
			peer.TxBytes = 4096;
			model.Show("Dashboard");
			await SettleAsync(window);
			var identity = (TextBlock)window.FindName("LocalSteamIdentity");
			Require(identity.IsVisible && identity.Text == "Local MiXeD 🎮 · 76561198000000001", "The active tunnel must display its local Steam nickname and SteamID with their original case.");

			var links = (ItemsControl)window.FindName("PeerLinks");
			var row = links.ItemContainerGenerator.ContainerFromItem(peer);
			Require(row != null, "The actual peer-links row did not materialize.");

			var texts = VisualDescendants(row!).OfType<TextBlock>().ToArray();
			TextBlock BoundText(string path) => texts.Single(text => BindingOperations.GetBinding(text, TextBlock.TextProperty)?.Path.Path == path);
			Run BoundRun(string path) => texts.SelectMany(text => text.Inlines.OfType<Run>()).Single(run => BindingOperations.GetBinding(run, Run.TextProperty)?.Path.Path == path);
			Require(BoundText("DisplayName").Text == peer.SteamName && BoundText("SteamId").Text == peer.SteamId, "Peer links must show both the case-preserved Steam nickname and stable SteamID.");
			Require(BoundRun("ReceiveText").Text == "3.25" && BoundRun("SendText").Text == "1.50" && BoundText("ReceiveTotalText").Text == "2 KiB" && BoundText("SendTotalText").Text == "4 KiB", "The actual peer links must render receive/send rates and accumulated payload totals.");
			Require(BoundText("DisplayName").FontSize >= 13 && BoundText("SteamId").FontSize >= 11 && BoundText("AllowedIps").FontSize >= 11 && BoundText("ReceiveTotalText").FontSize >= 11, "Peer identity, routes and accumulated counters must remain readable at normal display scale.");

			var receiveRate = (TextBlock)BoundRun("ReceiveText").Parent;
			var sendRate = (TextBlock)BoundRun("SendText").Parent;
			Require(receiveRate.FontSize >= 13 && sendRate.FontSize >= 13 && receiveRate.Inlines.OfType<Run>().Any(run => run.Text == " MBIT/S") && sendRate.Inlines.OfType<Run>().Any(run => run.Text == " MBIT/S"), "Peer rates and their units must share a text baseline.");
			Require(Math.Abs(receiveRate.TranslatePoint(new Point(0, 0), (UIElement)row!).Y - sendRate.TranslatePoint(new Point(0, 0), (UIElement)row!).Y) < 0.5 &&
				Math.Abs(BoundText("ReceiveTotalText").TranslatePoint(new Point(0, 0), (UIElement)row!).Y - BoundText("SendTotalText").TranslatePoint(new Point(0, 0), (UIElement)row!).Y) < 0.5,
				"Peer receive/send rate rows and total rows must align.");
			var brand = (TextBlock)window.FindName("BrandName");
			var brandLogo = (Image)window.FindName("BrandLogo");
			Require(brand.FontSize >= 21 && brand.FontWeight == FontWeights.Bold, "The brand must retain its larger bold wordmark.");

			foreach (double dpi in new[] { 96.0, 120.0, 144.0 }) {
				var brandInk = VisibleInk(brand, dpi);
				var logoInk = VisibleInk(brandLogo, dpi);
				Require(!brandInk.IsEmpty && !logoInk.IsEmpty &&
					Math.Abs(brand.TranslatePoint(new Point(0, brandInk.Top + brandInk.Height / 2), window).Y - brandLogo.TranslatePoint(new Point(0, logoInk.Top + logoInk.Height / 2), window).Y) <= 1,
					"The visible wordmark glyphs must center on the logo ink, including at fractional display scales.");
			}

			TestDashboardAlignment(window, model);
			var indicators = VisualDescendants(row!).OfType<TrafficLed>().ToArray();
			TrafficLed Indicator(string path) => indicators.Single(led => BindingOperations.GetBinding(led, TrafficLed.ActivityAtProperty)?.Path.Path == path);
			long activityAt = Stopwatch.GetTimestamp();
			peer.ReceiveActivityAt = peer.SendActivityAt = profile.ReceiveActivityAt = profile.SendActivityAt = activityAt;
			await SettleAsync(window);
			var rxLed = Indicator("ReceiveActivityAt");
			var txLed = Indicator("SendActivityAt");
			rxLed.RefreshAt(activityAt);
			txLed.RefreshAt(activityAt);
			Require(rxLed.IsLit && txLed.IsLit && ((SolidColorBrush)rxLed.Foreground).Color == Color.FromRgb(111, 255, 199) && ((SolidColorBrush)txLed.Foreground).Color == Color.FromRgb(106, 203, 255), "Actual peer LEDs must retain distinct receive/send colors and illuminate on observed byte deltas.");

			rxLed.RefreshAt(activityAt + Stopwatch.Frequency * 120 / 1000);
			Require(!rxLed.IsLit && rxLed.ClockRunning, "A busy peer LED must have a visible dark interval while its pulse clock remains active.");

			rxLed.RefreshAt(activityAt + Stopwatch.Frequency * 240 / 1000);
			Require(rxLed.IsLit, "Sustained recent traffic must produce another short LED pulse.");

			foreach (string name in new[] { "ReceiveTrafficLed", "SendTrafficLed", "ChartReceiveLed", "ChartSendLed" }) {
				var led = (TrafficLed)window.FindName(name);
				led.RefreshAt(activityAt);
				Require(led.ActivityAt == activityAt && led.IsLit, "Summary cards and chart RX/TX LEDs must follow actual aggregate byte activity.");
			}

			profile.LocalSteamName = "Updated Local Nick";
			peer.SteamName = "Updated Peer Nick";
			peer.ReceiveMbps = 0;
			peer.SendMbps = 0;
			peer.ReceiveActivityAt = peer.SendActivityAt = profile.ReceiveActivityAt = profile.SendActivityAt = 0;
			await SettleAsync(window);
			Require(identity.Text == "Updated Local Nick · 76561198000000001" && BoundText("DisplayName").Text == "Updated Peer Nick", "Live Steam nickname changes must reach the rendered local identity and peer row.");
			Require(!rxLed.IsLit && !txLed.IsLit && !rxLed.ClockRunning && !txLed.ClockRunning, "Idle peers must turn both LEDs and their pulse clocks off.");

			peer.ReceiveActivityAt = Stopwatch.GetTimestamp();
			model.Show("Logs");
			await SettleAsync(window);
			Require(!rxLed.IsLit && !rxLed.ClockRunning, "Hidden pages must stop their LED pulse clocks.");

			peer.ReceiveActivityAt = 0;
			model.Show("Dashboard");
			await SettleAsync(window);
			profile.Running = false;
			await SettleAsync(window);
			Require(!identity.IsVisible, "An inactive tunnel must not advertise a running local Steam identity.");
		} finally {
			profile.LocalSteamId = previousLocal.LocalSteamId;
			profile.LocalSteamName = previousLocal.LocalSteamName;
			profile.Running = previousLocal.Running;
			profile.State = previousLocal.State;
			profile.ReceiveActivityAt = previousLocal.ReceiveActivityAt;
			profile.SendActivityAt = previousLocal.SendActivityAt;
			peer.SteamName = previousPeer.SteamName;
			peer.ReceiveMbps = previousPeer.ReceiveMbps;
			peer.SendMbps = previousPeer.SendMbps;
			peer.RxBytes = previousPeer.RxBytes;
			peer.TxBytes = previousPeer.TxBytes;
			peer.ReceiveActivityAt = previousPeer.ReceiveActivityAt;
			peer.SendActivityAt = previousPeer.SendActivityAt;
			model.Show("Logs");
			await SettleAsync(window);
		}

		Console.WriteLine("PASS live peer UI: local/remote Steam IDs and case-preserved nickname changes, receive/send rates/totals, active/idle indicators and inactive identity visibility.");
	}

	private static void TestMenuChrome(MainWindow window, MainViewModel model) {
		var original = model.Selected;
		try {
			var menu = window.CreateProfileMenu(new Profile { Name = "Dark menu fixture" });
			menu.Items.Add(new Separator());
			menu.Items.Add(new MenuItem { Header = "COPY", Command = ApplicationCommands.Copy });
			menu.ApplyTemplate();
			menu.Measure(new Size(240, 500));
			menu.Arrange(new Rect(new Point(0, 0), menu.DesiredSize));
			menu.UpdateLayout();
			Require(menu.Template.FindName("MenuSurface", menu) is Border surface && surface.Background is SolidColorBrush background && background.Color == Color.FromRgb(16, 24, 36), "Context menus must replace the whole Windows menu template, including its light icon gutter.");
			Require(menu.FocusVisualStyle == null && menu.Items.OfType<MenuItem>().All(item => item.FocusVisualStyle == null), "Context-menu focus chrome must use the dark item template.");

			foreach (double dpi in new[] { 96.0, 120.0, 144.0, 192.0 }) {
				int width = (int)Math.Ceiling(menu.ActualWidth * dpi / 96), height = (int)Math.Ceiling(menu.ActualHeight * dpi / 96);
				Require(width > 0 && height > 0, "The context-menu template did not lay out.");

				var bitmap = new System.Windows.Media.Imaging.RenderTargetBitmap(width, height, dpi, dpi, PixelFormats.Pbgra32);
				bitmap.Render(menu);
				string? preview = Environment.GetEnvironmentVariable("WIRESTEAM_CONTEXT_RENDER");
				if (dpi == 96 && !string.IsNullOrWhiteSpace(preview)) {
					var encoder = new System.Windows.Media.Imaging.PngBitmapEncoder();
					encoder.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bitmap));

					using var output = System.IO.File.Create(preview);

					encoder.Save(output);
				}

				var pixels = new byte[width * height * 0x04];
				bitmap.CopyPixels(pixels, width * 0x04, 0x00);
				for (int y = (int)(10 * dpi / 96); y < height - 10 * dpi / 96; ++y) {
					int offset = (y * width + (int)(8 * dpi / 96)) * 0x04;
					Require(pixels[offset + 0x03] > 0 && pixels[offset] < 100 && pixels[offset + 0x01] < 100 && pixels[offset + 0x02] < 100, "The context menu rendered a transparent or light vertical gutter at " + dpi + " DPI.");
				}
			}

			var separator = menu.Items.OfType<Separator>().Single();
			Require(VisualDescendants(separator).OfType<Border>().Any(border => border.Background is SolidColorBrush brush && brush.Color == Color.FromRgb(42, 60, 80)), "Context-menu separators must use the dark separator template.");
		} finally {
			model.Selected = original;
		}

		Console.WriteLine("PASS dark popup chrome: custom menu surface, separators and focus styling; no light icon gutter at 100/125/150/200% scaling.");
	}

	private static void TestTrafficChartTypography() {
		var chart = new TrafficChart { Width = 400, Height = 240, Points = new[] { new TrafficPoint(12.5, 3.25), new TrafficPoint(19, 7) } };
		chart.Measure(new Size(400, 240));
		chart.Arrange(new Rect(0, 0, 400, 240));
		var bitmap = new System.Windows.Media.Imaging.RenderTargetBitmap(400, 240, 96, 96, PixelFormats.Pbgra32);
		bitmap.Render(chart);
		var drawing = VisualTreeHelper.GetDrawing(chart);
		var labels = drawing == null ? Array.Empty<GlyphRunDrawing>() : Glyphs(drawing).ToArray();
		Require(labels.Length >= 6 && labels.All(label => label.GlyphRun.FontRenderingEmSize >= 11), "Chart axes and history labels must render at readable sizes.");
		Require(labels.All(label => label.Bounds.Left >= -0.5 && label.Bounds.Top >= -0.5 && label.Bounds.Right <= 400.5 && label.Bounds.Bottom <= 240.5), "Larger chart labels must fit inside the plot control.");
		Require(labels.Take(4).Max(label => label.Bounds.Right) - labels.Take(4).Min(label => label.Bounds.Right) <= 1.5, "Chart tick labels must share a right edge instead of drifting with their number of digits.");
	}

	private static void TestDashboardAlignment(MainWindow window, MainViewModel model) {
		var metrics = new[] { "ReceiveStat", "SendStat", "LatencyStat" }.Select(name => (TextBlock)window.FindName(name)).ToArray();
		var baselines = metrics.Select(text => text.TranslatePoint(new Point(0, text.BaselineOffset), window).Y).ToArray();
		Require(baselines.Max() - baselines.Min() <= 0.5 && metrics.All(text => text.Inlines.OfType<Run>().Count() == 2 && text.Inlines.OfType<Run>().All(run => run.BaselineAlignment == BaselineAlignment.Baseline)), "Summary values and units must share true text baselines across all three statistic cards.");

		var navigation = new[] { model.ShowDashboardCommand, model.ShowLogsCommand, model.ShowSettingsCommand }
			.Select(command => Descendants(window).OfType<Button>().Single(button => button.Command == command && VisualDescendants(button).OfType<TechIcon>().Any(icon => icon.Width == 20))).ToArray();
		Require(navigation.Select(button => VisualDescendants(button).OfType<TextBlock>().Single().Text).SequenceEqual(new[] { "OVERVIEW", "EVENT LOG", "SETTINGS" }), "Sidebar navigation must contain only overview, logs and settings.");

		var leftEdges = navigation.Select(button => VisualDescendants(button).OfType<TextBlock>().Single().TranslatePoint(new Point(0, 0), window).X).ToArray();
		Require(leftEdges.Max() - leftEdges.Min() <= 0.5, "All sidebar navigation labels must share the same text column.");

		foreach (var button in navigation) {
			var label = VisualDescendants(button).OfType<TextBlock>().Single();
			var icon = VisualDescendants(button).OfType<TechIcon>().Single();
			var labelInk = VisibleInk(label, 96);
			var iconInk = VisibleInk(icon, 96);
			Require(Math.Abs(label.TranslatePoint(new Point(0, labelInk.Top + labelInk.Height / 2), window).Y - icon.TranslatePoint(new Point(0, iconInk.Top + iconInk.Height / 2), window).Y) <= 1.5, "Sidebar navigation text must align with its visible icon.");
		}
	}

	// Render only the element's local drawing. RenderTargetBitmap.Render(element)
	// also includes its parent-relative visual offset and can crop the measurement.
	private static Rect VisibleInk(FrameworkElement element, double dpi) {
		double scale = dpi / 96;
		int width = Math.Max(1, (int)Math.Ceiling(element.ActualWidth * scale)), height = Math.Max(1, (int)Math.Ceiling(element.ActualHeight * scale));
		var drawing = new DrawingVisual();
		using (var context = drawing.RenderOpen()) {
			context.DrawDrawing(VisualTreeHelper.GetDrawing(element));
		}

		var bitmap = new System.Windows.Media.Imaging.RenderTargetBitmap(width, height, dpi, dpi, PixelFormats.Pbgra32);
		bitmap.Render(drawing);
		var pixels = new byte[width * height * 0x04];
		bitmap.CopyPixels(pixels, width * 0x04, 0x00);
		int left = width, right = -1, top = height, bottom = -1;
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				if (pixels[(y * width + x) * 0x04 + 0x03] > 80) {
					left = Math.Min(left, x);
					right = Math.Max(right, x);
					top = Math.Min(top, y);
					bottom = Math.Max(bottom, y);
				}
			}
		}

		return right < left ? Rect.Empty : new Rect(left / scale, top / scale, (right - left + 1) / scale, (bottom - top + 1) / scale);
	}

	private static System.Collections.Generic.IEnumerable<GlyphRunDrawing> Glyphs(Drawing drawing) {
		if (drawing is GlyphRunDrawing glyph) {
			yield return glyph;
		} else if (drawing is DrawingGroup group) {
			foreach (var child in group.Children) {
				foreach (var descendant in Glyphs(child)) {
					yield return descendant;
				}
			}
		}
	}

	// Only changes an in-memory profile. It cannot run Steam, request elevation,
	// create an adapter, or read/write a real configuration.
	private sealed class LifecycleBackend : IBackend {
		public string EnginePath => "OFFLINE TEST";
		public string ProfilesDirectory => "OFFLINE TEST";
		public event EventHandler? StatusChanged {
			add {
			}

			remove {
			}
		}

		private static Exception Offline() => new InvalidOperationException("Offline lifecycle fixture.");
		public Task<System.Collections.Generic.IReadOnlyList<Profile>> LoadProfilesAsync() => Task.FromResult<System.Collections.Generic.IReadOnlyList<Profile>>(Array.Empty<Profile>());
		public Task<bool> CheckPrerequisitesAsync() => Task.FromResult(false);
		public Task StartAsync(Profile profile) {
			profile.Running = true;
			profile.Busy = true;
			profile.State = "Starting";

			return Task.CompletedTask;
		}

		public Task StopAsync(Profile profile) {
			profile.Busy = true;
			profile.State = "Stopping";

			return Task.CompletedTask;
		}

		public Task<Profile> ImportAsync(string sourcePath) => throw Offline();
		public Task ReloadAsync(Profile profile) => throw Offline();
		public Task SaveAsync(Profile profile, string content) => throw Offline();
		public Task<Profile> CreateAsync(string name, string content) => throw Offline();
		public Task<string> CreateTemplateAsync() => throw Offline();
		public Task<string> ReadConfigAsync(Profile profile) => throw Offline();
		public Task ExportAsync(Profile profile, string destination, bool overwrite = false) => throw Offline();
		public Task RemoveAsync(Profile profile) => throw Offline();
		public Task StopAllAsync() => Task.CompletedTask;

		public void Dispose() {
		}
	}

	private static bool AtBottom(ScrollViewer viewer) => Math.Abs(viewer.ScrollableHeight - viewer.VerticalOffset) < 0.5;

	private static void AssertStaticChromeUppercase(DependencyObject root) {
		foreach (var control in Descendants(root)) {
			if (control is TextBlock text && BindingOperations.GetBinding(text, TextBlock.TextProperty) == null) {
				RequireUppercase(text.Text, "Static text");
			}

			if (control is Button button && button.Content is string action) {
				RequireUppercase(action, "Action");
			}

			if (control is CheckBox checkBox && checkBox.Content is string option) {
				RequireUppercase(option, "Option");
			}

			if (control is FrameworkElement element && element.ToolTip is string tip &&
				!BindingOperations.IsDataBound(element, FrameworkElement.ToolTipProperty)) {
				RequireUppercase(tip, "Tooltip");
			}
		}
	}

	private static void RequireUppercase(string value, string kind) => Require(value == value.ToUpperInvariant(), kind + " must use uppercase: " + value + ".");
	private static bool HasButtonLabel(DependencyObject root, string label) => root is Button button && ButtonText(button) == label || LogicalTreeHelper.GetChildren(root).OfType<DependencyObject>().Any(child => HasButtonLabel(child, label));
	private static string ButtonText(Button button) => button.Content as string ?? string.Join("", Descendants(button).OfType<TextBlock>().Select(text => text.Text));
	private static System.Collections.Generic.IEnumerable<DependencyObject> Descendants(DependencyObject root) {
		foreach (var child in LogicalTreeHelper.GetChildren(root).OfType<DependencyObject>()) {
			yield return child;
			foreach (var descendant in Descendants(child)) {
				yield return descendant;
			}
		}
	}

	private static System.Collections.Generic.IEnumerable<DependencyObject> VisualDescendants(DependencyObject root) {
		for (int childIndex = 0; childIndex < VisualTreeHelper.GetChildrenCount(root); ++childIndex) {
			var child = VisualTreeHelper.GetChild(root, childIndex);
			yield return child;
			foreach (var descendant in VisualDescendants(child)) {
				yield return descendant;
			}
		}
	}

	private static async Task TestInternetModeLabelsAsync(MainWindow window, MainViewModel model, Profile profile) {
		var original = (profile.InternetMode, profile.InternetSubnet, profile.State, profile.Running);
		var previousSelection = model.Selected;
		string previousPage = model.IsLogsVisible ? "Logs" : model.IsSettingsVisible ? "Settings" : "Dashboard";
		var overviewContainer = (FrameworkElement)window.FindName("OverviewModeContainer");
		var settingsContainer = (FrameworkElement)window.FindName("SettingsModeContainer");
		var overviewBadge = (TextBlock)window.FindName("OverviewModeBadge");
		var settingsBadge = (TextBlock)window.FindName("SettingsModeBadge");
		try {
			profile.State = "Inactive";
			profile.Running = false;
			foreach (var role in new[] { (Mode: "off", Label: "PRIVATE NETWORK"), (Mode: "client", Label: "INTERNET CLIENT"), (Mode: "server", Label: "INTERNET GATEWAY") }) {
				profile.InternetMode = role.Mode;
				profile.InternetSubnet = role.Mode == "server" ? "10.77.0.0/24" : "";
				profile.RefreshMetadata();
				model.Show("Dashboard");
				await SettleAsync(window);
				Require(overviewContainer.IsVisible && overviewBadge.IsVisible && overviewBadge.Text == role.Label, "Overview must display the configured role badge: " + role.Label + ".");

				model.Show("Settings");
				await SettleAsync(window);
				Require(settingsContainer.IsVisible && settingsBadge.IsVisible && settingsBadge.Text == role.Label, "Settings must display the configured role badge: " + role.Label + ".");
				Require(((TextBlock)window.FindName("InternetSubnetValue")).Text == (role.Mode == "server" ? "10.77.0.0/24" : "—"), "The gateway subnet display is stale.");
				Require(profile.State == "Inactive" && !profile.Running, "A configured role must not imply an active or reachable Internet connection.");
			}

			model.Selected = null;
			model.Show("Dashboard");
			await SettleAsync(window);
			Require(overviewContainer.IsVisible && overviewBadge.IsVisible && overviewBadge.Text == "NETWORK MODE", "Overview must retain its neutral role label when no profile is selected.");

			model.Show("Settings");
			await SettleAsync(window);
			Require(settingsContainer.IsVisible && settingsBadge.IsVisible && settingsBadge.Text == "NO PROFILE SELECTED", "Settings must retain its no-profile role label.");
		} finally {
			profile.InternetMode = original.InternetMode;
			profile.InternetSubnet = original.InternetSubnet;
			profile.State = original.State;
			profile.Running = original.Running;
			profile.RefreshMetadata();
			model.Selected = previousSelection;
			model.Show(previousPage);
			await SettleAsync(window);
		}

		Console.WriteLine("PASS configured Internet roles: visible private-network, Internet-client and Internet-gateway badges on Overview and Settings, subnet refresh and no-profile fallback labels.");
	}

	private static async Task TestStatusTypographyAsync(MainWindow window, MainViewModel model, Profile profile) {
		var original = (profile.Name, profile.Detail, profile.PublicKey, model.StatusMessage);
		try {
			profile.Name = "MiXeD tunnel";
			profile.Detail = "Validated configuration";
			profile.PublicKey = "aBcD+eF/123=";
			profile.RefreshMetadata();
			model.StatusMessage = "Configuration validated and saved.";
			await SettleAsync(window);
			Require(((TextBlock)window.FindName("SelectedAlias")).Text == "MiXeD tunnel", "Tunnel names must retain the original case in the UI.");

			var detail = (TextBlock)window.FindName("SelectedDetail");
			Require(detail.Text == "VALIDATED CONFIGURATION" && detail.FontWeight == FontWeights.Normal, "Overview status must use regular-weight uppercase.");
			Require(((TextBlock)window.FindName("SelectedAlias")).FontWeight == FontWeights.Bold, "The selected tunnel heading must retain the bold hierarchy.");
			Require(((TextBlock)window.FindName("StatusLine")).Text == "CONFIGURATION VALIDATED AND SAVED.", "Displayed status messages must use uppercase.");
			Require(((TextBox)window.FindName("EnginePathValue")).Text == model.Backend.EnginePath, "Filesystem paths must retain exact case in the UI.");
			Require(profile.Name == "MiXeD tunnel" && profile.Detail == "Validated configuration" && model.StatusMessage == "Configuration validated and saved.", "Display casing must not mutate profile or status values.");
		} finally {
			profile.Name = original.Name;
			profile.Detail = original.Detail;
			profile.PublicKey = original.PublicKey;
			profile.RefreshMetadata();
			model.StatusMessage = original.StatusMessage;
		}
	}

	private static void TestForwardDialogChrome(MainWindow owner, MainViewModel model) {
		const string exact = "[Interface]\nPrivateKey = exact-Private+Key/value=\nAddress = 10.77.0.1/32, fd77::1/128\n[WireSteam]\nInternetMode = off\n[PortForward]\nProtocol = TCP\nListen = 10.77.0.1:25565\nTarget = 127.0.0.1:25565\nMaxConnections = 23\n";
		var dialog = new PortForwardDialog(owner, "MiXeD Profile", exact);
		try {
			var content = (FrameworkElement)dialog.Content;
			content.Measure(new Size(780, 520));
			content.Arrange(new Rect(0, 0, 780, 520));
			content.UpdateLayout();
			Require(dialog.Title == "PORT FORWARDING" && HasButtonLabel(dialog, "ADD FORWARD") && HasButtonLabel(dialog, "EDIT") && HasButtonLabel(dialog, "DELETE") && HasButtonLabel(dialog, "VALIDATE & SAVE"), "Forwarding manager must provide discoverable add/edit/delete/save actions.");

			var list = Descendants(dialog).OfType<ListBox>().Single();
			Require(list.Items.Count == 1 && list.Items[0] is PortForwardRule { Listen: "10.77.0.1:25565", Target: "127.0.0.1:25565" }, "Forwarding manager must show actual rule endpoints.");
			Require(Descendants(dialog).OfType<TextBlock>().Any(text => text.Text == "MiXeD Profile") && !Descendants(dialog).OfType<TextBlock>().Any(text => text.Text.Contains("exact-Private", StringComparison.Ordinal)), "Forwarding manager must preserve profile name case and keep private configuration out of its visible text.");

			foreach (var button in Descendants(dialog).OfType<Button>().Where(button => button.Content is string)) {
				var bounds = button.TransformToAncestor(content).TransformBounds(new Rect(button.RenderSize));
				Require(bounds.Left >= 0 && bounds.Right <= content.ActualWidth + .5 && bounds.Top >= 0 && bounds.Bottom <= content.ActualHeight + .5, "Forwarding actions must fit within the dialog viewport.");
			}

			var editor = new PortForwardDialog.ForwardRuleDialog(owner, new PortForwardConfiguration(exact), new PortForwardConfiguration(exact).Rules, null);
			try {
				var editorContent = (FrameworkElement)editor.Content;
				editorContent.Measure(new Size(630, 410));
				editorContent.Arrange(new Rect(0, 0, 630, 410));
				editorContent.UpdateLayout();
				var combos = Descendants(editor).OfType<ComboBox>().ToArray();
				Require(combos.Length == 2 && combos.Any(combo => combo.Items.Cast<string>().SequenceEqual(new[] { "10.77.0.1", "fd77::1" })), "Forward editor must offer profile-owned VPN addresses without wildcard choices.");
				Require(combos.Single(combo => combo.Items.Cast<string>().SequenceEqual(new[] { "TCP", "UDP" })).SelectedItem is "TCP", "New forwarding rules must offer TCP and UDP while defaulting to TCP.");
				Require(Descendants(editor).OfType<TextBox>().Any(box => box.Text == "127.0.0.1") && HasButtonLabel(editor, "ADD FORWARD"), "Forward editor must default to the loopback target and provide an explicit add action.");
			} finally {
				editor.Close();
			}

			var udpEditor = new PortForwardDialog.ForwardRuleDialog(owner, new PortForwardConfiguration(exact), Array.Empty<PortForwardRule>(), new PortForwardRule("10.77.0.1:25565", "127.0.0.1:25565", "UDP"));
			try {
				var editorContent = (FrameworkElement)udpEditor.Content;
				editorContent.Measure(new Size(630, 440));
				editorContent.Arrange(new Rect(0, 0, 630, 440));
				editorContent.UpdateLayout();
				Require(Descendants(udpEditor).OfType<ComboBox>().Single(combo => combo.Items.Cast<string>().SequenceEqual(new[] { "TCP", "UDP" })).SelectedItem is "UDP", "Editing an existing UDP forward must retain its protocol selection.");
			} finally {
				udpEditor.Close();
			}
		} finally {
			dialog.Close();
		}

		TestForwardRuleLimit(owner);

		var clientDialog = new PortForwardDialog(owner, "Client", "[Interface]\nAddress = 10.77.0.2/32\n[WireSteam]\nInternetMode = client\n");
		try {
			var content = (FrameworkElement)clientDialog.Content;
			content.Measure(new Size(780, 520));
			content.Arrange(new Rect(0, 0, 780, 520));
			content.UpdateLayout();
			Require(!Descendants(clientDialog).OfType<Button>().Single(button => button.Content is "ADD FORWARD").IsEnabled && Descendants(clientDialog).OfType<TextBlock>().Any(text => text.Text.StartsWith("INTERNET-CLIENT PROFILES CANNOT HOST", StringComparison.Ordinal)), "Internet-client profiles need an explicit explanation and a disabled add action.");
		} finally {
			clientDialog.Close();
		}

		var profile = model.Selected!;
		var state = (profile.Running, profile.Busy, profile.State);
		Profile? requested = null;
		EventHandler requestedHandler = (_, _) => requested = model.Selected;
		model.PortForwardsRequested += requestedHandler;
		try {
			foreach (var value in new[] { (Running: true, Busy: false, State: "Active"), (Running: false, Busy: true, State: "Starting"), (Running: true, Busy: true, State: "Stopping"), (Running: false, Busy: false, State: "Cleanup required") }) {
				profile.Running = value.Running;
				profile.Busy = value.Busy;
				profile.State = value.State;
				Require(!model.ManageForwardsCommand.CanExecute(profile), "Forwarding edits must be disabled for active, starting, stopping or recovery-owned profiles.");
			}

			profile.Running = profile.Busy = false;
			profile.State = "Inactive";
			Require(model.ManageForwardsCommand.CanExecute(profile), "Stopped profiles must expose forwarding management.");

			model.ManageForwardsCommand.Execute(profile);
			Require(requested == profile, "Forwarding management must keep the command's profile target.");
		} finally {
			model.PortForwardsRequested -= requestedHandler;
			profile.Running = state.Running;
			profile.Busy = state.Busy;
			profile.State = state.State;
		}

		Console.WriteLine("PASS forwarding UI: endpoint list, private-text isolation, profile name case, field defaults, valid VPN addresses, dialog layout and lifecycle/client restrictions.");
	}

	private static void TestForwardRuleLimit(MainWindow owner) {
		const string profile = "[Interface]\nAddress = 10.77.0.1/32\n[WireSteam]\nInternetMode = off\n";
		string forwards = string.Join("", Enumerable.Range(0, 31).Select(index => "[PortForward]\nProtocol = TCP\nListen = 10.77.0.1:" + (24000 + index) + "\nTarget = 127.0.0.1:" + (24000 + index) + "\n"));
		var manager = new PortForwardDialog(owner, "Rule limit", profile + forwards);
		Require(RunForwardDialog(manager, () => {
			var list = Descendants(manager).OfType<ListBox>().Single();
			var add = Descendants(manager).OfType<Button>().Single(button => button.Content is "ADD FORWARD");
			var edit = Descendants(manager).OfType<Button>().Single(button => button.Content is "EDIT");
			Require(list.Items.Count == 31 && add.IsEnabled && Descendants(manager).OfType<TextBlock>().Any(text => text.Text.Contains("UP TO 32 RULES", StringComparison.Ordinal)), "The forwarding manager must advertise 32 rules and allow adding rule 32.");
			RunForwardEditor(manager, "ADD FORWARD", editor => {
				Descendants(editor).OfType<ComboBox>().Single(combo => combo.Items.Cast<string>().SequenceEqual(new[] { "TCP", "UDP" })).SelectedItem = "UDP";
				Descendants(editor).OfType<Button>().Single(button => button.IsDefault).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
				Require(editor.Rule is { Protocol: "UDP" } && !editor.IsVisible, "The actual add action must accept one UDP rule at the 32-rule boundary.");
			});
			Require(list.Items.Count == 32 && !add.IsEnabled && edit.IsEnabled, "At 32 rules, add must be disabled while editing the selected rule remains available.");
			RunForwardEditor(manager, "EDIT", editor => {
				Descendants(editor).OfType<TextBox>().Single(box => box.Text == "127.0.0.1").Text = "127.0.0.2";
				Descendants(editor).OfType<Button>().Single(button => button.IsDefault).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
				Require(editor.Rule is { Protocol: "UDP", Target: "127.0.0.2:25565" } && !editor.IsVisible, "Editing an existing rule must succeed without consuming another rule slot.");
			});
			Require(list.Items.Count == 32 && !add.IsEnabled && list.SelectedItem is PortForwardRule { Target: "127.0.0.2:25565" }, "A full profile must retain its 32 rules after an endpoint edit.");
			Descendants(manager).OfType<Button>().Single(button => button.Content is "DELETE").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
			Require(list.Items.Count == 31 && add.IsEnabled, "Deleting one rule from a full profile must immediately reenable add.");
			RunForwardEditor(manager, "ADD FORWARD", editor => {
				Descendants(editor).OfType<Button>().Single(button => button.IsDefault).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
				Require(editor.Rule is { Protocol: "TCP" } && !editor.IsVisible, "The freed rule slot must accept a new TCP forward.");
			});
			Require(list.Items.Count == 32 && !add.IsEnabled, "Refilling the freed slot must restore the 32-rule add limit.");
			Descendants(manager).OfType<Button>().Single(button => button.Content is "VALIDATE & SAVE").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
		}) == true, "A full 32-rule profile must remain saveable after add, edit, delete and replacement actions.");
		Require(new PortForwardConfiguration(manager.Configuration).Rules.Count == 32, "Saving and reopening a full profile must preserve all 32 forwarding rules.");
		Console.WriteLine("PASS forwarding rule limit: add at 31, edit at 32, disabled add at 32, delete/re-add availability and full-profile save/reopen.");
	}

	private static bool? RunForwardDialog(Window dialog, Action interaction) {
		Exception? failure = null;
		dialog.Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, new Action(() => {
			try {
				interaction();
			} catch (Exception exception) {
				failure = exception;
			} finally {
				if (dialog.IsVisible) {
					dialog.Close();
				}
			}
		}));

		bool? result = dialog.ShowDialog();
		if (failure != null) {
			throw new InvalidOperationException("Forwarding dialog interaction failed.", failure);
		}

		return result;
	}

	private static void RunForwardEditor(PortForwardDialog manager, string action, Action<PortForwardDialog.ForwardRuleDialog> interaction) {
		Exception? failure = null;
		manager.Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, new Action(() => {
			PortForwardDialog.ForwardRuleDialog? editor = null;
			try {
				editor = Application.Current.Windows.OfType<PortForwardDialog.ForwardRuleDialog>().Single(window => window.IsVisible);
				interaction(editor);
			} catch (Exception exception) {
				failure = exception;
			} finally {
				if (editor?.IsVisible == true) {
					editor.Close();
				}
			}
		}));

		Descendants(manager).OfType<Button>().Single(button => button.Content as string == action).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
		if (failure != null) {
			throw new InvalidOperationException("Forwarding editor interaction failed.", failure);
		}
	}

	private static void TestConfigurationDialogChrome(MainWindow owner) {
		const string exact = "[Interface]\nPrivateKey = aBcD+eF=\n# CaseSensitive";
		var dialog = new ConfigDialog(owner, "Edit profile", "MiXeD Profile", exact);
		try {
			var footer = ((Grid)dialog.Content).Children.OfType<DockPanel>().Single();
			Require(dialog.Title == "EDIT PROFILE" && footer.Children.OfType<Button>().Any(button => button.Content is "VALIDATE & SAVE") && footer.Children.OfType<Button>().Any(button => button.Content is "CANCEL"), "Configuration dialog titles and actions must use uppercase.");
			Require(((Grid)dialog.Content).Children.OfType<TextBlock>().First().Text == "EDIT PROFILE" && footer.Children.OfType<TextBlock>().Single().Text == "CONTAINS PRIVATE KEYS. KEEP THIS CONFIGURATION PRIVATE.", "Configuration dialog guidance must use uppercase.");
			Require(dialog.Configuration == exact && dialog.ProfileName == "MiXeD Profile", "Configuration editor typography changed sensitive values.");
		} finally {
			dialog.Close();
		}
	}

	private static async Task TestActionLabelsAsync(MainWindow window, Profile profile) {
		var original = (profile.State, profile.Running, profile.Busy);
		var action = (Button)window.FindName("TunnelAction");
		try {
			foreach (var state in new[] {
				(State: "Inactive", Running: false, Busy: false, Label: "ACTIVATE TUNNEL"),
				(State: "Starting", Running: false, Busy: true, Label: "STARTING…"),
				(State: "Starting", Running: true, Busy: true, Label: "CANCEL ACTIVATION"),
				(State: "Active", Running: true, Busy: false, Label: "DEACTIVATE TUNNEL"),
				(State: "Stopping", Running: true, Busy: true, Label: "DISCONNECTING…"),
				(State: "Cleanup required", Running: false, Busy: false, Label: "CLEAN UP ADAPTER"),
			}) {
				profile.State = state.State;
				profile.Running = state.Running;
				profile.Busy = state.Busy;
				await SettleAsync(window);
				Require(ButtonText(action) == state.Label, "The actual tunnel action must use uppercase in state " + state.State + ".");
			}
		} finally {
			profile.State = original.State;
			profile.Running = original.Running;
			profile.Busy = original.Busy;
		}

		Console.WriteLine("PASS UI typography: uppercase actions, dialogs and status; original tunnel-name case, keys, paths, editor content and raw diagnostics preserved.");
	}

	private static async Task SettleAsync(Window window) {
		for (int settleIndex = 0; settleIndex < 3; ++settleIndex) {
			window.UpdateLayout();
			await Dispatcher.Yield(DispatcherPriority.ApplicationIdle);
		}
	}

	private static void Require(bool condition, string message) {
		if (!condition) {
			throw new InvalidOperationException(message);
		}
	}
}
