[CmdletBinding()]
param(
    [string] $InstallDirectory = "$env:ProgramFiles\LanLink",
    [switch] $RemoveData
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ClientSetup.psm1') -Force
Assert-LanLinkAdministrator
$InstallDirectory = Get-LanLinkFullPath $InstallDirectory
$markerPath = Join-Path $InstallDirectory 'installation.json'
if (-not (Test-Path -LiteralPath $markerPath)) { throw 'A LanLink installation marker is required.' }
$marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
if ($marker.install_directory -ne $InstallDirectory) { throw 'The installation marker does not match this directory.' }
$dataDirectory = Get-LanLinkFullPath $marker.data_directory
if (Get-Service -Name LanLink -ErrorAction SilentlyContinue) {
    Invoke-LanLinkServiceCommand (Join-Path $InstallDirectory 'bin\lanlink-service.exe') @('--uninstall')
    $deadline = (Get-Date).AddSeconds(30)
    while (Get-Service -Name LanLink -ErrorAction SilentlyContinue) {
        if ((Get-Date) -ge $deadline) { throw 'Timed out waiting for the service to be deleted.' }
        Start-Sleep -Milliseconds 200
    }
}
Remove-LanLinkFirewallRules
Unregister-LanLinkInstallation $InstallDirectory
$shortcut = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'LanLink.lnk'
if (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut -Force }
Remove-Item -LiteralPath $InstallDirectory -Recurse -Force
if ($RemoveData -and (Test-Path -LiteralPath $dataDirectory)) {
    Remove-Item -LiteralPath $dataDirectory -Recurse -Force
}
Write-Output $(if ($RemoveData) { 'LanLink and its data were removed.' } else { "LanLink removed. Data preserved in $dataDirectory." })
