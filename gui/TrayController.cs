using System;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading.Tasks;
using Forms = System.Windows.Forms;

namespace WireSteam.UI;

/// <summary>Create, use and dispose this tray owner on the application's UI thread.</summary>
public sealed class TrayController : IDisposable {
	private readonly MainViewModel model;
	private readonly Action show;
	private readonly Func<Task> quit;
	private readonly Forms.NotifyIcon icon;
	private readonly Forms.ContextMenuStrip menu;
	private const Forms.TextFormatFlags MenuTextFlags = Forms.TextFormatFlags.SingleLine | Forms.TextFormatFlags.VerticalCenter |
		Forms.TextFormatFlags.NoPrefix | Forms.TextFormatFlags.NoPadding | Forms.TextFormatFlags.EndEllipsis |
		Forms.TextFormatFlags.PreserveGraphicsTranslateTransform;
	private static readonly Color MenuBackground = Color.FromArgb(16, 24, 36);
	private static readonly Color MenuText = Color.FromArgb(226, 239, 251);
	private static readonly Color MenuMuted = Color.FromArgb(112, 132, 156);
	private static readonly Color MenuSelected = Color.FromArgb(32, 60, 54);
	private static readonly Color MenuAccent = Color.FromArgb(111, 255, 199);
	private Icon? image;
	private string lastStatus = "";
	private bool disposed;
	public TrayController(MainViewModel model, Action show, Func<Task> quit, bool visible = true) {
		this.model = model;
		this.show = show;
		this.quit = quit;
		menu = new AlignedContextMenu();
		try {
			StyleMenu(menu);
			menu.Opening += MenuOpening;
			// NotifyIcon positions the menu before raising Opening. Its first
			// invocation must already have items, dimensions and a valid layout.
			RebuildMenu();
			icon = new Forms.NotifyIcon();
			icon.ContextMenuStrip = menu;
			icon.Visible = false;
			icon.Text = "WIRESTEAM";
			icon.MouseClick += IconMouseClick;
			model.PresentationChanged += ModelChanged;
			Refresh();
			icon.Visible = visible;
		} catch {
			disposed = true;
			model.PresentationChanged -= ModelChanged;
			try {
				icon?.Dispose();
			} finally {
				try {
					menu.Dispose();
				} finally {
					image?.Dispose();
				}
			}

			throw;
		}
	}

	private void ModelChanged(object? sender, EventArgs eventArguments) => Refresh();

	private void MenuOpening(object? sender, CancelEventArgs eventArguments) {
		if (disposed || menu.IsDisposed) {
			eventArguments.Cancel = true;
			return;
		}

		RebuildMenu();
		// WinForms pre-cancels Opening when DisplayedItems was empty. Filling
		// the menu in this callback does not clear that initial cancellation.
		eventArguments.Cancel = menu.Items.Count == 0;
	}

	private void IconMouseClick(object? sender, Forms.MouseEventArgs eventArguments) {
		// NotifyIcon opens ContextMenuStrip itself for the right button. Keep
		// that native foreground/dismissal behavior and handle only left click.
		if (eventArguments.Button == Forms.MouseButtons.Left) {
			ShowWindow();
		}
	}

	private void ShowWindow() {
		if (disposed) {
			return;
		}

		if (menu.Visible) {
			menu.Close();
		}

		show();
	}

	private void Refresh() {
		if (disposed) {
			return;
		}

		int active = model.Profiles.Count(profile => profile.Running);
		bool error = model.Profiles.Any(profile => profile.State is "Error" or "Cleanup required");
		string status = error ? "WIRESTEAM · ACTION REQUIRED" : active > 0 ? $"WIRESTEAM · {active} ACTIVE TUNNEL{(active == 1 ? "" : "S")}" : "WIRESTEAM · INACTIVE";
		if (status == lastStatus) {
			return;
		}

		var replacement = MakeIcon(error ? Color.FromArgb(255, 118, 143) : active > 0 ? Color.FromArgb(111, 255, 199) : Color.FromArgb(112, 132, 156));
		var old = image;
		image = replacement;
		try {
			icon.Text = status;
			icon.Icon = replacement;
			lastStatus = status;
		} finally {
			// NotifyIcon can retain the new icon before its native update throws.
			if (ReferenceEquals(icon.Icon, replacement)) {
				old?.Dispose();
			} else {
				image = old;
				replacement.Dispose();
			}
		}
	}

