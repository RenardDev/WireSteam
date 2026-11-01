param(
	[Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$ManifestPath,
	[ValidateNotNullOrEmpty()][string]$PayloadRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Inspect the declared payload only. Never run an installed binary, load a DLL,
# invoke an installer, modify an adapter or write to the staged distribution.
$taskPayloadRoot = [IO.Path]::GetFullPath($PayloadRoot)
$taskManifestPath = [IO.Path]::GetFullPath($ManifestPath)
$taskUTF8Encoding = [Text.UTF8Encoding]::new($false, $true)
$taskExpected = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
foreach ($path in @('WireSteam.UI.exe', 'WireSteam.exe', 'WireSteamService.exe', 'steam_api64.dll', 'steam_appid.txt')) {
	$taskExpected.Add($path, 'core')
}

foreach ($path in @('README.md', 'CONFIGURATION.md', 'GUI.md', 'INTERNET.md', 'SPEEDTEST.md', 'INSTALLER.md', 'FORWARDING.md', 'THIRD_PARTY_NOTICES.md', 'LICENSE', 'docs/images/overview.png',
		'examples/internet-client.conf.example', 'examples/internet-server.conf.example', 'examples/peer-a.conf.example', 'examples/peer-b.conf.example', 'examples/port-forward.conf.example')) {
	$taskExpected.Add($path, 'docs')
}

foreach ($path in @('WireSteamSpeedServer.exe', 'WireSteamSpeedClient.exe')) {
	$taskExpected.Add($path, 'speedtest')
}

foreach ($path in @('WireSteamTestServer.exe', 'WireSteamTestClient.exe')) {
	$taskExpected.Add($path, 'testapps')
}

function Assert-Payload([bool]$Condition, [string]$Message) {
	if (!$Condition) {
		throw ('Installer payload: ' + $Message)
	}
}

function Assert-NoReparseAncestors([IO.FileSystemInfo]$Item) {
	while ($null -ne $Item) {
		Assert-Payload (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) ('Reparse points are not allowed in payload paths: ' + $Item.FullName)

		if ($Item -is [IO.FileInfo]) {
			$Item = $Item.Directory
		} else {
			$Item = $Item.Parent
		}
	}
}

function Resolve-PayloadFile([string]$RelativePath) {
	Assert-Payload (![string]::IsNullOrWhiteSpace($RelativePath)) 'An empty payload path is not allowed.'
	Assert-Payload ($RelativePath -cmatch '^[A-Za-z0-9][A-Za-z0-9._/-]*$') ('Expected a literal forward-slash relative path: ' + $RelativePath)
	$segments = $RelativePath.Split('/')
	Assert-Payload (
		@(
			$segments | Where-Object {
				$_ -eq '' -or $_ -eq '.' -or $_ -eq '..'
			}
		).Count -eq 0
	) ('Path traversal is not allowed: ' + $RelativePath)
	$absolute = [IO.Path]::GetFullPath((Join-Path $taskPayloadRoot $RelativePath.Replace('/', [IO.Path]::DirectorySeparatorChar)))
	$prefix = $taskPayloadRoot.TrimEnd([char[]]@('\', '/')) + [IO.Path]::DirectorySeparatorChar
	Assert-Payload ($absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) ('A payload path escaped its root: ' + $RelativePath)
	$item = Get-Item -LiteralPath $absolute -Force
	Assert-Payload (!$item.PSIsContainer) ('Expected a regular payload file: ' + $RelativePath)
	Assert-NoReparseAncestors $item

	return $absolute
}

function Assert-Properties($Value, [string[]]$Names, [string]$Context) {
	Assert-Payload ($null -ne $Value) ($Context + ' must be an object.')
	$actual = @($Value.PSObject.Properties.Name)
	Assert-Payload ($actual.Count -eq $Names.Count) ($Context + ' contains missing or unknown fields.')
	foreach ($name in $Names) {
		Assert-Payload ($actual -ccontains $name) ($Context + ' is missing ' + $name + '.')
	}
}

# The embedded RT_MANIFEST is read directly from the PE resource directory; no
# LoadLibrary/COM activation or external resource-extraction process is needed.
function Read-PayloadUInt16([byte[]]$Data, [int64]$Offset) {
	Assert-Payload ($Offset -ge 0 -and $Offset + 0x02 -le $Data.LongLength) 'Truncated PE/resource data.'

	return [BitConverter]::ToUInt16($Data, [int]$Offset)
}

function Read-PayloadUInt32([byte[]]$Data, [int64]$Offset) {
	Assert-Payload ($Offset -ge 0 -and $Offset + 0x04 -le $Data.LongLength) 'Truncated PE/resource data.'

	return [BitConverter]::ToUInt32($Data, [int]$Offset)
}

function Assert-X64PayloadImage([string]$Path) {
	$stream = [IO.File]::OpenRead($Path)
	try {
		$reader = [IO.BinaryReader]::new($stream)
		try {
			Assert-Payload ($reader.BaseStream.Length -ge 0x40 -and $reader.ReadUInt16() -eq 0x5A4D) ('Not a Windows PE image: ' + [IO.Path]::GetFileName($Path))
			[void]$reader.BaseStream.Seek(0x3C, [IO.SeekOrigin]::Begin)
			$peHeaderOffset = $reader.ReadUInt32()
			Assert-Payload ($peHeaderOffset -ge 0x40 -and [int64]$peHeaderOffset + 0x1A -le $reader.BaseStream.Length) 'Invalid PE header location.'
			[void]$reader.BaseStream.Seek($peHeaderOffset, [IO.SeekOrigin]::Begin)
			Assert-Payload ($reader.ReadUInt32() -eq 0x4550 -and $reader.ReadUInt16() -eq 0x8664) ('All shipped binaries must target Windows x64: ' + [IO.Path]::GetFileName($Path))
			[void]$reader.BaseStream.Seek([int64]$peHeaderOffset + 0x18, [IO.SeekOrigin]::Begin)
			Assert-Payload ($reader.ReadUInt16() -eq 0x20B) 'Expected a PE32+ optional header.'
		} finally {
			$reader.Dispose()
		}
	} finally {
		$stream.Dispose()
	}
}

function Resolve-PayloadRva([byte[]]$Data, [uint32]$Rva, [int64]$Sections, [int]$Count, [uint32]$HeaderSize) {
	if ($Rva -lt $HeaderSize -and $Rva -lt $Data.LongLength) {
		return [int64]$Rva
	}

	for ($index = 0; $index -lt $Count; ++$index) {
		$entry = $Sections + 0x28 * $index
		$virtualSize = Read-PayloadUInt32 $Data ($entry + 0x08)
		$virtualAddress = Read-PayloadUInt32 $Data ($entry + 0x0C)
		$rawSize = Read-PayloadUInt32 $Data ($entry + 0x10)
		$rawOffset = Read-PayloadUInt32 $Data ($entry + 0x14)
		if ($Rva -ge $virtualAddress -and [int64]$Rva - $virtualAddress -lt [Math]::Max($virtualSize, $rawSize)) {
			$delta = [int64]$Rva - $virtualAddress
			Assert-Payload ($delta -lt $rawSize -and [int64]$rawOffset + $delta -lt $Data.LongLength) 'PE resource RVA has no file content.'
			return [int64]$rawOffset + $delta
		}
	}

	throw 'Installer payload: Unmapped PE resource RVA.'
}

function Get-PayloadResourceEntries([byte[]]$Data, [int64]$Root, [uint32]$Size, [uint32]$Directory) {
	Assert-Payload ([int64]$Directory + 0x10 -le $Size) 'Invalid PE resource directory.'
	$count = (Read-PayloadUInt16 $Data ($Root + $Directory + 0x0C)) + (Read-PayloadUInt16 $Data ($Root + $Directory + 0x0E))
	Assert-Payload ([int64]$Directory + 0x10 + 0x08 * $count -le $Size) 'Truncated PE resource entries.'
	for ($index = 0; $index -lt $count; ++$index) {
		$offset = $Root + $Directory + 0x10 + 0x08 * $index
		[pscustomobject]@{
			Id = (Read-PayloadUInt32 $Data $offset)
			Target = (Read-PayloadUInt32 $Data ($offset + 0x04))
		}
	}
}

function Assert-UiManifest([string]$Path) {
	$data = [IO.File]::ReadAllBytes($Path)
	Assert-Payload ((Read-PayloadUInt16 $data 0x00) -eq 0x5A4D) 'The UI is not a Windows image.'
	$peHeaderOffset = Read-PayloadUInt32 $data 0x3C
	Assert-Payload ((Read-PayloadUInt32 $data $peHeaderOffset) -eq 0x4550 -and (Read-PayloadUInt16 $data ($peHeaderOffset + 0x04)) -eq 0x8664) 'The UI must be an x64 PE image.'
	$optionalHeaderOffset = [int64]$peHeaderOffset + 0x18
	$optionalSize = Read-PayloadUInt16 $data ($peHeaderOffset + 0x14)
	Assert-Payload ($optionalSize -ge 0x88 -and (Read-PayloadUInt16 $data $optionalHeaderOffset) -eq 0x20B -and (Read-PayloadUInt32 $data ($optionalHeaderOffset + 0x6C)) -ge 3) 'Missing PE32+ resource directory.'
	$sections = $optionalHeaderOffset + $optionalSize
	$sectionCount = Read-PayloadUInt16 $data ($peHeaderOffset + 0x06)
	$headerSize = Read-PayloadUInt32 $data ($optionalHeaderOffset + 0x3C)
	$resourceRVA = Read-PayloadUInt32 $data ($optionalHeaderOffset + 0x80)
	$resourceSize = Read-PayloadUInt32 $data ($optionalHeaderOffset + 0x84)
	Assert-Payload ($resourceRVA -gt 0 -and $resourceSize -gt 0) 'The UI embedded manifest is missing.'
	$resourceRoot = Resolve-PayloadRva $data $resourceRVA $sections $sectionCount $headerSize
	$types = @(
		Get-PayloadResourceEntries $data $resourceRoot $resourceSize 0x00 | Where-Object {
			$_.Id -eq 24
		}
	)
	# L keeps the high-bit mask positive before UInt32 conversion in Windows PowerShell.
	Assert-Payload ($types.Count -eq 1 -and ($types[0].Target -band [uint32]0x80000000L) -ne 0) 'RT_MANIFEST is missing or invalid.'
	$names = @(
		Get-PayloadResourceEntries $data $resourceRoot $resourceSize ($types[0].Target -band 0x7FFFFFFF) | Where-Object {
			$_.Id -eq 1
		}
	)
	Assert-Payload ($names.Count -eq 1 -and ($names[0].Target -band [uint32]0x80000000L) -ne 0) 'The process manifest resource is missing or invalid.'
	$languages = @(Get-PayloadResourceEntries $data $resourceRoot $resourceSize ($names[0].Target -band 0x7FFFFFFF))
	Assert-Payload ($languages.Count -gt 0) 'The process manifest has no language resources.'
	foreach ($language in $languages) {
		Assert-Payload (($language.Target -band [uint32]0x80000000L) -eq 0 -and [int64]$language.Target + 0x10 -le $resourceSize) 'Invalid process manifest data entry.'
		$entry = $resourceRoot + $language.Target
		$manifestRVA = Read-PayloadUInt32 $data $entry
		$manifestSize = Read-PayloadUInt32 $data ($entry + 0x04)
		Assert-Payload ($manifestSize -gt 0 -and $manifestSize -le 1MB) 'Invalid embedded manifest size.'
		$offset = Resolve-PayloadRva $data $manifestRVA $sections $sectionCount $headerSize
		Assert-Payload ($offset + $manifestSize -le $data.LongLength) 'Truncated embedded manifest.'
		$stream = [IO.MemoryStream]::new($data, [int]$offset, [int]$manifestSize, $false)
		try {
			$textReader = [IO.StreamReader]::new($stream, $taskUTF8Encoding, $true)
			try {
				$manifestText = $textReader.ReadToEnd()
			} finally {
				$textReader.Dispose()
			}
		} finally {
			$stream.Dispose()
		}

		$settings = [Xml.XmlReaderSettings]::new()
		$settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
		$settings.XmlResolver = $null
		$stringReader = [IO.StringReader]::new($manifestText)
		try {
			$xmlReader = [Xml.XmlReader]::Create($stringReader, $settings)
			try {
				$document = [Xml.XmlDocument]::new()
				$document.XmlResolver = $null
				$document.Load($xmlReader)
			} finally {
				$xmlReader.Dispose()
			}
		} finally {
			$stringReader.Dispose()
		}

		$namespaces = [Xml.XmlNamespaceManager]::new($document.NameTable)
		$namespaces.AddNamespace('asm', 'urn:schemas-microsoft-com:asm.v1')
		$namespaces.AddNamespace('security', 'urn:schemas-microsoft-com:asm.v3')
		$namespaces.AddNamespace('compat', 'urn:schemas-microsoft-com:compatibility.v1')
		$execution = @($document.SelectNodes('/asm:assembly/security:trustInfo/security:security/security:requestedPrivileges/security:requestedExecutionLevel', $namespaces))
		Assert-Payload ($execution.Count -eq 1 -and $execution[0].GetAttribute('level') -ceq 'asInvoker' -and $execution[0].GetAttribute('uiAccess') -ceq 'false') 'The shipped UI must remain asInvoker with uiAccess=false.'
		$systems = @(
			$document.SelectNodes('/asm:assembly/compat:compatibility/compat:application/compat:supportedOS', $namespaces) | Where-Object {
				$_.GetAttribute('Id') -ieq '{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}'
			}
		)
		Assert-Payload ($systems.Count -ge 1) 'The shipped UI must declare Windows 10 compatibility.'
	}
}

$rootItem = Get-Item -LiteralPath $taskPayloadRoot -Force
Assert-Payload $rootItem.PSIsContainer 'PayloadRoot must be a directory.'
Assert-NoReparseAncestors $rootItem
$manifestItem = Get-Item -LiteralPath $taskManifestPath -Force
Assert-Payload (!$manifestItem.PSIsContainer -and $manifestItem.Length -le 64KB) 'Expected a small JSON staging manifest.'
Assert-NoReparseAncestors $manifestItem
$taskManifest = [IO.File]::ReadAllText($taskManifestPath, $taskUTF8Encoding) | ConvertFrom-Json
Assert-Properties $taskManifest @('schemaVersion', 'files') 'Manifest'
Assert-Payload (($taskManifest.schemaVersion -is [int] -or $taskManifest.schemaVersion -is [long]) -and $taskManifest.schemaVersion -eq 1) 'Unknown manifest schema version.'
$taskEntries = @($taskManifest.files)
Assert-Payload ($taskEntries.Count -eq $taskExpected.Count) 'The manifest must contain exactly the declared supported payload files.'
$taskSeen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$taskFiles = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
foreach ($entry in $taskEntries) {
	Assert-Properties $entry @('path', 'component') 'Manifest file entry'
	Assert-Payload ($entry.path -is [string] -and $entry.component -is [string]) 'File paths and components must be strings.'
	$absolute = Resolve-PayloadFile $entry.path
	Assert-Payload ($taskSeen.Add($entry.path)) ('Duplicate payload path: ' + $entry.path)
	Assert-Payload ($taskExpected.ContainsKey($entry.path)) ('Unknown payload file; private configurations, SDK trees, PDBs, archives and runtime diagnostics must not be packaged: ' + $entry.path)
	Assert-Payload ($entry.component -ceq $taskExpected[$entry.path]) ('Unexpected component for ' + $entry.path)
	$taskFiles.Add($entry.path, $absolute)
}

$taskChecksumsPath = Resolve-PayloadFile 'SHA256SUMS.txt'
Assert-Payload ((Get-Item -LiteralPath $taskChecksumsPath).Length -le 64KB) 'The checksum manifest is too large.'
$taskHashes = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($line in [IO.File]::ReadAllLines($taskChecksumsPath, $taskUTF8Encoding)) {
	if ([string]::IsNullOrWhiteSpace($line)) {
		continue
	}

	Assert-Payload ($line -cmatch '^([a-fA-F0-9]{64})  ([A-Za-z0-9][A-Za-z0-9._/-]*)$') 'Invalid SHA256SUMS entry.'
	$hash = $Matches[1]
	$path = $Matches[2]
	Assert-Payload ($taskExpected.ContainsKey($path) -and !$taskHashes.ContainsKey($path)) ('Unknown or duplicate checksum path: ' + $path)
	$taskHashes.Add($path, $hash)
}

Assert-Payload ($taskHashes.Count -eq $taskExpected.Count) 'SHA256SUMS must cover exactly the declared payload.'
foreach ($path in $taskExpected.Keys) {
	Assert-Payload ($taskFiles.ContainsKey($path) -and $taskHashes.ContainsKey($path)) ('Missing payload/checksum entry: ' + $path)
	$actualHash = (Get-FileHash -LiteralPath $taskFiles[$path] -Algorithm SHA256).Hash
	Assert-Payload ($actualHash -ieq $taskHashes[$path]) ('Checksum mismatch: ' + $path)
}

Assert-Payload ([IO.File]::ReadAllText($taskFiles['steam_appid.txt'], $taskUTF8Encoding).Trim() -ceq '480') 'steam_appid.txt must contain AppID 480.'
foreach (
	$path in $taskExpected.Keys | Where-Object {
		$_.StartsWith('examples/', [StringComparison]::Ordinal)
	}
) {
	Assert-Payload ((Get-Item -LiteralPath $taskFiles[$path]).Length -le 64KB) ('An example is unexpectedly large: ' + $path)
	$text = [IO.File]::ReadAllText($taskFiles[$path], $taskUTF8Encoding)
	$keys = [Regex]::Matches($text, '(?im)^\s*(PrivateKey|PresharedKey)\s*=\s*([^\r\n]*)\r?$')
	Assert-Payload ($keys.Count -ge 1) ('An example is missing its private-key placeholder: ' + $path)
	foreach ($key in $keys) {
		$value = ($key.Groups[2].Value -split '\s*[#;]', 2)[0].Trim()
		Assert-Payload ($value -cmatch '^<[A-Z0-9_]+>$') ('An example contains an actual or invalid private key: ' + $path)
	}
}

foreach (
	$path in $taskExpected.Keys | Where-Object {
		$_ -match '\.(exe|dll)$'
	}
) {
	Assert-X64PayloadImage $taskFiles[$path]
}

& (Join-Path $PSScriptRoot 'windows_compatibility.ps1') -WireSteamPath $taskFiles['WireSteam.exe'] -ServicePath $taskFiles['WireSteamService.exe'] -UiPath $taskFiles['WireSteam.UI.exe']
Assert-UiManifest $taskFiles['WireSteam.UI.exe']
Write-Output ('PASS installer payload: ' + $taskExpected.Count + ' explicit files, component mapping, SHA256, AppID, template-only keys, safe paths, x64/Windows 10 and normal-user UI manifest.')
