param(
    [Parameter(Mandatory)] [string] $PackageDirectory,
    [switch] $ExpectIncompleteRollback
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
Import-Module (Join-Path $PackageDirectory 'ClientSetup.psm1') -Force
Assert-LanLinkAdministrator
if (Get-Service LanLink -ErrorAction SilentlyContinue) { throw 'Rollback tests require an isolated machine.' }
$root = Join-Path $env:RUNNER_TEMP "lanlink-rollback-lock-$([Guid]::NewGuid().ToString('N'))"
$program = Join-Path $root 'program files'
$state = Join-Path $root 'state files'
$candidate = Join-Path $root 'candidate'
$token = Join-Path $root 'original.token'
$replacement = Join-Path $root 'replacement.token'
$encoding = [Text.UTF8Encoding]::new($false)
New-Item -ItemType Directory -Path $root | Out-Null
[IO.File]::WriteAllText($token, 'lanlink-original-rollback-token-2026', $encoding)
[IO.File]::WriteAllText($replacement, 'lanlink-replacement-rollback-token-2026', $encoding)
$locked = $null
try {
    & (Join-Path $PackageDirectory 'Install-LanLink.ps1') -RelayHost original.example `
        -AuthenticationTokenFile $token -InstallDirectory $program -DataDirectory $state -NoStart
    Set-Service -Name LanLink -StartupType Manual
    [IO.File]::WriteAllText((Join-Path $program 'previous-only.marker'), 'preserved', $encoding)
    [IO.File]::WriteAllText((Join-Path $state 'device.identity'), 'identity-must-survive', $encoding)
    Copy-Item -LiteralPath $PackageDirectory -Destination $candidate -Recurse
    [IO.File]::WriteAllText((Join-Path $candidate 'bin\candidate-only.marker'), 'replacement', $encoding)
    $configPath = Join-Path $state 'client.conf'
    $tokenPath = Join-Path $state 'auth.token'
    $oldConfig = [IO.File]::ReadAllBytes($configPath)
    $oldToken = [IO.File]::ReadAllBytes($tokenPath)
    $identityHash = (Get-FileHash (Join-Path $state 'device.identity')).Hash
    $servicePath = (Get-CimInstance Win32_Service -Filter "Name='LanLink'").PathName
    $shortcutPath = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'LanLink.lnk'
    $shortcutHash = (Get-FileHash $shortcutPath).Hash
    foreach ($name in 'client.conf', 'auth.token') {
        [IO.File]::WriteAllBytes($configPath, $oldConfig)
        [IO.File]::WriteAllBytes($tokenPath, $oldToken)
        $acl = Get-Acl -LiteralPath $state
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.WindowsIdentity]::GetCurrent().User, 'ReadAndExecute', 'Allow'))
        Set-Acl -LiteralPath $state -AclObject $acl
        $oldAcl = (Get-Acl -LiteralPath $state).Sddl
        $configHash = (Get-FileHash $configPath).Hash
        $tokenHash = (Get-FileHash $tokenPath).Hash
        $locked = [IO.File]::Open((Join-Path $state $name), [IO.FileMode]::Open,
            [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $failure = $null
        try {
            & (Join-Path $candidate 'Install-LanLink.ps1') -RelayHost replacement.example `
                -AuthenticationTokenFile $replacement -InstallDirectory $program -DataDirectory $state -NoStart
        } catch { $failure = $_ }
        finally { $locked.Dispose(); $locked = $null }
        if ($null -eq $failure) { throw "A locked $name unexpectedly allowed installation." }
        $incomplete = $failure.Exception.Message.Contains('rollback was incomplete')
        if ($ExpectIncompleteRollback) {
            if (-not $incomplete -or (Get-Acl -LiteralPath $state).Sddl -eq $oldAcl) {
                throw "The released installer did not reproduce the $name rollback defect. Failure: $failure"
            }
            if ($name -eq 'client.conf' -and (Get-FileHash $tokenPath).Hash -eq $tokenHash) {
                throw 'The released installer did not reproduce the skipped token restoration.'
            }
            Write-Output "Confirmed released 0.2.0 defect: locked $name leaves incomplete rollback."
            continue
        }
        if ($incomplete) { throw "Locked-file rollback did not finish: $failure" }
        if ($failure.Exception.GetBaseException().HResult -ne -2147024864) { throw "Installation did not reach the expected file-sharing violation: $failure" }
        if ((Get-FileHash $configPath).Hash -ne $configHash -or (Get-FileHash $tokenPath).Hash -ne $tokenHash -or
            (Get-FileHash (Join-Path $state 'device.identity')).Hash -ne $identityHash) {
            throw "A locked $name changed persistent credentials or configuration."
        }
        if ((Get-Acl -LiteralPath $state).Sddl -ne $oldAcl) { throw 'Rollback did not restore the data directory ACL.' }
        if (-not (Test-Path (Join-Path $program 'previous-only.marker')) -or
            (Test-Path (Join-Path $program 'bin\candidate-only.marker'))) { throw 'Rollback did not restore the previous program tree.' }
        $service = Get-CimInstance Win32_Service -Filter "Name='LanLink'"
        if ($service.PathName -ne $servicePath -or $service.State -ne 'Stopped' -or $service.StartMode -ne 'Manual') {
            throw 'Rollback changed the previous service registration or startup mode.'
        }
        if ((Get-FileHash $shortcutPath).Hash -ne $shortcutHash) { throw 'Rollback changed the previous shortcut.' }
        if (@(Get-ChildItem -LiteralPath $root -Directory | Where-Object Name -Like 'program files.*-*').Count -ne 0) {
            throw 'Completed rollback left staging or backup directories.'
        }
        Write-Output "Locked ${name}: program, configuration, token, identity, ACL and service restored."
    }
} finally {
    if ($locked) { $locked.Dispose() }
    if (Test-Path (Join-Path $program 'installation.json')) {
        & (Join-Path $program 'Uninstall-LanLink.ps1') -InstallDirectory $program -RemoveData
    }
    Get-NetFirewallRule -Group LanLink -ErrorAction SilentlyContinue | Where-Object Name -Like '*-rollback-*' | Remove-NetFirewallRule
    if (Test-Path $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
