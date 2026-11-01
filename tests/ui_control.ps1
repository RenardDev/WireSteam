param([Parameter(Mandatory = $true)][string]$WireSteamPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskExecutablePath = [IO.Path]::GetFullPath($WireSteamPath)
$taskTemporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ('WireSteam-ui-cli-' + [guid]::NewGuid().ToString('N'))
$taskUTF8Encoding = [Text.UTF8Encoding]::new($false, $true)

function Assert-True([bool]$Condition, [string]$Message) {
	if (!$Condition) {
		throw $Message
	}
}

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

function Invoke-Application([string[]]$Arguments, [byte[]]$InputBytes = @()) {
	$cleanupTimeoutMilliseconds = 15000

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

		try {
			if ($InputBytes.Length) {
				$standardInputWriter.BaseStream.Write($InputBytes, 0x00, $InputBytes.Length)
			}
		} catch [IO.IOException] {
			# A rejected command may close its input before all bytes are sent.
		} finally {
			$standardInputWriter.Close()
		}

		Assert-True ($process.WaitForExit(10000)) 'GUI CLI command timed out'
		Assert-True ($standardOutput.Wait(3000) -and $standardError.Wait(3000)) 'GUI CLI output did not close'

		$record = [pscustomobject]@{
			Code = $process.ExitCode
			Out = $standardOutput.Result
			Error = $standardError.Result
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

				$exitConfirmed = $process.WaitForExit($cleanupTimeoutMilliseconds)
				if (!$exitConfirmed) {
					throw 'GUI CLI child did not exit after termination'
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
					if (!$readerTask.Wait($cleanupTimeoutMilliseconds)) {
						$readersSettled = $false
						throw 'GUI CLI output reader did not settle after process exit'
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

			$failure.Exception.Data['GUI CLI retained owner'] = $retainedOwner
		}

		if (!$exitConfirmed -or !$readersSettled -or ($null -ne $cleanupFailure)) {
			$script:taskChildrenClosed = $false
			$failure = if ($null -ne $primaryFailure) {
				$primaryFailure
			} else {
				$cleanupFailure
			}

			$failure.Exception.Data['GUI CLI temporary directory'] = $taskTemporaryDirectory
		}

		if ($null -ne $cleanupFailure) {
			if ($null -ne $primaryFailure) {
				$primaryFailure.Exception.Data['GUI CLI cleanup failure'] = $cleanupFailure
				Write-Warning -Message "GUI CLI child cleanup failed: Process=$processIdentifier, Path=$taskExecutablePath, StartTimeUTC=$processStartTime, ExitConfirmed=$exitConfirmed, ReadersSettled=$readersSettled; $($cleanupFailure.Exception.Message)" -WarningAction Continue
			} else {
				throw $cleanupFailure
			}
		}
	}

	return $record
}

function Assert-Result($Record, [bool]$Success, [string]$Pattern) {
	Assert-True (($Record.Code -eq 0) -eq $Success) 'GUI CLI returned an unexpected exit code'
	Assert-True (($Record.Out + $Record.Error) -match $Pattern) ('GUI CLI response did not match ' + $Pattern)
}

$primaryFailure = $null
$script:taskChildrenClosed = $true
try {
	[void](New-Item -ItemType Directory -Path $taskTemporaryDirectory)
	# Synthetic fixtures are intentionally not printed or passed on the command line.
	$privateKey = [Convert]::ToBase64String([byte[]](1..32))
	$peerKey = [Convert]::ToBase64String([byte[]](2..33))
	$text = "[Interface]`nPrivateKey = $privateKey`nAddress = 10.88.0.1/32`nListenPort = 51820`nMTU = 1280`n`n[WireSteam]`nAppID = 480`nRelayOnly = true`nStatsSeconds = 10`n`n[Peer]`nPublicKey = $peerKey`nSteamID = 76561198000000001`nAllowedIPs = 10.88.0.2/32`nDial = false`n"
	$path = Join-Path $taskTemporaryDirectory 'saved configuration.conf'
	$saved = Invoke-Application @('save-config', $path) ($taskUTF8Encoding.GetBytes($text))
	Assert-Result $saved $true 'Saved private configuration:'
	Assert-True (![string]::IsNullOrEmpty($saved.Out) -and [string]::IsNullOrEmpty($saved.Error)) 'Successful save emitted an error'
	Assert-True (!$saved.Out.Contains($privateKey) -and !$saved.Out.Contains($peerKey)) 'save-config exposed a key'
	Assert-True ([IO.File]::ReadAllText($path) -eq $text) 'save-config changed the UTF-8 configuration text'
	$acl = Get-Acl -LiteralPath $path
	Assert-True $acl.AreAccessRulesProtected 'Saved private file inherited directory permissions'
	foreach ($rule in $acl.Access) {
		$securityIdentifier = $rule.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
		Assert-True ($securityIdentifier -ne 'S-1-1-0' -and $securityIdentifier -ne 'S-1-5-32-545') 'Saved private file grants access to broad user groups'
	}

	Assert-Result (Invoke-Application @('save-config', $path) ($taskUTF8Encoding.GetBytes($text))) $false 'exist|overwrite'
	Assert-True ([IO.File]::ReadAllText($path) -eq $text) 'Rejected save changed the existing file'
	$replacement = $text.Replace('StatsSeconds = 10', 'StatsSeconds = 20')
	Assert-Result (Invoke-Application @('save-config', $path, '--overwrite') ($taskUTF8Encoding.GetBytes($replacement))) $true 'Saved private configuration:'
	Assert-True ([IO.File]::ReadAllText($path) -eq $replacement) 'Explicit overwrite did not replace the saved file'
	$bad = $text.Replace('10.88.0.2/32', '8.8.8.8/32')
	Assert-Result (Invoke-Application @('save-config', $path, '--overwrite') ($taskUTF8Encoding.GetBytes($bad))) $false 'validation'
	Assert-True ([IO.File]::ReadAllText($path) -eq $replacement) 'Invalid overwrite changed the existing file'

	$invalidPath = Join-Path $taskTemporaryDirectory 'invalid.conf'
	Assert-Result (Invoke-Application @('save-config', $invalidPath) ([byte[]](0xC3, 0x28))) $false 'UTF-8'
	Assert-Result (Invoke-Application @('save-config', $invalidPath) ([byte[]]@())) $false 'empty'
	$oversize = [byte[]]::new(1024 * 1024 + 1)
	Assert-Result (Invoke-Application @('save-config', $invalidPath) $oversize) $false '1 MiB'
	Assert-True (!(Test-Path -LiteralPath $invalidPath)) 'Invalid input unexpectedly created a file'
	$leak = $text.Replace('[Interface]', ('[Interface]' + "`n" + $privateKey + ' = hidden'))
	$rejected = Invoke-Application @('save-config', $invalidPath) ($taskUTF8Encoding.GetBytes($leak))
	Assert-Result $rejected $false 'Unsupported configuration setting'
	Assert-True (!(($rejected.Out + $rejected.Error).Contains($privateKey))) 'Validation diagnostics exposed an unknown private setting'
	$badSection = $text.Replace('[Interface]', ('[' + $privateKey + ']'))
	$rejectedSection = Invoke-Application @('save-config', $invalidPath) ($taskUTF8Encoding.GetBytes($badSection))
	Assert-Result $rejectedSection $false 'Unsupported configuration section'
	Assert-True (!(($rejectedSection.Out + $rejectedSection.Error).Contains($privateKey))) 'Validation diagnostics exposed an unknown private section'

	Assert-Result (Invoke-Application @('save-config', $path, '--overwrite', '--overwrite')) $false 'Duplicate'
	Assert-Result (Invoke-Application @('save-config', $path, '--verbose')) $false 'Unknown option|Missing value'
	foreach ($interval in @('0', '3601', '-1', 'NaN')) {
		Assert-Result (Invoke-Application @('bridge', $path, '--stats-seconds', $interval)) $false 'interval|numeric'
	}

	Assert-Result (Invoke-Application @('bridge', $path, '--stats-seconds', '1', '--stats-seconds', '2')) $false 'Duplicate'
	foreach ($eventName in @('Global\WireSteamStop-0123456789abcdef0123456789abcdef', 'Local\WireSteamStop-short', 'Local\Other-0123456789abcdef0123456789abcdef')) {
		Assert-Result (Invoke-Application @('bridge', $path, '--stop-event', $eventName)) $false 'stop-event'
	}

	$validName = 'Local\WireSteamStop-' + [guid]::NewGuid().ToString('N')
	Assert-Result (Invoke-Application @('bridge', $path, '--stop-event', $validName, '--stop-event', $validName)) $false 'Duplicate'
	Assert-Result (Invoke-Application @('--help')) $true 'save-config|stop-event|stats-seconds'
	Write-Output 'PASS GUI CLI: UTF-8 and size bounds, validated private save/overwrite, diagnostics redaction, stop-event names and statistics options'
} catch {
	$primaryFailure = $_
	throw
} finally {
	$temporaryCleanupFailure = $null
	if ($script:taskChildrenClosed) {
		foreach ($name in @('saved configuration.conf', 'invalid.conf')) {
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
	}

	if ($null -ne $temporaryCleanupFailure) {
		if ($null -ne $primaryFailure) {
			$primaryFailure.Exception.Data['GUI CLI temporary cleanup failure'] = $temporaryCleanupFailure
			Write-Warning -Message ("GUI CLI temporary cleanup failed: " + $temporaryCleanupFailure.Exception.Message) -WarningAction Continue
		} else {
			throw $temporaryCleanupFailure
		}
	}
}
