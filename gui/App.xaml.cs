using System;
using System.IO;
using System.Globalization;
using System.Linq;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using WireSteam.UI.Services;

namespace WireSteam.UI;

public partial class App : Application {
	private IBackend? backend;
	private MainViewModel? model;
	private MainWindow? window;
	private TrayController? tray;
	private Mutex? instance;
	private EventWaitHandle? showEvent;
	private RegisteredWaitHandle? showWait;
	private bool ownsMutex, notifiedHidden;
	public bool AllowClose {
		get;
		private set;
	}

	protected override async void OnStartup(StartupEventArgs args) {
		Console.OutputEncoding = new UTF8Encoding(false);
		CultureInfo.DefaultThreadCurrentUICulture = CultureInfo.GetCultureInfo("en-US");
		CultureInfo.DefaultThreadCurrentCulture = CultureInfo.GetCultureInfo("en-US");
		base.OnStartup(args);

		try {
			if (args.Args.Length > 0 && args.Args[0] == "--render-preview") {
				if (args.Args.Length is < 2 or > 3 || (args.Args.Length == 3 && args.Args[2] is not ("dashboard" or "logs" or "settings" or "empty"))) {
					throw new ArgumentException("Usage: WireSteam.UI.exe --render-preview <output.png> [dashboard|logs|settings|empty]");
				}

				RenderPreview(args.Args[1], args.Args.Length > 2 ? args.Args[2] : "dashboard");
				Shutdown(0);
				return;
			}

			if (args.Args.Contains("--self-test")) {
				await UiTests.RunAsync();
				string? enginePath = null;
				int option = Array.IndexOf(args.Args, "--engine");
				if (option >= 0 && option + 1 < args.Args.Length) {
					enginePath = args.Args[option + 1];
				}

				Console.WriteLine(await Backend.RunTestsAsync(enginePath));
				Console.WriteLine("WireSteam UI self-tests passed.");
				Shutdown(0);
				return;
			}

			if (args.Args.Any(argument => argument != "--tray")) {
				throw new ArgumentException("Usage: WireSteam.UI.exe [--tray]");
			}

			if (!AcquireInstance()) {
				Shutdown(0);
				return;
			}

			backend = new Backend();
			model = new MainViewModel(backend);
			window = new MainWindow(model);
			MainWindow = window;
			tray = new TrayController(model, ShowWindow, QuitAsync);
			window.StateChanged += (_, _) => {
				if (window.WindowState == WindowState.Minimized) {
					HideToTray();
				}
			};

			if (!args.Args.Contains("--tray")) {
				ShowWindow();
			}

			await model.InitializeAsync();
		} catch (Exception error) {
			Console.Error.WriteLine(args.Args.Contains("--self-test") ? error.ToString() : error.Message);

			if (!args.Args.Contains("--self-test") && !args.Args.Contains("--render-preview")) {
				MessageBox.Show(error.Message, "WIRESTEAM COULD NOT START", MessageBoxButton.OK, MessageBoxImage.Error);
			}

			AllowClose = true;
			Shutdown(1);
		}
	}

	private bool AcquireInstance() {
		string user;
		using (WindowsIdentity identity = WindowsIdentity.GetCurrent()) {
			user = identity.User?.Value ?? Environment.UserName;
		}

		string suffix = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(user)))[..20];
		instance = new Mutex(true, "Local\\WireSteamUI-" + suffix, out ownsMutex);

		if (!ownsMutex) {
			// A second launch only requests that the existing window be shown.
			try {
				using var existing = EventWaitHandle.OpenExisting("Local\\WireSteamUI.Show-" + suffix);
				existing.Set();
			} catch (WaitHandleCannotBeOpenedException) {
			}

			return false;
		}

		showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, "Local\\WireSteamUI.Show-" + suffix);
		showWait = ThreadPool.RegisterWaitForSingleObject(showEvent, (_, _) => Dispatcher.BeginInvoke(ShowWindow), null, Timeout.Infinite, false);

		return true;
	}

	public void ShowWindow() {
		if (window == null || AllowClose || Dispatcher.HasShutdownStarted) {
			return;
		}

		// Restore before showing a hidden minimized window, so the minimize
		// handler cannot immediately hide it again during the first tray click.
		if (window.WindowState == WindowState.Minimized) {
			window.WindowState = WindowState.Normal;
		}

		window.Show();
		window.Activate();
	}

	public void HideToTray() {
		window?.Hide();

		if (!notifiedHidden) {
			notifiedHidden = true;
			tray?.NotifyHidden();
		}
	}

	public async Task QuitAsync() {
		if (model == null || model.IsQuitting) {
			return;
		}

		model.IsQuitting = true;

		if (window != null) {
			window.IsEnabled = false;
		}

		model.StatusMessage = "Stopping tunnels and removing managed adapters…";
		try {
			if (backend != null) {
				await backend.StopAllAsync();
			}

			AllowClose = true;
			tray?.Dispose();
			tray = null;
			window?.Close();
			Shutdown(0);
		} catch (Exception error) {
			model.IsQuitting = false;

			if (window != null) {
				window.IsEnabled = true;
			}

			model.StatusMessage = "Could not finish cleanup: " + error.Message;
			ShowWindow();
			MessageBox.Show(window, "COULD NOT FINISH CLEANUP: " + error.Message, "WIRESTEAM IS STILL RUNNING", MessageBoxButton.OK, MessageBoxImage.Information);
		}
	}

	private void RenderPreview(string destination, string page) {
		using var previewBackend = new PreviewBackend();
		var preview = new MainViewModel(previewBackend);
		if (page != "empty") {
			preview.LoadPreview();
		} else {
			preview.EngineStatus = "DESIGN PREVIEW · NO TUNNELS";
			preview.StatusMessage = "Offline visual preview. No adapter or connection has been created.";
		}

		if (page is "logs" or "settings") {
			preview.Show(char.ToUpperInvariant(page[0]) + page[1..]);
		}

		var previewWindow = new MainWindow(preview, true) { Width = 1280, Height = 820 };
		var content = (FrameworkElement)previewWindow.Content;
		content.Measure(new Size(1280, 820));
		content.Arrange(new Rect(0, 0, 1280, 820));
		content.UpdateLayout();
		var pixels = new RenderTargetBitmap(1280, 820, 96, 96, PixelFormats.Pbgra32);
		pixels.Render(content);
		string path = Path.GetFullPath(destination);
		Directory.CreateDirectory(Path.GetDirectoryName(path)!);

		using var output = File.Create(path);

		var png = new PngBitmapEncoder();
		png.Frames.Add(BitmapFrame.Create(pixels));
		png.Save(output);
		Console.WriteLine(path);
	}

	protected override void OnExit(ExitEventArgs args) {
		tray?.Dispose();
		backend?.Dispose();
		showWait?.Unregister(null);
		showEvent?.Dispose();

		if (ownsMutex) {
			instance?.ReleaseMutex();
		}

		instance?.Dispose();
		base.OnExit(args);
	}
}