	private void RebuildMenu() {
		if (disposed || menu.IsDisposed) {
			return;
		}

		DisposeMenuItems(menu);
		var open = AddMenuItem(menu.Items, new TrayMenuItem("OPEN WIRESTEAM"));
		open.Click += (_, _) => ShowWindow();
		AddMenuItem(menu.Items, new TraySeparator());
		var tunnels = AddMenuItem(menu.Items, new TrayMenuItem("TUNNELS"));
		StyleMenu(tunnels.DropDown);
		foreach (var profile in model.Profiles) {
			var entry = AddMenuItem(tunnels.DropDownItems, new TrayMenuItem(profile.Name, profile.State.ToUpperInvariant()));
			entry.Checked = profile.Running;
			entry.Enabled = !model.IsQuitting && (!profile.Busy || (profile.Running && profile.State != "Stopping"));

			entry.Click += async (_, _) => {
				if (!disposed) {
					await model.ToggleAsync(profile);
				}
			};
		}

		if (tunnels.DropDownItems.Count == 0) {
			var empty = AddMenuItem(tunnels.DropDownItems, new TrayMenuItem("NO TUNNELS IMPORTED"));
			empty.Enabled = false;
		}

		var disconnect = AddMenuItem(menu.Items, new TrayMenuItem("DISCONNECT ALL"));
		disconnect.Enabled = !model.IsQuitting && model.Profiles.Any(profile => profile.Running || profile.State == "Cleanup required");
		disconnect.Click += async (_, _) => {
			if (disposed) {
				return;
			}

			try {
				await model.Backend.StopAllAsync();
			} catch (Exception error) {
				model.StatusMessage = error.Message;
				ShowWindow();
			}
		};

		AddMenuItem(menu.Items, new TraySeparator());
		var exit = AddMenuItem(menu.Items, new TrayMenuItem("QUIT WIRESTEAM"));
		exit.Enabled = !model.IsQuitting;
		exit.Click += async (_, _) => {
			if (!disposed) {
				await quit();
			}
		};

		LayoutMenu(tunnels.DropDown);
		LayoutMenu(menu);
	}

	private static TItem AddMenuItem<TItem>(Forms.ToolStripItemCollection items, TItem item) where TItem : Forms.ToolStripItem {
		try {
			items.Add(item);
		} catch {
			item.Dispose();
			throw;
		}

		return item;
	}

	private static void DisposeMenuItems(Forms.ToolStrip strip) {
		strip.SuspendLayout();
		try {
			while (strip.Items.Count > 0) {
				var item = strip.Items[0];
				// Disable descendants as well so a queued click on an old
				// profile cannot act after this menu snapshot is replaced.
				item.Enabled = false;

				if (item is Forms.ToolStripDropDownItem { HasDropDown: true } parent) {
					DisposeMenuItems(parent.DropDown);
				}

				// Native ToolStripItem.Dispose needs its Owner to complete
				// disposal and removes itself from that owner's collection.
				// Removing it first leaves its IsDisposed flag unset.
				item.Dispose();
			}
		} finally {
			strip.ResumeLayout(false);
		}
	}

	private static void StyleMenu(Forms.ToolStripDropDown strip) {
		strip.BackColor = MenuBackground;
		strip.ForeColor = MenuText;
		strip.Renderer = new TrayRenderer();

		if (strip is Forms.ToolStripDropDownMenu dropdown) {
			dropdown.ShowImageMargin = false;
			dropdown.ShowCheckMargin = false;
		}

		strip.ShowItemToolTips = true;
		strip.Opening += (_, _) => LayoutMenu(strip);
		strip.DpiChangedAfterParent += (_, _) => LayoutMenu(strip);
		strip.Disposed += (_, _) => (strip.Tag as MenuLayout)?.Dispose();
	}

	private sealed class TrayMenuItem : Forms.ToolStripMenuItem {
		public string Label {
			get;
		}

		public string Status {
			get;
		}

		public TrayMenuItem(string label, string status = "") : base(string.IsNullOrEmpty(status) ? label : label + " — " + status) {
			Label = label;
			Status = status;
			ToolTipText = string.IsNullOrEmpty(status) ? "" : Text;
		}

		protected override Forms.ToolStripDropDown CreateDefaultDropDown() => new AlignedDropDownMenu();

		protected override void SetBounds(Rectangle bounds) {
			// Native menu items subtract this padding from FlowLayout's X.
			// Compensate so all rows retain the shared outer gutter.
			if (Owner is Forms.ToolStripDropDownMenu dropdown) {
				bounds.X += dropdown.Padding.Left;
			}

			base.SetBounds(bounds);
		}
	}

	private sealed class TraySeparator : Forms.ToolStripItem {
		public TraySeparator() {
			AccessibleRole = Forms.AccessibleRole.Separator;
		}

