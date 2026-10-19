Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-LanLinkAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this installer from an administrator PowerShell session.'
    }
}

function Get-LanLinkFullPath([string] $Path) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    if ($full.Length -le [IO.Path]::GetPathRoot($full).Length -or $full -match '[\r\n]') {
        throw 'An installation directory cannot be a drive root or contain newlines.'
    }
    return $full
}

function Set-LanLinkDirectoryAcl([string] $Path, [switch] $AllowUsers) {
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true, $false)
    $inherit = [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
    foreach ($sid in 'S-1-5-18', 'S-1-5-32-544') {
        $rule = [Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.SecurityIdentifier]::new($sid), 'FullControl', $inherit,
            [Security.AccessControl.PropagationFlags]::None, 'Allow')
        $acl.AddAccessRule($rule)
    }
    if ($AllowUsers) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.SecurityIdentifier]::new('S-1-5-32-545'), 'ReadAndExecute',
            $inherit, [Security.AccessControl.PropagationFlags]::None, 'Allow'))
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}

function Get-LanLinkToken([string] $Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -gt 4096) { throw 'Authentication token exceeds 4096 bytes.' }
    $length = $bytes.Length
    while ($length -gt 0 -and $bytes[$length - 1] -in 10, 13) { $length-- }
    if ($length -lt 32 -or @($bytes[0..($length - 1)] | Where-Object { $_ -lt 33 -or $_ -gt 126 }).Count -gt 0) {
        throw 'Authentication token must contain at least 32 printable non-space ASCII characters.'
    }
    return [Text.Encoding]::ASCII.GetString($bytes, 0, $length)
}

function New-LanLinkClientConfig([string] $RelayHost, [int] $RelayPort, [string] $DataDirectory) {
    if ([string]::IsNullOrWhiteSpace($RelayHost) -or $RelayHost -match '[\s=#/\\]' -or
        $RelayPort -lt 1 -or $RelayPort -gt 65535) {
        throw 'A relay hostname or IP address and port between 1 and 65535 are required.'
    }
    return @(
        "relay_host=$RelayHost"
        "relay_port=$RelayPort"
        "device_identity_file=$DataDirectory\device.identity"
        "auth_token_file=$DataDirectory\auth.token"
        "log_directory=$DataDirectory\logs"
    ) -join "`n"
}

function Invoke-LanLinkServiceCommand([string] $Executable, [string[]] $Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "LanLink service command failed with exit code $LASTEXITCODE." }
}

function Stop-LanLinkService {
    $service = Get-Service -Name LanLink -ErrorAction SilentlyContinue
    if (-not $service) { return }
    try {
        if ($service.Status -ne 'Stopped') {
            Stop-Service -Name LanLink -ErrorAction Stop
            $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
        }
    } finally { $service.Dispose() }
}

function Remove-LanLinkService {
    Stop-LanLinkService
    $service = Get-Service -Name LanLink -ErrorAction SilentlyContinue
    if (-not $service) { return }
    $service.Dispose()
    & "$env:SystemRoot\System32\sc.exe" delete LanLink | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot remove the failed LanLink service installation.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $remaining = Get-Service -Name LanLink -ErrorAction SilentlyContinue
        if (-not $remaining) { return }
        $remaining.Dispose()
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Timed out removing the failed LanLink service installation.' }
        Start-Sleep -Milliseconds 100
    } while ($true)
}

function Get-LanLinkRegistrationSnapshot {
    $path = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink'
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $key = Get-Item -LiteralPath $path
    try {
        $values = @{}
        foreach ($name in $key.GetValueNames()) {
            $values[$name] = @{
                value = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                kind = $key.GetValueKind($name)
            }
        }
        return $values
    } finally { $key.Close() }
}

function Restore-LanLinkRegistration($Snapshot) {
    $path = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink'
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    if ($null -eq $Snapshot) { return }
    $key = New-Item -Path $path -Force
    try {
        foreach ($entry in $Snapshot.GetEnumerator()) {
            $key.SetValue($entry.Key, $entry.Value.value, $entry.Value.kind)
        }
    } finally { $key.Close() }
}

