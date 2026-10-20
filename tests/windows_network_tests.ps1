param(
    [Parameter(Mandatory)] [string] $PackageDirectory,
    [Parameter(Mandatory)] [string] $RelayExecutable,
    [Parameter(Mandatory)] [string] $DriverExecutable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'The Windows integration harness requires PowerShell 7.' }
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
$RelayExecutable = [IO.Path]::GetFullPath($RelayExecutable)
$DriverExecutable = [IO.Path]::GetFullPath($DriverExecutable)
Import-Module (Join-Path $PackageDirectory 'ClientSetup.psm1') -Force
Assert-LanLinkAdministrator
if (Get-CimInstance Win32_Service -Filter "Name='LanLink'") { throw 'Run the integration harness on an isolated machine without LanLink installed.' }
if (Get-NetAdapter -Name LanLink -ErrorAction SilentlyContinue) { throw 'An existing LanLink adapter must not be used by the integration harness.' }

$root = Join-Path $env:RUNNER_TEMP "lanlink-windows-network-$([Guid]::NewGuid().ToString('N'))"
$encoding = [Text.UTF8Encoding]::new($false)
$certificates = [Collections.Generic.List[string]]::new()
$trusted = $null
$relay = $null
$driver = $null
$installed = $null
$passed = $false
$firewallName = "LanLink-E2E-$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null

function Write-Pem([string] $Path, [string] $Label, [byte[]] $Bytes, [switch] $Append) {
    $base64 = [Convert]::ToBase64String($Bytes)
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add("-----BEGIN $Label-----")
    for ($offset = 0; $offset -lt $base64.Length; $offset += 64) {
        $lines.Add($base64.Substring($offset, [Math]::Min(64, $base64.Length - $offset)))
    }
    $lines.Add("-----END $Label-----")
    $text = ($lines -join "`n") + "`n"
    if ($Append) { [IO.File]::AppendAllText($Path, $text, $encoding) }
    else { [IO.File]::WriteAllText($Path, $text, $encoding) }
}

function Get-FreePort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try { return $listener.LocalEndpoint.Port } finally { $listener.Stop() }
}

function Start-Child([string] $Executable, [string[]] $Arguments) {
    $information = [Diagnostics.ProcessStartInfo]::new($Executable)
    $information.UseShellExecute = $false
    $information.RedirectStandardOutput = $true
    $information.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $information.ArgumentList.Add($argument) }
    return [Diagnostics.Process]::Start($information)
}

function Stop-Child($Process) {
    if ($null -eq $Process) { return }
    if (-not $Process.HasExited) { $Process.Kill($true) }
    if (-not $Process.WaitForExit(10000)) { throw 'Cannot stop the integration child process.' }
}

function Start-Relay([string] $Configuration, [int] $Port) {
    $process = Start-Child $RelayExecutable @($Configuration)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    try {
        while ([DateTime]::UtcNow -lt $deadline) {
            if ($process.HasExited) { throw "Relay startup failed: $($process.StandardError.ReadToEnd())" }
            $probe = [Net.Sockets.TcpClient]::new()
            try {
                $probe.Connect('127.0.0.1', $Port)
                return $process
            } catch { Start-Sleep -Milliseconds 50 }
            finally { $probe.Dispose() }
        }
        throw 'Relay startup timed out.'
    } catch { Stop-Child $process; throw }
}

function Invoke-Driver([string[]] $Arguments) {
    & $DriverExecutable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Native Windows integration helper failed: $Arguments" }
}