		public override bool CanSelect => false;
		protected override void OnPaint(Forms.PaintEventArgs e) => DrawDivider(e.Graphics, this);
	}

	// WinForms recalculates Padding from DefaultPadding on every menu layout.
	// Override that source so native layout cannot reintroduce uneven gutters.
	private static Forms.Padding MenuPadding(Forms.ToolStripDropDown strip) => new(
		strip.Tag is MenuLayout layout ? layout.Outer : (int)Math.Round(6 * strip.DeviceDpi / 96f));
	private sealed class AlignedContextMenu : Forms.ContextMenuStrip {
		protected override Forms.Padding DefaultPadding => MenuPadding(this);
		// Exercise the native Opening event and its pre-cancelled empty state
		// without showing a popup or registering a visible notification icon.
		internal bool RaiseOpeningForTest() {
			var eventArguments = new CancelEventArgs(DisplayedItems.Count == 0);
			OnOpening(eventArguments);

			return !eventArguments.Cancel;
		}
	}

	private sealed class AlignedDropDownMenu : Forms.ToolStripDropDownMenu {
		protected override Forms.Padding DefaultPadding => MenuPadding(this);
	}

	private sealed class MenuLayout : IDisposable {
		public readonly float Scale;
		public readonly Font Font;
		public readonly int Outer, Edge, Mark, Gap, Arrow, StatusGap, StatusWidth, RowWidth, RowHeight, SeparatorHeight;
		private MenuLayout? previous;
		public MenuLayout(Forms.ToolStripDropDown strip, int dpi, Font? existingFont = null) {
			Scale = dpi / 96f;
			Font = existingFont ?? new Font("Consolas", 10f * 96 / 72 * Scale, FontStyle.Regular, GraphicsUnit.Pixel);
			try {
				Outer = ScalePixels(6);
				Edge = ScalePixels(12);
				Mark = ScalePixels(16);
				Gap = ScalePixels(10);
				Arrow = ScalePixels(12);
				StatusGap = ScalePixels(20);
				RowHeight = Math.Max(ScalePixels(34), Font.Height + ScalePixels(14));
				SeparatorHeight = ScalePixels(12);
				int labelWidth = ScalePixels(168);
				foreach (Forms.ToolStripItem item in strip.Items) {
					if (item is TraySeparator) {
						continue;
					}

					labelWidth = Math.Max(labelWidth, MeasureLabel(item is TrayMenuItem entry ? entry.Label : item.Text ?? "", Font));

					if (item is TrayMenuItem profile && profile.Status.Length > 0) {
						StatusWidth = Math.Max(StatusWidth, MeasureLabel(profile.Status, Font));
					}
				}

				labelWidth = Math.Min(labelWidth, ScalePixels(340));
				RowWidth = Math.Max(ScalePixels(264), Edge + Mark + Gap + labelWidth + (StatusWidth > 0 ? StatusGap + StatusWidth : 0) + Gap + Arrow + Edge);
			} catch {
				if (existingFont == null) {
					Font.Dispose();
				}

				throw;
			}
		}

		public int ScalePixels(float value) => (int)Math.Round(value * Scale);
		public Rectangle Check(Size row) => new(Edge, (row.Height - Mark) / 2, Mark, Mark);
		public Rectangle ArrowBox(Size row) => new(row.Width - Edge - Arrow, (row.Height - Mark) / 2, Arrow, Mark);
		public Rectangle Status(Size row) => new(ArrowBox(row).Left - Gap - StatusWidth, 0, StatusWidth, row.Height);
		public Rectangle Label(Size row) {
			int left = Edge + Mark + Gap;
			int right = StatusWidth > 0 ? Status(row).Left - StatusGap : ArrowBox(row).Left - Gap;

			return new Rectangle(left, 0, Math.Max(1, right - left), row.Height);
		}

		public void KeepPrevious(MenuLayout? layout) => previous = layout;

		public void ReleasePrevious() {
			previous?.DisposeExcept(Font);
			previous = null;
		}

		private void DisposeExcept(Font retainedFont) {
			try {
				if (!ReferenceEquals(Font, retainedFont)) {
					Font.Dispose();
				}
			} finally {
				previous?.DisposeExcept(retainedFont);
				previous = null;
			}
		}

		public void Dispose() {
			try {
				Font.Dispose();
			} finally {
				ReleasePrevious();
			}
		}
	}

