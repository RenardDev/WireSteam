param(
	[Parameter(Mandatory = $true)][string]$ServerPath,
	[Parameter(Mandatory = $true)][string]$ClientPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskServerPath = [IO.Path]::GetFullPath($ServerPath)
$taskClientPath = [IO.Path]::GetFullPath($ClientPath)
$taskTemporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ('WireSteam-speed-' + [guid]::NewGuid().ToString('N'))
$taskApplications = [Collections.Generic.List[object]]::new()
$taskFiles = [Collections.Generic.List[string]]::new()

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

function Start-Application([string]$Executable, [string[]]$Arguments, [bool]$ReadOutput = $true) {
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
			Prefix = ''
			Endpoint = ''
			StandardInputWriter = $null
			StandardOutputReader = $null
			StandardErrorReader = $null
			ReadinessOutput = $null
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
		$process.StartInfo = $start
		[void]$process.Start()
		$application.Started = $true
		$application.ProcessIdentifier = $process.Id
		$application.StartTimeUTC = $process.StartTime.ToUniversalTime()
		$application.StandardInputWriter = $process.StandardInput
		$application.StandardOutputReader = $process.StandardOutput
		$application.StandardErrorReader = $process.StandardError
		$application.StandardInputWriter.Close()

		if ($ReadOutput) {
			$application.Output = $application.StandardOutputReader.ReadToEndAsync()
		}

		$application.ErrorOutput = $application.StandardErrorReader.ReadToEndAsync()

		return $application
	} finally {
		if (!$registered) {
			$process.Dispose()
		}
	}
}

function Wait-Application($Application, [int]$TimeoutMilliseconds = 20000) {
	if (!$Application.Process.WaitForExit($TimeoutMilliseconds)) {
		throw "Speed application timed out: $($Application.Process.StartInfo.FileName)"
	}

	if ($null -eq $Application.Output) {
		$Application.Output = $Application.StandardOutputReader.ReadToEndAsync()
	}

	Assert-True ($Application.Output.Wait(5000) -and $Application.ErrorOutput.Wait(5000)) 'Speed application output did not close'

	return [pscustomobject]@{
		Code = $Application.Process.ExitCode
		Text = $Application.Prefix + $Application.Output.Result + $Application.ErrorOutput.Result
	}
}

