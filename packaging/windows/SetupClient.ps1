param(
    [string] $RelayHost,
    [int] $RelayPort = 4433,
    [string] $AuthenticationTokenFile,
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
$result = 0
try {
    $parameters = @{ InstallDirectory = $InstallDirectory; DataDirectory = $DataDirectory; RelayPort = $RelayPort; NoStart = $NoStart }
    if ($RelayHost) { $parameters.RelayHost = $RelayHost }
    if ($AuthenticationTokenFile) { $parameters.AuthenticationTokenFile = $AuthenticationTokenFile }
    & (Join-Path $PSScriptRoot 'Install-LanLink.ps1') @parameters
} catch {
    Write-Output ($_ | Out-String)
    $result = 1
} finally { Stop-Transcript | Out-Null }
exit $result