	private static void LayoutMenu(Forms.ToolStripDropDown strip, int? dpi = null) {
		if (strip.IsDisposed) {
			return;
		}

		var previous = strip.Tag as MenuLayout;
		int targetDpi = dpi ?? strip.DeviceDpi;
		// WinForms retains the old reference for equal-valued font setters.
		// Reuse it at the same DPI rather than disposing a still-attached font.
		var layout = new MenuLayout(strip, targetDpi, previous?.Scale == targetDpi / 96f ? previous.Font : null);
		layout.KeepPrevious(previous);
		try {
			strip.SuspendLayout();
			try {
				strip.Tag = layout;
				strip.AutoSize = false;
				strip.Padding = new Forms.Padding(layout.Outer);
				strip.Font = layout.Font;
				int height = layout.Outer * 2;
				foreach (Forms.ToolStripItem item in strip.Items) {
					item.AutoSize = false;
					item.Margin = Forms.Padding.Empty;
					item.Padding = Forms.Padding.Empty;
					item.Font = layout.Font;
					item.TextAlign = ContentAlignment.MiddleLeft;
					item.Size = new Size(layout.RowWidth, item is TraySeparator ? layout.SeparatorHeight : layout.RowHeight);
					height += item.Height;
				}

				strip.Size = new Size(layout.RowWidth + layout.Outer * 2, height);
			} finally {
				strip.ResumeLayout(true);
			}
		} catch {
			// Once published, the layout retains every font that a partial update
			// may still borrow. A later successful layout or menu disposal closes them.
			if (!ReferenceEquals(strip.Tag, layout)) {
				layout.KeepPrevious(null);

				if (!ReferenceEquals(layout.Font, previous?.Font)) {
					layout.Dispose();
				}
			}

			throw;
		}

		layout.ReleasePrevious();
	}

	private static MenuLayout? LayoutOf(Forms.ToolStripItem? item) => item?.Owner?.Tag as MenuLayout;
	private static int MeasureLabel(string text, Font font) => Forms.TextRenderer.MeasureText(text, font, new Size(32767, 32767), MenuTextFlags & ~Forms.TextFormatFlags.EndEllipsis).Width;

	private static void DrawDivider(Graphics graphics, Forms.ToolStripItem item) {
		using var pen = new Pen(Color.FromArgb(42, 60, 80));
		int y = item.Height / 2, edge = LayoutOf(item)?.Edge ?? 12;
		graphics.DrawLine(pen, edge, y, Math.Max(edge, item.Width - edge), y);
	}

	public void NotifyHidden() {
		if (!disposed) {
			icon.ShowBalloonTip(2500, "WIRESTEAM IS RUNNING IN THE TRAY", "ACTIVE TUNNELS KEEP RUNNING. CLICK THE TRAY ICON TO REOPEN THE WINDOW.", Forms.ToolTipIcon.Info);
		}
	}

	internal static void RunTests(MainViewModel model) {
		using var tray = new TrayController(model, () => {
		}, () => Task.CompletedTask, false);
		TestFirstClickAndOpening(model);
		// Rebuild repeatedly, as Windows does each time the context menu opens.
		for (int rebuildIndex = 0; rebuildIndex < 3; ++rebuildIndex) {
			tray.RebuildMenu();
		}

		var tunnels = tray.menu.Items.OfType<Forms.ToolStripMenuItem>().Single(item => item.Text == "TUNNELS");
		if (tunnels.DropDownItems.Count != model.Profiles.Count || !tray.menu.Items.Cast<Forms.ToolStripItem>().Any(item => item.Text == "QUIT WIRESTEAM")) {
			throw new InvalidOperationException("The system tray menu is incomplete.");
		}

		foreach (var profile in model.Profiles) {
			if (!tunnels.DropDownItems.Cast<Forms.ToolStripItem>().Any(item => item.Text == profile.Name + " — " + profile.State.ToUpperInvariant())) {
				throw new InvalidOperationException("Tray tunnel names must retain their case, while states remain uppercase.");
			}
		}

		if (tray.icon.Text != tray.icon.Text.ToUpperInvariant()) {
			throw new InvalidOperationException("The tray tooltip must be uppercase.");
		}

		if (tray.menu.Renderer is not TrayRenderer || tunnels.DropDown.Renderer is not TrayRenderer) {
			throw new InvalidOperationException("The tray and nested profile menu must share the dark renderer.");
		}

		TestMenuRendering(tray.menu, tunnels.DropDown);
		Console.WriteLine("PASS tray alignment: real root/profile menu rows, shared check/name/status/arrow columns, symmetric edges, long names and repeated 100/125/150/200% layouts.");
		foreach (var color in new[] { Color.FromArgb(111, 255, 199), Color.FromArgb(112, 132, 156), Color.FromArgb(255, 118, 143) }) {
			using (var generated = MakeIcon(color)) {
				if (generated.Width < 16 || generated.Height < 16) {
					throw new InvalidOperationException("The tray icon has no usable frame.");
				}

				using var rendered = generated.ToBitmap();
				float diameter = Math.Max(5, rendered.Width * .28f), border = Math.Max(1, rendered.Width / 24f);
				int center = (int)Math.Round(rendered.Width - diameter / 2 - border);
				if (rendered.GetPixel(center, center).ToArgb() != color.ToArgb()) {
					throw new InvalidOperationException("The tray status badge lost its state color.");
				}
			}
		}

		model.IsQuitting = true;
		tray.RebuildMenu();
		tunnels = tray.menu.Items.OfType<Forms.ToolStripMenuItem>().Single(item => item.Text == "TUNNELS");
		if (tunnels.DropDownItems.Cast<Forms.ToolStripItem>().Any(item => item.Enabled)) {
			throw new InvalidOperationException("Tray activation remained enabled during shutdown.");
		}

		model.IsQuitting = false;
	}

