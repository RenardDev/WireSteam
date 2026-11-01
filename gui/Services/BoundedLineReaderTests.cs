using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace WireSteam.UI.Services;

internal static partial class BoundedLineReader {
	/// <summary>Uses only in-memory synthetic readers; no processes, files or network.</summary>
	internal static async Task RunTestsAsync() {
		static void Assert(bool condition, string message) {
			if (!condition) {
				throw new InvalidOperationException("Bounded line reader: " + message);
			}
		}

		static async Task ExpectAsync(TextReader reader, int limit, params string[] expected) {
			var actual = new List<string>();
			await foreach (string line in ReadLinesAsync(reader, limit).ConfigureAwait(false)) {
				Assert(line.Length <= limit, "a line exceeded its character budget.");
				actual.Add(line);
			}

			Assert(actual.Count == expected.Length, "the number of lines changed.");
			for (int index = 0; index < expected.Length; ++index) {
				Assert(actual[index] == expected[index], "line content or delimiters changed at index " + index + ".");
			}
		}

		await ExpectAsync(new ChunkedReader("", 1), 8);
		await ExpectAsync(new ChunkedReader("\r\n\n\r\r\n", 1), 8, "", "", "", "");
		await ExpectAsync(new ChunkedReader("a\nb\rc\r\nd\n\n\r\r\nend", 1), 8, "a", "b", "c", "d", "", "", "", "end");
		await ExpectAsync(new ChunkedReader("first\r\nsecond\nthird\rfourth", 1), 8, "first", "second", "third", "fourth");
		await ExpectAsync(new ChunkedReader("last\r\n", 2), 8, "last");
		await ExpectAsync(new ChunkedReader("12345678\r\n1234567\n123456789\nok", 3), 8, "12345678", "1234567", "1234567\u2026", "ok");
		await ExpectAsync(new ChunkedReader("AB\ud83d\ude00CD\r\n\ud83d\ude00\n", 1), 8, "AB\ud83d\ude00CD", "\ud83d\ude00");
		await ExpectAsync(new ChunkedReader("ab\ud83d\ude00z\nnext", 1), 4, "ab\u2026", "next");
		await ExpectAsync(new ChunkedReader("\ud83d\ude00xz\nend", 1), 3, "\ud83d\ude00\u2026", "end");
		await ExpectAsync(new ChunkedReader("\ud83d\ude00z\nok", 1), 2, "\u2026", "ok");
		await ExpectAsync(new ChunkedReader("ab\nq\n", 1), 1, "\u2026", "q");
		string bufferBoundary = new string('x', ReadBufferCharacters - 1) + "\ud83d\ude00z";
		await ExpectAsync(new ChunkedReader(bufferBoundary + "\n", int.MaxValue), ReadBufferCharacters * 2, bufferBoundary);

		// The source generates its giant first line directly into each requested
		// buffer. Neither the fixture nor the implementation creates that line.
		using (var giant = new RepeatingReader(4_000_000, "\r\nnext\r\nlast")) {
			long allocatedBefore = GC.GetTotalAllocatedBytes(true);
			await ExpectAsync(giant, 32, new string('x', 31) + "\u2026", "next", "last");
			Assert(GC.GetTotalAllocatedBytes(true) - allocatedBefore < 2_000_000, "draining a giant line allocated memory proportional to the discarded input.");
			Assert(giant.CharactersRead == 4_000_012 && giant.MaximumReadRequest <= ReadBufferCharacters, "giant input must be drained using fixed-size read requests.");
		}

		using (var giant = new RepeatingReader(4_000_000, "")) {
			await ExpectAsync(giant, 16, new string('x', 15) + "\u2026");
			Assert(giant.CharactersRead == 4_000_000 && giant.MaximumReadRequest <= ReadBufferCharacters, "a giant newline-less final line was not drained within the fixed read budget.");
		}

		using (var reader = new ChunkedReader("one\ntwo\nthree\n", 4)) {
			var lines = ReadLinesAsync(reader).GetAsyncEnumerator();
			Assert(await lines.MoveNextAsync() && lines.Current == "one", "the first line was not yielded.");
			int reads = reader.ReadCalls;
			await Task.Yield();
			Assert(reader.ReadCalls == reads, "the reader ran ahead while the consumer was suspended.");
			await lines.DisposeAsync();
			await Task.Yield();
			Assert(reader.ReadCalls == reads && !reader.IsDisposed, "disposing the iterator read ahead or disposed its caller-owned reader.");
		}

		using (var cancellation = new CancellationTokenSource()) {
			cancellation.Cancel();
			bool cancelled = false;
			try {
				await foreach (var _ in ReadLinesAsync(new ChunkedReader("test", 1), cancellationToken: cancellation.Token)) {
				}
			} catch (OperationCanceledException) {
				cancelled = true;
			}

			Assert(cancelled, "cancellation was ignored.");
		}

		bool rejected = false;
		try {
			await foreach (var _ in ReadLinesAsync(new ChunkedReader("test", 1), 0)) {
			}
		} catch (ArgumentOutOfRangeException) {
			rejected = true;
		}

		Assert(rejected, "a zero character budget was accepted.");
	}

	private sealed class ChunkedReader(string value, int maximumChunk) : TextReader {
		private int position;

		internal int ReadCalls {
			get;
			private set;
		}

		internal bool IsDisposed {
			get;
			private set;
		}

		public override ValueTask<int> ReadAsync(Memory<char> buffer, CancellationToken cancellationToken = default) {
			cancellationToken.ThrowIfCancellationRequested();
			++ReadCalls;
			int count = Math.Min(Math.Min(buffer.Length, maximumChunk), value.Length - position);
			value.AsMemory(position, count).CopyTo(buffer);
			position += count;

			return ValueTask.FromResult(count);
		}

		protected override void Dispose(bool disposing) {
			IsDisposed = true;
			base.Dispose(disposing);
		}
	}

	private sealed class RepeatingReader(int repetitions, string suffix) : TextReader {
		private int remaining = repetitions, suffixPosition;

		internal int CharactersRead {
			get;
			private set;
		}

		internal int MaximumReadRequest {
			get;
			private set;
		}

		public override ValueTask<int> ReadAsync(Memory<char> buffer, CancellationToken cancellationToken = default) {
			cancellationToken.ThrowIfCancellationRequested();
			MaximumReadRequest = Math.Max(MaximumReadRequest, buffer.Length);
			int count = Math.Min(remaining, buffer.Length);
			buffer.Span[..count].Fill('x');
			remaining -= count;
			if (remaining == 0 && count < buffer.Length) {
				int tail = Math.Min(buffer.Length - count, suffix.Length - suffixPosition);
				suffix.AsMemory(suffixPosition, tail).CopyTo(buffer[count..]);
				suffixPosition += tail;
				count += tail;
			}

			CharactersRead += count;

			return ValueTask.FromResult(count);
		}
	}
}
