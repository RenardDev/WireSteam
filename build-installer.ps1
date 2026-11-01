param(
	[ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
	[ValidateNotNullOrEmpty()][string]$PayloadDirectory = 'dist',
	[switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskRoot = $PSScriptRoot
$taskPayload = if ([IO.Path]::IsPathRooted($PayloadDirectory)) {
	[IO.Path]::GetFullPath($PayloadDirectory)
} else {
	[IO.Path]::GetFullPath((Join-Path $taskRoot $PayloadDirectory))
}

$taskWorkDirectory = Join-Path $taskRoot 'build-installer'
[IO.Directory]::CreateDirectory($taskWorkDirectory) | Out-Null

if (!$SkipBuild) {
	& (Join-Path $taskRoot 'build.ps1') -Configuration $Configuration -OutputDirectory $taskPayload
}

$taskManifestPath = Join-Path $taskRoot 'installer\payload.json'
& (Join-Path $taskRoot 'tests\installer_payload.ps1') -ManifestPath $taskManifestPath -PayloadRoot $taskPayload
$taskGuard = Join-Path $taskRoot "build\$Configuration\WireSteamInstallerGuard.dll"
if (!(Test-Path -LiteralPath $taskGuard -PathType Leaf)) {
	throw 'Build the native installer guard first (run build.ps1).'
}

# Build tools stay inside the ignored build directory. Never modify global tools.
# WiX 4.0.6 uses MS-RL and does not require accepting the separate WiX 7 OSMF EULA.
$taskWixVersion = '4.0.6'
$taskWix = Join-Path $taskWorkDirectory 'wix4\wix.exe'
if (!(Test-Path -LiteralPath $taskWix -PathType Leaf)) {
	$taskDotnet = Join-Path $taskRoot 'build-dotnet\dotnet.exe'
	if (!(Test-Path -LiteralPath $taskDotnet)) {
		$taskDotnet = (Get-Command dotnet -ErrorAction Stop).Source
	}

	& $taskDotnet tool install wix --tool-path (Join-Path $taskWorkDirectory 'wix4') --version $taskWixVersion --allow-roll-forward --configfile (Join-Path $taskRoot 'installer\nuget.config')
	if ($LASTEXITCODE -ne 0) {
		throw 'Could not restore the pinned WiX build tool from NuGet.'
	}
}

$taskActualVersion = & $taskWix --version
if ($LASTEXITCODE -ne 0 -or $taskActualVersion -notlike "$taskWixVersion+*") {
	throw 'Unexpected WiX compiler version.'
}

Push-Location -LiteralPath $taskWorkDirectory
try {
	$taskExtension = Join-Path $taskWorkDirectory ".wix\extensions\WixToolset.UI.wixext\$taskWixVersion\wixext4\WixToolset.UI.wixext.dll"
	if (!(Test-Path -LiteralPath $taskExtension -PathType Leaf)) {
		& $taskWix extension add "WixToolset.UI.wixext/$taskWixVersion"
		if ($LASTEXITCODE -ne 0) {
			throw 'Could not restore the pinned WiX UI extension.'
		}
	}

	if (!(Test-Path -LiteralPath $taskExtension -PathType Leaf)) {
		throw 'The restored WiX UI extension was not found in the local build cache.'
	}

	# Generate only the explicitly validated allowlist, never harvest dist/configs.
	$taskManifest = Get-Content -LiteralPath $taskManifestPath -Raw | ConvertFrom-Json
	$taskGenerated = New-Object Text.StringBuilder
	[void]$taskGenerated.AppendLine('<?xml version="1.0" encoding="utf-8"?>')
	[void]$taskGenerated.AppendLine('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
	[void]$taskGenerated.AppendLine('	<Fragment>')
	foreach ($taskGroup in @('core', 'docs', 'speedtest', 'testapps')) {
		[void]$taskGenerated.AppendLine(('		<ComponentGroup Id="Payload_{0}">' -f $taskGroup))
		foreach ($taskFile in $taskManifest.files | Where-Object {
				$_.component -eq $taskGroup
			}) {
			$taskRelativePath = [string]$taskFile.path
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

			$taskDirectoryIdentifier = if ($taskRelativePath.StartsWith('examples/', [StringComparison]::Ordinal)) {
				'ExamplesFolder'
			} elseif ($taskRelativePath -eq 'docs/images/overview.png') {
				'DocsImagesFolder'
			} else {
				'INSTALLFOLDER'
			}

			$taskSource = [Security.SecurityElement]::Escape((Join-Path $taskPayload $taskRelativePath))
			$taskFileName = [Security.SecurityElement]::Escape([IO.Path]::GetFileName($taskRelativePath))
			[void]$taskGenerated.AppendLine(('			<Component Id="Component_{0}" Directory="{1}" Guid="*" Bitness="always64">' -f $taskIdentifier, $taskDirectoryIdentifier))
			[void]$taskGenerated.AppendLine(('				<File Id="{0}" Source="{1}" Name="{2}" KeyPath="yes" />' -f $taskFileIdentifier, $taskSource, $taskFileName))
			[void]$taskGenerated.AppendLine('			</Component>')
		}

		[void]$taskGenerated.AppendLine('		</ComponentGroup>')
	}

	[void]$taskGenerated.AppendLine('	</Fragment>')
	[void]$taskGenerated.AppendLine('</Wix>')
	$taskGeneratedPath = Join-Path $taskWorkDirectory 'Payload.generated.wxs'
	[IO.File]::WriteAllText($taskGeneratedPath, $taskGenerated.ToString(), (New-Object Text.UTF8Encoding($false)))

	[xml]$taskUIProject = Get-Content -LiteralPath (Join-Path $taskRoot 'gui\WireSteam.UI.csproj') -Raw
	$taskProductVersion = [string]$taskUIProject.Project.PropertyGroup.Version
	if ($taskProductVersion -notmatch '^\d+\.\d+\.\d+$') {
		throw 'MSI requires a three-part release version in the UI project.'
	}

	$taskMSI = Join-Path $taskPayload "WireSteam-$taskProductVersion-x64.msi"
	$taskArguments = @('build', (Join-Path $taskRoot 'installer\Package.wxs'), (Join-Path $taskRoot 'installer\WireSteamUI.wxs'), $taskGeneratedPath,
		'-arch', 'x64', '-culture', 'en-US', '-ext', $taskExtension,
		'-d', "ProductVersion=$taskProductVersion", '-d', "ProjectRoot=$taskRoot", '-d', "GuardPath=$taskGuard",
		'-intermediatefolder', (Join-Path $taskWorkDirectory 'obj'), '-pdbtype', 'none', '-out', $taskMSI)
	& $taskWix @taskArguments
	if ($LASTEXITCODE -ne 0) {
		throw 'WiX MSI compilation failed.'
	}

	& (Join-Path $taskRoot 'installer\FinalizePackage.ps1') -MsiPath $taskMSI
	& (Join-Path $taskRoot 'tests\installer_msi.ps1') -MsiPath $taskMSI -ManifestPath $taskManifestPath -PayloadRoot $taskPayload
	& (Join-Path $taskRoot 'tests\installer_extract.ps1') -MsiPath $taskMSI -WixPath $taskWix -ManifestPath $taskManifestPath -PayloadRoot $taskPayload -GuardPath $taskGuard
	& (Join-Path $taskRoot 'tests\installer_host.ps1') -MsiPath $taskMSI
	$taskDigest = Get-FileHash -LiteralPath $taskMSI -Algorithm SHA256
	$taskChecksum = '{0}  {1}' -f $taskDigest.Hash.ToLowerInvariant(), [IO.Path]::GetFileName($taskMSI)
	Set-Content -LiteralPath ($taskMSI + '.sha256') -Value $taskChecksum -Encoding ascii
	Write-Output "MSI ready: $taskMSI"
	Write-Output "SHA256: $($taskMSI).sha256"
} finally {
	Pop-Location
}
