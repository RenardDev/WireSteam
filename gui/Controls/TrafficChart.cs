using System;
using System.Collections.Generic;
using System.Collections.Specialized;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Automation.Peers;
using System.Windows.Media;

namespace WireSteam.UI.Controls;

public sealed class TrafficChart : FrameworkElement {
	public static readonly DependencyProperty PointsProperty = DependencyProperty.Register(nameof(Points), typeof(IEnumerable<TrafficPoint>), typeof(TrafficChart), new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender, PointsChanged));

	private static readonly Brush GridBrush = Solid(29, 42, 58);
	private static readonly Brush LabelBrush = Solid(146, 170, 192);
	private static readonly Brush ReceiveBrush = Solid(111, 255, 199);
	private static readonly Brush SendBrush = Solid(106, 203, 255);
	private static readonly Pen GridPen = FrozenPen(GridBrush, 0.7);
	private static readonly Typeface ChartTypeface = new(new FontFamily("Cascadia Code, Consolas"), FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);

	public IEnumerable<TrafficPoint>? Points {
		get => (IEnumerable<TrafficPoint>?)GetValue(PointsProperty);
		set => SetValue(PointsProperty, value);
	}

	public TrafficChart() {
		SnapsToDevicePixels = true;
		ClipToBounds = true;
	}

	private static void PointsChanged(DependencyObject sender, DependencyPropertyChangedEventArgs eventArguments) {
		var chart = (TrafficChart)sender;
		if (eventArguments.OldValue is INotifyCollectionChanged oldCollection) {
			CollectionChangedEventManager.RemoveHandler(oldCollection, chart.CollectionChanged);
		}

		if (eventArguments.NewValue is INotifyCollectionChanged newCollection) {
			CollectionChangedEventManager.AddHandler(newCollection, chart.CollectionChanged);
		}
	}

	private void CollectionChanged(object? sender, NotifyCollectionChangedEventArgs eventArguments) {
		if (Dispatcher.CheckAccess()) {
			InvalidateVisual();
		} else {
			Dispatcher.BeginInvoke(new Action(InvalidateVisual));
		}
	}

	protected override AutomationPeer OnCreateAutomationPeer() => new FrameworkElementAutomationPeer(this);

	protected override void OnRender(DrawingContext context) {
		base.OnRender(context);

		if (ActualWidth < 100 || ActualHeight < 60) {
			return;
		}

		var plot = new Rect(52, 11, Math.Max(1, ActualWidth - 66), Math.Max(1, ActualHeight - 43));
		TrafficPoint[] values;
		try {
			values = Points?.TakeLast(90).ToArray() ?? Array.Empty<TrafficPoint>();
		} catch (InvalidOperationException) {
			return;
		}

		double maximum = values.Length == 0 ? 1 : values.Max(point => Math.Max(Clean(point.ReceiveMbps), Clean(point.SendMbps)));
		maximum = NiceMaximum(Math.Max(1, maximum * 1.12));
		for (int row = 0; row <= 3; ++row) {
			double y = plot.Top + plot.Height * row / 3;
			context.DrawLine(GridPen, new Point(plot.Left, y), new Point(plot.Right, y));
			var tick = Text(AxisLabel(maximum * (3 - row) / 3), 11);
			context.DrawText(tick, new Point(plot.Left - 14 - tick.Width, y - 7));
		}

		for (int column = 1; column < 6; ++column) {
			double x = plot.Left + plot.Width * column / 6;
			context.DrawLine(GridPen, new Point(x, plot.Top), new Point(x, plot.Bottom));
		}

		Label(context, "HISTORY", new Point(plot.Left, plot.Bottom + 10));
		Label(context, "NOW", new Point(plot.Right - Text("NOW", 11).Width, plot.Bottom + 10));

		if (values.Length == 0) {
			var waiting = Text("WAITING FOR TRAFFIC", 13);
			context.DrawText(waiting, new Point(plot.Left + (plot.Width - waiting.Width) / 2, plot.Top + plot.Height / 2 - 8));
			return;
		}

		DrawSeries(context, values.Select(point => Clean(point.SendMbps)).ToArray(), plot, maximum, SendBrush, Color.FromRgb(106, 203, 255), 0.08);
		DrawSeries(context, values.Select(point => Clean(point.ReceiveMbps)).ToArray(), plot, maximum, ReceiveBrush, Color.FromRgb(111, 255, 199), 0.16);
	}

	private static double Clean(double value) => double.IsFinite(value) && value > 0 ? Math.Min(value, 1e9) : 0;

	private static string AxisLabel(double value) => value >= 1e6 ? (value / 1e6).ToString("0.#", CultureInfo.InvariantCulture) + "M"
		: value >= 1000 ? (value / 1000).ToString("0.#", CultureInfo.InvariantCulture) + "k"
		: value.ToString(value < 10 ? "0.0" : "0", CultureInfo.InvariantCulture);

	private static double NiceMaximum(double value) {
		var magnitude = Math.Pow(10, Math.Floor(Math.Log10(value)));
		var unit = value / magnitude;

		return (unit <= 1 ? 1 : unit <= 2 ? 2 : unit <= 5 ? 5 : 10) * magnitude;
	}

	private void DrawSeries(DrawingContext context, double[] values, Rect plot, double maximum, Brush line, Color color, double opacity) {
		var points = new Point[values.Length];
		for (int pointIndex = 0; pointIndex < points.Length; ++pointIndex) {
			points[pointIndex] = new Point(points.Length == 1 ? plot.Right : plot.Left + plot.Width * pointIndex / (points.Length - 1), plot.Bottom - Math.Min(1, values[pointIndex] / maximum) * plot.Height);
		}

		if (points.Length > 1) {
			var curve = Curve(points, false, plot.Bottom);
			var area = Curve(points, true, plot.Bottom);
			var fill = new LinearGradientBrush(Color.FromArgb((byte)(255 * opacity), color.R, color.G, color.B), Color.FromArgb(0, color.R, color.G, color.B), new Point(0, 0), new Point(0, 1));
			fill.Freeze();
			context.DrawGeometry(fill, null, area);
			context.PushOpacity(0.12);
			context.DrawGeometry(null, FrozenPen(line, 5), curve);
			context.Pop();
			context.DrawGeometry(null, FrozenPen(line, 1.65), curve);
		}

		context.DrawEllipse(line, null, points[^1], 2.6, 2.6);
	}

	private static StreamGeometry Curve(Point[] points, bool area, double baseline) {
		var geometry = new StreamGeometry();
		using (var path = geometry.Open()) {
			path.BeginFigure(area ? new Point(points[0].X, baseline) : points[0], area, area);

			if (area) {
				path.LineTo(points[0], true, false);
			}

			for (int pointIndex = 1; pointIndex < points.Length; ++pointIndex) {
				double span = (points[pointIndex].X - points[pointIndex - 1].X) / 3;
				path.BezierTo(new Point(points[pointIndex - 1].X + span, points[pointIndex - 1].Y), new Point(points[pointIndex].X - span, points[pointIndex].Y), points[pointIndex], true, false);
			}

			if (area) {
				path.LineTo(new Point(points[^1].X, baseline), true, false);
			}
		}

		geometry.Freeze();

		return geometry;
	}

	private FormattedText Text(string value, double size) => new(value, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, ChartTypeface, size, LabelBrush, VisualTreeHelper.GetDpi(this).PixelsPerDip);

	private void Label(DrawingContext context, string value, Point position, double size = 11) => context.DrawText(Text(value, size), position);

	private static Brush Solid(byte red, byte green, byte blue) {
		var brush = new SolidColorBrush(Color.FromRgb(red, green, blue));
		brush.Freeze();

		return brush;
	}

	private static Pen FrozenPen(Brush brush, double thickness) {
		var pen = new Pen(brush, thickness);
		pen.Freeze();

		return pen;
	}
}
