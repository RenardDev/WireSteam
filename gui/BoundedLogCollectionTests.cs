using System;
using System.Collections.Specialized;
using System.Linq;

namespace WireSteam.UI;

internal static class BoundedLogCollectionTests {
	internal static void Run() {
		static void Require(bool condition, string message) {
			if (!condition) {
				throw new InvalidOperationException("Bounded log: " + message);
			}
		}

		static int Bytes(BoundedLogCollection logs) => logs.Sum(entry => (entry.Time.Length + entry.Level.Length + entry.Message.Length) * sizeof(char));

		static void Verify(BoundedLogCollection logs) {
			Require(logs.Count <= BoundedLogCollection.MaximumEntries && logs.StoredTextBytes <= BoundedLogCollection.MaximumTextBytes, "entry/text storage exceeded its budget.");
			Require(logs.StoredTextBytes == Bytes(logs), "text accounting no longer matches retained entries.");
		}

		var logs = new BoundedLogCollection();
		int resets = 0;
		logs.CollectionChanged += (_, eventArguments) => {
			Verify(logs);

			if (eventArguments.Action == NotifyCollectionChangedAction.Reset) {
				++resets;
			}
		};

		for (int entryIndex = 0; entryIndex < BoundedLogCollection.MaximumEntries; ++entryIndex) {
			logs.Add(new LogEntry("12:00:00", "INFO", "Count entry " + entryIndex));
		}

		Require(resets == 0 && logs.Count == BoundedLogCollection.MaximumEntries, "exact count limit must remain available until the next message.");

		var current = new LogEntry("12:00:01", "ERROR", "Current diagnostic survives the automatic clear.");
		logs.Add(current);
		Require(resets == 1 && logs.Count == 2 && logs[0].Level == "WARN" && logs[0].Message == BoundedLogCollection.AutomaticClearMessage && ReferenceEquals(logs[1], current), "count overflow must clear once, show a notice and retain the current diagnostic unchanged.");

		logs.Clear();
		resets = 0;
		var full = new LogEntry("", "", new string('x', BoundedLogCollection.MaximumMessageCharacters));
		int exactEntries = BoundedLogCollection.MaximumTextBytes / (full.Message.Length * sizeof(char));
		for (int entryIndex = 0; entryIndex < exactEntries; ++entryIndex) {
			logs.Add(full);
		}

		Require(resets == 0 && logs.StoredTextBytes == BoundedLogCollection.MaximumTextBytes, "an exact text-budget boundary was cleared prematurely.");
		logs.Add(new LogEntry("", "ERROR", "\ud83d\ude00"));
		Require(resets == 1 && logs.Count == 2 && logs[^1].Message == "\ud83d\ude00", "text overflow must preserve Unicode and clear below the entry limit.");

		// Exercise several complete retention cycles, not just the first reset.
		for (int cycle = 0; cycle < 4; ++cycle) {
			int before = resets;
			for (int entryIndex = 0; entryIndex <= exactEntries && resets == before; ++entryIndex) {
				logs.Add(full);
			}

			Require(resets == before + 1 && logs.Count == 2, "a subsequent overflow failed to clear the previous session buffer.");
		}

		logs.Clear();
		string splitPair = new string('a', BoundedLogCollection.MaximumMessageCharacters - 2) + "\ud83d\ude00" + "tail";
		logs.Add(new LogEntry(new string('t', 100), new string('l', 100), splitPair));
		Require(logs[0].Time.Length <= 32 && logs[0].Level.Length <= 16 && logs[0].Message.Length <= BoundedLogCollection.MaximumMessageCharacters &&
			logs[0].Message.EndsWith("…", StringComparison.Ordinal) && !char.IsHighSurrogate(logs[0].Message[^2]),
			"individual fields must be capped without cutting a UTF-16 surrogate pair.");

		logs.Insert(0, current);
		logs.Move(0, 1);
		logs.RemoveAt(0);
		logs[0] = full;
		Verify(logs);
		logs.Clear();
		Require(logs.Count == 0 && logs.StoredTextBytes == 0, "manual clear must reset byte accounting.");

		// A replacement can exceed the text budget without changing Count.
		for (int entryIndex = 0; entryIndex < exactEntries - 1; ++entryIndex) {
			logs.Add(full);
		}

		logs.Add(new LogEntry("", "", "a"));
		logs.Add(new LogEntry("", "", new string('z', full.Message.Length - 1)));
		Require(logs.StoredTextBytes == BoundedLogCollection.MaximumTextBytes, "replacement fixture did not fill the exact budget.");
		int beforeReplacement = resets;
		logs[exactEntries - 1] = new LogEntry("", "WARN", "ab");
		Require(resets == beforeReplacement + 1 && logs.Count == 2 && logs[^1].Message == "ab", "replacement bypassed the text limit.");

		var other = new Profile();
		other.Logs.Add(current);
		logs.Clear();
		Require(other.Logs.Count == 1 && ReferenceEquals(other.Logs[0], current), "clearing one profile must not discard another profile's log.");

		bool invalidInsert = false;
		try {
			logs.Insert(1, current);
		} catch (ArgumentOutOfRangeException) {
			invalidInsert = true;
		}

		Require(invalidInsert && logs.StoredTextBytes == 0 && logs.Count == 0, "an invalid insertion changed memory accounting.");

		Console.WriteLine("PASS log buffer: exact count/text limits, repeated automatic resets, retained diagnostics, Unicode-safe field caps, edits/removals/manual clear and per-profile isolation.");
	}
}