try {
    $authority = New-SelfSignedCertificate -Type Custom -Subject "CN=LanLink temporary authority $([Guid]::NewGuid())" `
        -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 `
        -Provider 'Microsoft Software Key Storage Provider' -KeyExportPolicy Exportable `
        -KeyUsage CertSign, CRLSign -TextExtension @('2.5.29.19={critical}{text}ca=1&pathlength=1') `
        -NotAfter (Get-Date).AddDays(1)
    $certificates.Add($authority.Thumbprint)
    $certificate = New-SelfSignedCertificate -Type Custom -Subject 'CN=LanLink loopback relay' -Signer $authority `
        -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 `
        -Provider 'Microsoft Software Key Storage Provider' -KeyExportPolicy Exportable `
        -KeyUsage DigitalSignature, KeyEncipherment `
        -TextExtension @('2.5.29.19={critical}{text}ca=0', '2.5.29.17={text}IPAddress=127.0.0.1', '2.5.29.37={text}1.3.6.1.5.5.7.3.1') `
        -NotAfter (Get-Date).AddDays(1)
    $certificates.Add($certificate.Thumbprint)
    [IO.File]::WriteAllBytes((Join-Path $root 'authority.cer'), $authority.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    Write-Pem (Join-Path $root 'server.cert') 'CERTIFICATE' $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert)
    Write-Pem (Join-Path $root 'server.cert') 'CERTIFICATE' $authority.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert) -Append
    $key = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($certificate)
    try { Write-Pem (Join-Path $root 'server.key') 'PRIVATE KEY' $key.ExportPkcs8PrivateKey() }
    finally { $key.Dispose() }
    [IO.File]::WriteAllText((Join-Path $root 'auth.token'), "lanlink-windows-loopback-integration-token-2026`n", $encoding)
    New-NetFirewallRule -Name $firewallName -DisplayName 'LanLink isolated integration peer' -Group 'LanLink-E2E' `
        -Direction Inbound -Action Allow -Program $DriverExecutable -LocalAddress '10.254.253.2' `
        -RemoteAddress '10.254.253.1' -Protocol Any -Profile Any | Out-Null

    foreach ($mode in 'quic', 'tcp') {
        $scenario = Join-Path $root $mode
        $program = Join-Path $scenario 'program files'
        $state = Join-Path $scenario 'state files'
        New-Item -ItemType Directory -Path $scenario | Out-Null
        Copy-Item -LiteralPath (Join-Path $root 'auth.token') -Destination $scenario
        $port = Get-FreePort
        $clientPort = if ($mode -eq 'tcp') { Get-FreePort } else { $port }
        $configuration = Join-Path $scenario 'relay.conf'
        $configurationText = @(
            'listen_host=127.0.0.1', "relay_port=$port"
            "tls_certificate_file=$(Join-Path $root 'server.cert')"
            "tls_private_key_file=$(Join-Path $root 'server.key')"
            "auth_token_file=$(Join-Path $root 'auth.token')"
            "relay_database_file=$(Join-Path $scenario 'relay.db')"
            "log_directory=$(Join-Path $scenario 'relay-logs')"
        ) -join "`n"
        [IO.File]::WriteAllText($configuration, $configurationText, $encoding)
        $relay = Start-Relay $configuration $port
        if ($mode -eq 'quic') {
            Invoke-Driver @('--tls-probe', '127.0.0.1', "$port", 'reject')
            $trusted = (Import-Certificate -FilePath (Join-Path $root 'authority.cer') -CertStoreLocation Cert:\LocalMachine\Root).Thumbprint
        }
        Invoke-Driver @('--tls-probe', '127.0.0.1', "$port", 'accept')
        Invoke-Driver @('--tls-probe', 'localhost', "$port", 'reject')
        & (Join-Path $PackageDirectory 'Install-LanLink.ps1') -RelayHost 127.0.0.1 -RelayPort $clientPort `
            -AuthenticationTokenFile (Join-Path $root 'auth.token') -InstallDirectory $program -DataDirectory $state -NoStart
        $installed = $program
        [IO.File]::AppendAllText((Join-Path $state 'client.conf'), "`nquic_handshake_timeout_ms=1000`nquic_idle_timeout_ms=5000`nquic_keep_alive_interval_ms=1000`nreconnect_initial_delay_ms=100`nreconnect_max_delay_ms=500`n", $encoding)
        & (Join-Path $program 'bin\lanlink-service.exe') --start
        if ($LASTEXITCODE -ne 0) { throw 'The installed Windows service did not start.' }
        $faultyPackage = Join-Path $scenario 'failed-upgrade'
        Copy-Item -LiteralPath $PackageDirectory -Destination $faultyPackage -Recurse
        $faultyInstaller = Join-Path $faultyPackage 'Install-LanLink.ps1'
        $source = [IO.File]::ReadAllText($faultyInstaller)
        $anchor = 'Register-LanLinkInstallation $InstallDirectory $version'
        if (-not $source.Contains($anchor)) { throw 'The running-upgrade failure anchor is missing.' }
        $source = $source.Replace($anchor, $anchor + "`n    throw 'Injected running upgrade failure'")
        [IO.File]::WriteAllText($faultyInstaller, $source, $encoding)
        $replacementToken = Join-Path $scenario 'replacement.token'
        [IO.File]::WriteAllText($replacementToken, 'lanlink-live-rollback-replacement-token-2026', $encoding)
        $driver = Start-Child $DriverExecutable @($scenario, "$clientPort", $mode, "$port")
        $deadline = [DateTime]::UtcNow.AddSeconds(180)
        $completed = 0
        while (-not $driver.HasExited) {
            if ([DateTime]::UtcNow -ge $deadline) { throw "The $mode Windows network scenario timed out." }
            $request = Join-Path $scenario "request-$($completed + 1)"
            if (Test-Path -LiteralPath $request) {
                $parts = ([IO.File]::ReadAllText($request)).Split(' ')
                if ([int] $parts[0] -gt $completed) {
                    switch ($parts[1]) {
                        'service-stop' {
                            if ($mode -eq 'quic') {
                                $capture = Join-Path $env:RUNNER_TEMP 'lanlink-desktop-connected.png'
                                $ui = Start-Child (Join-Path $program 'bin\lanlink-ui.exe') @('--screenshot', $capture)
                                try {
                                    if (-not $ui.WaitForExit(20000)) { Stop-Child $ui; throw 'Desktop capture timed out.' }
                                    if ($ui.ExitCode -ne 0 -or -not (Test-Path $capture)) {
                                        $detail = if (Test-Path ($capture + '.log')) { Get-Content ($capture + '.log') -Raw } else { $ui.StandardError.ReadToEnd() }
                                        throw "The installed desktop UI did not render: $detail"
                                    }
                                    Write-Output '::group::Desktop rendering capture'
                                    Write-Output ("LANLINK_DESKTOP_PNG=" + [Convert]::ToBase64String([IO.File]::ReadAllBytes($capture)))
                                    Write-Output '::endgroup::'
                                } finally { $ui.Dispose() }
                            }
                            Invoke-LanLinkServiceCommand (Join-Path $program 'bin\lanlink-service.exe') @('--stop')
                        }
                        'service-start' { Invoke-LanLinkServiceCommand (Join-Path $program 'bin\lanlink-service.exe') @('--start') }
                        'relay-stop' { Stop-Child $relay; $relay.Dispose(); $relay = $null }
                        'relay-start' { $relay = Start-Relay $configuration $port }
                        'upgrade-failure' {
                            $identityHash = (Get-FileHash (Join-Path $state 'device.identity')).Hash
                            $tokenHash = (Get-FileHash (Join-Path $state 'auth.token')).Hash
                            $caught = $null
                            try { & $faultyInstaller -InstallDirectory $program -DataDirectory $state -NoStart }
                            catch { $caught = $_ }
                            if ($null -eq $caught -or $caught.Exception.Message -ne 'Injected running upgrade failure') { throw "Unexpected running-upgrade rollback result: $caught" }
                            if ((Get-CimInstance Win32_Service -Filter "Name='LanLink'").State -ne 'Running') { throw 'Rollback did not restart the previously running service.' }
                            if ((Get-FileHash (Join-Path $state 'device.identity')).Hash -ne $identityHash -or
                                (Get-FileHash (Join-Path $state 'auth.token')).Hash -ne $tokenHash) { throw 'Rollback changed the live client credentials.' }
                        }
                        { $_ -like 'upgrade-locked-*' } {
                            $name = if ($parts[1] -eq 'upgrade-locked-config') { 'client.conf' } else { 'auth.token' }
                            $identityHash = (Get-FileHash (Join-Path $state 'device.identity')).Hash
                            $tokenHash = (Get-FileHash (Join-Path $state 'auth.token')).Hash
                            $configHash = (Get-FileHash (Join-Path $state 'client.conf')).Hash
                            $acl = Get-Acl -LiteralPath $state
                            $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
                                [Security.Principal.WindowsIdentity]::GetCurrent().User, 'ReadAndExecute', 'Allow'))
                            Set-Acl -LiteralPath $state -AclObject $acl
                            $oldAcl = (Get-Acl -LiteralPath $state).Sddl
                            $locked = [IO.File]::Open((Join-Path $state $name), [IO.FileMode]::Open,
                                [IO.FileAccess]::Read, [IO.FileShare]::Read)
                            $caught = $null
                            try {
                                & (Join-Path $PackageDirectory 'Install-LanLink.ps1') -RelayHost replacement.example `
                                    -AuthenticationTokenFile $replacementToken -InstallDirectory $program -DataDirectory $state -NoStart
                            } catch { $caught = $_ }
                            finally { $locked.Dispose() }
                            if ($null -eq $caught -or $caught.Exception.Message.Contains('rollback was incomplete')) { throw "Unexpected locked-file rollback result: $caught" }
                            if ($caught.Exception.GetBaseException().HResult -ne -2147024864) { throw "The running upgrade did not reach the expected file-sharing violation: $caught" }
                            if ((Get-CimInstance Win32_Service -Filter "Name='LanLink'").State -ne 'Running') { throw 'Locked-file rollback did not restart the previously running service.' }
                            if ((Get-FileHash (Join-Path $state 'device.identity')).Hash -ne $identityHash -or
                                (Get-FileHash (Join-Path $state 'auth.token')).Hash -ne $tokenHash -or
                                (Get-FileHash (Join-Path $state 'client.conf')).Hash -ne $configHash -or
                                (Get-Acl -LiteralPath $state).Sddl -ne $oldAcl) { throw 'Locked-file rollback changed the live client configuration, credentials or ACL.' }
                            Write-Output "$mode running upgrade with locked ${name}: previous service restored."
                        }
                        default { throw 'Invalid integration process control command.' }
                    }
                    $completed = [int] $parts[0]
                    [IO.File]::WriteAllText((Join-Path $scenario 'completed'), "$completed", $encoding)
                }
            }
            if ($null -ne $relay -and $relay.HasExited) { throw "Relay exited unexpectedly: $($relay.StandardError.ReadToEnd())" }
            Start-Sleep -Milliseconds 25
        }
        $driver.WaitForExit()
        $output = $driver.StandardOutput.ReadToEnd()
        $errors = $driver.StandardError.ReadToEnd()
        [IO.File]::WriteAllText((Join-Path $scenario 'driver.log'), $output + $errors, $encoding)
        Write-Output $output
        if ($driver.ExitCode -ne 0) { throw "Native $mode Windows scenario failed: $errors" }
        if ($completed -ne 8) { throw 'The Windows restart and running-upgrade recovery scenarios were incomplete.' }
        $driver.Dispose()
        $driver = $null
        & (Join-Path $program 'Uninstall-LanLink.ps1') -InstallDirectory $program -RemoveData
        $installed = $null
        Stop-Child $relay
        $relay.Dispose()
        $relay = $null
    }
    $passed = $true
    Write-Output 'Real Windows service, two Wintun adapters, kernel TCP/UDP, QUIC/TCP fallback, certificate validation and restart scenarios passed.'
} catch {
    try { Stop-LanLinkService } catch { Write-Warning "Integration service stop failed: $_" }
    Get-ChildItem -LiteralPath $root -Filter '*.log' -File -Recurse | ForEach-Object {
        Write-Output "Integration diagnostic: $($_.FullName)"
        Get-Content -LiteralPath $_.FullName -Tail 30
        if ($_.Name -eq 'service.log') {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $root 'service-failure.log') -Force
        }
    }
    throw
} finally {
    if ($null -ne $driver) {
        Stop-Child $driver
        $remainingOutput = $driver.StandardOutput.ReadToEnd() + $driver.StandardError.ReadToEnd()
        [IO.File]::WriteAllText((Join-Path $scenario 'driver-failure.log'), $remainingOutput, $encoding)
        Write-Output $remainingOutput
        $driver.Dispose()
    }
    if ($null -ne $installed -and (Test-Path (Join-Path $installed 'installation.json'))) {
        try { & (Join-Path $installed 'Uninstall-LanLink.ps1') -InstallDirectory $installed -RemoveData }
        catch { Write-Warning "Integration service cleanup failed: $_" }
    }
    if ($null -ne $relay) { Stop-Child $relay; $relay.Dispose() }
    Get-NetFirewallRule -Name $firewallName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    if ($null -ne $trusted) { Remove-Item "Cert:\LocalMachine\Root\$trusted" -Force }
    if (Get-Variable certificate -ErrorAction SilentlyContinue) { $certificate.Dispose() }
    if (Get-Variable authority -ErrorAction SilentlyContinue) { $authority.Dispose() }
    foreach ($thumbprint in $certificates) { Remove-Item "Cert:\CurrentUser\My\$thumbprint" -DeleteKey -Force }
    if ($passed) { Remove-Item -LiteralPath $root -Recurse -Force }
}
