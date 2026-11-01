param([Parameter(Mandatory = $true)][string]$WireSteamPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskExecutablePath = [IO.Path]::GetFullPath($WireSteamPath)
$taskTemporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ('WireSteam-cli-' + [guid]::NewGuid().ToString('N'))

function Quote-Argument([string]$Value) {
	$result = [Text.StringBuilder]::new('"')
	$slashes = 0
	foreach ($character in $Value.ToCharArray()) {
		if ([int]$character -eq 0x5C) {
			++$slashes
			continue
		}

		if ([int]$character -eq 0x22) {
			[void]$result.Append(('\' * (2 * $slashes + 1)))
		} else {
			[void]$result.Append(('\' * $slashes))
		}

		[void]$result.Append($character)
		$slashes = 0
	}

	[void]$result.Append(('\' * (2 * $slashes)))
	[void]$result.Append('"')

	return $result.ToString()
}

function Invoke-Application([string[]]$Arguments, [string]$InputText = '') {
	$timeoutMilliseconds = 15000

	$start = [Diagnostics.ProcessStartInfo]::new()
	$start.FileName = $taskExecutablePath
	$start.Arguments = (
		@(
			$Arguments | ForEach-Object {
				Quote-Argument $_
			}
		) -join ' '
	)
	$start.UseShellExecute = $false
	$start.CreateNoWindow = $true
	$start.RedirectStandardOutput = $true
	$start.RedirectStandardError = $true
	$start.RedirectStandardInput = $true
	$standardOutputReader = $null
	$standardErrorReader = $null
	$standardInputWriter = $null
	$standardOutput = $null
	$standardError = $null
	$started = $false
	$processIdentifier = $null
	$processStartTime = $null
	$primaryFailure = $null
	$record = $null
	$process = [Diagnostics.Process]::new()
	try {
		$process.StartInfo = $start
		[void]$process.Start()
		$started = $true
		$processIdentifier = $process.Id
		$processStartTime = $process.StartTime.ToUniversalTime()
		$standardOutputReader = $process.StandardOutput
		$standardErrorReader = $process.StandardError
		$standardInputWriter = $process.StandardInput
		$standardOutput = $standardOutputReader.ReadToEndAsync()
		$standardError = $standardErrorReader.ReadToEndAsync()
		$standardInputWriter.Write($InputText)
		$standardInputWriter.Close()

		if (!$process.WaitForExit($timeoutMilliseconds)) {
			throw 'CLI test timed out'
		}

		foreach ($readerTask in @($standardOutput, $standardError)) {
			if (!$readerTask.Wait($timeoutMilliseconds)) {
				throw 'CLI output reader did not settle after process exit'
			}
		}

		$record = [pscustomobject]@{
			Code = $process.ExitCode
			Text = $standardOutput.Result + $standardError.Result
		}
	} catch {
		$primaryFailure = $_
		throw
	} finally {
		$cleanupFailure = $null
		$exitConfirmed = !$started
		$readersSettled = $false
		try {
			if ($started) {
				if (!$process.HasExited) {
					# This helper owns only the short CLI command it started.
					$process.Kill()
				}

				$exitConfirmed = $process.WaitForExit($timeoutMilliseconds)
				if (!$exitConfirmed) {
					throw 'CLI child did not exit after termination'
				}
			}
		} catch {
			$cleanupFailure = $_
		}

		if ($exitConfirmed) {
			$readersSettled = $true
			foreach ($readerTask in @($standardOutput, $standardError)) {
				if ($null -eq $readerTask) {
					continue
				}

				try {
					if (!$readerTask.Wait($timeoutMilliseconds)) {
						$readersSettled = $false
						throw 'CLI output reader did not settle after process exit'
					}
				} catch {
					if (!$readerTask.IsCompleted) {
						$readersSettled = $false
					}

					if ($null -eq $cleanupFailure) {
						$cleanupFailure = $_
					}
				}
			}
		}

		if ($exitConfirmed -and $readersSettled) {
			foreach ($owner in @($standardInputWriter, $standardOutputReader, $standardErrorReader, $process)) {
				if ($null -eq $owner) {
					continue
				}

				try {
					$owner.Dispose()
				} catch {
					if ($null -eq $cleanupFailure) {
						$cleanupFailure = $_
					}
				}
			}
		} else {
			# Keep every unconfirmed owner reachable through the propagated error.
			$retainedOwner = [pscustomobject]@{
				Process = $process
				ProcessIdentifier = $processIdentifier
				ExecutablePath = $taskExecutablePath
				StartTimeUTC = $processStartTime
				ExitConfirmed = $exitConfirmed
				ReadersSettled = $readersSettled
				StandardInputWriter = $standardInputWriter
				StandardOutputReader = $standardOutputReader
				StandardErrorReader = $standardErrorReader
				StandardOutputTask = $standardOutput
				StandardErrorTask = $standardError
			}

			$failure = if ($null -ne $primaryFailure) {
				$primaryFailure
			} else {
				$cleanupFailure
			}

			$failure.Exception.Data['CLI retained owner'] = $retainedOwner
		}

		if ($null -ne $cleanupFailure) {
			if ($null -ne $primaryFailure) {
				$primaryFailure.Exception.Data['CLI cleanup failure'] = $cleanupFailure
				Write-Warning -Message "CLI child cleanup failed: Process=$processIdentifier, Path=$taskExecutablePath, StartTimeUTC=$processStartTime, ExitConfirmed=$exitConfirmed, ReadersSettled=$readersSettled; $($cleanupFailure.Exception.Message)" -WarningAction Continue
			} else {
				throw $cleanupFailure
			}
		}
	}

	return $record
}

function Assert-Result($Record, [bool]$Success, [string]$Match) {
	if (($Record.Code -eq 0) -ne $Success -or $Record.Text -notmatch $Match) {
		# CLI output is deliberately public-only. Never print config/key contents here.
		throw "CLI result mismatch: expected success=$Success, pattern=$Match, exit=$($Record.Code); $($Record.Text)"
	}
}

$taskPrimaryFailure = $null
try {
	[void](New-Item -ItemType Directory -Path $taskTemporaryDirectory)
	$keyPath = Join-Path $taskTemporaryDirectory 'private key.key'
	$configurationPath = Join-Path $taskTemporaryDirectory 'vpn config.conf'
	$keyResult = Invoke-Application @('keygen', $keyPath)
	Assert-Result $keyResult $true 'PublicKey = [A-Za-z0-9+/]{43}='
	$peerPublic = [regex]::Match($keyResult.Text, 'PublicKey = ([A-Za-z0-9+/]{43}=)').Groups[1].Value
	Assert-Result (Invoke-Application @('keygen', $keyPath)) $false 'exist'

	Assert-Result (Invoke-Application @('init', $configurationPath, '--address', '10.77.0.1/32')) $true 'Add a \[Peer\]'
	$template = [IO.File]::ReadAllText($configurationPath)
	$privateKey = [regex]::Match($template, 'PrivateKey = ([A-Za-z0-9+/]{43}=)').Groups[1].Value
	if ($privateKey.Length -ne 44) {
		throw 'init did not save a private key'
	}

	Assert-Result (Invoke-Application @('init', $configurationPath)) $false 'exist'

	$peer = "`n[Peer]`nPublicKey = $peerPublic`nSteamID = 76561198000000001`nAllowedIPs = 10.77.0.2/32`nDial = false`n"
	[IO.File]::AppendAllText($configurationPath, $peer)
	$checked = Invoke-Application @('check', $configurationPath)
	Assert-Result $checked $true 'Configuration is valid'

	if ($checked.Text.Contains($privateKey)) {
		throw 'check exposed the private key'
	}

	$forward = "`n[PortForward]`nProtocol = TCP`nListen = 10.77.0.1:25565`nTarget = 127.0.0.1:25565`n"
	[IO.File]::AppendAllText($configurationPath, $forward)
	$forwardChecked = Invoke-Application @('check', $configurationPath)
	Assert-Result $forwardChecked $true 'TCP 10\.77\.0\.1:25565 -> 127\.0\.0\.1:25565'

	if ($forwardChecked.Text.Contains($privateKey)) {
		throw 'forwarding check exposed the private key'
	}

	[IO.File]::AppendAllText($configurationPath, $forward.Replace('Protocol = TCP', 'Protocol = UDP'))
	$mixedChecked = Invoke-Application @('check', $configurationPath)
	Assert-Result $mixedChecked $true 'UDP 10\.77\.0\.1:25565 -> 127\.0\.0\.1:25565'

	if (!$mixedChecked.Text.Contains('Port forwards: 2') -or $mixedChecked.Text.Contains($privateKey)) {
		throw 'mixed TCP/UDP forwarding summary is incomplete or exposed a key'
	}

	Assert-Result (Invoke-Application @('bridge', $configurationPath, '--run-seconds', '1')) $false 'PortForward rules require up'

	$badPath = Join-Path $taskTemporaryDirectory 'bad.conf'
	[IO.File]::WriteAllText($badPath, ($template + $peer).Replace('10.77.0.2/32', '0.0.0.0/0'))
	Assert-Result (Invoke-Application @('check', $badPath)) $false 'private|RFC1918|route|ULA'

	Assert-Result (Invoke-Application @('check', $configurationPath, '--name', 'wrong')) $false 'Unknown option'

	Assert-Result (Invoke-Application @('status', '--name', '../unsafe')) $false 'name'

	Assert-Result (Invoke-Application @('not-a-command')) $false 'Unknown command'

	Assert-Result (Invoke-Application @() "0`n") $true 'Interactive|Choose|Create configuration'
	Assert-Result (Invoke-Application @() "2`n$configurationPath`n0`n") $true 'Configuration is valid'

	Write-Output 'PASS CLI: private init/keygen, no overwrite, check/redaction, route rejection, options and interactive launcher'
} catch {
	$taskPrimaryFailure = $_
	throw
} finally {
	$taskRetainedOwner = $null
	if (($null -ne $taskPrimaryFailure) -and $taskPrimaryFailure.Exception.Data.Contains('CLI retained owner')) {
		$taskRetainedOwner = $taskPrimaryFailure.Exception.Data['CLI retained owner']
	}

	if ($null -ne $taskRetainedOwner) {
		Write-Warning -Message "CLI temporary inputs retained until owned child/readers settle: Path=$taskTemporaryDirectory" -WarningAction Continue
	} else {
		$temporaryCleanupFailure = $null

		# Remove only this test's explicit files; attempt each owner even if another cleanup fails.
		foreach ($name in @('private key.key', 'vpn config.conf', 'bad.conf')) {
			try {
				$file = Join-Path $taskTemporaryDirectory $name
				if (Test-Path -LiteralPath $file -PathType Leaf) {
					Remove-Item -LiteralPath $file -Force
				}
			} catch {
				if ($null -eq $temporaryCleanupFailure) {
					$temporaryCleanupFailure = $_
				}
			}
		}

		try {
			if (Test-Path -LiteralPath $taskTemporaryDirectory -PathType Container) {
				[IO.Directory]::Delete($taskTemporaryDirectory)
			}
		} catch {
			if ($null -eq $temporaryCleanupFailure) {
				$temporaryCleanupFailure = $_
			}
		}

		if ($null -ne $temporaryCleanupFailure) {
			if ($null -ne $taskPrimaryFailure) {
				$taskPrimaryFailure.Exception.Data['CLI temporary cleanup failure'] = $temporaryCleanupFailure
				Write-Warning -Message "CLI temporary cleanup failed: Path=$taskTemporaryDirectory; $($temporaryCleanupFailure.Exception.Message)" -WarningAction Continue
			} else {
				throw $temporaryCleanupFailure
			}
		}
	}
}
