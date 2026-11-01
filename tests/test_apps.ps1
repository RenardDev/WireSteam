param(
	[Parameter(Mandatory = $true)][string]$ServerPath,
	[Parameter(Mandatory = $true)][string]$ClientPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$testServerPath = [System.IO.Path]::GetFullPath($ServerPath)
$testClientPath = [System.IO.Path]::GetFullPath($ClientPath)
if (!(Test-Path -LiteralPath $testServerPath -PathType Leaf)) {
	throw "Server executable not found: $testServerPath"
}

if (!(Test-Path -LiteralPath $testClientPath -PathType Leaf)) {
	throw "Client executable not found: $testClientPath"
}

$taskTemporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('WireSteam-test-apps-' + [guid]::NewGuid().ToString('N'))
$taskTemporaryRoot = [System.IO.Path]::GetFullPath($taskTemporaryRoot)
$ownedProcesses = [System.Collections.Generic.List[object]]::new()
$ownedFiles = [System.Collections.Generic.List[string]]::new()
$ownedSockets = [System.Collections.Generic.List[System.Net.Sockets.Socket]]::new()
$script:caseNumber = 0

function Read-Log([string]$Path) {
	if (!(Test-Path -LiteralPath $Path)) {
		return ''
	}

	$stream = $null
	$reader = $null
	try {
		$sharing = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
		$stream = [System.IO.FileStream]::new($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $sharing)
		$reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8)

		return $reader.ReadToEnd()
	} catch [System.IO.FileNotFoundException] {
		return ''
	} finally {
		try {
			if ($null -ne $reader) {
				$reader.Dispose()
			}
		} finally {
			if ($null -ne $stream) {
				$stream.Dispose()
			}
		}
	}
}

