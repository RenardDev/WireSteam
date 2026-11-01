using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.Specialized;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace WireSteam.UI.Controls;

/// <summary>A continuous, selectable log document that appends without replacing existing text.</summary>
public sealed class LogViewer : RichTextBox {
	public static readonly DependencyProperty EntriesProperty = DependencyProperty.Register(nameof(Entries), typeof(IEnumerable), typeof(LogViewer), new PropertyMetadata(null, EntriesChanged));
	public static readonly DependencyProperty FollowTailProperty = DependencyProperty.Register(nameof(FollowTail), typeof(bool), typeof(LogViewer), new PropertyMetadata(true, FollowTailChanged));

	private static readonly Brush TimestampBrush = FrozenBrush("#7D91A8");
	private static readonly Brush MessageBrush = FrozenBrush("#B2C1D2");
	private static readonly Brush ErrorBrush = FrozenBrush("#FF768F");
	private static readonly Brush WarningBrush = FrozenBrush("#F8C46D");
	private static readonly Brush InformationBrush = FrozenBrush("#8FA5BD");
	private readonly List<LogEntry> rendered = new();
	private INotifyCollectionChanged? observed;
	private bool pendingScroll;

	public IEnumerable? Entries {
		get => (IEnumerable?)GetValue(EntriesProperty);
		set => SetValue(EntriesProperty, value);
	}

	public bool FollowTail {
		get => (bool)GetValue(FollowTailProperty);
		set => SetValue(FollowTailProperty, value);
	}

	internal int EntryCount => rendered.Count;
	internal string PlainText => new TextRange(Document.ContentStart, Document.ContentEnd).Text;

	public LogViewer() {
		IsReadOnly = true;
		IsReadOnlyCaretVisible = false;
		IsUndoEnabled = false;
		IsDocumentEnabled = false;
		BorderThickness = new Thickness(0);
		Padding = new Thickness(0);
		Background = Brushes.Transparent;
		Foreground = MessageBrush;
		FontFamily = new FontFamily("Cascadia Mono, Cascadia Code, Consolas");
		FontSize = 10;
		SelectionBrush = FrozenBrush("#285C55");
		SelectionOpacity = 0.85;
		VerticalScrollBarVisibility = ScrollBarVisibility.Auto;
		HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled;
		Document = CreateDocument();

		ContextMenu = new ContextMenu();
		ContextMenu.Items.Add(new MenuItem { Header = "COPY", Command = ApplicationCommands.Copy, CommandTarget = this });
		ContextMenu.Items.Add(new Separator());
		ContextMenu.Items.Add(new MenuItem { Header = "SELECT ALL", Command = ApplicationCommands.SelectAll, CommandTarget = this });

		Loaded += (_, _) => {
			Observe();
			Synchronize();
			RequestScroll();
		};

		Unloaded += (_, _) => {
			Unobserve();
			ResetDocument();
		};

		IsVisibleChanged += (_, _) => {
			if (IsVisible) {
				RequestScroll();
			}
		};
	}

	private static Brush FrozenBrush(string color) {
		var brush = (SolidColorBrush)new BrushConverter().ConvertFromString(color)!;
		brush.Freeze();

		return brush;
	}

	private static void EntriesChanged(DependencyObject target, DependencyPropertyChangedEventArgs eventArguments) {
		var viewer = (LogViewer)target;
		viewer.Unobserve();
		viewer.ResetDocument();
		viewer.Observe();
		viewer.Synchronize();
	}

	private static void FollowTailChanged(DependencyObject target, DependencyPropertyChangedEventArgs eventArguments) {
		if ((bool)eventArguments.NewValue) {
			((LogViewer)target).RequestScroll();
		}
	}

	private void Observe() {
		if (observed != null || Entries is not INotifyCollectionChanged changes) {
			return;
		}

		observed = changes;
		CollectionChangedEventManager.AddHandler(observed, CollectionChanged);
	}

	private void Unobserve() {
		if (observed == null) {
			return;
		}

		CollectionChangedEventManager.RemoveHandler(observed, CollectionChanged);
		observed = null;
	}

	private static FlowDocument CreateDocument() => new() { PagePadding = new Thickness(0), ColumnWidth = double.PositiveInfinity };

	private void ResetDocument() {
		// A fresh text container releases the old paragraphs, selection anchors and
		// layout caches. RichTextBox forbids replacing it inside BeginChange.
		rendered.Clear();
		Document = CreateDocument();
	}

	private void CollectionChanged(object? sender, NotifyCollectionChangedEventArgs eventArguments) {
		if (eventArguments.Action == NotifyCollectionChangedAction.Reset) {
			ResetDocument();
		}

		Synchronize();
	}

	private void Synchronize() {
		var entries = Entries?.Cast<object>().OfType<LogEntry>().ToList() ?? new List<LogEntry>();
		// Preserve existing paragraphs and selection anchors for ordinary appends
		// or head removals. A complete reset uses a fresh document instead.
		int removed = 0;
		if (rendered.Count > 0 && entries.Count > 0 && !ReferenceEquals(rendered[0], entries[0])) {
			removed = rendered.FindIndex(entry => ReferenceEquals(entry, entries[0]));
		}

		bool prefix = removed >= 0 && rendered.Count - removed <= entries.Count;
		for (int renderedIndex = Math.Max(0, removed); prefix && renderedIndex < rendered.Count; ++renderedIndex) {
			prefix = ReferenceEquals(rendered[renderedIndex], entries[renderedIndex - removed]);
		}

		if (!prefix) {
			ResetDocument();
			removed = 0;
		}

		double offset = VerticalOffset;
		var selectionStart = Selection.Start.GetPositionAtOffset(0, LogicalDirection.Forward)!;
		var selectionEnd = Selection.End.GetPositionAtOffset(0, LogicalDirection.Backward)!;
		bool hadSelection = !Selection.IsEmpty;
		BeginChange();
		try {
			if (removed > 0) {
				for (int removedEntryIndex = 0; removedEntryIndex < removed; ++removedEntryIndex) {
					Document.Blocks.Remove(Document.Blocks.FirstBlock!);
				}

				rendered.RemoveRange(0, removed);
			}

			for (int entryIndex = rendered.Count; entryIndex < entries.Count; ++entryIndex) {
				var entry = entries[entryIndex];
				var paragraph = new Paragraph { Margin = new Thickness(0, 4, 0, 4), LineHeight = 15 };
				paragraph.Inlines.Add(new Run(entry.Time + "  ") { Foreground = TimestampBrush });
				paragraph.Inlines.Add(new Run(entry.Level.PadRight(5) + "  ") { Foreground = entry.Level == "ERROR" ? ErrorBrush : entry.Level == "WARN" ? WarningBrush : InformationBrush });
				paragraph.Inlines.Add(new Run(entry.Message) { Foreground = MessageBrush });
				Document.Blocks.Add(paragraph);
				rendered.Add(entry);
			}

			if (prefix && hadSelection) {
				Selection.Select(selectionStart, selectionEnd);
			}
		} finally {
			EndChange();
		}

		// A text selection uses normal RichTextBox COPY / Ctrl+C behavior. It
		// stays intact while new lines arrive, including when following the tail.
		if (!FollowTail) {
			ScrollToVerticalOffset(offset);
		}

		RequestScroll();
	}

	private void RequestScroll() {
		if (pendingScroll || !FollowTail || Dispatcher.HasShutdownStarted) {
			return;
		}

		pendingScroll = true;
		Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() => {
			pendingScroll = false;

			if (FollowTail && IsVisible) {
				ScrollToEnd();
			}
		}));
	}
}
