[CmdletBinding()]
param(
    [ValidateSet('Modify', 'Repair', 'Uninstall')] [string] $Mode = 'Modify',
    [switch] $Silent,
    [switch] $RemoveData
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Mode ' + $Mode
    if ($Silent) { $arguments += ' -Silent' }
    if ($RemoveData) { $arguments += ' -RemoveData' }
    $elevated = Start-Process -FilePath "$PSHOME\powershell.exe" -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    exit $elevated.ExitCode
}
# Execute a private temporary copy so repair/uninstall can replace or remove
# every installed file, including the cached Setup itself.
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('LanLink-maintenance-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
$result = 1
try {
    Import-Module (Join-Path $PSScriptRoot 'ClientSetup.psm1') -Force
    Set-LanLinkDirectoryAcl $temporary
    $setup = Join-Path $temporary 'LanLink-Setup.exe'
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'maintenance\LanLink-Setup.exe') -Destination $setup
    $arguments = @("/MODE=$Mode", '/NORESTART')
    if ($Silent) { $arguments += @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/SP-') }
    if ($RemoveData) { $arguments += '/RemoveData=1' }
    $process = Start-Process -FilePath $setup -ArgumentList $arguments -Wait -PassThru
    $result = $process.ExitCode
} finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
exit $result
