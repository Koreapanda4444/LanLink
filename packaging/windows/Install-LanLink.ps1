[CmdletBinding()]
param(
    [ValidateSet('Install', 'Modify', 'Repair')] [string] $Mode = 'Install',
    [string] $SetupExecutable,
    [switch] $NoCli,
    [switch] $NoStartMenu,
    [switch] $DesktopShortcut,
    [string] $InstallDirectory = "$env:ProgramFiles\LanLink",
    [string] $DataDirectory = "$env:ProgramData\LanLink",
    [switch] $NoStart
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ClientSetup.psm1') -Force
Assert-LanLinkAdministrator
$InstallDirectory = Get-LanLinkFullPath $InstallDirectory
$DataDirectory = Get-LanLinkFullPath $DataDirectory
if ($InstallDirectory -eq $DataDirectory -or
    $InstallDirectory.StartsWith("$DataDirectory\", [StringComparison]::OrdinalIgnoreCase) -or
    $DataDirectory.StartsWith("$InstallDirectory\", [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Program files and persistent data must use separate directories.'
}
foreach ($path in $InstallDirectory, $DataDirectory) {
    if ((Test-Path -LiteralPath $path) -and
        ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Installation directories cannot be links or junctions.'
    }
}
$hadProgram = Test-Path -LiteralPath $InstallDirectory
$hadData = Test-Path -LiteralPath $DataDirectory
if ($hadProgram) {
    $marker = Join-Path $InstallDirectory 'installation.json'
    if (-not (Test-Path -LiteralPath $marker)) { throw 'The target directory is not a LanLink installation.' }
    $previous = Get-Content -LiteralPath $marker -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($previous.install_directory -ne $InstallDirectory -or $previous.data_directory -ne $DataDirectory) {
        throw 'An upgrade must preserve the existing program and data directories.'
    }
}
if ($Mode -ne 'Install' -and -not $hadProgram) { throw 'Maintenance requires an existing LanLink installation.' }
if ($Mode -eq 'Repair') {
    $NoCli = $previous.PSObject.Properties['cli'] -and -not $previous.cli
    $NoStartMenu = $previous.PSObject.Properties['start_menu'] -and -not $previous.start_menu
    $DesktopShortcut = $previous.PSObject.Properties['desktop'] -and $previous.desktop
}
foreach ($file in 'bin\lanlink-service.exe', 'bin\lanlink-ui.exe', 'bin\lanlink-ui-cli.exe',
                  'bin\wintun.dll', 'package-version.txt', 'Uninstall-LanLink.ps1',
                  'build-info.json', 'INSTALL.md', 'SERVER-SETUP.md', 'RELEASE-NOTES.md', 'Maintenance-LanLink.ps1') {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $file) -PathType Leaf)) {
        throw "The client package is incomplete: $file"
    }
}
if ($PSScriptRoot -eq $InstallDirectory) { throw 'Extract the new package outside the installed directory.' }
$configPath = Join-Path $DataDirectory 'client.conf'
$tokenPath = Join-Path $DataDirectory 'auth.token'
# Installation owns only local paths. It never accepts or rewrites server credentials.
$configuration = New-LanLinkUnconfiguredClientConfig $DataDirectory
$encoding = [Text.UTF8Encoding]::new($false)
$staging = "$InstallDirectory.staging-$([Guid]::NewGuid().ToString('N'))"
$backup = "$InstallDirectory.previous-$([Guid]::NewGuid().ToString('N'))"
$service = Get-Service -Name LanLink -ErrorAction SilentlyContinue
$hadService = $null -ne $service
$wasRunning = $hadService -and $service.Status -eq 'Running'
$serviceStartMode = $null
if ($service) {
    $service.Dispose()
    if (-not $hadProgram) { throw 'The existing LanLink service must be upgraded from its current installation directory.' }
    $serviceDetails = Get-CimInstance Win32_Service -Filter "Name='LanLink'"
    $expectedExecutable = Join-Path $InstallDirectory 'bin\lanlink-service.exe'
    if (-not $serviceDetails.PathName.StartsWith('"' + $expectedExecutable + '" ', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The existing LanLink service belongs to another installation directory.'
    }
    $serviceStartMode = $serviceDetails.StartMode
}
$oldConfig = if (Test-Path -LiteralPath $configPath) { ,[IO.File]::ReadAllBytes($configPath) } else { $null }
$oldDataAcl = if ($hadData) { Get-Acl -LiteralPath $DataDirectory } else { $null }
$oldRegistration = Get-LanLinkRegistrationSnapshot
$oldUrl = Get-LanLinkUrlSnapshot
$urlChanged = $false
$shortcutPath = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'LanLink.lnk'
$desktopPath = Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) 'LanLink.lnk'
$oldShortcut = if (Test-Path -LiteralPath $shortcutPath) { ,[IO.File]::ReadAllBytes($shortcutPath) } else { $null }
$oldDesktop = if (Test-Path -LiteralPath $desktopPath) { ,[IO.File]::ReadAllBytes($desktopPath) } else { $null }
$firewallBackups = @()
$programBackedUp = $false
$programInstallStarted = $false
$serviceStopped = $false
$serviceChanged = $false
$dataChanged = $false
$firewallChanged = $false
$shortcutChanged = $false
$registrationChanged = $false
$committed = $false
$rollbackComplete = $false
try {
    New-Item -ItemType Directory -Path $staging -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'bin') -Destination $staging -Recurse
    foreach ($file in 'Install-LanLink.ps1', 'Uninstall-LanLink.ps1', 'ClientSetup.psm1', 'package-version.txt',
                     'build-info.json', 'INSTALL.md', 'SERVER-SETUP.md', 'RELEASE-NOTES.md', 'Maintenance-LanLink.ps1') {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $staging
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses') -Destination $staging -Recurse
    $version = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package-version.txt') -Raw).Trim()
    if ($NoCli) { Remove-Item -LiteralPath (Join-Path $staging 'bin\lanlink-ui-cli.exe') -Force }
    if ($SetupExecutable) {
        New-Item -ItemType Directory -Path (Join-Path $staging 'maintenance') | Out-Null
        Copy-Item -LiteralPath $SetupExecutable -Destination (Join-Path $staging 'maintenance\LanLink-Setup.exe')
    }
    $marker = @{ install_directory = $InstallDirectory; data_directory = $DataDirectory; version = $version
        cli = -not $NoCli; start_menu = -not $NoStartMenu; desktop = [bool]$DesktopShortcut } | ConvertTo-Json
    [IO.File]::WriteAllText((Join-Path $staging 'installation.json'), $marker, $encoding)
    Set-LanLinkDirectoryAcl $staging -AllowUsers
    $firewallBackups = Backup-LanLinkFirewallRules
    $dataChanged = $true
    New-Item -ItemType Directory -Path $DataDirectory -Force | Out-Null
    Set-LanLinkDirectoryAcl $DataDirectory
    if ($hadService) {
        $serviceStopped = $true
        Stop-LanLinkService
    }
    # Same-volume renames must be atomic. Move-Item can fall back to moving
    # individual children and leave half a program tree after a sharing error.
    if ($hadProgram) {
        [IO.Directory]::Move($InstallDirectory, $backup)
        $programBackedUp = $true
    }
    $programInstallStarted = $true
    [IO.Directory]::Move($staging, $InstallDirectory)
    if (-not (Test-Path -LiteralPath $configPath)) { [IO.File]::WriteAllText($configPath, "$configuration`n", $encoding) }
    $executable = Join-Path $InstallDirectory 'bin\lanlink-service.exe'
    $serviceChanged = $true
    Push-Location $DataDirectory
    try { Invoke-LanLinkServiceCommand $executable @('--install', $configPath) } finally { Pop-Location }
    $firewallChanged = $true
    Set-LanLinkFirewallRules $executable
    $shortcutChanged = $true
    if (-not $NoStartMenu) { Invoke-LanLinkServiceCommand $executable @('--create-shortcut', $shortcutPath) }
    elseif (Test-Path -LiteralPath $shortcutPath) { Remove-Item -LiteralPath $shortcutPath -Force }
    if ($DesktopShortcut) { Invoke-LanLinkServiceCommand $executable @('--create-shortcut', $desktopPath) }
    elseif (Test-Path -LiteralPath $desktopPath) { Remove-Item -LiteralPath $desktopPath -Force }
    $preserveServiceMode = $hadService -and $Mode -ne 'Install'
    if (-not $NoStart -and (-not $preserveServiceMode -or $wasRunning)) {
        Invoke-LanLinkServiceCommand $executable @('--start')
    }
    if ($preserveServiceMode) {
        $startupType = switch ($serviceStartMode) { 'Auto' { 'Automatic' }; 'Manual' { 'Manual' }; 'Disabled' { 'Disabled' } }
        Set-Service -Name LanLink -StartupType $startupType
    }
    $registrationChanged = $true
    Register-LanLinkInstallation $InstallDirectory $version
    if ($null -ne $oldUrl -and (-not $oldUrl[''].ContainsKey('LanLinkOwnerPath') -or $oldUrl['']['LanLinkOwnerPath'].value -ne $InstallDirectory)) { throw 'The lanlink URL scheme belongs to another installation.' }
    $urlChanged = $true
    Register-LanLinkUrl $InstallDirectory
    $committed = $true
    Write-Output "LanLink $version installed in $InstallDirectory. Data: $DataDirectory"
} catch {
    $failure = $_
    $rollbackErrors = [Collections.Generic.List[Exception]]::new()
    $actions = @(
        {
            if ($serviceChanged -or $serviceStopped) { Stop-LanLinkService }
            if ($serviceChanged -and -not $hadService) { Remove-LanLinkService }
        },
        {
            if ($programInstallStarted -and (Test-Path -LiteralPath $InstallDirectory)) {
                Remove-Item -LiteralPath $InstallDirectory -Recurse -Force
            }
            if ($programBackedUp) { [IO.Directory]::Move($backup, $InstallDirectory) }
        },
        { if ($dataChanged) { Restore-LanLinkFile $configPath $oldConfig } },
        {
            if ($dataChanged) {
                if ($hadData) { Set-Acl -LiteralPath $DataDirectory -AclObject $oldDataAcl }
                elseif (Test-Path -LiteralPath $DataDirectory) { Remove-Item -LiteralPath $DataDirectory -Recurse -Force }
            }
        },
        { if ($firewallChanged) { Restore-LanLinkFirewallRules $firewallBackups } },
        {
            if ($shortcutChanged) {
                Restore-LanLinkFile $shortcutPath $oldShortcut
                Restore-LanLinkFile $desktopPath $oldDesktop
            }
        },
        { if ($registrationChanged) { Restore-LanLinkRegistration $oldRegistration } },
        { if ($urlChanged) { Restore-LanLinkUrl $oldUrl } },
        {
            if ($hadService -and ($serviceChanged -or $serviceStopped)) {
                $executable = Join-Path $InstallDirectory 'bin\lanlink-service.exe'
                $startupType = switch ($serviceStartMode) { 'Auto' { 'Automatic' }; 'Manual' { 'Manual' }; 'Disabled' { 'Disabled' } }
                try {
                    if ($rollbackErrors.Count -ne 0) {
                        throw 'Service restart was skipped because rollback did not fully restore the previous state.'
                    }
                    if ($serviceChanged) {
                        Push-Location $DataDirectory
                        try { Invoke-LanLinkServiceCommand $executable @('--install', $configPath) } finally { Pop-Location }
                    }
                    if ($wasRunning) { Invoke-LanLinkServiceCommand $executable @('--start') }
                } finally { Set-Service -Name LanLink -StartupType $startupType }
            }
        }
    )
    foreach ($action in $actions) {
        try { & $action } catch { $rollbackErrors.Add($_.Exception) }
    }
    $rollbackComplete = $rollbackErrors.Count -eq 0
    if (-not $rollbackComplete) {
        $rollbackErrors.Insert(0, $failure.Exception)
        throw [AggregateException]::new("LanLink installation failed and rollback was incomplete. Retained backup: $backup", $rollbackErrors)
    }
    throw $failure
} finally {
    if ($committed -or $rollbackComplete) {
        try { Remove-LanLinkFirewallBackups $firewallBackups } catch { Write-Warning "Cannot remove temporary firewall backups: $_" }
    }
    if ($committed -and (Test-Path -LiteralPath $backup)) {
        try { Remove-Item -LiteralPath $backup -Recurse -Force } catch { Write-Warning "Previous program files remain in ${backup}: $_" }
    }
    if (Test-Path -LiteralPath $staging) {
        try { Remove-Item -LiteralPath $staging -Recurse -Force } catch { Write-Warning "Staging files remain in ${staging}: $_" }
    }
}