	private static Icon MakeIcon(Color color) {
		// Reuse the generated app artwork; only the status badge is drawn at runtime.
		var resource = System.Windows.Application.GetResourceStream(new Uri("/Assets/WireSteam.generated.png", UriKind.Relative))
			?? throw new InvalidOperationException("The generated WireSteam artwork is missing.");
		using var sourceStream = resource.Stream;
		using var source = new Bitmap(sourceStream);
		int[] sizes = [16, 20, 24, 32, 40, 48, 64];
		byte[][] frames = sizes.Select(size => RenderIconFrame(source, size, color)).ToArray();

		using var stream = new MemoryStream();

		using (var writer = new BinaryWriter(stream, System.Text.Encoding.UTF8, true)) {
			writer.Write((ushort)0x00);
			writer.Write((ushort)0x01);
			writer.Write((ushort)sizes.Length);
			int offset = 0x06 + 0x10 * sizes.Length;
			for (int sizeIndex = 0; sizeIndex < sizes.Length; ++sizeIndex) {
				writer.Write((byte)sizes[sizeIndex]);
				writer.Write((byte)sizes[sizeIndex]);
				writer.Write((byte)0x00);
				writer.Write((byte)0x00);
				writer.Write((ushort)1);
				writer.Write((ushort)32);
				writer.Write(frames[sizeIndex].Length);
				writer.Write(offset);
				offset += frames[sizeIndex].Length;
			}

			foreach (byte[] frame in frames) {
				writer.Write(frame);
			}
		}

		stream.Position = 0x00;
		using var temporary = new Icon(stream, Forms.SystemInformation.SmallIconSize);

		return (Icon)temporary.Clone();
	}

	private static byte[] RenderIconFrame(Bitmap source, int size, Color status) {
		using var bitmap = new Bitmap(size, size);
		using (var graphics = Graphics.FromImage(bitmap)) {
			graphics.Clear(Color.Transparent);
			graphics.CompositingMode = CompositingMode.SourceCopy;
			graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
			graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
			float scale = Math.Min(size / (float)source.Width, size / (float)source.Height);
			float width = source.Width * scale, height = source.Height * scale;
			graphics.DrawImage(source, new RectangleF((size - width) / 2, (size - height) / 2, width, height));
			graphics.CompositingMode = CompositingMode.SourceOver;
			graphics.SmoothingMode = SmoothingMode.AntiAlias;
			float diameter = Math.Max(5, size * .28f);
			float border = Math.Max(1, size / 24f);
			var badge = new RectangleF(size - diameter - border, size - diameter - border, diameter, diameter);
			using var edge = new SolidBrush(Color.FromArgb(9, 13, 21));
			graphics.FillEllipse(edge, badge.X - border, badge.Y - border, diameter + 2 * border, diameter + 2 * border);
			using var dot = new SolidBrush(status);
			graphics.FillEllipse(dot, badge);
		}

		using var output = new MemoryStream();

		bitmap.Save(output, System.Drawing.Imaging.ImageFormat.Png);

		return output.ToArray();
	}

	public void Dispose() {
		if (disposed) {
			return;
		}

		disposed = true;
		try {
			model.PresentationChanged -= ModelChanged;
			icon.MouseClick -= IconMouseClick;
			icon.ContextMenuStrip = null;
			icon.Visible = false;

			if (!menu.IsDisposed) {
				menu.Close();
			}

			if (!menu.IsDisposed) {
				DisposeMenuItems(menu);
			}
		} finally {
			try {
				icon.Dispose();
			} finally {
				try {
					menu.Dispose();
				} finally {
					var old = image;
					image = null;
					old?.Dispose();
				}
			}
		}
	}

