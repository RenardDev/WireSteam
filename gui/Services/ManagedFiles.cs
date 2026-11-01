using System;
using System.Collections.Generic;
using System.IO;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading.Tasks;

namespace WireSteam.UI.Services;

internal static class ManagedFiles {
	public const int MaximumBytes = 1024 * 1024;
	private static readonly UTF8Encoding Utf8 = new(false, true);

	public static void CheckPath(string path, bool allowMissingFile = false) {
		path = Path.GetFullPath(path);
		var current = new FileInfo(path);
		if (current.Exists && (current.Attributes & FileAttributes.ReparsePoint) != 0) {
			throw new IOException("Symbolic links and reparse points are not supported for managed files.");
		}

		if (!current.Exists && !allowMissingFile) {
			throw new FileNotFoundException("The configuration file was not found.", path);
		}

		for (var directory = current.Directory; directory != null; directory = directory.Parent) {
			if (directory.Exists && (directory.Attributes & FileAttributes.ReparsePoint) != 0) {
				throw new IOException("Managed paths cannot contain symbolic links or reparse points.");
			}
		}
	}

	private static IEnumerable<SecurityIdentifier> AllowedIdentities() {
		SecurityIdentifier currentUser;
		using (var identity = WindowsIdentity.GetCurrent()) {
			currentUser = identity.User ?? throw new IOException("Cannot determine the current Windows account.");
		}

		yield return currentUser;
		yield return new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
		yield return new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
	}

	public static void EnsureDirectory(string path) {
		var pending = new Stack<DirectoryInfo>();
		var current = new DirectoryInfo(Path.GetFullPath(path));
		while (!current.Exists) {
			pending.Push(current);
			current = current.Parent ?? throw new IOException("Cannot locate the parent directory.");
		}

		for (var ancestor = current; ancestor != null; ancestor = ancestor.Parent) {
			if ((ancestor.Attributes & FileAttributes.ReparsePoint) != 0) {
				throw new IOException("Private directories cannot contain reparse points.");
			}
		}

		while (pending.Count > 0) {
			var directory = pending.Pop();
			var security = new DirectorySecurity();
			security.SetAccessRuleProtection(true, false);
			foreach (var identity in AllowedIdentities()) {
				security.AddAccessRule(new FileSystemAccessRule(identity, FileSystemRights.FullControl, InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
			}

			directory.Create(security);
			directory.Refresh();

			if ((directory.Attributes & FileAttributes.ReparsePoint) != 0) {
				throw new IOException("A private directory became a reparse point.");
			}
		}
	}

	public static async Task<string> ReadAsync(string path) {
		CheckPath(path);

		await using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 4096, FileOptions.Asynchronous | FileOptions.SequentialScan);
		if (file.Length > MaximumBytes) {
			throw new IOException("Configuration and metadata files must be at most 1 MiB.");
		}

		using var reader = new StreamReader(file, Utf8, true, 4096, true);
		var text = await reader.ReadToEndAsync().ConfigureAwait(false);
		if (Utf8.GetByteCount(text) > MaximumBytes) {
			throw new IOException("The decoded file exceeds 1 MiB.");
		}

		return text;
	}

	public static async Task WritePrivateAsync(string path, string contents, bool overwrite) {
		path = Path.GetFullPath(path);
		var temporary = path;
		bool created = false, completed = false;
		var bytes = Utf8.GetBytes(contents);
		try {
			if (bytes.Length > MaximumBytes) {
				throw new IOException("The file exceeds 1 MiB.");
			}

			EnsureDirectory(Path.GetDirectoryName(path)!);
			CheckPath(path, true);

			if (!overwrite && File.Exists(path)) {
				throw new IOException("The destination already exists. Choose a new file.");
			}

			temporary = overwrite ? path + "." + Guid.NewGuid().ToString("N") + ".tmp" : path;
			var security = new FileSecurity();
			security.SetAccessRuleProtection(true, false);
			foreach (var identity in AllowedIdentities()) {
				security.AddAccessRule(new FileSystemAccessRule(identity, FileSystemRights.FullControl, AccessControlType.Allow));
			}

			await using (var stream = new FileInfo(temporary).Create(FileMode.CreateNew, FileSystemRights.FullControl, FileShare.None, 4096, FileOptions.Asynchronous | FileOptions.WriteThrough, security)) {
				created = true;
				await stream.WriteAsync(bytes).ConfigureAwait(false);
				await stream.FlushAsync().ConfigureAwait(false);
				stream.Flush(true);
			}

			if (overwrite) {
				CheckPath(path, true);
				File.Move(temporary, path, true);
			}

			completed = true;
		} finally {
			CryptographicOperations.ZeroMemory(bytes);

			if (created && !completed) {
				DeleteOwned(temporary);
			}
		}
	}

	public static void DeleteOwned(string path) {
		if (!File.Exists(path)) {
			return;
		}

		CheckPath(path);
		File.Delete(path);
	}
}
