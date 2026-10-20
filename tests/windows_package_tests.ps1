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

function Get-DirectoryAclSnapshot([string] $Path) {
    $acl = Get-Acl -LiteralPath $Path
    $rules = @($acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) | ForEach-Object {
        [pscustomobject][ordered]@{
            identity = $_.IdentityReference.Value
            rights = [long] $_.FileSystemRights
            action = [int] $_.AccessControlType
            inheritance = [int] $_.InheritanceFlags
            propagation = [int] $_.PropagationFlags
            inherited = $_.IsInherited
        }
    } | Sort-Object identity, rights, action, inheritance, propagation, inherited)
    [pscustomobject][ordered]@{
        owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
        group = $acl.GetGroup([Security.Principal.SecurityIdentifier]).Value
        protected = $acl.AreAccessRulesProtected
        rules = $rules
    } | ConvertTo-Json -Depth 5 -Compress
}

$root = Join-Path $env:RUNNER_TEMP "lanlink-package-$([Guid]::NewGuid().ToString('N'))"
$program = Join-Path $root 'program files'
$state = Join-Path $root 'state files'
$token = Join-Path $root 'token.txt'
$install = Join-Path $PackageDirectory 'Install-LanLink.ps1'
$uninstall = Join-Path $PackageDirectory 'Uninstall-LanLink.ps1'
$encoding = [Text.UTF8Encoding]::new($false)
New-Item -ItemType Directory -Path $root | Out-Null
$shortcutPath = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'LanLink.lnk'
$registrationPath = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink'
if ((Test-Path $shortcutPath) -or (Test-Path $registrationPath)) { throw 'The package test requires no existing LanLink registration or shortcut.' }

function New-FailingPackage([string] $Name, [string] $Anchor) {
    $directory = Join-Path $root "failure-$Name"
    Copy-Item -LiteralPath $PackageDirectory -Destination $directory -Recurse
    $path = Join-Path $directory 'Install-LanLink.ps1'
    $source = [IO.File]::ReadAllText($path)
    $position = $source.IndexOf($Anchor, [StringComparison]::Ordinal)
    Assert-True ($position -ge 0) "Failure injection anchor is missing: $Name"
    $position += $Anchor.Length
    $source = $source.Insert($position, "`n    throw 'Injected package failure: $Name'`n")
    [IO.File]::WriteAllText($path, $source, $encoding)
    [IO.File]::WriteAllText((Join-Path $directory 'bin\candidate-only.marker'), 'candidate', $encoding)
    return $path
}

