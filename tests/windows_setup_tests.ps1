param([Parameter(Mandatory)] [string] $Installer)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$expectedVersion = (Get-Content (Join-Path (Split-Path -Parent $PSScriptRoot) 'vcpkg.json') -Raw | ConvertFrom-Json).'version-string'
if (Get-Service -Name LanLink -ErrorAction SilentlyContinue) { throw 'Setup tests require an isolated machine.' }
$root = Join-Path $env:RUNNER_TEMP "lanlink-setup-$([Guid]::NewGuid().ToString('N'))"
$program = Join-Path $root 'program 한글'
$state = Join-Path $root 'state 한글'
New-Item -ItemType Directory -Path $root | Out-Null
$encoding = [Text.UTF8Encoding]::new($false)
$shortcut = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'LanLink.lnk'
$desktopShortcut = Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) 'LanLink.lnk'
$registry = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink'
function Assert-True([bool] $Condition, [string] $Message) { if (-not $Condition) { throw $Message } }
function Invoke-Setup([string[]] $Arguments, [bool] $Success = $true) {
    $information = [Diagnostics.ProcessStartInfo]::new([IO.Path]::GetFullPath($Installer))
    $information.UseShellExecute = $false
    foreach ($argument in @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', "/DIR=$program", "/LOG=$root/setup.log") + $Arguments) {
        $information.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($information)
    try {
        if (-not $process.WaitForExit(180000)) { $process.Kill($true); throw 'The EXE installer timed out.' }
        if (($process.ExitCode -eq 0) -ne $Success) {
            foreach ($log in @('setup.log.client.log', 'setup.log')) {
                if (Test-Path (Join-Path $root $log)) { Get-Content (Join-Path $root $log) -Tail 70 }
            }
            throw "Unexpected EXE installer exit code: $($process.ExitCode)"
        }
    } finally { $process.Dispose() }
}
function Invoke-Maintenance([string] $Mode, [switch] $RemoveData) {
    # Exercise the Installed Apps launcher and its cached copy of the same EXE.
    $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
        (Join-Path $program 'Maintenance-LanLink.ps1'), '-Mode', $Mode, '-Silent')
    if ($RemoveData) { $arguments += '-RemoveData' }
    & powershell.exe @arguments
    Assert-True ($LASTEXITCODE -eq 0) 'Installed maintenance launcher failed.'
}
function Assert-Preserved {
    foreach ($entry in $script:saved.GetEnumerator()) {
        Assert-True ((Get-FileHash -LiteralPath $entry.Key).Hash -eq $entry.Value) "Maintenance changed saved data: $($entry.Key)"
    }
}
try {
    Invoke-Setup @('/LANG=ko', "/DataDir=$state")
    $service = Get-CimInstance Win32_Service -Filter "Name='LanLink'"
    Assert-True ($service.State -eq 'Running' -and $service.StartMode -eq 'Auto') 'Fresh EXE must start an automatic service without a server.'
    $status = & (Join-Path $program 'bin\lanlink-ui-cli.exe') status
    Assert-True ($LASTEXITCODE -eq 0 -and ($status -join "`n").Contains('server configuration: unconfigured')) 'Unconfigured IPC status is incorrect.'
    Assert-True (-not (Test-Path (Join-Path $state 'auth.token'))) 'Setup must not require or create a relay token.'
    Assert-True (-not (Test-Path (Join-Path $state 'device.identity'))) 'Idle service must not load device credentials.'
    Assert-True (-not (Get-Content (Join-Path $state 'client.conf') -Raw).Contains('relay_host=')) 'Setup must not select a server.'
    $registration = Get-ItemProperty $registry
    Assert-True ($registration.NoModify -eq 0 -and $registration.ModifyPath.Contains('Maintenance-LanLink.ps1') -and $registration.UninstallString.Contains('-Mode Uninstall')) 'Installed Apps Modify/Uninstall are not connected.'
    Assert-True ((Get-FileHash $Installer).Hash -eq (Get-FileHash (Join-Path $program 'maintenance\LanLink-Setup.exe')).Hash) 'Maintenance must use the same Setup EXE.'
    $build = Get-Content (Join-Path $program 'build-info.json') -Raw | ConvertFrom-Json
    Assert-True ($build.version -eq $expectedVersion -and $build.source_commit -eq $env:GITHUB_SHA) 'EXE payload provenance differs from the CI commit.'
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LanLinkIconResources {
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    public static extern uint ExtractIconEx(string file, int index, IntPtr large, IntPtr small, uint count);
}
'@
    foreach ($name in 'lanlink-service.exe', 'lanlink-ui.exe', 'lanlink-ui-cli.exe') {
        $executable = Join-Path $program "bin\$name"
        Assert-True ([LanLinkIconResources]::ExtractIconEx($executable, -1, [IntPtr]::Zero, [IntPtr]::Zero, 0) -ge 1) "No embedded icon: $name"
        Assert-True ((Get-Item $executable).VersionInfo.FileVersion -eq $expectedVersion) "Incorrect version: $name"
    }
    Start-Process -FilePath $shortcut
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $ui = $null
    do {
        $ui = Get-Process -Name lanlink-ui -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $ui) { Start-Sleep -Milliseconds 100 }
    } while (-not $ui -and [DateTime]::UtcNow -lt $deadline)
    Assert-True ($null -ne $ui) 'Start menu shortcut did not launch the desktop.'
    try {
        Assert-True ($ui.Path -eq (Join-Path $program 'bin\lanlink-ui.exe') -and -not $ui.WaitForExit(1500)) 'Unconfigured desktop did not remain running.'
    } finally {
        if (-not $ui.HasExited) { $ui.Kill(); $ui.WaitForExit() }
        $ui.Dispose()
    }
    & (Join-Path $program 'bin\lanlink-service.exe') --stop
    Assert-True ($LASTEXITCODE -eq 0) 'Installed service could not stop.'
    # Connection data belongs to the app. Installer maintenance must preserve it.
    [IO.File]::WriteAllText((Join-Path $state 'auth.token'), 'preserve-device-credential-fixture-2026', $encoding)
    [IO.File]::WriteAllText((Join-Path $state 'device.identity'), 'preserve-identity-fixture', $encoding)
    [IO.File]::WriteAllText((Join-Path $state 'server-profiles.json'), '{"servers":[]}', $encoding)
    [IO.File]::WriteAllText((Join-Path $state 'settings.json'), '{"language":"ko"}', $encoding)
    $saved = @{}
    Get-ChildItem -LiteralPath $state -File | ForEach-Object { $saved[$_.FullName] = (Get-FileHash $_.FullName).Hash }
    Invoke-Setup @('/MODE=modify', '/Cli=0', '/StartMenu=0', '/Desktop=1', '/NoStart=1', '/LANG=en')
    Assert-True (-not (Test-Path (Join-Path $program 'bin\lanlink-ui-cli.exe')) -and -not (Test-Path $shortcut) -and (Test-Path $desktopShortcut)) 'Modify did not change the selected tools and shortcuts.'
    Assert-Preserved
    $dllHash = (Get-FileHash (Join-Path $program 'bin\wintun.dll')).Hash
    Remove-Item (Join-Path $program 'bin\wintun.dll') -Force
    Remove-Item (Join-Path $program 'bin\lanlink-ui.exe') -Force
    & (Join-Path $program 'bin\lanlink-service.exe') --uninstall
    Assert-True ($LASTEXITCODE -eq 0) 'Repair fixture could not remove the service.'
    Invoke-Maintenance 'Repair'
    Assert-True ((Get-Service LanLink).Status -eq 'Running') 'Repair did not restore the service.'
    Assert-True ((Get-FileHash (Join-Path $program 'bin\wintun.dll')).Hash -eq $dllHash -and (Test-Path (Join-Path $program 'bin\lanlink-ui.exe'))) 'Repair did not restore required files.'
    Assert-True (-not (Test-Path (Join-Path $program 'bin\lanlink-ui-cli.exe')) -and -not (Test-Path $shortcut) -and (Test-Path $desktopShortcut)) 'Repair changed selected components.'
    Assert-Preserved
    Invoke-Setup @('/MODE=modify', '/Cli=1', '/StartMenu=1', '/Desktop=0', '/NoStart=1')
    Assert-True ((Test-Path (Join-Path $program 'bin\lanlink-ui-cli.exe')) -and (Test-Path $shortcut) -and -not (Test-Path $desktopShortcut)) 'Modify did not restore optional components.'
    Invoke-Setup @('/MODE=install', '/NoStart=1')
    Assert-Preserved
    # Lock an installed file to force a real repair failure and check rollback.
    $locked = [IO.File]::Open((Join-Path $program 'bin\lanlink-ui.exe'), 'Open', 'Read', 'Read')
    try { Invoke-Setup @('/MODE=repair', '/NoStart=1') $false } finally { $locked.Dispose() }
    Assert-Preserved
    Assert-True ((Test-Path (Join-Path $program 'bin\wintun.dll')) -and (Get-Service LanLink).Status -eq 'Stopped') 'Failed repair did not restore the previous installation.'
    Invoke-Maintenance 'Uninstall'
    Assert-True (-not (Test-Path $program) -and -not (Test-Path $registry) -and -not (Get-Service LanLink -ErrorAction SilentlyContinue)) 'Uninstall did not remove the installation.'
    Assert-Preserved
    Invoke-Setup @('/MODE=install', "/DataDir=$state", '/NoStart=1')
    Assert-Preserved
    Invoke-Maintenance 'Uninstall' -RemoveData
    Assert-True (-not (Test-Path $state) -and -not (Test-Path $program) -and -not (Test-Path $shortcut)) 'Explicit data removal failed.'
    Write-Output 'Windows EXE: credential-free install/start/UI, Modify, cached Repair, file/service recovery, preservation, real failure rollback and Uninstall/purge passed.'
} finally {
    if (Test-Path (Join-Path $program 'installation.json')) {
        & (Join-Path $program 'Uninstall-LanLink.ps1') -InstallDirectory $program -RemoveData
    }
    Remove-Item -LiteralPath $root -Recurse -Force
}
