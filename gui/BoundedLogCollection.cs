using System;
using System.Collections.ObjectModel;

namespace WireSteam.UI;

/// <summary>Session-only diagnostics with bounded text and entry storage.</summary>
public sealed class BoundedLogCollection : ObservableCollection<LogEntry> {
	public const int MaximumEntries = 2000;
	// UTF-16 text storage; WPF/object overhead is bounded separately by the entry count.
	public const int MaximumTextBytes = 1024 * 1024;
	public const int MaximumMessageCharacters = 8192;
	internal const string AutomaticClearMessage = "Log cleared automatically (limit: 2000 entries or 1 MiB of text).";

	public int StoredTextBytes {
		get;
		private set;
	}

	protected override void InsertItem(int index, LogEntry item) {
		CheckReentrancy();
		ArgumentOutOfRangeException.ThrowIfNegative(index);

		if (index > Count) {
			throw new ArgumentOutOfRangeException(nameof(index));
		}

		item = Normalize(item);
		int size = TextBytes(item);
		if (Count >= MaximumEntries || StoredTextBytes + size > MaximumTextBytes) {
			ResetFor(item.Time);
			index = Count;
		}

		StoredTextBytes += size;
		base.InsertItem(index, item);
	}

	protected override void SetItem(int index, LogEntry item) {
		CheckReentrancy();
		var previous = this[index];
		item = Normalize(item);
		int size = TextBytes(item);
		if (StoredTextBytes - TextBytes(previous) + size > MaximumTextBytes) {
			ResetFor(item.Time);
			StoredTextBytes += size;
			base.InsertItem(Count, item);
		} else {
			StoredTextBytes += size - TextBytes(previous);
			base.SetItem(index, item);
		}
	}

	protected override void RemoveItem(int index) {
		CheckReentrancy();
		StoredTextBytes -= TextBytes(this[index]);
		base.RemoveItem(index);
	}

	protected override void ClearItems() {
		CheckReentrancy();
		StoredTextBytes = 0;
		base.ClearItems();
	}

	private void ResetFor(string time) {
		ClearItems();
		var notice = new LogEntry(time, "WARN", AutomaticClearMessage);
		StoredTextBytes = TextBytes(notice);
		base.InsertItem(0, notice);
	}

	private static LogEntry Normalize(LogEntry entry) {
		ArgumentNullException.ThrowIfNull(entry);
		string time = Limit(entry.Time, 32), level = Limit(entry.Level, 16), message = Limit(entry.Message, MaximumMessageCharacters);

		return ReferenceEquals(time, entry.Time) && ReferenceEquals(level, entry.Level) && ReferenceEquals(message, entry.Message) ? entry : new LogEntry(time, level, message);
	}

	private static string Limit(string text, int maximum) {
		ArgumentNullException.ThrowIfNull(text);

		if (text.Length <= maximum) {
			return text;
		}

		int length = maximum - 1;
		if (char.IsHighSurrogate(text[length - 1]) && char.IsLowSurrogate(text[length])) {
			--length;
		}

		return text[..length] + "…";
	}

	private static int TextBytes(LogEntry entry) => (entry.Time.Length + entry.Level.Length + entry.Message.Length) * sizeof(char);
}
