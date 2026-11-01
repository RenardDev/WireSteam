param(
	[Parameter(Mandatory = $true)][string]$WireSteamPath,
	[Parameter(Mandatory = $true)][string]$ServicePath,
	[string]$UiPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Inspect the shipped images without loading them or changing networking.
# This catches a new minimum OS or an accidental loader-time dependency on
# the Windows 11 routing API. It does not replace execution on Windows 10.
function Read-UInt16([byte[]]$Data, [int64]$Offset) {
	if ($Offset -lt 0 -or $Offset + 0x02 -gt $Data.LongLength) {
		throw 'Truncated PE image'
	}

	return [BitConverter]::ToUInt16($Data, [int]$Offset)
}

function Read-UInt32([byte[]]$Data, [int64]$Offset) {
	if ($Offset -lt 0 -or $Offset + 0x04 -gt $Data.LongLength) {
		throw 'Truncated PE image'
	}

	return [BitConverter]::ToUInt32($Data, [int]$Offset)
}

function Read-UInt64([byte[]]$Data, [int64]$Offset) {
	if ($Offset -lt 0 -or $Offset + 0x08 -gt $Data.LongLength) {
		throw 'Truncated PE image'
	}

	return [BitConverter]::ToUInt64($Data, [int]$Offset)
}

function Read-ASCII([byte[]]$Data, [int64]$Offset) {
	$maximumStringBytes = 4096

	if ($Offset -lt 0 -or $Offset -ge $Data.LongLength) {
		throw 'Invalid PE string offset'
	}

	$endOffset = $Offset
	while ($endOffset -lt $Data.LongLength -and $Data[$endOffset] -ne 0x00 -and $endOffset - $Offset -le $maximumStringBytes) {
		++$endOffset
	}

	if ($endOffset -ge $Data.LongLength -or $endOffset - $Offset -gt $maximumStringBytes) {
		throw 'Unterminated PE string'
	}

	return [Text.Encoding]::ASCII.GetString($Data, [int]$Offset, [int]($endOffset - $Offset))
}

function Resolve-RVA([byte[]]$Data, [int64]$RVA, [int64]$SectionsOffset, [int]$SectionCount, [int64]$HeaderSize) {
	if ($RVA -ge 0 -and $RVA -lt $HeaderSize -and $RVA -lt $Data.LongLength) {
		return $RVA
	}

	for ($sectionIndex = 0; $sectionIndex -lt $SectionCount; ++$sectionIndex) {
		$sectionOffset = $SectionsOffset + 0x28 * $sectionIndex
		$virtualSize = Read-UInt32 $Data ($sectionOffset + 0x08)
		$virtualAddress = Read-UInt32 $Data ($sectionOffset + 0x0C)
		$rawSize = Read-UInt32 $Data ($sectionOffset + 0x10)
		$rawOffset = Read-UInt32 $Data ($sectionOffset + 0x14)

		if ($RVA -ge $virtualAddress -and $RVA - $virtualAddress -lt [Math]::Max($virtualSize, $rawSize)) {
			$sectionDelta = $RVA - $virtualAddress
			if ($sectionDelta -ge $rawSize -or $rawOffset + $sectionDelta -ge $Data.LongLength) {
				throw 'PE RVA has no file content'
			}

			return [int64]$rawOffset + $sectionDelta
		}
	}

	throw ('Unmapped PE RVA: ' + $RVA)
}

function Assert-Imports([byte[]]$Data, [int64]$DirectoryRVA, [int64]$DirectoryBytes, [int64]$SectionsOffset, [int]$SectionCount, [int64]$HeaderSize, [bool]$Delayed) {
	if (!$DirectoryRVA) {
		return
	}

	$descriptorSize = 0x14
	$maximumThunkCount = 65536

	if ($Delayed) {
		$descriptorSize = 0x20
	}

	for ($descriptorIndex = 0; $descriptorIndex * $descriptorSize -lt $DirectoryBytes; ++$descriptorIndex) {
		$descriptorOffset = Resolve-RVA $Data ($DirectoryRVA + $descriptorIndex * $descriptorSize) $SectionsOffset $SectionCount $HeaderSize
		$nameRVA = Read-UInt32 $Data ($descriptorOffset + 0x0C)
		$thunkRVA = Read-UInt32 $Data $descriptorOffset

		if ($Delayed) {
			$attributes = Read-UInt32 $Data $descriptorOffset
			$nameRVA = Read-UInt32 $Data ($descriptorOffset + 0x04)
			$thunkRVA = Read-UInt32 $Data ($descriptorOffset + 0x10)

			if ($nameRVA -and $attributes -ne 0x01) {
				throw 'The delay import table must use RVAs'
			}
		} elseif (!$thunkRVA) {
			$thunkRVA = Read-UInt32 $Data ($descriptorOffset + 0x10)
		}

		if (!$nameRVA -and !$thunkRVA) {
			return
		}

		if (!$nameRVA -or !$thunkRVA) {
			throw 'Invalid PE import descriptor'
		}

		$libraryName = Read-ASCII $Data (Resolve-RVA $Data $nameRVA $SectionsOffset $SectionCount $HeaderSize)

		for ($thunkIndex = 0; $thunkIndex -lt $maximumThunkCount; ++$thunkIndex) {
			$thunkOffset = Resolve-RVA $Data ($thunkRVA + 0x08 * $thunkIndex) $SectionsOffset $SectionCount $HeaderSize
			$thunkValue = Read-UInt64 $Data $thunkOffset
			if (!$thunkValue) {
				break
			}

			if ($thunkValue -gt [uint64][int64]::MaxValue) {
				continue
			} # Import by ordinal.

			$functionName = Read-ASCII $Data ((Resolve-RVA $Data ([int64]$thunkValue) $SectionsOffset $SectionCount $HeaderSize) + 0x02)
			if ($functionName -eq 'FwpmConnectionPolicyAdd0') {
				throw ('Windows 10 cannot load a hard import of ' + $libraryName + '!' + $functionName + '; keep runtime GetProcAddress detection')
			}
		}

		if ($thunkIndex -eq $maximumThunkCount) {
			throw 'Unterminated PE import thunk table'
		}
	}

	throw 'Unterminated PE import descriptor table'
}

function Assert-CompatibleImage([string]$Path) {
	$absolutePath = [IO.Path]::GetFullPath($Path)
	$data = [IO.File]::ReadAllBytes($absolutePath)
	if ((Read-UInt16 $data 0x00) -ne 0x5A4D) {
		throw 'Expected a Windows executable'
	}

	$PEHeaderOffset = Read-UInt32 $data 0x3C
	if ((Read-UInt32 $data $PEHeaderOffset) -ne 0x4550 -or (Read-UInt16 $data ($PEHeaderOffset + 0x04)) -ne 0x8664) {
		throw 'Expected an x64 PE image'
	}

	$sectionCount = Read-UInt16 $data ($PEHeaderOffset + 0x06)
	$optionalHeaderSize = Read-UInt16 $data ($PEHeaderOffset + 0x14)
	$optionalHeaderOffset = $PEHeaderOffset + 0x18
	if ($optionalHeaderSize -lt 0x70 -or (Read-UInt16 $data $optionalHeaderOffset) -ne 0x20B) {
		throw 'Expected a PE32+ optional header'
	}

	$operatingSystemMajor = Read-UInt16 $data ($optionalHeaderOffset + 0x28)
	$operatingSystemMinor = Read-UInt16 $data ($optionalHeaderOffset + 0x2A)
	$subsystemMajor = Read-UInt16 $data ($optionalHeaderOffset + 0x30)
	$subsystemMinor = Read-UInt16 $data ($optionalHeaderOffset + 0x32)

	if ($operatingSystemMajor -gt 10 -or ($operatingSystemMajor -eq 10 -and $operatingSystemMinor -gt 0) -or
		$subsystemMajor -gt 10 -or ($subsystemMajor -eq 10 -and $subsystemMinor -gt 0)) {
		throw ('Executable declares a newer minimum OS/subsystem than Windows 10: ' + $absolutePath)
	}

	$headerSize = Read-UInt32 $data ($optionalHeaderOffset + 0x3C)
	$directoryCount = Read-UInt32 $data ($optionalHeaderOffset + 0x6C)

	foreach ($directoryEntry in @(@{ Index = 1; Delayed = $false }, @{ Index = 13; Delayed = $true })) {
		if ($directoryEntry.Index -ge $directoryCount) {
			continue
		}

		if (0x70 + 0x08 * ($directoryEntry.Index + 1) -gt $optionalHeaderSize) {
			throw 'Truncated PE data directory table'
		}

		$directoryRVA = Read-UInt32 $data ($optionalHeaderOffset + 0x70 + 0x08 * $directoryEntry.Index)
		$directoryBytes = Read-UInt32 $data ($optionalHeaderOffset + 0x74 + 0x08 * $directoryEntry.Index)
		Assert-Imports $data $directoryRVA $directoryBytes ($optionalHeaderOffset + $optionalHeaderSize) $sectionCount $headerSize $directoryEntry.Delayed
	}

	Write-Output ([IO.Path]::GetFileName($absolutePath) + ': Windows 10 PE/import compatibility checks passed')
}

Assert-CompatibleImage $WireSteamPath
Assert-CompatibleImage $ServicePath

if ($UiPath) {
	Assert-CompatibleImage $UiPath
}
