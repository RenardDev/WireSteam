using System;
using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace WireSteam.UI.Controls;

// A timestamp is evidence of a byte delta, never a periodic idle heartbeat.
// Repeated observations extend freshness without restarting the pulse phase.
internal sealed class TrafficPulse {
	internal const double LitMilliseconds = 80, CycleMilliseconds = 240, FreshMilliseconds = 1250;
	private long lastActivity, startedAt;

	internal void Observe(long activityAt, long frequency) {
		if (activityAt <= 0) {
			lastActivity = startedAt = 0;
			return;
		}

		if (lastActivity <= 0 || activityAt < lastActivity || (activityAt - lastActivity) * 1000.0 / frequency >= FreshMilliseconds) {
			startedAt = activityAt;
		}

		lastActivity = activityAt;
	}

	internal bool IsFresh(long now, long frequency) => lastActivity > 0 && now >= lastActivity && (now - lastActivity) * 1000.0 / frequency < FreshMilliseconds;
	internal bool IsLit(long now, long frequency) => IsFresh(now, frequency) && (now - startedAt) * 1000.0 / frequency % CycleMilliseconds < LitMilliseconds;

	internal static void RunTests() {
		static void Require(bool value, string message) {
			if (!value) {
				throw new InvalidOperationException(message);
			}
		}

		var rx = new TrafficPulse();
		var tx = new TrafficPulse();
		rx.Observe(10000, 1000);
		Require(rx.IsLit(10000, 1000) && rx.IsLit(10079, 1000) && !rx.IsLit(10080, 1000) && !rx.IsLit(10239, 1000) && rx.IsLit(10240, 1000), "Traffic LEDs need short distinct pulses separated by a dark interval.");

		for (long now = 10000; now < 11000; now += 20) {
			rx.Observe(now, 1000);
		}

		Require(!rx.IsLit(11080, 1000) && rx.IsLit(11200, 1000), "Continuous byte observations must not restart the phase into a permanently lit LED.");
		Require(!tx.IsLit(11200, 1000), "Receive activity must not illuminate the independent send LED.");

		tx.Observe(11200, 1000);
		rx.Observe(0, 1000);
		Require(!rx.IsLit(11200, 1000) && tx.IsLit(11200, 1000), "Idle/disconnected activity must clear immediately without clearing another direction.");
		Require(tx.IsFresh(12449, 1000) && !tx.IsFresh(12450, 1000) && !tx.IsLit(12480, 1000), "Missing telemetry must expire activity without an idle heartbeat.");

		tx.Observe(15000, 1000);
		Require(tx.IsLit(15000, 1000) && !tx.IsLit(15080, 1000), "Traffic after an idle period must start a new finite pulse sequence.");

		Console.WriteLine("PASS traffic LEDs: 80 ms pulses/160 ms dark intervals, sustained traffic, independent RX/TX, immediate idle reset and 1.25 s freshness expiry.");
	}
}

public sealed class TrafficLed : Control {
	private static readonly Brush Dark = FrozenBrush(Color.FromRgb(52, 70, 82));
	private readonly TrafficPulse pulse = new();
	private readonly DispatcherTimer timer;
	public static readonly DependencyProperty ActivityAtProperty = DependencyProperty.Register(nameof(ActivityAt), typeof(long), typeof(TrafficLed), new FrameworkPropertyMetadata(0L, ActivityChanged));

	public long ActivityAt {
		get => (long)GetValue(ActivityAtProperty);
		set => SetValue(ActivityAtProperty, value);
	}

	internal bool IsLit {
		get;
		private set;
	}

	internal bool ClockRunning => timer.IsEnabled;

	static TrafficLed() {
		FocusableProperty.OverrideMetadata(typeof(TrafficLed), new FrameworkPropertyMetadata(false));
		IsHitTestVisibleProperty.OverrideMetadata(typeof(TrafficLed), new FrameworkPropertyMetadata(false));
		ForegroundProperty.OverrideMetadata(typeof(TrafficLed), new FrameworkPropertyMetadata(FrozenBrush(Color.FromRgb(111, 255, 199)), FrameworkPropertyMetadataOptions.Inherits | FrameworkPropertyMetadataOptions.AffectsRender));
	}

	public TrafficLed() {
		timer = new DispatcherTimer(TimeSpan.FromMilliseconds(40), DispatcherPriority.Render, (_, _) => RefreshAt(Stopwatch.GetTimestamp()), Dispatcher);
		timer.Stop();
		Loaded += (_, _) => RefreshAt(Stopwatch.GetTimestamp());
		Unloaded += (_, _) => {
			timer.Stop();
			SetLit(false);
		};
		IsVisibleChanged += (_, _) => RefreshAt(Stopwatch.GetTimestamp());
	}

	private static Brush FrozenBrush(Color color) {
		var brush = new SolidColorBrush(color);
		brush.Freeze();

		return brush;
	}

	private static void ActivityChanged(DependencyObject target, DependencyPropertyChangedEventArgs eventArguments) {
		var led = (TrafficLed)target;
		led.pulse.Observe((long)eventArguments.NewValue, Stopwatch.Frequency);
		led.RefreshAt(Stopwatch.GetTimestamp());
	}

	internal void RefreshAt(long now) {
		bool active = IsLoaded && IsVisible && pulse.IsFresh(now, Stopwatch.Frequency);
		SetLit(active && pulse.IsLit(now, Stopwatch.Frequency));

		if (active) {
			timer.Start();
		} else {
			timer.Stop();
		}
	}

	private void SetLit(bool value) {
		if (IsLit != value) {
			IsLit = value;
			InvalidateVisual();
		}
	}

	protected override Size MeasureOverride(Size constraint) => new(double.IsPositiveInfinity(constraint.Width) ? 6 : Math.Min(6, Math.Max(0, constraint.Width)), double.IsPositiveInfinity(constraint.Height) ? 6 : Math.Min(6, Math.Max(0, constraint.Height)));

	protected override void OnRender(DrawingContext drawing) {
		base.OnRender(drawing);
		double radius = Math.Max(0, Math.Min(ActualWidth, ActualHeight) / 2);
		drawing.DrawEllipse(IsLit ? Foreground : Dark, null, new Point(ActualWidth / 2, ActualHeight / 2), radius, radius);
	}
}