function Invoke-Application([string]$Executable, [string[]]$Arguments, [int]$TimeoutMilliseconds = 20000) {
	return Wait-Application (Start-Application $Executable $Arguments) $TimeoutMilliseconds
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
				throw 'Speed child did not exit after termination'
			}
		}
	} catch {
		$cleanupFailure = $_
	}

	if ($exitConfirmed) {
		$readersSettled = $true
		foreach ($readerTask in @($Application.ReadinessOutput, $Application.Output, $Application.ErrorOutput)) {
			if ($null -eq $readerTask) {
				continue
			}

			try {
				if (!$readerTask.Wait($cleanupTimeoutMilliseconds)) {
					$readersSettled = $false
					throw 'Speed output reader did not settle after process exit'
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
		$failure = [InvalidOperationException]::new("Speed child cleanup failed: Process=$($Application.ProcessIdentifier), Path=$($Application.ExecutablePath), StartTimeUTC=$($Application.StartTimeUTC), ExitConfirmed=$exitConfirmed, ReadersSettled=$readersSettled; $($cleanupFailure.Exception.Message)", $cleanupFailure.Exception)
		$failure.Data['Speed application owner'] = $Application
		throw $failure
	}
}

function Assert-Exit($Result, [bool]$Success, [string]$Label) {
	if (($Result.Code -eq 0) -ne $Success) {
		throw "$Label returned exit $($Result.Code): $($Result.Text)"
	}
}

function Start-SpeedServer([bool]$Once = $false, [int]$RunSeconds = 60) {
	$readinessTimeout = [TimeSpan]::FromSeconds(8)
	$arguments = @('--listen', '127.0.0.1:0', '--run-seconds', [string]$RunSeconds)
	if ($Once) {
		$arguments += '--once'
	}

	$application = Start-Application $taskServerPath $arguments $false
	$readinessTimer = [Diagnostics.Stopwatch]::StartNew()
	while ($readinessTimer.Elapsed -lt $readinessTimeout) {
		$remaining = [int][Math]::Max(1, ($readinessTimeout - $readinessTimer.Elapsed).TotalMilliseconds)
		$line = $application.StandardOutputReader.ReadLineAsync()
		$application.ReadinessOutput = $line
		Assert-True ($line.Wait($remaining)) 'Speed server did not announce readiness'

		if ($null -eq $line.Result) {
			$result = Wait-Application $application 2000
			throw "Speed server exited before readiness: $($result.Text)"
		}

		$application.Prefix += $line.Result + "`n"

		if ($line.Result -match 'Speed server listening on (127\.0\.0\.1:[0-9]+)') {
			$application.Endpoint = $Matches[1]
			$application.Output = $application.StandardOutputReader.ReadToEndAsync()
			return $application
		}
	}

	throw 'Speed server readiness timed out'
}

function Open-Control([string]$Endpoint) {
	$port = [int]($Endpoint.Split(':')[-1])
	$client = [Net.Sockets.TcpClient]::new()
	try {
		$connect = $client.ConnectAsync([Net.IPAddress]::Loopback, $port)
		Assert-True ($connect.Wait(3000)) 'Cannot connect to loopback speed control socket'
		$stream = $client.GetStream()
		$stream.ReadTimeout = 3000
		$stream.WriteTimeout = 3000

		return $client
	} catch {
		$client.Dispose()
		throw
	}
}

function Send-BadFrame([string]$Endpoint, [byte[]]$Bytes) {
	$client = Open-Control $Endpoint
	try {
		$stream = $client.GetStream()
		if ($Bytes.Length) {
			$stream.Write($Bytes, 0x00, $Bytes.Length)
		}

		$client.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
		# The server may send a bounded protocol error or close immediately.
		$buffer = [byte[]]::new(4096)
		try {
			[void]$stream.Read($buffer, 0x00, $buffer.Length)
		} catch [IO.IOException] {
		}
	} finally {
		$client.Dispose()
	}
}

function Assert-ResultJSON([string]$Path, [string[]]$ExpectedPhases, [int]$Streams) {
	Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) 'Client did not save JSON output'
	$resultJSON = [IO.File]::ReadAllText($Path) | ConvertFrom-Json
	Assert-True ($resultJSON.schema -eq 'wiresteam.speed.v1') 'Unexpected benchmark JSON schema'
	Assert-True ($resultJSON.units.throughput -eq 'Mbit/s') 'JSON throughput units are inconsistent'
	Assert-True ($resultJSON.rttMs.min -ge 0 -and $resultJSON.rttMs.mean -ge $resultJSON.rttMs.min -and $resultJSON.rttMs.max -ge $resultJSON.rttMs.mean) 'Invalid control RTT statistics'
	$results = @($resultJSON.results)
	Assert-True ($results.Count -eq $ExpectedPhases.Count) 'Unexpected benchmark phase count'
	$actualPhases = @()
	foreach ($result in $results) {
		$phase = "$($result.protocol)/$($result.direction)"
		$actualPhases += $phase
		Assert-True ($ExpectedPhases -contains $phase) "Unexpected measurement phase $phase"
		Assert-True ($result.requestedSeconds -eq 1) "Wrong requested duration in $phase"
		Assert-True ($result.sender.bytes -gt 0 -and $result.receiver.bytes -gt 0) "No transferred payload in $phase"
		Assert-True ($result.sender.seconds -gt 0 -and $result.receiver.seconds -gt 0) "Invalid measurement interval in $phase"
		Assert-True ($result.receiverGoodputMbps -gt 0 -and $result.receiver.mbps -gt 0) "No positive receiver throughput in $phase"
		$computed = [double]$result.receiver.bytes * 8.0 / [double]$result.receiver.seconds / 1000000.0
		$tolerance = [Math]::Max(0.02, [Math]::Abs($computed) * 0.01)
		Assert-True ([Math]::Abs([double]$result.receiverGoodputMbps - $computed) -le $tolerance) "Receiver bytes/time/goodput disagree in $phase"
		Assert-True ([Math]::Abs([double]$result.receiver.mbps - [double]$result.receiverGoodputMbps) -le 0.01) "Receiver summary and detailed throughput disagree in $phase"
		Assert-True ($result.receiver.bytes -le $result.sender.bytes) "Receiver count exceeds sender count in $phase"

		if ($result.protocol -eq 'tcp') {
			Assert-True ($result.streams -eq $Streams) 'TCP stream count was not preserved'
			Assert-True ($result.receiver.bytes -eq $result.sender.bytes) 'TCP stream did not deliver all counted bytes'
		} else {
			Assert-True ($result.udpTargetMbps -eq 16) 'UDP offered rate was not preserved'
			Assert-True ($result.sender.packets -gt 0 -and $result.receiver.packets -gt 0) 'UDP packet accounting is empty'
			Assert-True ($result.lossPercent -ge 0 -and $result.lossPercent -le 100) 'Invalid UDP loss percentage'
			Assert-True ($result.receiver.invalid -eq 0) 'Loopback benchmark reported malformed UDP packets'
		}
	}

	Assert-True ((@($actualPhases | Select-Object -Unique)).Count -eq $ExpectedPhases.Count) 'Benchmark phases are duplicated'
}

$primaryFailure = $null
try {
	[void](New-Item -ItemType Directory -Path $taskTemporaryDirectory)
	foreach ($executable in @($taskServerPath, $taskClientPath)) {
		Assert-Exit (Invoke-Application $executable @('--help')) $true 'Help'
		Assert-Exit (Invoke-Application $executable @('--self-test')) $true 'Unit tests'
	}

	$invalidClientCases = @(
		@('--seconds', '0'), @('--seconds', '301'), @('--seconds', 'abc'),
		@('--streams', '0'), @('--streams', '33'), @('--protocol', 'invalid'),
		@('--direction', 'invalid'), @('--udp-size', '63'), @('--udp-size', '65460'),
		@('--udp-mbps', '0'), @('--udp-mbps', 'nan'), @('--block-size', '0'),
		@('--block-size', '4294967295'), @('--target', '127.0.0.1:0'), @('--unknown'),
		@('--protocol', 'udp', '--seconds', '300', '--udp-mbps', '100000', '--udp-size', '64')
	)
	foreach ($arguments in $invalidClientCases) {
		$invalid = Invoke-Application $taskClientPath $arguments
		Assert-True ($invalid.Code -eq 2 -and $invalid.Text -match 'Argument error') ('Expected argument rejection before connecting: ' + ($arguments -join ' ') + '; ' + $invalid.Text)
	}

	foreach ($arguments in @(@('--listen', 'not-an-endpoint'), @('--run-seconds', '0'), @('--unknown'))) {
		Assert-Exit (Invoke-Application $taskServerPath $arguments) $false ('Invalid server arguments: ' + ($arguments -join ' '))
	}

	# Reserve a TCP port and close its listener, leaving no service to connect to.
	$unused = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
	try {
		$unused.Start()
		$deadEndpoint = '127.0.0.1:' + $unused.LocalEndpoint.Port
	} finally {
		$unused.Stop()
	}

	Assert-Exit (Invoke-Application $taskClientPath @('--target', $deadEndpoint, '--seconds', '1', '--protocol', 'tcp') 15000) $false 'Unavailable endpoint'

	$server = Start-SpeedServer
	# Persistent servers must reject malformed frames without losing readiness.
	Send-BadFrame $server.Endpoint ([byte[]]@())
	Send-BadFrame $server.Endpoint ([byte[]]@(0x42, 0x41, 0x44, 0x21, 0x00, 0x00, 0x00, 0x01, 0x00))
	Send-BadFrame $server.Endpoint ([byte[]]@(0x57, 0x53, 0x53, 0x31, 0x7F, 0xFF, 0xFF, 0xFF))
	Assert-True (!$server.Process.HasExited) 'Malformed control traffic terminated the persistent server'
	$combinedJSONPath = Join-Path $taskTemporaryDirectory 'combined results.json'
	[void]$taskFiles.Add($combinedJSONPath)
	$arguments = @('--target', $server.Endpoint, '--seconds', '1', '--streams', '4', '--protocol', 'both', '--direction', 'both', '--udp-mbps', '16', '--udp-size', '1100', '--block-size', '32768', '--json', $combinedJSONPath, '--quiet')
	$combined = Invoke-Application $taskClientPath $arguments 45000
	Assert-Exit $combined $true 'Bidirectional TCP/UDP measurement'
	Assert-ResultJSON $combinedJSONPath @('tcp/upload', 'tcp/download', 'udp/upload', 'udp/download') 4
	Assert-True (!$server.Process.HasExited) 'Persistent server exited after a successful client'

	# A separate --once server proves that a valid client finishes the control session.
	$once = Start-SpeedServer $true
	$onceJSONPath = Join-Path $taskTemporaryDirectory 'once.json'
	[void]$taskFiles.Add($onceJSONPath)
	$onceClient = Invoke-Application $taskClientPath @('--target', $once.Endpoint, '--seconds', '1', '--streams', '1', '--protocol', 'tcp', '--direction', 'upload', '--block-size', '1024', '--json', $onceJSONPath, '--quiet') 20000
	Assert-Exit $onceClient $true 'One-shot TCP measurement'
	Assert-ResultJSON $onceJSONPath @('tcp/upload') 1
	Assert-Exit (Wait-Application $once 5000) $true 'Orderly one-shot server shutdown'
	$expiring = Start-SpeedServer $false 1
	Assert-Exit (Wait-Application $expiring 4000) $true 'Bounded idle server shutdown'

	Write-Output 'PASS speed applications: bounded CLI validation, dead endpoint, malformed framing, 4-stream TCP/UDP upload/download, receiver JSON accounting and orderly one-shot shutdown'
} catch {
	$primaryFailure = $_
	throw
} finally {
	$cleanupFailure = $null
	$allApplicationsClosed = $true
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
		# Delete only the exact files owned by this invocation; never recurse.
		foreach ($file in $taskFiles) {
			try {
				if ([IO.Path]::GetDirectoryName($file) -ne $taskTemporaryDirectory) {
					throw 'Unexpected temporary output path'
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
		$cleanupFailure.Exception.Data['Speed application owners'] = $taskApplications
		$cleanupFailure.Exception.Data['Speed temporary directory'] = $taskTemporaryDirectory

		if ($null -ne $primaryFailure) {
			$primaryFailure.Exception.Data['Speed cleanup failure'] = $cleanupFailure
			Write-Warning -Message ("Speed application cleanup failed: " + $cleanupFailure.Exception.Message) -WarningAction Continue
		} else {
			throw $cleanupFailure
		}
	}
}
