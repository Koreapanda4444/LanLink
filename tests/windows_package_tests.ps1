param([Parameter(Mandatory)] [string] $PackageDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
Import-Module (Join-Path $PackageDirectory 'ClientSetup.psm1') -Force
Assert-LanLinkAdministrator
if (Get-Service LanLink -ErrorAction SilentlyContinue) { throw 'The package test requires a machine without LanLink installed.' }

function Assert-True([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw $Message }
}
function Assert-Rejected([scriptblock] $Action) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Assert-True $rejected 'Invalid installer input was accepted.'
}

$root = Join-Path $env:RUNNER_TEMP "lanlink-package-$([Guid]::NewGuid().ToString('N'))"
$program = Join-Path $root 'program files'
$state = Join-Path $root 'state files'
$token = Join-Path $root 'token.txt'
$install = Join-Path $PackageDirectory 'Install-LanLink.ps1'
$uninstall = Join-Path $PackageDirectory 'Uninstall-LanLink.ps1'
$encoding = [Text.UTF8Encoding]::new($false)
New-Item -ItemType Directory -Path $root | Out-Null
try {
    Assert-Rejected { New-LanLinkClientConfig "relay.example`nrelay_port=1" 4433 $state }
    Assert-Rejected { Get-LanLinkFullPath 'C:\' }
    [IO.File]::WriteAllText($token, 'too-short', $encoding)
    Assert-Rejected { Get-LanLinkToken $token }
    [IO.File]::WriteAllBytes($token, [byte[]]@(128) * 32)
    Assert-Rejected { Get-LanLinkToken $token }
    [IO.File]::WriteAllText($token, "lanlink-package-test-token-0123456789`n", $encoding)
    & $install -RelayHost relay.example -AuthenticationTokenFile $token -InstallDirectory $program -DataDirectory $state -NoStart
    $service = Get-CimInstance Win32_Service -Filter "Name='LanLink'"
    Assert-True ($service.StartMode -eq 'Auto' -and $service.State -eq 'Stopped') 'Service must be installed for automatic start without running in this test.'
    Assert-True ($service.PathName.Contains('program files\bin\lanlink-service.exe')) 'The service executable path must be quoted and point at the package.'
    Assert-True ($service.PathName.Contains('state files\client.conf')) 'The service must use the persistent configuration.'
    $registration = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink'
    Assert-True ($registration.InstallLocation -eq $program -and $registration.UninstallString.Contains('Uninstall-LanLink.ps1')) 'The installation must appear in Windows installed apps.'
    $build = Get-Content (Join-Path $program 'build-info.json') -Raw | ConvertFrom-Json
    Assert-True ($build.version -eq '0.1.0' -and $build.source_clean -and $build.architecture -eq 'x64') 'Installed build metadata must identify the release.'
    Assert-True ((Test-Path (Join-Path $program 'INSTALL.md')) -and (Test-Path (Join-Path $program 'RELEASE-NOTES.md'))) 'Installation instructions and release notes must be preserved.'
    $config = [IO.File]::ReadAllBytes((Join-Path $state 'client.conf'))
    Assert-True ($config[0] -eq [byte][char]'r') 'The configuration must be UTF-8 without a BOM.'
    $userRules = @((Get-Acl $state).Access | Where-Object { $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -eq 'S-1-5-32-545' })
    Assert-True ($userRules.Count -eq 0) 'Persistent client credentials must not be readable by ordinary users.'
    $identity = Join-Path $state 'device.identity'
    [IO.File]::WriteAllText($identity, 'persistent-identity-fixture', $encoding)
    $configHash = (Get-FileHash (Join-Path $state 'client.conf')).Hash
    $tokenHash = (Get-FileHash (Join-Path $state 'auth.token')).Hash
    & $install -InstallDirectory $program -DataDirectory $state -NoStart
    Assert-True ((Get-FileHash (Join-Path $state 'client.conf')).Hash -eq $configHash) 'An upgrade must preserve configuration.'
    Assert-True ((Get-FileHash (Join-Path $state 'auth.token')).Hash -eq $tokenHash) 'An upgrade must preserve authentication tokens.'
    Assert-True ((Get-Content $identity -Raw) -eq 'persistent-identity-fixture') 'An upgrade must preserve device identity.'
    $rules = @(Get-NetFirewallRule -Group LanLink)
    Assert-True ($rules.Count -eq 3) 'Repeated installation must create exactly three firewall rules.'
    $filter = Get-NetFirewallRule -Name LanLink-Virtual-LAN | Get-NetFirewallAddressFilter
    Assert-True ($filter.RemoteAddress -contains '10.77.0.0/255.255.0.0' -or $filter.RemoteAddress -contains '10.77.0.0/16') 'Inbound access must be limited to the virtual address pool.'
    Assert-True ($filter.LocalAddress -contains '10.77.0.0/255.255.0.0' -or $filter.LocalAddress -contains '10.77.0.0/16') 'Inbound rules must target virtual LAN addresses before the adapter exists.'
    & $uninstall -InstallDirectory $program
    Assert-True (-not (Test-Path $program)) 'Uninstall must remove program files.'
    Assert-True (-not (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink')) 'Uninstall must remove its Windows registration.'
    Assert-True (Test-Path $identity) 'Uninstall must preserve data by default.'
    Assert-True (@(Get-NetFirewallRule -Group LanLink -ErrorAction SilentlyContinue).Count -eq 0) 'Uninstall must remove only the LanLink rules.'
    & $install -InstallDirectory $program -DataDirectory $state -NoStart
    & $uninstall -InstallDirectory $program -RemoveData
    Assert-True (-not (Test-Path $state)) 'Explicit data removal must remove persistent state.'
    Write-Output 'Windows package install, upgrade, firewall and uninstall scenarios passed.'
} finally {
    if (Test-Path (Join-Path $program 'installation.json')) { & $uninstall -InstallDirectory $program -RemoveData }
    if (Test-Path $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
