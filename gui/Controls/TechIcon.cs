using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace WireSteam.UI.Controls;

public enum TechIconKind {
	Brand,
	Shield,
	Overview,
	Logs,
	Settings,
	Download,
	Upload,
	Latency,
	Import,
	Add,
	External,
	Edit,
	Export,
	Search,
	Power,
	Link,
	Globe
}

/// <summary>A coherent set of font-independent, resolution-independent interface symbols.</summary>
public sealed class TechIcon : Control {
	private static readonly Brush Cyan = Solid(106, 203, 255);
	private static readonly Brush Mint = Solid(111, 255, 199);
	public static readonly DependencyProperty KindProperty = DependencyProperty.Register(nameof(Kind), typeof(TechIconKind), typeof(TechIcon), new FrameworkPropertyMetadata(TechIconKind.Brand, FrameworkPropertyMetadataOptions.AffectsRender));
	public static readonly DependencyProperty AccentProperty = DependencyProperty.Register(nameof(Accent), typeof(Brush), typeof(TechIcon), new FrameworkPropertyMetadata(Cyan, FrameworkPropertyMetadataOptions.AffectsRender));

	private static readonly IReadOnlyDictionary<TechIconKind, Artwork> Symbols = CreateSymbols();

	static TechIcon() {
		ForegroundProperty.OverrideMetadata(typeof(TechIcon), new FrameworkPropertyMetadata(Mint, FrameworkPropertyMetadataOptions.Inherits | FrameworkPropertyMetadataOptions.AffectsRender));
		FocusableProperty.OverrideMetadata(typeof(TechIcon), new FrameworkPropertyMetadata(false));
		IsHitTestVisibleProperty.OverrideMetadata(typeof(TechIcon), new FrameworkPropertyMetadata(false));
	}

	public TechIconKind Kind {
		get => (TechIconKind)GetValue(KindProperty);
		set => SetValue(KindProperty, value);
	}

	public Brush Accent {
		get => (Brush)GetValue(AccentProperty);
		set => SetValue(AccentProperty, value);
	}

	// FrameworkElement clips an oversized measure result to Width/Height, but then
	// arranges the element at its unclipped desired size. Returning 24 for a 14-DIP
	// icon therefore rendered a 24-DIP drawing behind a 14-DIP clip.
	protected override Size MeasureOverride(Size constraint) => new(
		double.IsPositiveInfinity(constraint.Width) ? 24 : Math.Min(24, Math.Max(0, constraint.Width)),
		double.IsPositiveInfinity(constraint.Height) ? 24 : Math.Min(24, Math.Max(0, constraint.Height)));

	protected override void OnRender(DrawingContext drawing) {
		base.OnRender(drawing);

		if (ActualWidth <= 0 || ActualHeight <= 0 || !Symbols.TryGetValue(Kind, out var artwork)) {
			return;
		}

		double scale = Math.Min(ActualWidth, ActualHeight) / 24;
		drawing.PushTransform(new TranslateTransform((ActualWidth - 24 * scale) / 2, (ActualHeight - 24 * scale) / 2));
		drawing.PushTransform(new ScaleTransform(scale, scale));
		var primary = Stroke(Foreground ?? Mint);
		var secondary = Stroke(Accent ?? Foreground ?? Mint);
		drawing.DrawGeometry(null, primary, artwork.Primary);

		if (artwork.Accent != null) {
			drawing.DrawGeometry(null, secondary, artwork.Accent);
		}

		drawing.Pop();
		drawing.Pop();
	}

	private static Pen Stroke(Brush brush) => new(brush, 1.6) {
		StartLineCap = PenLineCap.Round,
		EndLineCap = PenLineCap.Round,
		LineJoin = PenLineJoin.Round
	};

	private static SolidColorBrush Solid(byte red, byte green, byte blue) {
		var brush = new SolidColorBrush(Color.FromRgb(red, green, blue));
		brush.Freeze();

		return brush;
	}

