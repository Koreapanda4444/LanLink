param([Parameter(Mandatory)] [string] $Installer)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Get-Service -Name LanLink -ErrorAction SilentlyContinue) { throw 'Setup tests require an isolated machine.' }
$root = Join-Path $env:RUNNER_TEMP "lanlink-setup-$([Guid]::NewGuid().ToString('N'))"
$program = Join-Path $root 'program 한글'
$state = Join-Path $root 'state 한글'
New-Item -ItemType Directory -Path $root | Out-Null
$token = Join-Path $root 'auth token'
[IO.File]::WriteAllText($token, 'lanlink-executable-installer-test-token-2026')
function Invoke-Setup([string[]] $Arguments, [bool] $Success = $true) {
    $information = [Diagnostics.ProcessStartInfo]::new([IO.Path]::GetFullPath($Installer))
    $information.UseShellExecute = $false
    foreach ($argument in @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', "/DIR=$program", "/LOG=$root/setup.log") + $Arguments) {
        $information.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($information)
    try {
        if (-not $process.WaitForExit(120000)) { $process.Kill($true); throw 'The EXE installer timed out.' }
        if (($process.ExitCode -eq 0) -ne $Success) {
            if (Test-Path (Join-Path $root 'setup.log.client.log')) { Get-Content (Join-Path $root 'setup.log.client.log') -Raw }
            if (Test-Path (Join-Path $root 'setup.log')) { Get-Content (Join-Path $root 'setup.log') -Tail 50 }
            throw "Unexpected EXE installer exit code: $($process.ExitCode)"
        }
    } finally { $process.Dispose() }
}
try {
    Invoke-Setup @("/DataDir=$state", '/NoStart=1') $false
    if ((Test-Path $program) -or (Test-Path $state) -or (Get-Service LanLink -ErrorAction SilentlyContinue)) {
        throw 'Rejected setup left a partial installation.'
    }
    Invoke-Setup @("/DataDir=$state", '/RelayHost=127.0.0.1', '/RelayPort=4433', "/TokenFile=$token", '/NoStart=1')
    $service = Get-CimInstance Win32_Service -Filter "Name='LanLink'"
    if ($service.State -ne 'Stopped' -or $service.StartMode -ne 'Auto') { throw 'EXE setup did not register an automatic service.' }
    $build = Get-Content (Join-Path $program 'build-info.json') -Raw | ConvertFrom-Json
    if ($build.version -ne '0.2.0' -or $build.source_commit -ne $env:GITHUB_SHA) { throw 'EXE payload provenance differs from the CI commit.' }
    & (Join-Path $program 'bin\lanlink-service.exe') --start
    if ($LASTEXITCODE -ne 0) { throw 'The EXE-installed service could not start from a Unicode path.' }
    if (-not (Test-Path (Join-Path $state 'device.identity'))) { throw 'The service did not create its identity in the selected Unicode data directory.' }
    $shortcutPath = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'LanLink.lnk'
    Start-Process -FilePath $shortcutPath
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $desktop = $null
    do {
        $desktop = Get-Process -Name lanlink-ui -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($desktop) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $desktop) { throw 'The installed Start menu shortcut did not launch the desktop UI.' }
    try {
        if ($desktop.Path -ne (Join-Path $program 'bin\lanlink-ui.exe')) { throw 'The Start menu shortcut launched another installation.' }
        if ($desktop.WaitForExit(1500)) { throw 'The installed desktop UI exited unexpectedly.' }
    } finally {
        if (-not $desktop.HasExited) { $desktop.Kill(); $desktop.WaitForExit() }
        $desktop.Dispose()
    }
    & (Join-Path $program 'bin\lanlink-service.exe') --stop
    if ($LASTEXITCODE -ne 0) { throw 'The EXE-installed service could not stop.' }
    [IO.File]::AppendAllText((Join-Path $state 'client.conf'), "`nlog_max_files=7`n")
    $configHash = (Get-FileHash (Join-Path $state 'client.conf')).Hash
    $tokenHash = (Get-FileHash (Join-Path $state 'auth.token')).Hash
    $identityHash = (Get-FileHash (Join-Path $state 'device.identity')).Hash
    Invoke-Setup @('/NoStart=1')
    if ((Get-FileHash (Join-Path $state 'client.conf')).Hash -ne $configHash -or
        (Get-FileHash (Join-Path $state 'auth.token')).Hash -ne $tokenHash -or
        (Get-FileHash (Join-Path $state 'device.identity')).Hash -ne $identityHash) { throw 'EXE upgrade changed persistent settings or credentials.' }
    $invalidToken = Join-Path $root 'invalid token'
    [IO.File]::WriteAllText($invalidToken, 'too-short')
    Invoke-Setup @("/TokenFile=$invalidToken", '/NoStart=1') $false
    if ((Get-FileHash (Join-Path $state 'auth.token')).Hash -ne $tokenHash) { throw 'Rejected EXE upgrade changed the token.' }
    & (Join-Path $program 'Uninstall-LanLink.ps1') -InstallDirectory $program -RemoveData
    if (Get-Service LanLink -ErrorAction SilentlyContinue) { throw 'EXE installation could not be uninstalled.' }
    Write-Output 'Windows EXE setup validation, provenance, installation, upgrade, failure propagation and uninstall passed.'
} finally {
    if (Test-Path (Join-Path $program 'installation.json')) {
        & (Join-Path $program 'Uninstall-LanLink.ps1') -InstallDirectory $program -RemoveData
    }
    Remove-Item -LiteralPath $root -Recurse -Force
}
