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

function Set-LanLinkFirewallRules([string] $Executable) {
    foreach ($protocol in 'TCP', 'UDP') {
        $name = "LanLink-Client-$protocol"
        Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        New-NetFirewallRule -Name $name -DisplayName "LanLink client $protocol" -Group 'LanLink' `
            -Direction Outbound -Action Allow -Program $Executable -Protocol $protocol -Profile Any | Out-Null
    }
    Get-NetFirewallRule -Name 'LanLink-Virtual-LAN' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -Name 'LanLink-Virtual-LAN' -DisplayName 'LanLink virtual LAN' -Group 'LanLink' `
        -Direction Inbound -Action Allow -InterfaceAlias 'LanLink' -RemoteAddress '10.77.0.0/16' `
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
