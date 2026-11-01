param([Parameter(Mandatory = $true)][string]$WireSteamPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskExecutablePath = [IO.Path]::GetFullPath($WireSteamPath)
$taskTemporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ('WireSteam-preflight-' + [guid]::NewGuid().ToString('N'))
$taskConfigurationPath = Join-Path $taskTemporaryDirectory 'preflight.conf'
$taskUTF8Encoding = [Text.UTF8Encoding]::new($false)

function Assert-True([bool]$Condition, [string]$Message) {
	if (!$Condition) {
		throw $Message
	}
}

function Invoke-Preflight {
	$cleanupTimeoutMilliseconds = 15000

	$start = [Diagnostics.ProcessStartInfo]::new()
	$start.FileName = $taskExecutablePath
	$start.Arguments = 'preflight "' + $taskConfigurationPath + '"'
	$start.UseShellExecute = $false
	$start.CreateNoWindow = $true
	$start.RedirectStandardOutput = $true
	$start.RedirectStandardError = $true
	$start.StandardOutputEncoding = $taskUTF8Encoding
	$start.StandardErrorEncoding = $taskUTF8Encoding
	$standardOutputReader = $null
	$standardErrorReader = $null
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
		$standardOutput = $standardOutputReader.ReadToEndAsync()
		$standardError = $standardErrorReader.ReadToEndAsync()

		Assert-True ($process.WaitForExit(10000)) 'Local adapter preflight timed out'
		Assert-True ($standardOutput.Wait(3000) -and $standardError.Wait(3000)) 'Local adapter preflight output did not close'
		$text = $standardOutput.Result + $standardError.Result
		Assert-True (!$text.Contains($script:privateKey) -and !$text.Contains($script:peerKey)) 'Preflight diagnostics exposed a key'
		Assert-True ($text -notmatch 'Setting breakpad|SteamInternal_|Installing WireSteam-|VPN adapter is running') 'Read-only preflight started Steam or an adapter'

		$record = [pscustomobject]@{
			Code = $process.ExitCode
			Text = $text
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
					# This helper owns only the short preflight command it started.
					$process.Kill()
				}

				$exitConfirmed = $process.WaitForExit($cleanupTimeoutMilliseconds)
				if (!$exitConfirmed) {
					throw 'Preflight child did not exit after termination'
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
						throw 'Preflight output reader did not settle after process exit'
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
			foreach ($owner in @($standardOutputReader, $standardErrorReader, $process)) {
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

			$failure.Exception.Data['Preflight retained owner'] = $retainedOwner
		}

		if ($null -ne $cleanupFailure) {
			if ($null -ne $primaryFailure) {
				$primaryFailure.Exception.Data['Preflight cleanup failure'] = $cleanupFailure
				Write-Warning -Message "Preflight child cleanup failed: Process=$processIdentifier, Path=$taskExecutablePath, StartTimeUTC=$processStartTime, ExitConfirmed=$exitConfirmed, ReadersSettled=$readersSettled; $($cleanupFailure.Exception.Message)" -WarningAction Continue
			} else {
				throw $cleanupFailure
			}
		}
	}

	return $record
}

function Write-Fixture([int]$Port, [string]$Address = $script:testAddress) {
	# These deterministic keys exist only in the test fixture and are never real profiles.
	$text = "[Interface]`nPrivateKey = $script:privateKey`nAddress = $Address`nListenPort = $Port`nMTU = 1280`n`n[WireSteam]`nAppID = 480`nRelayOnly = true`n`n[Peer]`nPublicKey = $script:peerKey`nSteamID = 76561198000000001`nAllowedIPs = 10.254.241.2/32`nDial = false`n"
	[IO.File]::WriteAllText($taskConfigurationPath, $text, $taskUTF8Encoding)
}

$occupied = $null
$taskPrimaryFailure = $null
try {
	[void](New-Item -ItemType Directory -Path $taskTemporaryDirectory)
	$privateKey = [Convert]::ToBase64String([byte[]](1..32))
	$peerKey = [Convert]::ToBase64String([byte[]](2..33))
	$randomAddress = 'fd' + [guid]::NewGuid().ToString('N').Substring(0, 30)
	$testAddress = (
		(
			0..7 | ForEach-Object {
				$randomAddress.Substring(0x04 * $_, 4)
			}
		) -join ':'
	) + '/128'
	$occupied = [Net.Sockets.Socket]::new([Net.Sockets.AddressFamily]::InterNetwork, [Net.Sockets.SocketType]::Dgram, [Net.Sockets.ProtocolType]::Udp)
	$occupied.ExclusiveAddressUse = $true
	$occupied.Bind([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, 0))
	$port = $occupied.LocalEndPoint.Port
	Write-Fixture $port
	$blocked = Invoke-Preflight
	Assert-True ($blocked.Code -ne 0 -and $blocked.Text.Contains("ListenPort $port/UDP") -and $blocked.Text.Contains('Stop the existing tunnel')) 'An owned occupied UDP port was not rejected with recovery advice'

	Write-Fixture 0
	$automatic = Invoke-Preflight
	Assert-True ($automatic.Code -eq 0 -and $automatic.Text.Contains('Local adapter preflight passed')) 'Automatic ListenPort unexpectedly failed local preflight'

	# Exercise the real adapter inventory with an existing private address when available.
	$existingAddress = $null
	foreach ($adapter in [Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
		if ($adapter.OperationalStatus -ne [Net.NetworkInformation.OperationalStatus]::Up) {
			continue
		}

		foreach ($unicast in $adapter.GetIPProperties().UnicastAddresses) {
			$bytes = $unicast.Address.GetAddressBytes()
			if ($bytes.Length -eq 0x04 -and ($bytes[0x00] -eq 0x0A -or ($bytes[0x00] -eq 0xAC -and $bytes[0x01] -ge 0x10 -and $bytes[0x01] -le 0x1F) -or ($bytes[0x00] -eq 0xC0 -and $bytes[0x01] -eq 0xA8))) {
				$existingAddress = $unicast.Address.ToString() + '/32'
				break
			}

			if ($bytes.Length -eq 0x10 -and ($bytes[0x00] -band 0xFE) -eq 0xFC) {
				$existingAddress = $unicast.Address.ToString().Split('%')[0] + '/128'
				break
			}
		}

		if ($existingAddress) {
			break
		}
	}

	if ($existingAddress) {
		Write-Fixture 0 $existingAddress
		$addressConflict = Invoke-Preflight
		Assert-True ($addressConflict.Code -ne 0 -and $addressConflict.Text.Contains('already assigned to active interface') -and $addressConflict.Text.Contains('Stop the existing tunnel')) 'An existing active adapter address was not rejected with recovery advice'
	}

	Write-Output 'PASS local adapter preflight CLI: occupied owned UDP socket, automatic ListenPort, active address inventory, key redaction and no Steam/adapter startup'
} catch {
	$taskPrimaryFailure = $_
	throw
} finally {
	$temporaryCleanupFailure = $null
	try {
		if ($occupied) {
			$occupied.Dispose()
		}
	} catch {
		$temporaryCleanupFailure = $_
	}

	$taskRetainedOwner = $null
	if (($null -ne $taskPrimaryFailure) -and $taskPrimaryFailure.Exception.Data.Contains('Preflight retained owner')) {
		$taskRetainedOwner = $taskPrimaryFailure.Exception.Data['Preflight retained owner']
	}

	if ($null -ne $taskRetainedOwner) {
		Write-Warning -Message "Preflight temporary inputs retained until owned child/readers settle: Path=$taskTemporaryDirectory" -WarningAction Continue
	} else {
		try {
			if (Test-Path -LiteralPath $taskConfigurationPath -PathType Leaf) {
				Remove-Item -LiteralPath $taskConfigurationPath -Force
			}
		} catch {
			if ($null -eq $temporaryCleanupFailure) {
				$temporaryCleanupFailure = $_
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
		if ($null -ne $taskPrimaryFailure) {
			$taskPrimaryFailure.Exception.Data['Preflight temporary cleanup failure'] = $temporaryCleanupFailure
			Write-Warning -Message "Preflight temporary cleanup failed: Path=$taskTemporaryDirectory; $($temporaryCleanupFailure.Exception.Message)" -WarningAction Continue
		} else {
			throw $temporaryCleanupFailure
		}
	}
}
