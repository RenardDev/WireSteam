param(
	[Parameter(Mandatory = $true)][string]$ServerPath,
	[Parameter(Mandatory = $true)][string]$ClientPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskServerPath = [IO.Path]::GetFullPath($ServerPath)
$taskClientPath = [IO.Path]::GetFullPath($ClientPath)
$taskTemporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ('WireSteam-concurrent-' + [guid]::NewGuid().ToString('N'))
$taskApplications = [Collections.Generic.List[object]]::new()
$taskSockets = [Collections.Generic.List[Net.Sockets.TcpClient]]::new()
$taskFiles = [Collections.Generic.List[string]]::new()

# A tracked managed reader task preserves server line ordering without requiring
# a PowerShell runspace on its continuations. The application owns its reader.
if (!('WireSteamConcurrentLog' -as [type])) {
	Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

public sealed class WireSteamConcurrentLog {
	private readonly ConcurrentQueue<string> lines = new ConcurrentQueue<string>();
	private Exception failure;
	private Task reading;

	// The owning application starts one reader and keeps it alive until this task settles.
	public Task Start(StreamReader reader) {
		reading = ReadLinesAsync(reader);

		return reading;
	}

	private async Task ReadLinesAsync(StreamReader reader) {
		try {
			while (true) {
				string line = await reader.ReadLineAsync().ConfigureAwait(false);
				if (line == null) {
					return;
				}

				try {
					lines.Enqueue(line);
				} catch (Exception exception) {
					// Keep draining the pipe even when retaining one line fails.
					Interlocked.CompareExchange(ref failure, exception, null);
				}
			}
		} catch (Exception exception) {
			Interlocked.CompareExchange(ref failure, exception, null);
			throw;
		}
	}

	public string[] Snapshot() {
		ThrowIfFailed();

		return lines.ToArray();
	}

	public bool WaitForEnd(int milliseconds) {
		bool ended = reading.Wait(milliseconds);
		ThrowIfFailed();

		return ended;
	}

	private void ThrowIfFailed() {
		Exception capturedFailure = Interlocked.CompareExchange(ref failure, null, null);
		if (capturedFailure != null) {
			throw new InvalidOperationException("Concurrent speed output collection failed.", capturedFailure);
		}
	}
}
'@
}

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

function Start-Application([string]$Executable, [string[]]$Arguments) {
	$start = [Diagnostics.ProcessStartInfo]::new()
	$start.FileName = $Executable
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
	$registered = $false
	$process = [Diagnostics.Process]::new()
	try {
		$application = [pscustomobject]@{
			Process = $process
			Output = $null
			ErrorOutput = $null
			Endpoint = ''
			StandardInputWriter = $null
			StandardOutputReader = $null
			StandardErrorReader = $null
			Log = $null
			Started = $false
			ProcessIdentifier = $null
			ExecutablePath = $Executable
			StartTimeUTC = $null
			ExitConfirmed = $false
			ReadersSettled = $false
			Closed = $false
			CleanupFailure = $null
		}

		# The main try/finally owns this record before any child can start.
		[void]$taskApplications.Add($application)
		$registered = $true
		$collector = [WireSteamConcurrentLog]::new()
		$application.Log = $collector
		$process.StartInfo = $start
		[void]$process.Start()
		$application.Started = $true
		$application.ProcessIdentifier = $process.Id
		$application.StartTimeUTC = $process.StartTime.ToUniversalTime()
		$application.StandardInputWriter = $process.StandardInput
		$application.StandardOutputReader = $process.StandardOutput
		$application.StandardErrorReader = $process.StandardError
		$application.StandardInputWriter.Close()

		$application.Output = $collector.Start($application.StandardOutputReader)

		$application.ErrorOutput = $application.StandardErrorReader.ReadToEndAsync()

		return $application
	} finally {
		if (!$registered) {
			$process.Dispose()
		}
	}
}

function Read-ApplicationText($Application) {
	$text = $Application.Log.Snapshot() -join "`n"
	if ($Application.ErrorOutput.IsCompleted) {
		$text += "`n" + $Application.ErrorOutput.Result
	}

	return $text
}

function Wait-Application($Application, [int]$TimeoutMilliseconds = 20000) {
	if (!$Application.Process.WaitForExit($TimeoutMilliseconds)) {
		throw "Concurrent speed application timed out: $(Read-ApplicationText $Application)"
	}

	Assert-True ($Application.Log.WaitForEnd(3000) -and $Application.ErrorOutput.Wait(3000)) 'Application output did not close'

	return [pscustomobject]@{
		Code = $Application.Process.ExitCode
		Text = (Read-ApplicationText $Application)
	}
}

function Close-Application($Application) {
	$cleanupTimeoutMilliseconds = 15000
	$cleanupFailure = $null
	$exitConfirmed = !$Application.Started
	$readersSettled = $false
	try {
		if ($Application.Started) {
			if (!$Application.Process.HasExited) {
				# Only the exact short test application owned by this record is stopped.
				$Application.Process.Kill()
			}

			$exitConfirmed = $Application.Process.WaitForExit($cleanupTimeoutMilliseconds)
			if (!$exitConfirmed) {
				throw 'Concurrent speed child did not exit after termination'
			}
		}
	} catch {
		$cleanupFailure = $_
	}

	if ($exitConfirmed) {
		$readersSettled = $true
		foreach ($readerTask in @($Application.Output, $Application.ErrorOutput)) {
			if ($null -eq $readerTask) {
				continue
			}

			try {
				if (!$readerTask.Wait($cleanupTimeoutMilliseconds)) {
					$readersSettled = $false
					throw 'Concurrent speed output reader did not settle after process exit'
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

	$Application.ExitConfirmed = $exitConfirmed
	$Application.ReadersSettled = $readersSettled

	if ($exitConfirmed -and $readersSettled) {
		$ownersDisposed = $true
		foreach ($owner in @($Application.StandardInputWriter, $Application.StandardOutputReader, $Application.StandardErrorReader, $Application.Process)) {
			if ($null -eq $owner) {
				continue
			}

			try {
				$owner.Dispose()
			} catch {
				$ownersDisposed = $false

				if ($null -eq $cleanupFailure) {
					$cleanupFailure = $_
				}
			}
		}

		$Application.Closed = $ownersDisposed
	}

	if ($null -ne $cleanupFailure) {
		$Application.CleanupFailure = $cleanupFailure
		$failure = [InvalidOperationException]::new("Concurrent speed child cleanup failed: Process=$($Application.ProcessIdentifier), Path=$($Application.ExecutablePath), StartTimeUTC=$($Application.StartTimeUTC), ExitConfirmed=$exitConfirmed, ReadersSettled=$readersSettled; $($cleanupFailure.Exception.Message)", $cleanupFailure.Exception)
		$failure.Data['Concurrent speed application owner'] = $Application
		throw $failure
	}
}

function Assert-Success($Result, [string]$Label) {
	Assert-True ($Result.Code -eq 0) "$Label failed with exit $($Result.Code): $($Result.Text)"
}

function Wait-Log($Application, [string]$Pattern, [int]$TimeoutMilliseconds = 5000) {
	$timer = [Diagnostics.Stopwatch]::StartNew()
	while ($timer.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
		foreach ($line in $Application.Log.Snapshot()) {
			if ($line -match $Pattern) {
				return $line
			}
		}

		if ($Application.Process.HasExited) {
			throw "Server exited while waiting for $Pattern : $(Read-ApplicationText $Application)"
		}

		Start-Sleep -Milliseconds 20
	}

	throw "Server did not log $Pattern : $(Read-ApplicationText $Application)"
}

function Start-SpeedServer([int]$MaxClients, [int]$RunSeconds = 30) {
	$application = Start-Application $taskServerPath @('--listen', '127.0.0.1:0', '--max-clients', [string]$MaxClients, '--run-seconds', [string]$RunSeconds)
	$ready = Wait-Log $application 'Speed server listening on (127\.0\.0\.1:[0-9]+)' 8000
	Assert-True ($ready -match 'Speed server listening on (127\.0\.0\.1:[0-9]+)') 'No resolved loopback endpoint'
	$application.Endpoint = $Matches[1]

	return $application
}

function Open-Control([string]$Endpoint) {
	$client = [Net.Sockets.TcpClient]::new()
	try {
		Assert-True ($client.ConnectAsync([Net.IPAddress]::Loopback, [int]$Endpoint.Split(':')[-1]).Wait(3000)) 'Cannot connect loopback control socket'
		$client.GetStream().WriteTimeout = 2000
		$client.GetStream().ReadTimeout = 2000
		[void]$taskSockets.Add($client)

		return $client
	} catch {
		$client.Dispose()
		throw
	}
}

function Create-ClientPattern($Socket, [string]$Suffix) {
	# .NET may expose an IPv4-mapped IPv6 local address even though the
	# explicitly IPv4 server sees an ordinary 127.0.0.1 peer.
	$endpoint = [regex]::Escape('127.0.0.1:' + $Socket.Client.LocalEndPoint.Port)

	return '^\[client ([0-9]+) ' + $endpoint + '\] ' + $Suffix
}

function New-ResultPath([string]$Name) {
	$file = Join-Path $taskTemporaryDirectory $Name
	[void]$taskFiles.Add($file)

	return $file
}

function Assert-ResultJSON([string]$Path, [int]$Rate, [int]$Payload, [int]$Streams = 2, [int]$Seconds = 1, [bool]$Both = $true) {
	Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) "Missing result file $Path"
	$resultJSON = [IO.File]::ReadAllText($Path) | ConvertFrom-Json
	Assert-True ($resultJSON.schema -eq 'wiresteam.speed.v1') 'Incorrect concurrent JSON schema'
	$results = @($resultJSON.results)
	$expected = @('tcp/upload')
	if ($Both) {
		$expected = @('tcp/upload', 'tcp/download', 'udp/upload', 'udp/download')
	}

	Assert-True ($results.Count -eq $expected.Count) 'Unexpected concurrent result count'
	$phases = @()
	foreach ($result in $results) {
		$phase = "$($result.protocol)/$($result.direction)"
		$phases += $phase
		Assert-True ($expected -contains $phase) "Unexpected result $phase"
		Assert-True ($result.requestedSeconds -eq $Seconds) "Wrong duration in $phase"
		Assert-True ($result.sender.bytes -gt 0 -and $result.receiver.bytes -gt 0) "Missing independent traffic in $phase"
		Assert-True ($result.receiver.bytes -le $result.sender.bytes -and $result.receiverGoodputMbps -gt 0) "Invalid independent receiver accounting in $phase"
		$computed = [double]$result.receiver.bytes * 8 / [double]$result.receiver.seconds / 1000000
		Assert-True ([Math]::Abs($computed - $result.receiverGoodputMbps) -le [Math]::Max(0.02, $computed * 0.01)) "Inconsistent bytes/time/goodput in $phase"

		if ($result.protocol -eq 'tcp') {
			Assert-True ($result.streams -eq $Streams -and $result.sender.bytes -eq $result.receiver.bytes) 'Concurrent TCP accounting crossed sessions'
		} else {
			Assert-True ($result.udpTargetMbps -eq $Rate) 'Concurrent UDP rate crossed sessions'
			Assert-True ($result.sender.bytes -eq $result.sender.packets * $Payload) 'Concurrent UDP sender payload accounting crossed sessions'
			Assert-True ($result.receiver.bytes -eq $result.receiver.packets * $Payload) 'Concurrent UDP receiver payload accounting crossed sessions'
			Assert-True ($result.receiver.invalid -eq 0 -and $result.receiver.packets -le $result.sender.packets) 'Concurrent UDP tokens or counters crossed sessions'
			Assert-True ($result.lossPercent -ge 0 -and $result.lossPercent -le 100) 'Invalid concurrent UDP loss'
		}
	}

	Assert-True ((@($phases | Select-Object -Unique)).Count -eq $expected.Count) 'Duplicate concurrent phases'
}

$primaryFailure = $null
try {
	[void](New-Item -ItemType Directory -Path $taskTemporaryDirectory)
	foreach ($arguments in @(@('--max-clients', '0'), @('--max-clients', '65'), @('--max-clients'), @('--max-clients', '2', '--max-clients', '3'), @('--once', '--max-clients', '2'), @('--max-clients', '2', '--once'))) {
		$invalid = Wait-Application (Start-Application $taskServerPath $arguments) 5000
		Assert-True ($invalid.Code -ne 0 -and $invalid.Text -match 'max-clients|1-64|1 to 64|Duplicate') 'Concurrent client limit was not validated'
	}

	$server = Start-SpeedServer 4
	$idle = Open-Control $server.Endpoint
	$idlePattern = Create-ClientPattern $idle 'Control client connected'
	$idleLine = Wait-Log $server $idlePattern
	Assert-True ($idleLine -match '^\[client ([0-9]+) ') 'Idle client has no isolated ID'
	$idleClientIdentifier = $Matches[1]
	$firstPath = New-ResultPath 'first.json'
	$secondPath = New-ResultPath 'second.json'
	$shared = @('--target', $server.Endpoint, '--seconds', '1', '--streams', '2', '--protocol', 'both', '--direction', 'both', '--quiet')
	$first = Start-Application $taskClientPath ($shared + @('--udp-mbps', '8', '--udp-size', '1100', '--json', $firstPath))
	$second = Start-Application $taskClientPath ($shared + @('--udp-mbps', '16', '--udp-size', '900', '--json', $secondPath))

	# A log ordering barrier proves simultaneous work without relying on a tight
	# wall-clock throughput threshold on a busy CI machine.
	$barrier = [Diagnostics.Stopwatch]::StartNew()
	$overlapping = $false
	while ($barrier.ElapsedMilliseconds -lt 5000) {
		$started = [Collections.Generic.HashSet[string]]::new()
		foreach ($line in $server.Log.Snapshot()) {
			if ($line -match '^\[client ([0-9]+) [^\]]+\] Starting TCP upload phase 1\b') {
				[void]$started.Add($Matches[1])
			}

			if ($line -match '\bRESULT:') {
				break
			}
		}

		if ($started.Count -ge 2) {
			$overlapping = $true
			break
		}

		if ($first.Process.HasExited -or $second.Process.HasExited) {
			break
		}

		Start-Sleep -Milliseconds 20
	}

	Assert-True $overlapping "Two clients did not start data phases before the first result: $(Read-ApplicationText $server)"

	# A malformed fourth client must not cancel either running measurement.
	$bad = Open-Control $server.Endpoint
	$badPattern = Create-ClientPattern $bad 'Control client connected'
	$badLine = Wait-Log $server $badPattern
	Assert-True ($badLine -match '^\[client ([0-9]+) ') 'Malformed client has no isolated ID'
	$malformedClientIdentifier = $Matches[1]
	$badBytes = [byte[]]@(0x57, 0x53, 0x53, 0x31, 0x7F, 0xFF, 0xFF, 0xFF)
	$bad.GetStream().Write($badBytes, 0x00, $badBytes.Length)
	$bad.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
	$bad.Close()
	Assert-Success (Wait-Application $first 30000) 'First concurrent client'
	Assert-Success (Wait-Application $second 30000) 'Second concurrent client'
	Assert-ResultJSON $firstPath 8 1100
	Assert-ResultJSON $secondPath 16 900
	$serverText = Read-ApplicationText $server
	Assert-True ($serverText -match ('(?m)^\[client ' + $malformedClientIdentifier + ' [^\]]+\] Control client failed:')) 'Malformed client was not isolated and reported'
	Assert-True ($serverText -notmatch ('(?m)^\[client ' + $idleClientIdentifier + ' [^\]]+\] (Control client failed|Control client finished)')) 'Idle connection ended before legitimate clients completed'
	Assert-True (!$server.Process.HasExited) 'A worker failure stopped the concurrent listener'
	$idle.Close()

	# Capacity rejection must be immediate, and closing the idle owner must free
	# the slot for a subsequent ordinary client.
	$limited = Start-SpeedServer 1 20
	$owner = Open-Control $limited.Endpoint
	[void](Wait-Log $limited (Create-ClientPattern $owner 'Control client connected'))
	$busyTimer = [Diagnostics.Stopwatch]::StartNew()
	$busy = Wait-Application (Start-Application $taskClientPath @('--target', $limited.Endpoint, '--seconds', '1', '--protocol', 'tcp', '--direction', 'upload', '--quiet')) 5000
	Assert-True ($busy.Code -ne 0 -and $busy.Text -match 'busy|maximum concurrent') "Capacity rejection was not explicit: $($busy.Text)"
	Assert-True ($busyTimer.ElapsedMilliseconds -lt 5000) 'Capacity rejection waited for the idle client timeout'
	$owner.Close()
	[void](Wait-Log $limited 'Control client failed:' 3000)
	Start-Sleep -Milliseconds 350
	$reusePath = New-ResultPath 'reused-slot.json'
	$reuse = Wait-Application (Start-Application $taskClientPath @('--target', $limited.Endpoint, '--seconds', '1', '--streams', '1', '--protocol', 'tcp', '--direction', 'upload', '--json', $reusePath, '--quiet')) 15000
	Assert-Success $reuse 'Client after slot release'
	Assert-ResultJSON $reusePath 0 0 1 1 $false

	# Expiring the accept window during a phase must still deliver that phase's
	# result and join the worker before the server exits.
	$draining = Start-SpeedServer 2 1
	$drainPath = New-ResultPath 'drained-phase.json'
	$drain = Wait-Application (Start-Application $taskClientPath @('--target', $draining.Endpoint, '--seconds', '2', '--streams', '1', '--protocol', 'tcp', '--direction', 'upload', '--json', $drainPath, '--quiet')) 15000
	Assert-Success $drain 'Client during server expiry'
	Assert-ResultJSON $drainPath 0 0 1 2 $false
	Assert-Success (Wait-Application $draining 5000) 'Server after draining active phase'

	# The finish grace is not another work window: the next requested phase
	# must be rejected explicitly instead of extending the server lifetime.
	$expired = Start-SpeedServer 2 1
	$expiredClient = Wait-Application (Start-Application $taskClientPath @('--target', $expired.Endpoint, '--seconds', '2', '--streams', '1', '--protocol', 'tcp', '--direction', 'both', '--quiet')) 15000
	Assert-True ($expiredClient.Code -ne 0 -and $expiredClient.Text -match 'lifetime ended|no new test phases') "Fresh phase during expiry was not explicitly rejected: $($expiredClient.Text)"
	Assert-True ($expiredClient.Text -match 'TCP UPLOAD .* RESULT:') 'The already-running phase did not finish before expiry rejection'
	Assert-Success (Wait-Application $expired 5000) 'Server after rejecting a phase during finish grace'
	Assert-True ((Read-ApplicationText $expired) -notmatch 'Starting TCP download') 'Server admitted new measurement work after listener expiry'

	Write-Output 'PASS concurrent speed server: simultaneous independent TCP/UDP sessions, idle/malformed isolation, explicit capacity rejection, slot reuse, active-phase shutdown drain and expiry rejection'
} catch {
	$primaryFailure = $_
	throw
} finally {
	$cleanupFailure = $null
	$allApplicationsClosed = $true
	foreach ($socket in $taskSockets) {
		try {
			$socket.Dispose()
		} catch {
			if ($null -eq $cleanupFailure) {
				$cleanupFailure = $_
			}
		}
	}

	foreach ($application in $taskApplications) {
		try {
			Close-Application $application
		} catch {
			if ($null -eq $cleanupFailure) {
				$cleanupFailure = $_
			}
		}

		if (!$application.Closed) {
			$allApplicationsClosed = $false
		}
	}

	if ($allApplicationsClosed) {
		foreach ($file in $taskFiles) {
			try {
				if ([IO.Path]::GetDirectoryName($file) -ne $taskTemporaryDirectory) {
					throw 'Unexpected concurrent test output path'
				}

				if (Test-Path -LiteralPath $file -PathType Leaf) {
					Remove-Item -LiteralPath $file -Force
				}
			} catch {
				if ($null -eq $cleanupFailure) {
					$cleanupFailure = $_
				}
			}
		}

		try {
			if (Test-Path -LiteralPath $taskTemporaryDirectory -PathType Container) {
				[IO.Directory]::Delete($taskTemporaryDirectory)
			}
		} catch {
			if ($null -eq $cleanupFailure) {
				$cleanupFailure = $_
			}
		}
	}

	if ($null -ne $cleanupFailure) {
		$cleanupFailure.Exception.Data['Concurrent application owners'] = $taskApplications
		$cleanupFailure.Exception.Data['Concurrent temporary directory'] = $taskTemporaryDirectory

		if ($null -ne $primaryFailure) {
			$primaryFailure.Exception.Data['Concurrent cleanup failure'] = $cleanupFailure
			Write-Warning -Message ("Concurrent speed cleanup failed: " + $cleanupFailure.Exception.Message) -WarningAction Continue
		} else {
			throw $cleanupFailure
		}
	}
}
