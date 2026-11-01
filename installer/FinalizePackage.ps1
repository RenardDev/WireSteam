param(
	[Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$MsiPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# This post-link step edits only the explicitly selected generated MSI database.
# Removing its unused ListBox table prevents InstallValidate from presenting a
# manual-close prompt before the deferred, scoped WireSteam shutdown executes.
$taskMsiPath = [IO.Path]::GetFullPath($MsiPath)
$taskInstaller = $null
$taskDatabase = $null

function Assert-Package([bool]$Condition, [string]$Message) {
	if (!$Condition) {
		throw ('MSI finalization: ' + $Message)
	}
}

function Invoke-MsiMember($Object, [string]$Name, [Reflection.BindingFlags]$Flags, [object[]]$Arguments = @()) {
	return $Object.GetType().InvokeMember($Name, $Flags, $null, $Object, $Arguments)
}

function Release-MsiObject($Object) {
	if ($null -ne $Object -and [Runtime.InteropServices.Marshal]::IsComObject($Object)) {
		[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($Object)
	}
}

function Read-MsiRows([string]$Table, [string[]]$Columns) {
	Assert-Package ($Table -cmatch '^[A-Za-z_][A-Za-z0-9_]*$') 'Invalid table identifier.'
	$quotedColumns = @($Columns | ForEach-Object {
			Assert-Package ($_ -cmatch '^[A-Za-z_][A-Za-z0-9_]*$') 'Invalid column identifier.'
			'`' + $_ + '`'
		})
	$sql = 'SELECT ' + ($quotedColumns -join ', ') + ' FROM `' + $Table + '`'
	$view = $null
	try {
		$view = Invoke-MsiMember $taskDatabase 'OpenView' ([Reflection.BindingFlags]::InvokeMethod) @($sql)
		[void](Invoke-MsiMember $view 'Execute' ([Reflection.BindingFlags]::InvokeMethod))
		while ($true) {
			$record = Invoke-MsiMember $view 'Fetch' ([Reflection.BindingFlags]::InvokeMethod)
			if ($null -eq $record) {
				break
			}

			try {
				$row = [ordered]@{}
				for ($columnIndex = 0; $columnIndex -lt $Columns.Count; ++$columnIndex) {
					$row[$Columns[$columnIndex]] = [string](Invoke-MsiMember $record 'StringData' ([Reflection.BindingFlags]::GetProperty) @($columnIndex + 1))
				}

				[pscustomobject]$row
			} finally {
				Release-MsiObject $record
			}
		}
	} finally {
		if ($null -ne $view) {
			try {
				[void](Invoke-MsiMember $view 'Close' ([Reflection.BindingFlags]::InvokeMethod))
			} finally {
				Release-MsiObject $view
			}
		}
	}
}

Assert-Package ([IO.Path]::GetExtension($taskMsiPath) -ieq '.msi') 'The selected package must have an .msi extension.'
$taskPackageFile = Get-Item -LiteralPath $taskMsiPath -Force
Assert-Package (!$taskPackageFile.PSIsContainer) 'The selected package must be a regular file.'
$taskPathEntry = $taskPackageFile
while ($null -ne $taskPathEntry) {
	Assert-Package (($taskPathEntry.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'The package and its parent directories must not be reparse points.'
	$taskPathEntry = if ($taskPathEntry -is [IO.FileInfo]) {
		$taskPathEntry.Directory
	} else {
		$taskPathEntry.Parent
	}
}

try {
	$taskInstallerType = [Type]::GetTypeFromProgID('WindowsInstaller.Installer', $true)
	$taskInstaller = [Activator]::CreateInstance($taskInstallerType)
	# Mode 1 is transactional. A rejected guard or failed statement is discarded
	# when the database closes without Commit; this never executes the package.
	$taskDatabase = Invoke-MsiMember $taskInstaller 'OpenDatabase' ([Reflection.BindingFlags]::InvokeMethod) @($taskMsiPath, 1)
	$taskTables = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach ($table in @(Read-MsiRows '_Tables' @('Name'))) {
		[void]$taskTables.Add($table.Name)
	}

	foreach ($requiredTable in @('Property', 'CustomAction', 'Dialog', 'Control')) {
		Assert-Package ($taskTables.Contains($requiredTable)) ('Missing required WireSteam package table: ' + $requiredTable)
	}

	$taskProperties = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
	foreach ($property in @(Read-MsiRows 'Property' @('Property', 'Value'))) {
		$taskProperties.Add($property.Property, $property.Value)
	}

	foreach ($expected in @{
			ProductName = 'WireSteam'
			Manufacturer = 'WireSteam'
			UpgradeCode = '{A646733A-FA64-45AF-A446-61BF9FB53071}'
		}.GetEnumerator()) {
		Assert-Package ($taskProperties.ContainsKey($expected.Key) -and $taskProperties[$expected.Key] -ieq $expected.Value) ('The selected MSI is not the expected WireSteam package: ' + $expected.Key)
	}

	$taskShutdownActions = @(
		Read-MsiRows 'CustomAction' @('Action', 'Type', 'Source', 'Target') | Where-Object {
			$_.Action -ceq 'WireSteamStopComponents'
		}
	)
	Assert-Package ($taskShutdownActions.Count -eq 1 -and $taskShutdownActions[0].Type -eq '3073' -and
		$taskShutdownActions[0].Source -ceq 'InstallerGuard' -and $taskShutdownActions[0].Target -ceq 'WireSteamStopComponents') 'The package must have the checked, deferred system-context WireSteam shutdown action.'
	$taskListBoxControls = @(
		Read-MsiRows 'Control' @('Type') | Where-Object {
			$_.Type -ieq 'ListBox'
		}
	)
	Assert-Package ($taskListBoxControls.Count -eq 0) 'A ListBox control still needs the table; refusing to remove it.'
	$taskFilesInUseDialogs = @(
		Read-MsiRows 'Dialog' @('Dialog') | Where-Object {
			$_.Dialog -iin @('FilesInUse', 'MsiRMFilesInUse')
		}
	)
	Assert-Package ($taskFilesInUseDialogs.Count -eq 0) 'A FilesInUse dialog would interrupt scoped shutdown; remove its authoring before finalization.'
	if (!$taskTables.Contains('ListBox')) {
		Write-Output 'PASS MSI finalization: unused ListBox table is already absent.'
		return
	}

	Assert-Package (@(Read-MsiRows 'ListBox' @('Property')).Count -eq 0) 'The ListBox table contains data; refusing to remove it.'
	$taskDropView = $null
	try {
		$taskDropView = Invoke-MsiMember $taskDatabase 'OpenView' ([Reflection.BindingFlags]::InvokeMethod) @('DROP TABLE `ListBox`')
		[void](Invoke-MsiMember $taskDropView 'Execute' ([Reflection.BindingFlags]::InvokeMethod))
	} finally {
		if ($null -ne $taskDropView) {
			try {
				[void](Invoke-MsiMember $taskDropView 'Close' ([Reflection.BindingFlags]::InvokeMethod))
			} finally {
				Release-MsiObject $taskDropView
			}
		}
	}

	[void](Invoke-MsiMember $taskDatabase 'Commit' ([Reflection.BindingFlags]::InvokeMethod))
	Write-Output 'PASS MSI finalization: removed the unused empty ListBox table before validation and signing.'
} finally {
	try {
		Release-MsiObject $taskDatabase
	} finally {
		Release-MsiObject $taskInstaller
	}
}
