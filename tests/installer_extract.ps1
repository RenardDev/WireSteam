param(
	[Parameter(Mandatory = $true)][string]$MsiPath,
	[Parameter(Mandatory = $true)][string]$WixPath,
	[Parameter(Mandatory = $true)][string]$ManifestPath,
	[Parameter(Mandatory = $true)][string]$PayloadRoot,
	[Parameter(Mandatory = $true)][string]$GuardPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$taskWorkDirectory = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build-installer'))
$taskVerification = [IO.Path]::GetFullPath((Join-Path $taskWorkDirectory ('verify-' + [guid]::NewGuid().ToString('N'))))
if ([IO.Path]::GetDirectoryName($taskVerification) -ne $taskWorkDirectory) {
	throw 'Unexpected extraction path.'
}

$taskExtracted = Join-Path $taskVerification 'extracted'
$taskSource = Join-Path $taskVerification 'extracted.wxs'
try {
	[IO.Directory]::CreateDirectory($taskVerification) | Out-Null

	# Decompile/extract only; never execute MSI installation or custom actions.
	& $WixPath msi decompile $MsiPath -x $taskExtracted -out $taskSource -sui
	if ($LASTEXITCODE -ne 0) {
		throw 'Could not extract the MSI for verification.'
	}

	[xml]$taskXML = Get-Content -LiteralPath $taskSource -Raw
	$taskNamespaces = New-Object Xml.XmlNamespaceManager($taskXML.NameTable)
	$taskNamespaces.AddNamespace('w', 'http://wixtoolset.org/schemas/v4/wxs')
	$taskManifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
	$taskFiles = @($taskXML.SelectNodes('//w:File', $taskNamespaces))
	if ($taskFiles.Count -ne $taskManifest.files.Count) {
		throw 'The extracted MSI has an unexpected file count.'
	}

	$taskKnown = @{}
	foreach ($taskEntry in $taskManifest.files) {
		$taskRelativePath = [string]$taskEntry.path
		$taskHash = [Security.Cryptography.SHA256]::Create()
		try {
			$taskIdentifier = [BitConverter]::ToString($taskHash.ComputeHash([Text.Encoding]::UTF8.GetBytes($taskRelativePath))).Replace('-', '').Substring(0, 24)
		} finally {
			$taskHash.Dispose()
		}

		$taskFileIdentifier = if ($taskRelativePath -eq 'WireSteam.UI.exe') {
			'WireSteamUIFile'
		} else {
			'File_' + $taskIdentifier
		}

		$taskKnown[$taskFileIdentifier] = $taskRelativePath
	}

	foreach ($taskFile in $taskFiles) {
		$taskFileIdentifier = [string]$taskFile.Id
		if (!$taskKnown.ContainsKey($taskFileIdentifier)) {
			throw 'The extracted MSI has an unexpected file identifier.'
		}

		$taskActual = Join-Path $taskExtracted ('File\' + $taskFileIdentifier)
		$taskExpected = Join-Path $PayloadRoot $taskKnown[$taskFileIdentifier]
		if ((Get-FileHash -LiteralPath $taskActual -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $taskExpected -Algorithm SHA256).Hash) {
			throw ('The embedded MSI payload differs from dist: ' + $taskKnown[$taskFileIdentifier])
		}
	}

	$taskGuard = Join-Path $taskExtracted 'Binary\InstallerGuard'
	if ((Get-FileHash -LiteralPath $taskGuard -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $GuardPath -Algorithm SHA256).Hash) {
		throw 'The embedded installer guard differs from the tested build.'
	}

	$taskIcon = Join-Path $taskExtracted 'Icon\WireSteamIcon'
	if ((Get-FileHash -LiteralPath $taskIcon -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath (Join-Path $taskRoot 'gui\Assets\WireSteam.ico') -Algorithm SHA256).Hash) {
		throw 'The embedded MSI icon differs from the application icon.'
	}

	Write-Output "PASS MSI extraction: all $($taskFiles.Count) embedded payload files, native guard and application icon match their tested SHA256 bytes. No installation was executed."
} finally {
	# Only delete this invocation's exact, validated child of the build directory.
	$taskResolved = [IO.Path]::GetFullPath($taskVerification)
	if ([IO.Path]::GetDirectoryName($taskResolved) -ne $taskWorkDirectory -or [IO.Path]::GetFileName($taskResolved) -notmatch '^verify-[a-f0-9]{32}$') {
		throw 'Refusing cleanup outside the installer verification directory.'
	}

	if (Test-Path -LiteralPath $taskResolved) {
		Remove-Item -LiteralPath $taskResolved -Recurse -Force
	}
}