	private static void TestFirstClickAndOpening(MainViewModel model) {
		int shown = 0;
		using var fixture = new TrayController(model, () => ++shown, () => Task.CompletedTask, false);
		var context = (AlignedContextMenu)fixture.menu;
		if (context.Items.Count == 0 || context.Width <= 0 || context.Height <= 0) {
			throw new InvalidOperationException("The tray menu must be populated and laid out before its first click.");
		}

		if (!context.Items.OfType<TrayMenuItem>().Select(item => item.Label).SequenceEqual(new[] { "OPEN WIRESTEAM", "TUNNELS", "DISCONNECT ALL", "QUIT WIRESTEAM" })) {
			throw new InvalidOperationException("The tray must expose window, tunnel and shutdown actions in the expected order.");
		}

		var original = context.Items.OfType<TrayMenuItem>().Single(item => item.Label == "TUNNELS");
		var originalOpen = context.Items.OfType<TrayMenuItem>().Single(item => item.Label == "OPEN WIRESTEAM");
		var originalDropDown = original.DropDown;
		var originalProfiles = originalDropDown.Items.Cast<Forms.ToolStripItem>().ToArray();
		bool firstOpening = context.RaiseOpeningForTest();
		if (!firstOpening || !original.IsDisposed || !originalDropDown.IsDisposed) {
			throw new InvalidOperationException($"The first tray opening must succeed and release the previous profile dropdown: opening={firstOpening}, itemDisposed={original.IsDisposed}, dropdownDisposed={originalDropDown.IsDisposed}.");
		}

		if (!originalOpen.IsDisposed || originalOpen.Enabled || originalProfiles.Any(item => !item.IsDisposed || item.Enabled)) {
			throw new InvalidOperationException("Replaced tray items must be disposed while owned, and stale profile actions must be disabled.");
		}

		originalOpen.PerformClick();
		foreach (var item in originalProfiles) {
			item.PerformClick();
		}

		if (shown != 0) {
			throw new InvalidOperationException("A replaced tray menu retained a live action.");
		}

		// Reproduce WinForms' original first-open cancellation, including a
		// fresh empty DisplayedItems collection, rather than calling Rebuild
		// directly as the earlier tray tests did.
		DisposeMenuItems(context);
		context.PerformLayout();

		if (!context.RaiseOpeningForTest() || context.Items.Count == 0 || !context.RaiseOpeningForTest()) {
			throw new InvalidOperationException("A freshly populated tray menu retained WinForms' initial empty-menu cancellation.");
		}

		// Raise NotifyIcon's actual managed event so losing the subscription
		// would fail this test. No native tray icon, input or popup is used.
		var raiseClick = typeof(Forms.NotifyIcon).GetMethod("OnMouseClick", BindingFlags.Instance | BindingFlags.NonPublic)
			?? throw new InvalidOperationException("The NotifyIcon click-event test hook is unavailable.");
		void Click(Forms.MouseButtons button) => raiseClick.Invoke(fixture.icon, [new Forms.MouseEventArgs(button, 1, 0, 0, 0)]);
		Click(Forms.MouseButtons.Left);

		if (shown != 1) {
			throw new InvalidOperationException("The first left tray click must immediately reopen the window.");
		}

		Click(Forms.MouseButtons.Right);
		Click(Forms.MouseButtons.Middle);

		if (shown != 1) {
			throw new InvalidOperationException("A tray context-menu click must not activate the main window.");
		}

		Click(Forms.MouseButtons.Left);

		if (shown != 2) {
			throw new InvalidOperationException("Repeated left tray clicks must remain responsive.");
		}

		fixture.Dispose();
		fixture.Dispose();
		Click(Forms.MouseButtons.Left);
		fixture.Refresh();
		fixture.NotifyHidden();

		if (shown != 2 || !context.IsDisposed || !originalDropDown.IsDisposed || fixture.icon.ContextMenuStrip != null) {
			throw new InvalidOperationException("A disposed tray controller retained an active icon or stale menu callback.");
		}

		Console.WriteLine("PASS first tray interaction: prepared menu, native Opening cancellation reset, immediate left click, right-click isolation and idempotent dropdown/icon disposal.");
	}

