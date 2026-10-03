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
if (Test-Path -LiteralPath $InstallDirectory) {
    $marker = Join-Path $InstallDirectory 'installation.json'
    if (-not (Test-Path -LiteralPath $marker)) { throw 'The target directory is not a LanLink installation.' }
    $previous = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
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
$wasRunning = $service -and $service.Status -eq 'Running'
$oldConfig = if (Test-Path -LiteralPath $configPath) { [IO.File]::ReadAllBytes($configPath) } else { $null }
$oldToken = if (Test-Path -LiteralPath $tokenPath) { [IO.File]::ReadAllBytes($tokenPath) } else { $null }
$moved = $false
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
    New-Item -ItemType Directory -Path $DataDirectory -Force | Out-Null
    Set-LanLinkDirectoryAcl $DataDirectory
    if ($service) {
        try {
            if ($service.Status -ne 'Stopped') {
                $service.Stop()
                $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
            }
        } finally { $service.Dispose() }
    }
    if (Test-Path -LiteralPath $InstallDirectory) { Move-Item -LiteralPath $InstallDirectory -Destination $backup }
    Move-Item -LiteralPath $staging -Destination $InstallDirectory
    $moved = $true
    if ($AuthenticationTokenFile) { [IO.File]::WriteAllText($tokenPath, "$token`n", $encoding) }
    if ($configuration) { [IO.File]::WriteAllText($configPath, "$configuration`n", $encoding) }
    $executable = Join-Path $InstallDirectory 'bin\lanlink-service.exe'
    Push-Location $DataDirectory
    try { Invoke-LanLinkServiceCommand $executable @('--install', $configPath) } finally { Pop-Location }
    Set-LanLinkFirewallRules $executable
    $shortcutPath = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'LanLink.lnk'
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = Join-Path $InstallDirectory 'bin\lanlink-ui.exe'
    $shortcut.WorkingDirectory = Join-Path $InstallDirectory 'bin'
    $shortcut.Save()
    if (-not $NoStart) { Invoke-LanLinkServiceCommand $executable @('--start') }
    Register-LanLinkInstallation $InstallDirectory $version
    if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Recurse -Force }
    Write-Output "LanLink $version installed in $InstallDirectory. Data: $DataDirectory"
} catch {
    $failure = $_
    if ($moved -and (Test-Path -LiteralPath $backup)) {
        Stop-Service -Name LanLink -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $InstallDirectory -Recurse -Force
        Move-Item -LiteralPath $backup -Destination $InstallDirectory
        if ($null -ne $oldConfig) { [IO.File]::WriteAllBytes($configPath, $oldConfig) }
        if ($null -ne $oldToken) { [IO.File]::WriteAllBytes($tokenPath, $oldToken) }
        $executable = Join-Path $InstallDirectory 'bin\lanlink-service.exe'
        Push-Location $DataDirectory
        try { Invoke-LanLinkServiceCommand $executable @('--install', $configPath) } finally { Pop-Location }
        Set-LanLinkFirewallRules $executable
        if ($wasRunning) { Invoke-LanLinkServiceCommand $executable @('--start') }
        Register-LanLinkInstallation $InstallDirectory $previous.version
    }
    throw $failure
} finally {
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
}
