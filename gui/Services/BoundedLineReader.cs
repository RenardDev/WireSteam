using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.CompilerServices;
using System.Threading;

namespace WireSteam.UI.Services;

/// <summary>Reads redirected process output without allocating an unbounded line.</summary>
internal static partial class BoundedLineReader {
	internal const int DefaultMaximumCharacters = 8192;
	private const int ReadBufferCharacters = 1024;

	/// <summary>
	/// Preserves TextReader line boundaries and backpressure. Truncated lines include
	/// an ellipsis within the character budget; their remaining input is discarded.
	/// The caller retains ownership of the reader.
	/// </summary>
	internal static async IAsyncEnumerable<string> ReadLinesAsync(TextReader reader, int maximumCharacters = DefaultMaximumCharacters, [EnumeratorCancellation] CancellationToken cancellationToken = default) {
		ArgumentNullException.ThrowIfNull(reader);
		ArgumentOutOfRangeException.ThrowIfLessThan(maximumCharacters, 1);

		var readBuffer = new char[ReadBufferCharacters];
		var lineBuffer = new char[maximumCharacters];
		int length = 0;
		bool truncated = false, skipLineFeed = false;
		while (true) {
			int count = await reader.ReadAsync(readBuffer.AsMemory(), cancellationToken).ConfigureAwait(false);
			if (count == 0) {
				break;
			}

			for (int index = 0; index < count; ++index) {
				char character = readBuffer[index];
				if (skipLineFeed) {
					skipLineFeed = false;

					if (character == '\n') {
						continue;
					}
				}

				if (character is '\r' or '\n') {
					cancellationToken.ThrowIfCancellationRequested();
					skipLineFeed = character == '\r';
					yield return FinishLine(lineBuffer, length, truncated);
					length = 0;
					truncated = false;
				} else if (length < lineBuffer.Length) {
					lineBuffer[length++] = character;
				} else {
					truncated = true;
				}
			}
		}

		if (length > 0 || truncated) {
			cancellationToken.ThrowIfCancellationRequested();
			yield return FinishLine(lineBuffer, length, truncated);
		}
	}

	private static string FinishLine(char[] buffer, int length, bool truncated) {
		if (!truncated) {
			return new string(buffer, 0, length);
		}

		int prefixLength = Math.Min(length, buffer.Length - 1);
		// A valid UTF-16 pair may cross either the read buffer or retained prefix.
		// Keep pairs intact when reserving the final character for the ellipsis.
		if (prefixLength > 0 && char.IsHighSurrogate(buffer[prefixLength - 1])) {
			--prefixLength;
		}

		buffer[prefixLength] = '\u2026';

		return new string(buffer, 0, prefixLength + 1);
	}
}