	private static void TestMenuRendering(Forms.ToolStripDropDown root, Forms.ToolStripDropDown profiles) {
		using var fixture = new AlignedDropDownMenu();
		StyleMenu(fixture);
		var active = AddMenuItem(fixture.Items, new TrayMenuItem("Short", "ACTIVE"));
		active.Checked = true;
		AddMenuItem(fixture.Items, new TrayMenuItem("MiXeD & Unicode Ω profile with an intentionally very long name", "INACTIVE"));
		var disabled = AddMenuItem(fixture.Items, new TrayMenuItem("Disabled profile", "CLEANUP REQUIRED"));
		disabled.Enabled = false;
		string? preview = Environment.GetEnvironmentVariable("WIRESTEAM_TRAY_RENDER");
		foreach (int dpi in new[] { 96, 96, 120, 120, 144, 192, 192 }) {
			using var first = RenderMenu(root, dpi);
			using var second = RenderMenu(profiles, dpi);
			using var third = RenderMenu(fixture, dpi);
			if (!string.IsNullOrWhiteSpace(preview)) {
				using var bitmap = new Bitmap(first.Width + second.Width + third.Width + 32, Math.Max(first.Height, Math.Max(second.Height, third.Height)));
				bitmap.SetResolution(dpi, dpi);
				using var graphics = Graphics.FromImage(bitmap);
				graphics.Clear(Color.FromArgb(9, 13, 21));
				graphics.DrawImageUnscaled(first, 0, 0);
				graphics.DrawImageUnscaled(second, first.Width + 16, 0);
				graphics.DrawImageUnscaled(third, first.Width + second.Width + 32, 0);
				string path = Path.Combine(Path.GetDirectoryName(preview)!, Path.GetFileNameWithoutExtension(preview) + "-" + dpi + "dpi.png");
				bitmap.Save(path, System.Drawing.Imaging.ImageFormat.Png);
			}
		}

		LayoutMenu(root);
		LayoutMenu(profiles);
	}

	private static Bitmap RenderMenu(Forms.ToolStripDropDown strip, int dpi) {
		// Render the actual native menu controls and their actual item bounds,
		// without displaying a popup or moving the user's pointer.
		_ = strip.Handle;
		LayoutMenu(strip, dpi);
		strip.PerformLayout();
		var layout = (MenuLayout)strip.Tag!;
		int top = layout.Outer;
		foreach (Forms.ToolStripItem item in strip.Items) {
			if (item.Bounds.Left != layout.Outer || item.Bounds.Right != strip.ClientSize.Width - layout.Outer || item.Bounds.Top != top) {
				throw new InvalidOperationException($"Tray menu edges are uneven at {dpi} DPI: {item.Bounds}, client={strip.ClientSize}, outer={layout.Outer}, expected top={top}.");
			}

			top = item.Bounds.Bottom;
		}

		if (top != strip.ClientSize.Height - layout.Outer) {
			throw new InvalidOperationException("Tray top and bottom spacing must match.");
		}

		var items = strip.Items.Cast<Forms.ToolStripItem>().Where(item => item is not TraySeparator).ToArray();
		if (items.Length > 0) {
			items[0].Select();
		}

		foreach (var item in items) {
			var label = layout.Label(item.Size);
			var status = layout.Status(item.Size);
			var arrow = layout.ArrowBox(item.Size);
			if (item.Width != layout.RowWidth || item.Height != layout.RowHeight || item.Bounds.Left != layout.Outer ||
				label.Left != layout.Edge + layout.Mark + layout.Gap || label.Right > arrow.Left - layout.Gap ||
				layout.Check(item.Size).Top + layout.Mark / 2 != item.Height / 2 ||
				item is TrayMenuItem profile && profile.Status.Length > 0 && (label.Right + layout.StatusGap > status.Left || status.Right != arrow.Left - layout.Gap)) {
				throw new InvalidOperationException($"Tray alignment at {dpi} DPI: bounds={item.Bounds}, expected row={layout.RowWidth}x{layout.RowHeight} x={layout.Outer}; label={label}; status={status}; arrow={arrow}; check={layout.Check(item.Size)}.");
			}
		}

		var bitmap = new Bitmap(strip.Width, strip.Height);
		try {
			bitmap.SetResolution(dpi, dpi);
			strip.DrawToBitmap(bitmap, new Rectangle(Point.Empty, strip.Size));
			for (int x = 0; x < bitmap.Width; ++x) {
				for (int y = 0; y < bitmap.Height; ++y) {
					if (bitmap.GetPixel(x, y).ToArgb() == Color.White.ToArgb()) {
						throw new InvalidOperationException("A system white selection or check background leaked into the tray menu.");
					}
				}
			}
		} catch {
			bitmap.Dispose();
			throw;
		}

		return bitmap;
	}

	private sealed class TrayRenderer : Forms.ToolStripProfessionalRenderer {
		public TrayRenderer() : base(new Forms.ProfessionalColorTable { UseSystemColors = false }) {
			RoundedEdges = false;
		}

