[CmdletBinding()]
param(
    [string] $RelayHost,
    [ValidateRange(1, 65535)] [int] $RelayPort = 4433,
    [string] $AuthenticationTokenFile,
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
foreach ($file in 'bin\lanlink-service.exe', 'bin\lanlink-ui.exe', 'bin\lanlink-ui-cli.exe',
                  'bin\wintun.dll', 'package-version.txt', 'Uninstall-LanLink.ps1',
                  'build-info.json', 'INSTALL.md', 'RELEASE-NOTES.md') {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $file) -PathType Leaf)) {
        throw "The client package is incomplete: $file"
    }
}
if ($PSScriptRoot -eq $InstallDirectory) { throw 'Extract the new package outside the installed directory.' }
$configPath = Join-Path $DataDirectory 'client.conf'
$tokenPath = Join-Path $DataDirectory 'auth.token'
if (-not (Test-Path -LiteralPath $configPath) -and [string]::IsNullOrWhiteSpace($RelayHost)) {
    throw 'Specify -RelayHost for the first installation.'
}
if (-not (Test-Path -LiteralPath $tokenPath) -and [string]::IsNullOrWhiteSpace($AuthenticationTokenFile)) {
    throw 'Specify -AuthenticationTokenFile for the first installation.'
}
$token = if ($AuthenticationTokenFile) { Get-LanLinkToken $AuthenticationTokenFile } else { Get-LanLinkToken $tokenPath }
$configuration = if ($RelayHost) { New-LanLinkClientConfig $RelayHost $RelayPort $DataDirectory } else { $null }
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
$oldToken = if (Test-Path -LiteralPath $tokenPath) { ,[IO.File]::ReadAllBytes($tokenPath) } else { $null }
$oldDataAcl = if ($hadData) { Get-Acl -LiteralPath $DataDirectory } else { $null }
$oldRegistration = Get-LanLinkRegistrationSnapshot
$shortcutPath = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'LanLink.lnk'
$oldShortcut = if (Test-Path -LiteralPath $shortcutPath) { ,[IO.File]::ReadAllBytes($shortcutPath) } else { $null }
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
                     'build-info.json', 'INSTALL.md', 'RELEASE-NOTES.md') {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $staging
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses') -Destination $staging -Recurse
    $version = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package-version.txt') -Raw).Trim()
    $marker = @{ install_directory = $InstallDirectory; data_directory = $DataDirectory; version = $version } | ConvertTo-Json
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
    if ($hadProgram) {
        Move-Item -LiteralPath $InstallDirectory -Destination $backup
        $programBackedUp = $true
    }
    $programInstallStarted = $true
    Move-Item -LiteralPath $staging -Destination $InstallDirectory
    if ($AuthenticationTokenFile) { [IO.File]::WriteAllText($tokenPath, "$token`n", $encoding) }
    if ($configuration) { [IO.File]::WriteAllText($configPath, "$configuration`n", $encoding) }
    $executable = Join-Path $InstallDirectory 'bin\lanlink-service.exe'
    $serviceChanged = $true
    Push-Location $DataDirectory
    try { Invoke-LanLinkServiceCommand $executable @('--install', $configPath) } finally { Pop-Location }
    $firewallChanged = $true
    Set-LanLinkFirewallRules $executable
    $shortcutChanged = $true
    Invoke-LanLinkServiceCommand $executable @('--create-shortcut', $shortcutPath)
    if (-not $NoStart) { Invoke-LanLinkServiceCommand $executable @('--start') }
    $registrationChanged = $true
    Register-LanLinkInstallation $InstallDirectory $version
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
            if ($programBackedUp) { Move-Item -LiteralPath $backup -Destination $InstallDirectory }
        },
        { if ($dataChanged) { Restore-LanLinkFile $configPath $oldConfig } },
        { if ($dataChanged) { Restore-LanLinkFile $tokenPath $oldToken } },
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
            }
        },
        { if ($registrationChanged) { Restore-LanLinkRegistration $oldRegistration } },
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
