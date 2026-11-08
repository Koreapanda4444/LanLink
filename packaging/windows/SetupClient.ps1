param(
    [ValidateSet('Install', 'Modify', 'Repair', 'Uninstall')] [string] $Mode = 'Install',
    [string] $SetupExecutable,
    [string] $ProgressFile,
    [switch] $NoCli,
    [switch] $NoStartMenu,
    [switch] $DesktopShortcut,
    [switch] $RemoveData,
    [Parameter(Mandatory)] [string] $InstallDirectory,
    [Parameter(Mandatory)] [string] $DataDirectory,
    [Parameter(Mandatory)] [string] $LogFile,
    [switch] $NoStart
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -eq 'Desktop') {
    $env:PSModulePath = "$PSHOME\Modules" + [IO.Path]::PathSeparator +
        [Environment]::GetFolderPath('ProgramFiles') + '\WindowsPowerShell\Modules'
}
Start-Transcript -LiteralPath $LogFile -Force | Out-Null
Import-Module (Join-Path $PSScriptRoot 'ClientSetup.psm1') -Force
$result = 0
try {
    if ($Mode -eq 'Uninstall') {
        & (Join-Path $PSScriptRoot 'Uninstall-LanLink.ps1') -InstallDirectory $InstallDirectory -RemoveData:$RemoveData -ProgressFile $ProgressFile
    } else {
        $parameters = @{ InstallDirectory = $InstallDirectory; DataDirectory = $DataDirectory; NoStart = $NoStart
            Mode = $Mode; SetupExecutable = $SetupExecutable; ProgressFile = $ProgressFile; NoCli = $NoCli; NoStartMenu = $NoStartMenu; DesktopShortcut = $DesktopShortcut }
        & (Join-Path $PSScriptRoot 'Install-LanLink.ps1') @parameters
    }
} catch {
    Write-Output ($_ | Out-String)
    $result = 1
} finally { Stop-Transcript | Out-Null }
if ($result -eq 0) { Write-LanLinkSetupProgress $ProgressFile 100 'complete' }
exit $result