function ConvertTo-WindowsArgument([string]$Value) {
	if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
		return $Value
	}

	$quoted = [System.Text.StringBuilder]::new()
	[void]$quoted.Append('"')
	$slashes = 0
	foreach ($character in $Value.ToCharArray()) {
		if ([int]$character -eq 0x5C) {
			++$slashes
			continue
		}

		if ([int]$character -eq 0x22) {
			[void]$quoted.Append(('\' * (2 * $slashes + 1)))
			[void]$quoted.Append('"')
		} else {
			[void]$quoted.Append(('\' * $slashes))
			[void]$quoted.Append($character)
		}

		$slashes = 0
	}

	[void]$quoted.Append(('\' * (2 * $slashes)))
	[void]$quoted.Append('"')

	return $quoted.ToString()
}

function Start-TestApplication([string]$Name, [string]$Executable, [string[]]$Arguments) {
	++$script:caseNumber
	$standardOutputPath = Join-Path $taskTemporaryRoot ($script:caseNumber.ToString() + '-' + $Name + '-out.log')
	$standardErrorPath = Join-Path $taskTemporaryRoot ($script:caseNumber.ToString() + '-' + $Name + '-err.log')
	$ownedFiles.Add($standardOutputPath)
	$ownedFiles.Add($standardErrorPath)
	# Start-Process joins its ArgumentList without escaping. Build one command
	# line using the Windows C-runtime quote/backslash rules (also works on PS5).
	$commandLine = (
		@(
			$Arguments | ForEach-Object {
				ConvertTo-WindowsArgument $_
			}
		) -join ' '
	)
	$record = [pscustomobject]@{
		Name = $Name
		Process = $null
		Stdout = $standardOutputPath
		Stderr = $standardErrorPath
		ExecutablePath = $Executable
		ProcessIdentifier = $null
		StartTimeUTC = $null
		ExitConfirmed = $false
		Closed = $false
	}

	# The main finally owns this record before Start-Process can return a child.
	$ownedProcesses.Add($record)
	$record.Process = Start-Process -FilePath $Executable -ArgumentList $commandLine -PassThru -WindowStyle Hidden `
		-RedirectStandardOutput $standardOutputPath -RedirectStandardError $standardErrorPath
	# Keep the owned process handle alive so Windows PowerShell 5.1 can still
	# query the actual exit code after a short-lived child exits and is refreshed.
	$null = $record.Process.Handle
	$record.ProcessIdentifier = $record.Process.Id
	$record.StartTimeUTC = $record.Process.StartTime.ToUniversalTime()

	return $record
}

function Stop-TestApplication($Record) {
	if ($null -eq $Record.Process) {
		$Record.ExitConfirmed = $true
		return
	}

	try {
		if (!$Record.Process.HasExited) {
			try {
				# Stop only this invocation's exact owned test process handle.
				$Record.Process.Kill()
			} catch {
				if (!$Record.Process.HasExited) {
					throw
				}
			}
		}

		$Record.ExitConfirmed = $Record.Process.WaitForExit(2000)
		if (!$Record.ExitConfirmed) {
			throw 'Owned test application did not exit after termination'
		}
	} catch {
		$failure = [InvalidOperationException]::new("Test application cleanup failed: Process=$($Record.ProcessIdentifier), Path=$($Record.ExecutablePath), StartTimeUTC=$($Record.StartTimeUTC), ExitConfirmed=$($Record.ExitConfirmed); $($_.Exception.Message)", $_.Exception)
		$failure.Data['Test application owner'] = $Record
		throw $failure
	}
}

function Close-TestApplication($Record) {
	Stop-TestApplication $Record

	if ($null -ne $Record.Process) {
		$Record.Process.Dispose()
	}

	$Record.Closed = $true
}

function Wait-TestApplication($Record, [int]$Seconds = 12) {
	if (!$Record.Process.WaitForExit($Seconds * 1000)) {
		throw "Timed out waiting for $($Record.Name) after $Seconds seconds"
	}

	$Record.Process.Refresh()
}

function Assert-TestResult($Record, [bool]$Success) {
	$output = (Read-Log $Record.Stdout) + (Read-Log $Record.Stderr)
	$exitCode = $Record.Process.ExitCode
	if ($Success) {
		if ($exitCode -ne 0 -or $output -notmatch '(?im)\bPASS\b') {
			throw "$($Record.Name) failed (exit $exitCode):`n$output"
		}
	} elseif ($exitCode -eq 0) {
		throw "$($Record.Name) unexpectedly succeeded:`n$output"
	}

	Write-Output "[PASS] $($Record.Name)"
}

function Invoke-TestClient([string]$Name, [string[]]$Arguments, [bool]$Success = $true, [int]$Seconds = 12) {
	$record = Start-TestApplication $Name $testClientPath $Arguments
	Wait-TestApplication $record $Seconds
	Assert-TestResult $record $Success
}

function Reserve-DeadEndpoint([System.Net.Sockets.SocketType]$Type, [System.Net.Sockets.ProtocolType]$Protocol) {
	$socket = [System.Net.Sockets.Socket]::new([System.Net.Sockets.AddressFamily]::InterNetwork, $Type, $Protocol)
	$registered = $false
	try {
		$socket.ExclusiveAddressUse = $true
		$socket.Bind([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, 0))
		$ownedSockets.Add($socket)
		$registered = $true

		return '127.0.0.1:' + $socket.LocalEndPoint.Port.ToString()
	} finally {
		if (!$registered) {
			$socket.Dispose()
		}
	}
}

function Get-FreeSharedEndpoint {
	for ($attempt = 0; $attempt -lt 16; ++$attempt) {
		$tcpSocket = $null
		$udpSocket = $null
		try {
			$tcpSocket = [System.Net.Sockets.Socket]::new([System.Net.Sockets.AddressFamily]::InterNetwork, [System.Net.Sockets.SocketType]::Stream, [System.Net.Sockets.ProtocolType]::Tcp)
			$tcpSocket.ExclusiveAddressUse = $true
			$tcpSocket.Bind([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, 0))
			$port = $tcpSocket.LocalEndPoint.Port
			$udpSocket = [System.Net.Sockets.Socket]::new([System.Net.Sockets.AddressFamily]::InterNetwork, [System.Net.Sockets.SocketType]::Dgram, [System.Net.Sockets.ProtocolType]::Udp)
			$udpSocket.ExclusiveAddressUse = $true
			$udpSocket.Bind([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, $port))

			return '127.0.0.1:' + $port.ToString()
		} catch [System.Net.Sockets.SocketException] {
			if ($attempt -eq 15) {
				throw
			}
		} finally {
			try {
				if ($null -ne $udpSocket) {
					$udpSocket.Dispose()
				}
			} finally {
				if ($null -ne $tcpSocket) {
					$tcpSocket.Dispose()
				}
			}
		}
	}

	throw 'Cannot reserve a shared TCP/UDP test port'
}

function Start-CombinedServer {
	# Release both reservations immediately before starting the server. Retry
	# the small release/bind race, while retaining every owned PID for cleanup.
	$combinedReadinessTimeout = [TimeSpan]::FromSeconds(4)
	for ($attempt = 0; $attempt -lt 4; ++$attempt) {
		$endpoint = Get-FreeSharedEndpoint
		$record = Start-TestApplication ('shared-echo-server-' + $attempt) $testServerPath @('--listen', $endpoint, '--run-seconds', '45')
		$readinessTimer = [Diagnostics.Stopwatch]::StartNew()
		while (!$record.Process.HasExited -and $readinessTimer.Elapsed -lt $combinedReadinessTimeout) {
			$ready = Read-Log $record.Stdout
			$tcpReady = $ready -match ('READY TCP ' + [regex]::Escape($endpoint) + '\r?\n')
			$udpReady = $ready -match ('READY UDP ' + [regex]::Escape($endpoint) + '\r?\n')
			if ($tcpReady -and $udpReady) {
				return [pscustomobject]@{ Server = $record; Endpoint = $endpoint }
			}

			Start-Sleep -Milliseconds 20
		}

		Stop-TestApplication $record
	}

	throw 'Combined TCP/UDP echo server failed to bind and publish readiness after four attempts'
}

$primaryFailure = $null
try {
	[void](New-Item -ItemType Directory -Path $taskTemporaryRoot)
	$serverReadinessTimeout = [TimeSpan]::FromSeconds(8)
	$server = Start-TestApplication 'echo-server' $testServerPath @('--listen', '127.0.0.1:0', '--run-seconds', '60')
	$readinessTimer = [Diagnostics.Stopwatch]::StartNew()
	$tcpTarget = $null
	$udpTarget = $null
	while (!$tcpTarget -or !$udpTarget) {
		$ready = Read-Log $server.Stdout
		if ($ready -match 'READY TCP (127\.0\.0\.1:\d+)') {
			$tcpTarget = $Matches[1]
		}

		if ($ready -match 'READY UDP (127\.0\.0\.1:\d+)') {
			$udpTarget = $Matches[1]
		}

		if ($server.Process.HasExited) {
			throw "Echo server exited before readiness:`n$ready`n$(Read-Log $server.Stderr)"
		}

		if ($readinessTimer.Elapsed -ge $serverReadinessTimeout) {
			throw "Echo server did not publish TCP/UDP readiness:`n$ready`n$(Read-Log $server.Stderr)"
		}

		if (!$tcpTarget -or !$udpTarget) {
			Start-Sleep -Milliseconds 20
		}
	}

	Write-Output "[PASS] default server publishes separate ephemeral TCP/UDP listeners: $tcpTarget / $udpTarget"

	Invoke-TestClient 'tcp-three-connections-large-half-close' @(
		'--target', $tcpTarget, '--tcp', '--tcp-size', '2097152', '--count', '2', '--connections', '3',
		'--half-close', '--timeout-ms', '5000', '--interval-ms', '0')
	Invoke-TestClient 'udp-three-connections' @(
		'--target', $udpTarget, '--udp', '--udp-size', '64', '--count', '3', '--connections', '3',
		'--timeout-ms', '3000', '--interval-ms', '0')
	Invoke-TestClient 'udp-maximum-datagram' @(
		'--target', $udpTarget, '--udp', '--udp-size', '65507', '--count', '3', '--connections', '3',
		'--timeout-ms', '3000', '--interval-ms', '0')
	Invoke-TestClient 'udp-zero-length-datagram' @(
		'--target', $udpTarget, '--udp', '--udp-size', '0', '--count', '2', '--connections', '2',
		'--timeout-ms', '3000', '--interval-ms', '0')
	Invoke-TestClient 'tcp-custom-message' @(
		'--target', $tcpTarget, '--tcp', '--message', 'WireSteam-custom-message-123', '--count', '2',
		'--connections', '2', '--half-close', '--interval-ms', '0')
	Invoke-TestClient 'udp-custom-message' @(
		'--target', $udpTarget, '--udp', '--message', 'WireSteam-custom-message-123', '--count', '2',
		'--connections', '2', '--allow-udp-loss', '--interval-ms', '0')

	$shared = Start-CombinedServer
	$message = 'WireSteam echo check with spaces'
	$bothClient = Start-TestApplication 'default-both-protocols-quoted-message' $testClientPath @(
		'--target', $shared.Endpoint, '--message', $message, '--count', '2', '--connections', '2',
		'--half-close', '--interval-ms', '0')
	Wait-TestApplication $bothClient
	Assert-TestResult $bothClient $true
	$bothOutput = Read-Log $bothClient.Stdout
	if ($bothOutput -notmatch 'RESULT TCP connection=2 sent=2 received=2' -or
		$bothOutput -notmatch 'RESULT UDP connection=2 sent=2 received=2' -or
		!$bothOutput.Contains(('MESSAGE bytes=' + $message.Length + ' preview="' + $message + '"'))) {
		throw "Default TCP+UDP selection or native argument quoting was not preserved:`n$bothOutput"
	}

	if ($shared.Server.Process.HasExited) {
		throw 'Combined echo server unexpectedly stopped'
	}

	Invoke-TestClient 'reject-oversized-udp' @('--udp', '--udp-size', '65508') $false 3
	Invoke-TestClient 'reject-zero-target-port' @('--target', '127.0.0.1:0') $false 3
	Invoke-TestClient 'reject-message-size-conflict' @('--tcp', '--message', 'abc', '--tcp-size', '3') $false 3
	Invoke-TestClient 'reject-zero-count' @('--tcp', '--count', '0') $false 3
	$invalidServer = Start-TestApplication 'reject-zero-server-limit' $testServerPath @('--max-clients', '0')
	Wait-TestApplication $invalidServer 3
	Assert-TestResult $invalidServer $false

	# The TCP endpoint is bound but not listening, guaranteeing refusal. The
	# UDP endpoint stays bound without reading, guaranteeing a bounded timeout
	# without depending on firewall rules, an external host or ICMP behavior.
	$deadTcp = Reserve-DeadEndpoint ([System.Net.Sockets.SocketType]::Stream) ([System.Net.Sockets.ProtocolType]::Tcp)
	$deadUdp = Reserve-DeadEndpoint ([System.Net.Sockets.SocketType]::Dgram) ([System.Net.Sockets.ProtocolType]::Udp)
	Invoke-TestClient 'dead-tcp-backend-fails' @(
		'--target', $deadTcp, '--tcp', '--count', '1', '--timeout-ms', '200', '--interval-ms', '0') $false 4
	Invoke-TestClient 'silent-udp-backend-times-out' @(
		'--target', $deadUdp, '--udp', '--udp-size', '64', '--count', '1', '--timeout-ms', '200', '--interval-ms', '0') $false 4

	if ($server.Process.HasExited) {
		throw "Echo server unexpectedly stopped:`n$(Read-Log $server.Stderr)"
	}

	Write-Output 'All standalone test-app integration checks passed. No Steam connection was used.'
} catch {
	$primaryFailure = $_
	Write-Error $_ -ErrorAction Continue
	foreach ($record in $ownedProcesses) {
		try {
			$output = (Read-Log $record.Stdout) + (Read-Log $record.Stderr)
			Write-Output "--- $($record.Name) ---`n$output"
		} catch {
			Write-Warning -Message ("Read test application logs for $($record.Name): " + $_.Exception.Message) -WarningAction Continue
		}
	}

	exit 1
} finally {
	$cleanupFailure = $null
	$allProcessesClosed = $true
	foreach ($record in $ownedProcesses) {
		try {
			Close-TestApplication $record
		} catch {
			if ($null -eq $cleanupFailure) {
				$cleanupFailure = $_
			}
		}

		if (!$record.Closed) {
			$allProcessesClosed = $false
		}
	}

	foreach ($socket in $ownedSockets) {
		try {
			$socket.Dispose()
		} catch {
			if ($null -eq $cleanupFailure) {
				$cleanupFailure = $_
			}
		}
	}

	if ($allProcessesClosed) {
		# Delete only exact log paths created by this invocation. No recursive
		# deletion or process-name matching is needed.
		$approvedPrefix = $taskTemporaryRoot + [System.IO.Path]::DirectorySeparatorChar
		foreach ($file in $ownedFiles) {
			try {
				$absolute = [System.IO.Path]::GetFullPath($file)
				if ($absolute.StartsWith($approvedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
					if (Test-Path -LiteralPath $absolute -PathType Leaf) {
						Remove-Item -LiteralPath $absolute -Force
					}
				}
			} catch {
				if ($null -eq $cleanupFailure) {
					$cleanupFailure = $_
				}
			}
		}

		try {
			if (Test-Path -LiteralPath $taskTemporaryRoot -PathType Container) {
				[System.IO.Directory]::Delete($taskTemporaryRoot)
			}
		} catch {
			if ($null -eq $cleanupFailure) {
				$cleanupFailure = $_
			}
		}
	}

	if ($null -ne $cleanupFailure) {
		$cleanupFailure.Exception.Data['Test application owners'] = $ownedProcesses
		$cleanupFailure.Exception.Data['Test temporary directory'] = $taskTemporaryRoot

		if ($null -ne $primaryFailure) {
			$primaryFailure.Exception.Data['Test cleanup failure'] = $cleanupFailure
			Write-Warning -Message ("Test application cleanup failed: " + $cleanupFailure.Exception.Message) -WarningAction Continue
		} else {
			throw $cleanupFailure
		}
	}
}