	private sealed record Artwork(Geometry Primary, Geometry? Accent);

	private static Artwork Shape(string primary, string? accent = null) {
		Geometry primaryGeometry = Geometry.Parse(primary);
		primaryGeometry.Freeze();
		Geometry? accentGeometry = accent == null ? null : Geometry.Parse(accent);
		accentGeometry?.Freeze();

		return new Artwork(primaryGeometry, accentGeometry);
	}

	private static IReadOnlyDictionary<TechIconKind, Artwork> CreateSymbols() => new Dictionary<TechIconKind, Artwork> {
		[TechIconKind.Brand] = Shape("M12,2 L21,5 L19.5,14 Q18,19 12,22 Q6,19 4.5,14 L3,5 Z M7,8.5 L9.4,15.5 L12,11 L14.6,15.5 L17,8.5", "M6.1,8.5 A0.9,0.9 0 1 0 7.9,8.5 A0.9,0.9 0 1 0 6.1,8.5 M16.1,8.5 A0.9,0.9 0 1 0 17.9,8.5 A0.9,0.9 0 1 0 16.1,8.5 M12,16.8 L12,18.7"),
		[TechIconKind.Shield] = Shape("M12,2 L21,5 L19.5,14 Q18,19 12,22 Q6,19 4.5,14 L3,5 Z M7,8.5 L9.4,15.5 L12,11 L14.6,15.5 L17,8.5", "M6.1,8.5 A0.9,0.9 0 1 0 7.9,8.5 A0.9,0.9 0 1 0 6.1,8.5 M16.1,8.5 A0.9,0.9 0 1 0 17.9,8.5 A0.9,0.9 0 1 0 16.1,8.5 M12,16.8 L12,18.7"),
		[TechIconKind.Overview] = Shape("M3,3 L10,3 L10,10 L3,10 Z M14,3 L21,3 L21,10 L14,10 Z M3,14 L10,14 L10,21 L3,21 Z M14,14 L21,14 L21,21 L14,21 Z"),
		[TechIconKind.Logs] = Shape("M5,4 L19,4 Q21,4 21,6 L21,18 Q21,20 19,20 L5,20 Q3,20 3,18 L3,6 Q3,4 5,4 Z M7,9 L10,12 L7,15", "M13,15 L17,15"),
		[TechIconKind.Settings] = Shape("M6,3 L6,6 M6,12 L6,21 M12,3 L12,12 M12,18 L12,21 M18,3 L18,4 M18,10 L18,21", "M3,9 A3,3 0 1 0 9,9 A3,3 0 1 0 3,9 M9,15 A3,3 0 1 0 15,15 A3,3 0 1 0 9,15 M15,7 A3,3 0 1 0 21,7 A3,3 0 1 0 15,7"),
		[TechIconKind.Download] = Shape("M12,3 L12,15 M6,9 L12,15 L18,9 M4,16 L4,21 L20,21 L20,16"),
		[TechIconKind.Upload] = Shape("M12,15 L12,3 M6,9 L12,3 L18,9 M4,16 L4,21 L20,21 L20,16"),
		[TechIconKind.Latency] = Shape("M2,12 L6,12 L9,4 L15,20 L18,12 L22,12"),
		[TechIconKind.Import] = Shape("M10,4 L20,4 L20,20 L10,20 M3,12 L15,12 M11,8 L15,12 L11,16"),
		[TechIconKind.Add] = Shape("M5,12 L19,12 M12,5 L12,19"),
		[TechIconKind.External] = Shape("M10,4 L4,4 L4,20 L20,20 L20,14", "M13,3 L21,3 L21,11 M21,3 L11,13"),
		[TechIconKind.Edit] = Shape("M4,16 L4,21 L9,21 L21,9 L16,4 Z M13,7 L18,12", "M4,21 L9,21 M11,21 L21,21"),
		[TechIconKind.Export] = Shape("M14,4 L4,4 L4,20 L14,20 M10,12 L21,12 M17,8 L21,12 L17,16", "M19,4 L21,4 M19,20 L21,20"),
		[TechIconKind.Search] = Shape("M3,10 A7,7 0 1 0 17,10 A7,7 0 1 0 3,10 M15,15 L21,21", "M6,10 A4,4 0 0 1 10,6"),
		[TechIconKind.Power] = Shape("M6,5.5 A9,9 0 1 0 18,5.5", "M12,2 L12,12"),
		[TechIconKind.Link] = Shape("M9.5,14.5 L8,16 A3.5,3.5 0 0 1 3,11 L7,7 A3.5,3.5 0 0 1 12,7 M14.5,9.5 L16,8 A3.5,3.5 0 0 1 21,13 L17,17 A3.5,3.5 0 0 1 12,17", "M8,16 L16,8"),
		[TechIconKind.Globe] = Shape("M2.5,12 A9.5,9.5 0 1 0 21.5,12 A9.5,9.5 0 1 0 2.5,12 M2.5,12 L21.5,12 M12,2.5 C6,8 6,16 12,21.5 M12,2.5 C18,8 18,16 12,21.5", "M5,6.5 C9,9 15,9 19,6.5 M5,17.5 C9,15 15,15 19,17.5")
	};

