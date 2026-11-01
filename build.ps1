param(
	[ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
	[ValidateNotNullOrEmpty()][string]$OutputDirectory = 'dist',
	[switch]$Installer
)

$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
& cmake -S $taskRoot -B (Join-Path $taskRoot 'build') -A x64
if ($LASTEXITCODE -ne 0) {
	throw 'CMake configuration failed'
}

& cmake --build (Join-Path $taskRoot 'build') --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
	throw 'Compilation failed'
}

& ctest --test-dir (Join-Path $taskRoot 'build') -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) {
	throw 'Tests failed'
}

$taskDistributionDirectory = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
	[IO.Path]::GetFullPath($OutputDirectory)
} else {
	[IO.Path]::GetFullPath((Join-Path $taskRoot $OutputDirectory))
}

$taskUIPath = [IO.Path]::GetFullPath((Join-Path $taskDistributionDirectory 'WireSteam.UI.exe'))
$taskUIBackupPath = $null
if (Test-Path -LiteralPath $taskUIPath -PathType Leaf) {
	$taskUIBusy = $false
	try {
		$taskUIProbe = [IO.File]::Open($taskUIPath, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
		$taskUIProbe.Dispose()
	} catch [IO.IOException] {
		$taskUIBusy = $true
	}

	if ($taskUIBusy) {
		# Windows can rename a mapped executable. Preserve the running image
		# inside build, then install the new one; never terminate the UI/VPN.
		$taskBackupRoot = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build'))
		$taskUIBackupPath = [IO.Path]::GetFullPath((Join-Path $taskBackupRoot ('WireSteam.UI.previous-' + [guid]::NewGuid().ToString('N') + '.exe')))
		if (([IO.Path]::GetDirectoryName($taskUIPath) -ne $taskDistributionDirectory) -or
			([IO.Path]::GetDirectoryName($taskUIBackupPath) -ne $taskBackupRoot)) {
			throw 'Unexpected UI update paths'
		}

		Move-Item -LiteralPath $taskUIPath -Destination $taskUIBackupPath
		Write-Output 'The running UI was preserved. Quit and reopen WireSteam after the update to load the new version.'
	}
}

& cmake --install (Join-Path $taskRoot 'build') --config $Configuration --prefix $taskDistributionDirectory
if ($LASTEXITCODE -ne 0) {
	if ($taskUIBackupPath -and !(Test-Path -LiteralPath $taskUIPath)) {
		Move-Item -LiteralPath $taskUIBackupPath -Destination $taskUIPath
	}

	throw 'Packaging failed'
}

$taskPayloadNames = @(
	'WireSteam.UI.exe',
	'WireSteam.exe',
	'WireSteamService.exe',
	'WireSteamTestServer.exe',
	'WireSteamTestClient.exe',
	'WireSteamSpeedServer.exe',
	'WireSteamSpeedClient.exe',
	'steam_api64.dll',
	'steam_appid.txt',
	'README.md',
	'CONFIGURATION.md',
	'GUI.md',
	'SPEEDTEST.md',
	'INTERNET.md',
	'INSTALLER.md',
	'FORWARDING.md',
	'THIRD_PARTY_NOTICES.md',
	'LICENSE',
	'docs',
	'examples'
)
$taskFiles = @($taskPayloadNames | ForEach-Object {
		Join-Path $taskDistributionDirectory $_
	})
$taskChecksums = @($taskFiles | ForEach-Object {
		$entry = Get-Item -LiteralPath $_
		if ($entry.PSIsContainer) {
			Get-ChildItem -LiteralPath $_ -File -Recurse
		} else {
			$entry
		}
	} | ForEach-Object {
		$digest = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
		$relativePath = $_.FullName.Substring($taskDistributionDirectory.Length + 1).Replace('\', '/')
		'{0}  {1}' -f $digest.Hash.ToLowerInvariant(), $relativePath
	})
Set-Content -LiteralPath (Join-Path $taskDistributionDirectory 'SHA256SUMS.txt') -Value $taskChecksums -Encoding ascii
Compress-Archive -LiteralPath ($taskFiles + (Join-Path $taskDistributionDirectory 'SHA256SUMS.txt')) -DestinationPath (Join-Path $taskDistributionDirectory 'WireSteam-win64.zip') -Force
Compress-Archive -LiteralPath @((Join-Path $taskDistributionDirectory 'WireSteamSpeedServer.exe'), (Join-Path $taskDistributionDirectory 'WireSteamSpeedClient.exe'), (Join-Path $taskDistributionDirectory 'SPEEDTEST.md')) -DestinationPath (Join-Path $taskDistributionDirectory 'WireSteam-speedtest-win64.zip') -Force
Write-Output "Ready: $(Join-Path $taskDistributionDirectory 'WireSteam.exe')"
Write-Output "Desktop UI: $(Join-Path $taskDistributionDirectory 'WireSteam.UI.exe')"
Write-Output "Archive: $(Join-Path $taskDistributionDirectory 'WireSteam-win64.zip')"

if ($Installer) {
	& (Join-Path $taskRoot 'build-installer.ps1') -Configuration $Configuration -PayloadDirectory $taskDistributionDirectory -SkipBuild
}