function Assert-InstallFailure([string] $Installer, [string] $Name, [string] $TokenFile) {
    $failure = $null
    try {
        & $Installer -RelayHost update.example -AuthenticationTokenFile $TokenFile `
            -InstallDirectory $program -DataDirectory $state -NoStart
    } catch { $failure = $_ }
    Assert-True ($null -ne $failure) "The injected $Name failure was not reached."
    Assert-True ($failure.Exception.Message -eq "Injected package failure: $Name") "Unexpected installer or rollback failure: $failure"
    Assert-True (@(Get-ChildItem -LiteralPath $root -Directory | Where-Object Name -Like 'program files.*-*').Count -eq 0) 'A failed transaction must not leave program staging or backup folders.'
    Assert-True (@(Get-NetFirewallRule -Group LanLink -ErrorAction SilentlyContinue | Where-Object Name -Like '*-rollback-*').Count -eq 0) 'A completed rollback must remove temporary firewall backups.'
}

try {
    Assert-Rejected { New-LanLinkClientConfig "relay.example`nrelay_port=1" 4433 $state }
    Assert-Rejected { Get-LanLinkFullPath 'C:\' }
    [IO.File]::WriteAllText($token, 'too-short', $encoding)
    Assert-Rejected { Get-LanLinkToken $token }
    [IO.File]::WriteAllBytes($token, [byte[]]@(128) * 32)
    Assert-Rejected { Get-LanLinkToken $token }
    [IO.File]::WriteAllText($token, "lanlink-package-test-token-0123456789`n", $encoding)
    $serviceFailure = New-FailingPackage 'service' "try { Invoke-LanLinkServiceCommand `$executable @('--install', `$configPath) } finally { Pop-Location }"
    Assert-InstallFailure $serviceFailure 'service' $token
    Assert-True (-not (Test-Path $program) -and -not (Test-Path $state)) 'A first-install failure must remove newly created program and state folders.'
    Assert-True (-not (Get-CimInstance Win32_Service -Filter "Name='LanLink'")) 'A first-install failure must remove a newly registered service.'
    Assert-True (-not (Test-Path $registrationPath) -and -not (Test-Path $shortcutPath)) 'A first-install failure must not leave registration or shortcuts.'

    New-Item -ItemType Directory -Path $state | Out-Null
    [IO.File]::WriteAllText((Join-Path $state 'client.conf'), "relay_host=retained.example`n", $encoding)
    [IO.File]::WriteAllText((Join-Path $state 'auth.token'), 'retained-token-01234567890123456789', $encoding)
    [IO.File]::WriteAllText((Join-Path $state 'device.identity'), 'persistent-identity-fixture', $encoding)
    $stateAcl = Get-DirectoryAclSnapshot $state
    $originalConfig = (Get-FileHash (Join-Path $state 'client.conf')).Hash
    $originalToken = (Get-FileHash (Join-Path $state 'auth.token')).Hash
    [IO.File]::WriteAllText($shortcutPath, 'preexisting-shortcut-fixture', $encoding)
    New-Item -Path $registrationPath -Force | Out-Null
    New-ItemProperty -Path $registrationPath -Name DisplayName -Value 'Preexisting registration fixture' -Force | Out-Null
    New-ItemProperty -Path $registrationPath -Name Fixture -Value 23 -PropertyType DWord -Force | Out-Null
    $registrationFailure = New-FailingPackage 'registration' 'Register-LanLinkInstallation $InstallDirectory $version'
    Assert-InstallFailure $registrationFailure 'registration' $token
    Assert-True (-not (Test-Path $program) -and -not (Get-CimInstance Win32_Service -Filter "Name='LanLink'")) 'Late first-install failure must remove programs and service.'
    $restoredAcl = Get-DirectoryAclSnapshot $state
    Assert-True ($restoredAcl -eq $stateAcl) "Rollback must restore the previous directory owner, group, permissions and inheritance. Before: $stateAcl After: $restoredAcl"
    Assert-True ((Get-FileHash (Join-Path $state 'client.conf')).Hash -eq $originalConfig -and (Get-FileHash (Join-Path $state 'auth.token')).Hash -eq $originalToken) 'Rollback must restore existing configuration and token bytes.'
    Assert-True ((Get-Content $shortcutPath -Raw) -eq 'preexisting-shortcut-fixture') 'Rollback must restore a preexisting shortcut exactly.'
    $restored = Get-ItemProperty $registrationPath
    Assert-True ($restored.DisplayName -eq 'Preexisting registration fixture' -and $restored.Fixture -eq 23) 'Rollback must restore preexisting registry values and their types.'
    Assert-True (@(Get-NetFirewallRule -Group LanLink -ErrorAction SilentlyContinue).Count -eq 0) 'Late first-install failure must remove the new firewall rules.'
    Remove-Item -LiteralPath $shortcutPath -Force
    Remove-Item -LiteralPath $registrationPath -Recurse -Force

    & $install -RelayHost relay.example -AuthenticationTokenFile $token -InstallDirectory $program -DataDirectory $state -NoStart
    $service = Get-CimInstance Win32_Service -Filter "Name='LanLink'"
    Assert-True ($service.StartMode -eq 'Auto' -and $service.State -eq 'Stopped') 'Service must be installed for automatic start without running in this test.'
    Assert-True ($service.PathName.Contains('program files\bin\lanlink-service.exe')) 'The service executable path must be quoted and point at the package.'
    Assert-True ($service.PathName.Contains('state files\client.conf')) 'The service must use the persistent configuration.'
    $registration = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink'
    Assert-True ($registration.InstallLocation -eq $program -and $registration.UninstallString.Contains('Uninstall-LanLink.ps1')) 'The installation must appear in Windows installed apps.'
    $build = Get-Content (Join-Path $program 'build-info.json') -Raw | ConvertFrom-Json
    Assert-True ($build.version -eq '0.2.1' -and $build.source_clean -and $build.architecture -eq 'x64') 'Installed build metadata must identify the release.'
    Assert-True ((Test-Path (Join-Path $program 'INSTALL.md')) -and (Test-Path (Join-Path $program 'RELEASE-NOTES.md'))) 'Installation instructions and release notes must be preserved.'
    $config = [IO.File]::ReadAllBytes((Join-Path $state 'client.conf'))
    Assert-True ($config[0] -eq [byte][char]'r') 'The configuration must be UTF-8 without a BOM.'
    $userRules = @((Get-Acl $state).Access | Where-Object { $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -eq 'S-1-5-32-545' })
    Assert-True ($userRules.Count -eq 0) 'Persistent client credentials must not be readable by ordinary users.'
    $identity = Join-Path $state 'device.identity'
    [IO.File]::WriteAllText($identity, 'persistent-identity-fixture', $encoding)
    $configHash = (Get-FileHash (Join-Path $state 'client.conf')).Hash
    $tokenHash = (Get-FileHash (Join-Path $state 'auth.token')).Hash
    $oldServicePath = $service.PathName
    [IO.File]::WriteAllText((Join-Path $program 'previous-only.marker'), 'previous', $encoding)
    Set-NetFirewallRule -Name LanLink-Client-UDP -Enabled False -Description 'Preserve custom firewall settings' | Out-Null
    Set-ItemProperty -Path $registrationPath -Name DisplayVersion -Value '0.0.7-fixture'
    New-ItemProperty -Path $registrationPath -Name Fixture -Value 29 -PropertyType DWord -Force | Out-Null
    $shortcutHash = (Get-FileHash $shortcutPath).Hash
    $replacementToken = Join-Path $root 'replacement.token'
    [IO.File]::WriteAllText($replacementToken, 'replacement-token-01234567890123456789', $encoding)
    $backupFailure = New-FailingPackage 'backup' '$programBackedUp = $true'
    foreach ($case in @(@{ path = $backupFailure; name = 'backup' }, @{ path = $registrationFailure; name = 'registration' })) {
        Assert-InstallFailure $case.path $case.name $replacementToken
        Assert-True ((Test-Path (Join-Path $program 'previous-only.marker')) -and -not (Test-Path (Join-Path $program 'bin\candidate-only.marker'))) 'Failed upgrade must restore the previous program tree even when placement of the replacement has not begun.'
        Assert-True ((Get-FileHash (Join-Path $state 'client.conf')).Hash -eq $configHash -and (Get-FileHash (Join-Path $state 'auth.token')).Hash -eq $tokenHash) 'Failed upgrade must restore credentials and configuration.'
        Assert-True ((Get-Content $identity -Raw) -eq 'persistent-identity-fixture') 'Failed upgrade must preserve device identity.'
        Assert-True ((Get-FileHash $shortcutPath).Hash -eq $shortcutHash) 'Failed upgrade must restore the original shortcut.'
        $restoredService = Get-CimInstance Win32_Service -Filter "Name='LanLink'"
        Assert-True ($restoredService.PathName -eq $oldServicePath -and $restoredService.State -eq 'Stopped' -and $restoredService.StartMode -eq 'Auto') 'Failed upgrade must restore service configuration and state.'
        $restoredRegistration = Get-ItemProperty $registrationPath
        Assert-True ($restoredRegistration.DisplayVersion -eq '0.0.7-fixture' -and $restoredRegistration.Fixture -eq 29) 'Failed upgrade must restore the previous uninstall registration.'
        $udpRule = Get-NetFirewallRule -Name LanLink-Client-UDP
        Assert-True ($udpRule.Enabled -eq 'False' -and $udpRule.Description -eq 'Preserve custom firewall settings') 'Failed upgrade must restore customized firewall rules.'
        Assert-True (@(Get-NetFirewallRule -Group LanLink).Count -eq 3) 'Failed upgrade must retain exactly the original firewall rules.'
    }
    & $install -InstallDirectory $program -DataDirectory $state -NoStart
    Assert-True ((Get-FileHash (Join-Path $state 'client.conf')).Hash -eq $configHash) 'An upgrade must preserve configuration.'
    Assert-True ((Get-FileHash (Join-Path $state 'auth.token')).Hash -eq $tokenHash) 'An upgrade must preserve authentication tokens.'
    Assert-True ((Get-Content $identity -Raw) -eq 'persistent-identity-fixture') 'An upgrade must preserve device identity.'
    $rules = @(Get-NetFirewallRule -Group LanLink)
    Assert-True ($rules.Count -eq 3) 'Repeated installation must create exactly three firewall rules.'
    $filter = Get-NetFirewallRule -Name LanLink-Virtual-LAN | Get-NetFirewallAddressFilter
    Assert-True ($filter.RemoteAddress -contains '10.64.0.0/255.192.0.0' -or $filter.RemoteAddress -contains '10.64.0.0/10') 'Inbound access must be limited to the virtual address pool.'
    Assert-True ($filter.LocalAddress -contains '10.64.0.0/255.192.0.0' -or $filter.LocalAddress -contains '10.64.0.0/10') 'Inbound rules must target virtual LAN addresses before the adapter exists.'
    & $uninstall -InstallDirectory $program
    Assert-True (-not (Test-Path $program)) 'Uninstall must remove program files.'
    Assert-True (-not (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink')) 'Uninstall must remove its Windows registration.'
    Assert-True (Test-Path $identity) 'Uninstall must preserve data by default.'
    Assert-True (@(Get-NetFirewallRule -Group LanLink -ErrorAction SilentlyContinue).Count -eq 0) 'Uninstall must remove only the LanLink rules.'
    & $install -InstallDirectory $program -DataDirectory $state -NoStart
    & $uninstall -InstallDirectory $program -RemoveData
    Assert-True (-not (Test-Path $state)) 'Explicit data removal must remove persistent state.'
    Write-Output 'Windows package install, first-install rollback, upgrade rollback, firewall and uninstall scenarios passed.'
} finally {
    if (Test-Path (Join-Path $program 'installation.json')) { & $uninstall -InstallDirectory $program -RemoveData }
    if (Test-Path $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
