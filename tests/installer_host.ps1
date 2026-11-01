param([Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$MsiPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Open a real MSI session and invoke only the known read-only search/costing
# actions and guard. Never invoke INSTALL, an action sequence or file/registry
# changes. IGNOREMACHINESTATE cannot run DLL actions, so use a normal session.
# Run only in a dedicated test process with a validated project-built MSI and
# no existing or concurrent Installer UI registration; this test clears its own
# external handler rather than reconstructing another handler's filter/context.
if (-not ('WireSteamInstallerHostTest' -as [type])) {
	Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

public static class WireSteamInstallerHostTest {
	[DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern uint MsiOpenPackageExW(string path, uint options, out uint package);
	[DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern uint MsiDoActionW(uint package, string action);
	[DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern uint MsiGetPropertyW(uint package, string name, StringBuilder value, ref uint size);
	[DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern int MsiQueryProductStateW(string product);
	[DllImport("msi.dll")] static extern uint MsiSetInternalUI(uint level, IntPtr window);
	[DllImport("msi.dll")] static extern uint MsiCloseHandle(uint handle);
	[DllImport("ole32.dll")] static extern int CoInitializeEx(IntPtr reserved, uint model);
	[DllImport("ole32.dll")] static extern void CoUninitialize();
	[UnmanagedFunctionPointer(CallingConvention.Winapi, CharSet = CharSet.Unicode)]
	delegate int Handler(IntPtr context, uint type, IntPtr textAddress);
	[DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern IntPtr MsiSetExternalUIW(Handler handler, uint filter, IntPtr context);

	static void Check(uint code, string action) {
		if (code != 0) {
			throw new InvalidOperationException(action + ": " + code);
		}
	}

	static string ReadProperty(uint package, string name) {
		uint size = 4096;
		var value = new StringBuilder((int)size);
		Check(MsiGetPropertyW(package, name, value, ref size), "Read MSI property");

		return value.ToString();
	}

	static void CheckCallbackFailure(Exception failure) {
		if (failure != null) {
			throw new InvalidOperationException("The MSI message handler failed.", failure);
		}
	}

	public static string[] Run(string msi) {
		uint package = 0;
		string product = null;
		int originalState = 0;
		var messages = new List<string>();
		Exception callbackFailure = null;
		Handler callback = delegate (IntPtr context, uint type, IntPtr textAddress) {
			try {
				// Convert the borrowed LPCWSTR inside the boundary; automatic string
				// marshaling could allocate before this handler's catch is active.
				string text = textAddress == IntPtr.Zero ? null : Marshal.PtrToStringUni(textAddress);
				lock (messages) {
					if (text != null && messages.Count < 128 && text.IndexOf("WireSteam", StringComparison.OrdinalIgnoreCase) >= 0) {
						messages.Add(text);
					}
				}

				return 0;
			} catch (Exception failure) {
				Interlocked.CompareExchange(ref callbackFailure, failure, null);
				return -1;
			}
		};

		bool internalUIChanged = false;
		bool externalUIRegistered = false;
		uint previousUILevel = 0;
		int comInitializationResult = CoInitializeEx(IntPtr.Zero, 0x02);
		try {
			previousUILevel = MsiSetInternalUI(2, IntPtr.Zero);
			internalUIChanged = true;
			MsiSetExternalUIW(callback, 0x3FFF, IntPtr.Zero);
			externalUIRegistered = true;

			uint openResult = MsiOpenPackageExW(msi, 0, out package);
			CheckCallbackFailure(Interlocked.CompareExchange(ref callbackFailure, null, null));
			Check(openResult, "Open read-only guard test session");
			product = ReadProperty(package, "ProductCode");
			originalState = MsiQueryProductStateW(product);
			foreach (string action in new[] { "AppSearch", "CostInitialize", "FileCost", "CostFinalize" }) {
				uint actionResult = MsiDoActionW(package, action);
				CheckCallbackFailure(Interlocked.CompareExchange(ref callbackFailure, null, null));
				Check(actionResult, action);
			}

			uint result = MsiDoActionW(package, "WireSteamValidateSession");
			string[] capturedMessages;
			lock (messages) {
				capturedMessages = messages.ToArray();
			}

			CheckCallbackFailure(Interlocked.CompareExchange(ref callbackFailure, null, null));

			string native = null;
			string compatibility = null;
			foreach (string message in capturedMessages) {
				if (message.StartsWith("WireSteam native Windows version: ", StringComparison.Ordinal)) {
					native = message;
				}

				if (message.StartsWith("WireSteam Windows compatibility report: ", StringComparison.Ordinal)) {
					compatibility = message;
				}
			}

			if (native == null) {
				throw new InvalidOperationException("The actual MSI DLL guard did not report its native OS version.");
			}

			Match version = Regex.Match(native, @"version: ([0-9]+)\.([0-9]+)\.([0-9]+); platform=2; productType=1;");
			if (!version.Success || !((uint.Parse(version.Groups[1].Value) > 10) ||
				(uint.Parse(version.Groups[1].Value) == 10 && (uint.Parse(version.Groups[2].Value) > 0 || uint.Parse(version.Groups[3].Value) >= 19045)))) {
				throw new InvalidOperationException("The MSI guard did not identify a supported native Windows client: " + native);
			}

			bool normalBlock = false;
			foreach (string message in capturedMessages) {
				if (message.IndexOf("Quit WireSteam using QUIT WIRESTEAM", StringComparison.Ordinal) >= 0) {
					normalBlock = true;
				}
			}

			if (result != 0 && !(result == 1603 && normalBlock)) {
				throw new InvalidOperationException("The MSI guard failed unexpectedly: " + result + "\n" + string.Join("\n", capturedMessages));
			}

			if (MsiQueryProductStateW(product) != originalState) {
				throw new InvalidOperationException("The guard test unexpectedly changed the product installation state.");
			}

			var output = new List<string>();
			output.Add(native);

			if (compatibility != null) {
				output.Add(compatibility);
			}

			output.Add("PASS MSI host: native OS accepted by the actual embedded DLL action" +
				(result == 0 ? " (guard result 0)" : " (existing-session cleanup guard retained)") + "; product registration unchanged. No installation or networking actions executed.");

			return output.ToArray();
		} finally {
			try {
				if (package != 0) {
					MsiCloseHandle(package);
				}
			} finally {
				try {
					if (externalUIRegistered) {
						MsiSetExternalUIW(null, 0, IntPtr.Zero);
					}
				} finally {
					try {
						if (internalUIChanged) {
							MsiSetInternalUI(previousUILevel, IntPtr.Zero);
						}
					} finally {
						try {
							if (comInitializationResult >= 0) {
								CoUninitialize();
							}
						} finally {
							GC.KeepAlive(callback);
						}
					}
				}
			}
		}
	}
}
'@
}

[WireSteamInstallerHostTest]::Run((Resolve-Path -LiteralPath $MsiPath).Path)