		protected override void OnRenderToolStripBackground(Forms.ToolStripRenderEventArgs e) {
			using var brush = new SolidBrush(MenuBackground);
			e.Graphics.FillRectangle(brush, e.ToolStrip.ClientRectangle);
		}

		protected override void OnRenderImageMargin(Forms.ToolStripRenderEventArgs e) {
		}

		protected override void OnRenderToolStripBorder(Forms.ToolStripRenderEventArgs e) {
			using var pen = new Pen(Color.FromArgb(42, 65, 78));
			e.Graphics.DrawRectangle(pen, 0, 0, Math.Max(0, e.ToolStrip.Width - 1), Math.Max(0, e.ToolStrip.Height - 1));
		}

		protected override void OnRenderMenuItemBackground(Forms.ToolStripItemRenderEventArgs e) {
			using var background = new SolidBrush(MenuBackground);
			e.Graphics.FillRectangle(background, new Rectangle(Point.Empty, e.Item.Size));

			if (e.Item.Enabled && (e.Item.Selected || e.Item.Pressed)) {
				using var selected = new SolidBrush(MenuSelected);
				e.Graphics.FillRectangle(selected, new Rectangle(2, 1, Math.Max(0, e.Item.Width - 4), Math.Max(0, e.Item.Height - 2)));
			}

			if (e.Item is Forms.ToolStripMenuItem { Checked: true } && LayoutOf(e.Item) is { } layout) {
				DrawCheck(e.Graphics, e.Item, layout.Check(e.Item.Size), layout.Scale);
			}
		}

		protected override void OnRenderItemText(Forms.ToolStripItemTextRenderEventArgs e) {
			e.TextColor = !e.Item.Enabled ? MenuMuted : e.Item.Selected || e.Item.Pressed ? MenuAccent : MenuText;

			if (LayoutOf(e.Item) is not { } layout) {
				return;
			}

			var profile = e.Item as TrayMenuItem;
			Forms.TextRenderer.DrawText(e.Graphics, profile?.Label ?? e.Text, layout.Font, layout.Label(e.Item.Size), e.TextColor, MenuTextFlags | Forms.TextFormatFlags.Left);

			if (profile is { Status.Length: > 0 }) {
				Forms.TextRenderer.DrawText(e.Graphics, profile.Status, layout.Font, layout.Status(e.Item.Size), e.TextColor, MenuTextFlags | Forms.TextFormatFlags.Right);
			}
		}

		protected override void OnRenderItemCheck(Forms.ToolStripItemImageRenderEventArgs e) {
		}

		private static void DrawCheck(Graphics graphics, Forms.ToolStripItem item, Rectangle bounds, float scale) {
			var state = graphics.Save();
			try {
				graphics.SmoothingMode = SmoothingMode.AntiAlias;
				using var pen = new Pen(item.Enabled ? MenuAccent : MenuMuted, 1.8f * scale);
				pen.StartCap = LineCap.Round;
				pen.EndCap = LineCap.Round;
				graphics.DrawLines(pen, new[] { new PointF(bounds.Left + bounds.Width * .18f, bounds.Top + bounds.Height * .52f), new PointF(bounds.Left + bounds.Width * .43f, bounds.Top + bounds.Height * .76f), new PointF(bounds.Left + bounds.Width * .84f, bounds.Top + bounds.Height * .25f) });
			} finally {
				graphics.Restore(state);
			}
		}

		protected override void OnRenderArrow(Forms.ToolStripArrowRenderEventArgs e) {
			var state = e.Graphics.Save();
			try {
				e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
				var layout = LayoutOf(e.Item);
				var bounds = layout != null && e.Item != null ? layout.ArrowBox(e.Item.Size) : e.ArrowRectangle;
				float scale = layout?.Scale ?? 1;
				float x = bounds.Left + bounds.Width / 2f, y = bounds.Top + bounds.Height / 2f;
				float direction = e.Direction == Forms.ArrowDirection.Left ? -1 : 1;
				using var pen = new Pen(e.Item?.Enabled == false ? MenuMuted : e.Item?.Selected == true ? MenuAccent : MenuText, 1.5f * scale);
				e.Graphics.DrawLines(pen, new[] { new PointF(x - direction * 2 * scale, y - 4 * scale), new PointF(x + direction * 2 * scale, y), new PointF(x - direction * 2 * scale, y + 4 * scale) });
			} finally {
				e.Graphics.Restore(state);
			}
		}

		protected override void OnRenderSeparator(Forms.ToolStripSeparatorRenderEventArgs e) {
			DrawDivider(e.Graphics, e.Item);
		}
	}
}
