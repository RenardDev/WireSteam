param(
	[Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$MsiPath,
	[Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$ManifestPath,
	[ValidateNotNullOrEmpty()][string]$PayloadRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# OpenDatabase mode 0 reads the MSI tables. This script never invokes MSI install,
# repair, extraction, service, process, adapter or uninstall operations.
$taskMsiPath = [IO.Path]::GetFullPath($MsiPath)
$taskManifestPath = [IO.Path]::GetFullPath($ManifestPath)
$taskPayloadRoot = [IO.Path]::GetFullPath($PayloadRoot)
$taskUpgradeCode = '{A646733A-FA64-45AF-A446-61BF9FB53071}'
$taskUtf8 = [Text.UTF8Encoding]::new($false, $true)
$taskInstaller = $null
$taskDatabase = $null
$taskTables = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)

function Assert-Msi([bool]$Condition, [string]$Message) {
	if (!$Condition) {
		throw ('MSI package: ' + $Message)
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

function Read-MsiTable([string]$Name, [string[]]$Columns, [bool]$Required = $true) {
	if ($Name -cnotin @('_Tables', '_Streams') -and !$taskTables.Contains($Name)) {
		Assert-Msi (!$Required) ('Missing table: ' + $Name)
		return
	}

	Assert-Msi ($Name -cmatch '^[A-Za-z_][A-Za-z0-9_]*$') 'Invalid table identifier.'
	$quoted = @($Columns | ForEach-Object {
			Assert-Msi ($_ -cmatch '^[A-Za-z_][A-Za-z0-9_]*$') 'Invalid column identifier.'
			'`' + $_ + '`'
		})
	$sql = 'SELECT ' + ($quoted -join ', ') + ' FROM `' + $Name + '`'
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
				for ($index = 0; $index -lt $Columns.Count; ++$index) {
					$row[$Columns[$index]] = [string](Invoke-MsiMember $record 'StringData' ([Reflection.BindingFlags]::GetProperty) @($index + 1))
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

function Get-MsiLongName([string]$Name) {
	# Directory DefaultDir can contain target:source; both names can use short|long.
	return (($Name -split ':', 2)[0] -split '\|')[-1]
}

function Resolve-MsiRelativeDirectory([string]$Id, [string]$Root) {
	$segments = [Collections.Generic.List[string]]::new()
	$visited = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	while ($Id -cne $Root) {
		Assert-Msi ($Id -ne '' -and $taskDirectories.ContainsKey($Id) -and $visited.Add($Id)) ('Invalid directory ancestry under ' + $Root + '.')
		$entry = $taskDirectories[$Id]
		$name = Get-MsiLongName $entry.DefaultDir
		Assert-Msi ($name -ne '' -and $name -notmatch '[/\\]' -and $name -ne '..') 'Invalid install-relative directory name.'
		if ($name -ne '.') {
			$segments.Insert(0, $name)
		}

		$Id = $entry.Directory_Parent
	}

	return $segments -join '/'
}

function Get-MsiComponentFeatures([string]$ComponentId) {
	Assert-Msi ($taskComponentFeatures.ContainsKey($ComponentId)) ('An installed component is not assigned to a feature: ' + $ComponentId)

	return @($taskComponentFeatures[$ComponentId])
}

function Assert-MsiSingleFeature([string]$ComponentId, [string]$FeatureId) {
	$features = @(Get-MsiComponentFeatures $ComponentId)
	Assert-Msi ($features.Count -eq 1 -and $features[0] -ceq $FeatureId) ('Unexpected feature assignment for component ' + $ComponentId + '.')
}

& (Join-Path $PSScriptRoot 'installer_payload.ps1') -ManifestPath $taskManifestPath -PayloadRoot $taskPayloadRoot
$taskManifest = [IO.File]::ReadAllText($taskManifestPath, $taskUtf8) | ConvertFrom-Json
$taskExpectedFiles = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
foreach ($file in $taskManifest.files) {
	$taskExpectedFiles.Add($file.path, $file.component)
}

$taskProjectPath = Join-Path (Split-Path $PSScriptRoot -Parent) 'gui\WireSteam.UI.csproj'
[xml]$taskProject = [IO.File]::ReadAllText($taskProjectPath, $taskUtf8)
$taskVersion = [string]$taskProject.Project.PropertyGroup.Version
Assert-Msi ($taskVersion -match '^\d+\.\d+\.\d+$') 'The UI project must define a three-part MSI product version.'
Assert-Msi (Test-Path -LiteralPath $taskMsiPath -PathType Leaf) 'The MSI file is missing.'

try {
	$taskInstallerType = [Type]::GetTypeFromProgID('WindowsInstaller.Installer', $true)
	$taskInstaller = [Activator]::CreateInstance($taskInstallerType)
	$taskDatabase = Invoke-MsiMember $taskInstaller 'OpenDatabase' ([Reflection.BindingFlags]::InvokeMethod) @($taskMsiPath, 0)
	foreach ($table in @(Read-MsiTable '_Tables' @('Name'))) {
		[void]$taskTables.Add($table.Name)
	}

	$taskProperties = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
	foreach ($property in @(Read-MsiTable 'Property' @('Property', 'Value'))) {
		$taskProperties.Add($property.Property, $property.Value)
	}

	foreach ($expected in @{
			ProductName = 'WireSteam'
			Manufacturer = 'WireSteam'
			ProductVersion = $taskVersion

			UpgradeCode = $taskUpgradeCode
			ProductLanguage = '1033'
			ALLUSERS = '1'

			MSIRESTARTMANAGERCONTROL = 'DisableShutdown'
			MSIDISABLERMRESTART = '1'
			REBOOT = 'ReallySuppress'
		}.GetEnumerator()) {
		Assert-Msi ($taskProperties.ContainsKey($expected.Key) -and $taskProperties[$expected.Key] -ieq $expected.Value) ('Unexpected property: ' + $expected.Key)
	}

	$taskProductCode = [Guid]::Empty
	Assert-Msi ($taskProperties.ContainsKey('ProductCode') -and [Guid]::TryParse($taskProperties['ProductCode'], [ref]$taskProductCode) -and
		$taskProductCode -ne [Guid]::Empty -and $taskProductCode -ne [Guid]$taskUpgradeCode) 'The product code must be a valid, distinct GUID.'
	$summary = $null
	try {
		$summary = Invoke-MsiMember $taskDatabase 'SummaryInformation' ([Reflection.BindingFlags]::GetProperty) @(0)
		$template = [string](Invoke-MsiMember $summary 'Property' ([Reflection.BindingFlags]::GetProperty) @(7))
		Assert-Msi (($template -split ';')[0] -ceq 'x64') 'The MSI summary template must target x64.'
	} finally {
		Release-MsiObject $summary
	}

	$taskDirectories = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
	foreach ($directory in @(Read-MsiTable 'Directory' @('Directory', 'Directory_Parent', 'DefaultDir'))) {
		$taskDirectories.Add($directory.Directory, $directory)
	}

	Assert-Msi ($taskDirectories.ContainsKey('INSTALLFOLDER') -and $taskDirectories.ContainsKey('ExamplesFolder')) 'The application and examples directories are missing.'
	Assert-Msi ((Resolve-MsiRelativeDirectory 'INSTALLFOLDER' 'ProgramFiles64Folder') -ceq 'WireSteam') 'The default application directory must be Program Files (64-bit)\WireSteam.'
	Assert-Msi ((Resolve-MsiRelativeDirectory 'ExamplesFolder' 'INSTALLFOLDER') -ceq 'examples') 'Examples must be installed below the application directory.'
	Assert-Msi ($taskDirectories.ContainsKey('DocsImagesFolder') -and (Resolve-MsiRelativeDirectory 'DocsImagesFolder' 'INSTALLFOLDER') -ceq 'docs/images') 'The documentation image must retain its install-relative docs/images directory.'
	$taskComponents = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
	foreach ($component in @(Read-MsiTable 'Component' @('Component', 'Directory_', 'Attributes', 'KeyPath'))) {
		Assert-Msi (([int]$component.Attributes -band 0x0100) -ne 0) ('A component uses the wrong registry/file architecture: ' + $component.Component)
		$taskComponents.Add($component.Component, $component)
	}

	$taskFeatures = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
	foreach ($feature in @(Read-MsiTable 'Feature' @('Feature', 'Feature_Parent', 'Title', 'Description', 'Level', 'Attributes'))) {
		$taskFeatures.Add($feature.Feature, $feature)
	}

	Assert-Msi ($taskFeatures.Count -eq 4) 'Expected only the application, speed test, test applications and desktop shortcut features.'
	foreach ($id in @('MainFeature', 'SpeedTestFeature', 'TestAppsFeature', 'DesktopShortcutFeature')) {
		Assert-Msi ($taskFeatures.ContainsKey($id)) ('Missing feature: ' + $id)
	}

	Assert-Msi ($taskFeatures['MainFeature'].Level -eq '1' -and $taskFeatures['DesktopShortcutFeature'].Level -eq '2') 'The application must be selected by default and the desktop shortcut must be optional by default.'
	Assert-Msi (([int]$taskFeatures['MainFeature'].Attributes -band 0x10) -ne 0) 'The main application feature must not be removable through optional-feature selection.'
	foreach ($feature in $taskFeatures.Values) {
		Assert-Msi (([int]$feature.Attributes -band 0x08) -ne 0) 'Ordinary EXE application features must not offer unsupported advertised/install-on-demand states.'
	}

	Assert-Msi ($taskFeatures['SpeedTestFeature'].Level -eq '1' -and $taskFeatures['TestAppsFeature'].Level -eq '1') 'Speed and connectivity tools must be selected by default.'
	$taskComponentFeatures = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
	foreach ($mapping in @(Read-MsiTable 'FeatureComponents' @('Feature_', 'Component_'))) {
		Assert-Msi ($taskFeatures.ContainsKey($mapping.Feature_) -and $taskComponents.ContainsKey($mapping.Component_)) 'A feature references an unknown component.'

		if (!$taskComponentFeatures.ContainsKey($mapping.Component_)) {
			$taskComponentFeatures.Add($mapping.Component_, [Collections.Generic.List[string]]::new())
		}

		$taskComponentFeatures[$mapping.Component_].Add($mapping.Feature_)
	}

	foreach ($component in $taskComponents.Keys) {
		[void](Get-MsiComponentFeatures $component)
	}

	$taskFiles = @(Read-MsiTable 'File' @('File', 'Component_', 'FileName', 'FileSize', 'Sequence'))
	Assert-Msi ($taskFiles.Count -eq $taskExpectedFiles.Count) 'The MSI File table must contain exactly the declared payload.'
	$taskSeenPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach ($file in $taskFiles) {
		Assert-Msi ($taskComponents.ContainsKey($file.Component_)) 'A file references an unknown component.'
		$relativeDirectory = Resolve-MsiRelativeDirectory $taskComponents[$file.Component_].Directory_ 'INSTALLFOLDER'
		$name = Get-MsiLongName $file.FileName
		$path = if ($relativeDirectory) {
			$relativeDirectory + '/' + $name
		} else {
			$name
		}

		Assert-Msi ($taskExpectedFiles.ContainsKey($path) -and $taskSeenPaths.Add($path)) ('Unknown or duplicate installed file path: ' + $path)
		$source = Join-Path $taskPayloadRoot $path.Replace('/', [IO.Path]::DirectorySeparatorChar)
		Assert-Msi ([int64]$file.FileSize -eq (Get-Item -LiteralPath $source).Length) ('Stale or incorrect MSI payload size: ' + $path)
		$feature = switch ($taskExpectedFiles[$path]) {
			'core' {
				'MainFeature'
			}

			'docs' {
				'MainFeature'
			}

			'speedtest' {
				'SpeedTestFeature'
			}

			'testapps' {
				'TestAppsFeature'
			}
		}

		Assert-MsiSingleFeature $file.Component_ $feature

		if ($path -ceq 'docs/images/overview.png') {
			Assert-Msi ($taskComponents[$file.Component_].Directory_ -ceq 'DocsImagesFolder') 'The documentation image component must target its nested documentation directory.'
		}

		if ($path -ceq 'WireSteam.UI.exe') {
			Assert-Msi ($file.File -ceq 'WireSteamUIFile') 'The UI file identifier must retain its stable payload authoring identity.'
		}
	}

	$taskStreams = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach ($stream in @(Read-MsiTable '_Streams' @('Name'))) {
		[void]$taskStreams.Add($stream.Name)
	}

	$taskMedia = @(Read-MsiTable 'Media' @('DiskId', 'LastSequence', 'Cabinet'))
	Assert-Msi ($taskMedia.Count -gt 0) 'The embedded cabinet is missing.'
	foreach ($media in $taskMedia) {
		Assert-Msi ($media.Cabinet.StartsWith('#', [StringComparison]::Ordinal) -and $taskStreams.Contains($media.Cabinet.Substring(1))) 'Every payload cabinet must be embedded in the MSI.'
	}

	$taskLastSequence = (
		$taskMedia | ForEach-Object {
			[int]$_.LastSequence
		} | Measure-Object -Maximum
	).Maximum
	foreach ($file in $taskFiles) {
		Assert-Msi ([int]$file.Sequence -gt 0 -and [int]$file.Sequence -le $taskLastSequence) 'A file sequence is not covered by the cabinet media.'
	}

	$taskIcons = @(Read-MsiTable 'Icon' @('Name'))
	Assert-Msi (
		@(
			$taskIcons | Where-Object {
				$_.Name -ceq 'WireSteamIcon'
			}
		).Count -eq 1
	) 'The application icon is missing.'
	$taskShortcuts = @(Read-MsiTable 'Shortcut' @('Shortcut', 'Directory_', 'Component_', 'Target', 'Arguments', 'Icon_', 'WkDir'))
	Assert-Msi ($taskShortcuts.Count -eq 2) 'Expected the Start menu and optional desktop shortcuts.'
	Assert-Msi ($taskProperties['ALLUSERS'] -ceq '1' -and ([int]$taskFeatures['MainFeature'].Attributes -band 0x10) -ne 0) 'Shared shortcuts require a per-machine installation and a mandatory main application feature.'
	$taskDesktop = 0
	$taskStartMenu = 0
	foreach ($shortcut in $taskShortcuts) {
		Assert-Msi ($shortcut.Target -ceq '[INSTALLFOLDER]WireSteam.UI.exe' -and $shortcut.Arguments -eq '' -and $shortcut.Icon_ -ceq 'WireSteamIcon' -and $shortcut.WkDir -ceq 'INSTALLFOLDER') 'Shortcuts must open the normal-user UI without activation arguments.'

		if ($shortcut.Directory_ -ceq 'DesktopFolder') {
			++$taskDesktop
			Assert-MsiSingleFeature $shortcut.Component_ 'DesktopShortcutFeature'
		} else {
			Assert-Msi ((Resolve-MsiRelativeDirectory $shortcut.Directory_ 'ProgramMenuFolder') -ceq 'WireSteam') 'The Start menu shortcut must stay in its own product folder.'
			++$taskStartMenu
			Assert-MsiSingleFeature $shortcut.Component_ 'MainFeature'
		}
	}

	Assert-Msi ($taskDesktop -eq 1 -and $taskStartMenu -eq 1) 'A Start menu or optional desktop shortcut is missing.'

	$taskActions = @(Read-MsiTable 'CustomAction' @('Action', 'Type', 'Source', 'Target'))
	$taskGuard = @(
		$taskActions | Where-Object {
			$_.Action -ceq 'WireSteamValidateSession'
		}
	)
	Assert-Msi ($taskGuard.Count -eq 1 -and $taskGuard[0].Type -eq '1' -and $taskGuard[0].Source -ceq 'InstallerGuard' -and $taskGuard[0].Target -ceq 'WireSteamValidateSession') 'The immediate, checked session-validation DLL action is missing or unsafe.'
	$taskInstallDirectorySetter = @(
		$taskActions | Where-Object {
			$_.Action -ceq 'SetINSTALLFOLDER'
		}
	)
	Assert-Msi ($taskInstallDirectorySetter.Count -eq 1 -and $taskInstallDirectorySetter[0].Type -eq '51' -and
		$taskInstallDirectorySetter[0].Source -ceq 'INSTALLFOLDER' -and $taskInstallDirectorySetter[0].Target -ceq '[WIRESTEAM_PREVIOUSINSTALLDIR]') 'Previous installation discovery must set the INSTALLFOLDER property with a Type 51 action before directory costing.'
	$taskArpLocationSetter = @(
		$taskActions | Where-Object {
			$_.Action -ceq 'SetARPINSTALLLOCATION'
		}
	)
	Assert-Msi ($taskArpLocationSetter.Count -eq 1 -and $taskArpLocationSetter[0].Type -eq '51' -and
		$taskArpLocationSetter[0].Source -ceq 'ARPINSTALLLOCATION' -and $taskArpLocationSetter[0].Target -ceq '[INSTALLFOLDER]') 'The ARP location must be formatted from the resolved INSTALLFOLDER through a Type 51 action.'
	$taskStop = @(
		$taskActions | Where-Object {
			$_.Action -ceq 'WireSteamStopComponents'
		}
	)
	Assert-Msi ($taskStop.Count -eq 1 -and $taskStop[0].Type -eq '3073' -and $taskStop[0].Source -ceq 'InstallerGuard' -and $taskStop[0].Target -ceq 'WireSteamStopComponents') 'The checked, deferred system-context component shutdown action is missing or unsafe.'
	foreach ($action in $taskActions) {
		Assert-Msi ($action.Action -cin @('WireSteamValidateSession', 'WireSteamStopComponents') -or $action.Type -eq '51') ('Unexpected executable, script or deferred custom action: ' + $action.Action)
	}

	Assert-Msi (
		@(
			Read-MsiTable 'Binary' @('Name') | Where-Object {
				$_.Name -ceq 'InstallerGuard'
			}
		).Count -eq 1
	) 'The session guard DLL is not embedded.'
	$taskSequence = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
	foreach ($action in @(Read-MsiTable 'InstallExecuteSequence' @('Action', 'Condition', 'Sequence'))) {
		$taskSequence.Add($action.Action, $action)
	}

	$taskUiSequence = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
	foreach ($action in @(Read-MsiTable 'InstallUISequence' @('Action', 'Condition', 'Sequence'))) {
		$taskUiSequence.Add($action.Action, $action)
	}

	foreach ($sequence in @(
			@{
				Name = 'InstallUISequence'
				Rows = $taskUiSequence
			},
			@{
				Name = 'InstallExecuteSequence'
				Rows = $taskSequence
			}
		)) {
		$rows = $sequence.Rows
		foreach ($action in @('AppSearch', 'SetINSTALLFOLDER', 'CostInitialize', 'CostFinalize')) {
			Assert-Msi ($rows.ContainsKey($action)) ('Missing installation-directory action in ' + $sequence.Name + ': ' + $action)
		}

		Assert-Msi ([int]$rows['AppSearch'].Sequence -lt [int]$rows['SetINSTALLFOLDER'].Sequence -and
			[int]$rows['SetINSTALLFOLDER'].Sequence -lt [int]$rows['CostInitialize'].Sequence -and
			[int]$rows['SetINSTALLFOLDER'].Sequence -lt [int]$rows['CostFinalize'].Sequence) ('Previous installation discovery must run after AppSearch and before directory costing in ' + $sequence.Name + '.')
		$condition = $rows['SetINSTALLFOLDER'].Condition -replace '[()\s]', ''
		Assert-Msi ($condition -ceq 'WIRESTEAM_PREVIOUSINSTALLDIRANDNOTINSTALLFOLDER') ('Previous installation discovery must preserve an explicitly selected directory in ' + $sequence.Name + '.')

		if ($rows.ContainsKey('SetARPINSTALLLOCATION')) {
			Assert-Msi ([int]$rows['SetARPINSTALLLOCATION'].Sequence -gt [int]$rows['CostFinalize'].Sequence) ('The ARP installation location must be set after directory costing in ' + $sequence.Name + '.')
		}
	}

	Assert-Msi ($taskSequence.ContainsKey('SetARPINSTALLLOCATION')) 'The ARP installation location must be set in the execute sequence.'
	foreach ($action in @('WireSteamValidateSession', 'WireSteamStopComponents', 'InstallValidate', 'InstallInitialize', 'InstallFiles', 'InstallExecute', 'RemoveExistingProducts', 'ProcessComponents', 'InstallFinalize')) {
		Assert-Msi ($taskSequence.ContainsKey($action)) ('Missing execute action: ' + $action)
	}

	Assert-Msi ([int]$taskSequence['WireSteamValidateSession'].Sequence -gt 0 -and [int]$taskSequence['WireSteamValidateSession'].Sequence -lt [int]$taskSequence['InstallValidate'].Sequence) 'Session validation must precede InstallValidate.'
	Assert-Msi ($taskSequence['WireSteamValidateSession'].Condition -eq '') 'Execution must always validate the final servicing action, including silent removal.'
	Assert-Msi ($taskUiSequence.ContainsKey('WireSteamValidateSession') -and ($taskUiSequence['WireSteamValidateSession'].Condition -replace '[()\s]', '') -ceq 'NOTInstalled') 'Maintenance UI must allow reaching removal before the final OS/removal check.'
	Assert-Msi ([int]$taskSequence['WireSteamStopComponents'].Sequence -gt [int]$taskSequence['InstallInitialize'].Sequence -and
		[int]$taskSequence['WireSteamStopComponents'].Sequence -lt [int]$taskSequence['InstallExecute'].Sequence) 'Owned component shutdown must run elevated inside the transaction before application file replacement.'
	Assert-Msi (($taskSequence['WireSteamStopComponents'].Condition -replace '[()\s]', '') -ceq 'NOTInstalledANDWIX_UPGRADE_DETECTEDANDNOTREMOVE') 'Forced shutdown must be limited to replacing a registered related product.'
	Assert-Msi ([int]$taskSequence['RemoveExistingProducts'].Sequence -gt [int]$taskSequence['InstallExecute'].Sequence -and
		[int]$taskSequence['RemoveExistingProducts'].Sequence -lt [int]$taskSequence['ProcessComponents'].Sequence -and
		[int]$taskSequence['RemoveExistingProducts'].Sequence -lt [int]$taskSequence['InstallFiles'].Sequence -and
		[int]$taskSequence['RemoveExistingProducts'].Sequence -lt [int]$taskSequence['InstallFinalize'].Sequence) 'Related-product removal must follow executed shutdown and precede component/file installation within the rollback transaction.'
	$taskUpgrades = @(Read-MsiTable 'Upgrade' @('UpgradeCode', 'VersionMin', 'VersionMax', 'Attributes', 'ActionProperty'))
	foreach ($upgrade in $taskUpgrades) {
		Assert-Msi ($upgrade.UpgradeCode -ieq $taskUpgradeCode) 'An upgrade row targets another product family.'
	}

	Assert-Msi ($taskUpgrades.Count -eq 1 -and $taskUpgrades[0].ActionProperty -ceq 'WIX_UPGRADE_DETECTED' -and
		$taskUpgrades[0].VersionMax -eq '' -and ([int]$taskUpgrades[0].Attributes -band 0x02) -eq 0) 'Replacement must remove related older, newer and same-version products with another product code.'
	$taskConditions = @(Read-MsiTable 'LaunchCondition' @('Condition', 'Description'))
	Assert-Msi (@($taskConditions | Where-Object { $_.Condition -match 'WIX_DOWNGRADE_DETECTED' }).Count -eq 0) 'Downgrades must use the same related-product replacement path.'
	Assert-Msi ($taskProperties.ContainsKey('SecureCustomProperties') -and
		($taskProperties['SecureCustomProperties'] -split ';') -ccontains 'WIX_UPGRADE_DETECTED') 'Related-product detection must survive elevation.'

	foreach ($table in @('ServiceInstall', 'ServiceControl', 'Environment')) {
		Assert-Msi (@(Read-MsiTable $table @($table) $false).Count -eq 0) ('The installer must not declare global service or environment changes: ' + $table)
	}

	$taskShortcutRegistryComponents = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach ($table in @('Registry', 'RemoveRegistry')) {
		foreach ($registry in @(Read-MsiTable $table @('Root', 'Key', 'Name', 'Component_') $false)) {
			Assert-Msi ($registry.Key -ieq 'Software\WireSteam\Installer') 'Registry changes must remain inside the owned installer key.'
			$ownedShortcutValue = $table -ceq 'Registry' -and (
				($registry.Name -ceq 'StartMenuShortcut' -and $registry.Component_ -ceq 'StartMenuShortcut') -or
				($registry.Name -ceq 'DesktopShortcut' -and $registry.Component_ -ceq 'DesktopShortcut')
			)
			if ($ownedShortcutValue) {
				Assert-Msi ($registry.Root -eq '-1' -and $taskShortcutRegistryComponents.Add($registry.Component_)) 'Only the two exact shortcut marker/component pairs may use HKMU, once each; ALLUSERS=1 resolves them to HKLM.'
			} else {
				Assert-Msi ($registry.Root -eq '2') 'All other installer registry changes must remain in HKLM.'
			}
		}
	}

	Assert-Msi ($taskShortcutRegistryComponents.Count -eq 2) 'Both shortcut components must retain their scope-aware registry keypaths.'

	foreach ($removal in @(Read-MsiTable 'RemoveFile' @('FileName', 'DirProperty', 'InstallMode') $false)) {
		Assert-Msi ($removal.FileName -eq '' -and $removal.InstallMode -eq '2') 'Uninstall may remove only empty owned folders, never user configuration files.'
		$owned = $removal.DirProperty -ceq 'INSTALLFOLDER' -or $removal.DirProperty -ceq 'ExamplesFolder'
		if (!$owned) {
			try {
				$owned = (Resolve-MsiRelativeDirectory $removal.DirProperty 'ProgramMenuFolder') -ceq 'WireSteam'
			} catch {
				$owned = $false
			}
		}

		Assert-Msi $owned 'Folder cleanup escaped the owned application or Start menu folder.'
	}

	$taskDialogs = @(Read-MsiTable 'Dialog' @('Dialog', 'Title'))
	Assert-Msi (!$taskTables.Contains('ListBox') -and @($taskDialogs | Where-Object { $_.Dialog -cin @('FilesInUse', 'MsiRMFilesInUse') }).Count -eq 0) 'A manual-close dialog must not interrupt replacement before its deferred component shutdown.'
	Assert-Msi (
		$taskDialogs.Count -gt 0 -and @(
			$taskDialogs | Where-Object {
				$_.Dialog -match '(?i)license|eula'
			}
		).Count -eq 0
	) 'The English installer UI must have no license-agreement dialog.'
	$taskControls = @(Read-MsiTable 'Control' @('Dialog_', 'Control', 'Text'))
	foreach (
		$text in @(
			$taskDialogs | ForEach-Object {
				$_.Title
			}
		) + @(
			$taskControls | ForEach-Object {
				$_.Text
			}
		) + @(
			$taskFeatures.Values | ForEach-Object {
				$_.Title
				$_.Description
			}
		)
	) {
		Assert-Msi ($text -notmatch '[\p{IsCyrillic}\p{IsHebrew}\p{IsArabic}\p{IsCJKUnifiedIdeographs}]' -and $text -notmatch '(?i)\blicense agreement\b|accept.*\blicense\b') 'Installer-facing strings must be English and must not add a license agreement.'
	}

	Write-Output ('PASS MSI: WireSteam ' + $taskVersion + ', x64 per-machine, exact ' + $taskFiles.Count + '-file payload, features/shortcuts, embedded cabinets, prerequisite guard, scoped shutdown and rollback replacement, normal restart policy and private-data boundaries.')
} finally {
	try {
		Release-MsiObject $taskDatabase
	} finally {
		Release-MsiObject $taskInstaller
	}
}