function Backup-LanLinkFirewallRules {
    $backups = [Collections.Generic.List[object]]::new()
    try {
        foreach ($name in 'LanLink-Client-TCP', 'LanLink-Client-UDP', 'LanLink-Virtual-LAN') {
            $rule = Get-NetFirewallRule -Name $name -PolicyStore PersistentStore -ErrorAction SilentlyContinue
            if (-not $rule) { continue }
            $backupName = "$name-rollback-$([Guid]::NewGuid().ToString('N'))"
            $backups.Add(@{ name = $name; backup = $backupName; enabled = $rule.Enabled })
            Copy-NetFirewallRule -Name $name -PolicyStore PersistentStore -NewName $backupName | Out-Null
            Disable-NetFirewallRule -Name $backupName -PolicyStore PersistentStore | Out-Null
        }
        return ,$backups
    } catch {
        foreach ($entry in $backups) {
            Get-NetFirewallRule -Name $entry.backup -PolicyStore PersistentStore -ErrorAction SilentlyContinue |
                Remove-NetFirewallRule
        }
        throw
    }
}

function Restore-LanLinkFirewallRules($Backups) {
    Remove-LanLinkFirewallRules
    foreach ($entry in $Backups) {
        Copy-NetFirewallRule -Name $entry.backup -PolicyStore PersistentStore -NewName $entry.name | Out-Null
        Set-NetFirewallRule -Name $entry.name -PolicyStore PersistentStore -Enabled $entry.enabled | Out-Null
    }
}

function Remove-LanLinkFirewallBackups($Backups) {
    foreach ($entry in $Backups) {
        Get-NetFirewallRule -Name $entry.backup -PolicyStore PersistentStore -ErrorAction SilentlyContinue |
            Remove-NetFirewallRule
    }
}

function Set-LanLinkFirewallRules([string] $Executable) {
    foreach ($protocol in 'TCP', 'UDP') {
        $name = "LanLink-Client-$protocol"
        Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        New-NetFirewallRule -Name $name -DisplayName "LanLink client $protocol" -Group 'LanLink' `
            -Direction Outbound -Action Allow -Program $Executable -Protocol $protocol -Profile Any | Out-Null
    }
    Get-NetFirewallRule -Name 'LanLink-Virtual-LAN' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -Name 'LanLink-Virtual-LAN' -DisplayName 'LanLink virtual LAN' -Group 'LanLink' `
        -Direction Inbound -Action Allow -LocalAddress '10.64.0.0/10' -RemoteAddress '10.64.0.0/10' `
        -Protocol Any -Profile Any | Out-Null
}

function Remove-LanLinkFirewallRules {
    foreach ($name in 'LanLink-Client-TCP', 'LanLink-Client-UDP', 'LanLink-Virtual-LAN') {
        Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    }
}

function Register-LanLinkInstallation([string] $Directory, [string] $Version) {
    $key = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink'
    New-Item -Path $key -Force | Out-Null
    $command = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' +
        (Join-Path $Directory 'Uninstall-LanLink.ps1') + '" -InstallDirectory "' + $Directory + '"'
    foreach ($entry in @{
        DisplayName = 'LanLink'; DisplayVersion = $Version; Publisher = 'Koreapanda4444'
        InstallLocation = $Directory; UninstallString = $command
        DataDirectory = (Get-Content -LiteralPath (Join-Path $Directory 'installation.json') -Raw -Encoding UTF8 | ConvertFrom-Json).data_directory
        DisplayIcon = (Join-Path $Directory 'bin\lanlink-ui.exe')
    }.GetEnumerator()) {
        New-ItemProperty -Path $key -Name $entry.Key -Value $entry.Value -PropertyType String -Force | Out-Null
    }
    New-ItemProperty -Path $key -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $key -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null
}

function Unregister-LanLinkInstallation([string] $Directory) {
    $key = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink'
    if (Test-Path $key) {
        if ((Get-ItemProperty $key).InstallLocation -ne $Directory) {
            throw 'The registered uninstall path does not match this installation.'
        }
        Remove-Item -Path $key -Recurse -Force
    }
}

Export-ModuleMember -Function *-LanLink*