	internal static void RunTests() {
		foreach (TechIconKind kind in Enum.GetValues<TechIconKind>()) {
			if (!Symbols.TryGetValue(kind, out var symbol) || symbol.Primary.IsEmpty() ||
				!symbol.Primary.IsFrozen || symbol.Primary.Bounds.Left < 0 || symbol.Primary.Bounds.Top < 0 ||
				symbol.Primary.Bounds.Right > 24 || symbol.Primary.Bounds.Bottom > 24) {
				throw new InvalidOperationException($"The {kind} vector icon is invalid.");
			}

			var strokeBounds = symbol.Primary.GetRenderBounds(Stroke(Mint));
			if (symbol.Accent != null) {
				strokeBounds.Union(symbol.Accent.GetRenderBounds(Stroke(Cyan)));
			}

			if (strokeBounds.Left < 0 || strokeBounds.Top < 0 || strokeBounds.Right > 24 || strokeBounds.Bottom > 24) {
				throw new InvalidOperationException($"The {kind} icon stroke extends beyond its artwork viewport: {strokeBounds}.");
			}

			foreach (int size in new[] { 14, 16, 20, 24, 26, 32, 48 }) {
				var control = new TechIcon { Kind = kind, Width = size, Height = size };
				control.Measure(new Size(size, size));
				control.Arrange(new Rect(0, 0, size, size));

				if (Math.Abs(control.ActualWidth - size) > 0.01 || Math.Abs(control.ActualHeight - size) > 0.01) {
					throw new InvalidOperationException($"The {kind} icon was arranged outside its {size}-DIP viewport.");
				}

				foreach (double dpi in new[] { 96.0, 120.0, 144.0, 168.0, 192.0 }) {
					int pixelsWide = (int)Math.Ceiling(size * dpi / 96);
					var bitmap = new System.Windows.Media.Imaging.RenderTargetBitmap(pixelsWide, pixelsWide, dpi, dpi, PixelFormats.Pbgra32);
					bitmap.Render(control);
					var pixels = new byte[pixelsWide * pixelsWide * 0x04];
					bitmap.CopyPixels(pixels, pixelsWide * 0x04, 0x00);

					bool visible = false;
					for (int alphaByteIndex = 0x03; alphaByteIndex < pixels.Length; alphaByteIndex += 0x04) {
						if (pixels[alphaByteIndex] != 0) {
							visible = true;
							break;
						}
					}

					if (!visible) {
						throw new InvalidOperationException($"The {kind} icon did not render at {size} DIPs / {dpi} DPI.");
					}
				}
			}
		}
	}
}
