[CmdletBinding()]
param(
    [string] $InstallDirectory = "$env:ProgramFiles\LanLink",
    [switch] $RemoveData,
    [switch] $PromptForData
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ClientSetup.psm1') -Force
Assert-LanLinkAdministrator
$InstallDirectory = Get-LanLinkFullPath $InstallDirectory
$markerPath = Join-Path $InstallDirectory 'installation.json'
if (-not (Test-Path -LiteralPath $markerPath)) { throw 'A LanLink installation marker is required.' }
$marker = Get-Content -LiteralPath $markerPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($marker.install_directory -ne $InstallDirectory) { throw 'The installation marker does not match this directory.' }
$dataDirectory = Get-LanLinkFullPath $marker.data_directory
if ($PromptForData -and -not $RemoveData) {
    Add-Type -AssemblyName System.Windows.Forms
    $choice = [Windows.Forms.MessageBox]::Show('Delete saved LanLink server profiles, device credentials and settings? Choose No to keep them.',
        'Uninstall LanLink', 'YesNoCancel', 'Question', 'Button2')
    if ($choice -eq 'Cancel') { return }
    $RemoveData = $choice -eq 'Yes'
}
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
foreach ($folder in 'CommonPrograms', 'CommonDesktopDirectory') {
    $shortcut = Join-Path ([Environment]::GetFolderPath($folder)) 'LanLink.lnk'
    if (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut -Force }
}
Remove-Item -LiteralPath $InstallDirectory -Recurse -Force
if ($RemoveData) {
    if (Test-Path -LiteralPath $dataDirectory) { Remove-Item -LiteralPath $dataDirectory -Recurse -Force }
    $preferences = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'LanLink'
    if (Test-Path -LiteralPath $preferences) { Remove-Item -LiteralPath $preferences -Recurse -Force }
}
Write-Output $(if ($RemoveData) { 'LanLink and its data were removed.' } else { "LanLink removed. Data preserved in $dataDirectory." })
