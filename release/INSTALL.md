# Install LanLink 0.1.0

Use the Windows x64 ZIP for clients and the Oracle Linux 9 x64 tarball for the relay. Keep `SHA256SUMS` beside the downloaded files. On Linux, verify them with `sha256sum --check SHA256SUMS --ignore-missing`. In PowerShell, compare `(Get-FileHash .\LanLink-0.1.0-windows-x64.zip -Algorithm SHA256).Hash` with the matching entry in `SHA256SUMS`.

## Relay

Prepare a TLS certificate and its matching PEM private key for the hostname or IP clients will use. The certificate must be trusted by both Windows and OpenSSL; configure both trust stores when using your own CA. Generate a printable shared token with `umask 077; openssl rand -hex 32 > auth.token`. Supply the same token file to authorized clients.

On Oracle Linux 9 x64, install the package prerequisites with `sudo dnf install -y python3 openssl systemd firewalld`. Start firewalld, allow inbound TCP and UDP on port 4433 in the Oracle Cloud security list or NSG, and extract the relay tarball into a staging directory. From the extracted package directory, run:

```bash
sudo bash ./install-relay.sh --certificate /absolute/server.cert \
  --private-key /absolute/server.key --token-file /absolute/auth.token
sudo systemctl status lanlink-relay
sudo journalctl -u lanlink-relay -n 50
```

The installer uses `/opt/lanlink` for programs, `/etc/lanlink` for configuration and credentials, `/var/lib/lanlink` for SQLite state, and `/var/log/lanlink` for application logs. It starts an unprivileged `lanlink` service and opens the selected TCP/UDP port in firewalld. Use `--port`, `--bind`, and `--firewall-zone` to customize installation. Use `--no-firewall` when the host firewall is managed separately.

The following optional entries in `/etc/lanlink/relay.conf` show the defaults. Restart the service after changing them.

```ini
relay_max_connections=128
relay_max_tls_handshakes=16
relay_send_queue_bytes=4194304
relay_send_queue_frames=256
relay_control_requests_per_second=32
```

Connection capacity is shared by QUIC and TCP, including unauthenticated sessions. TLS handshake capacity must be between 1 and the connection limit. Each connection's output limit includes sends awaiting completion. The queue reserves 65536 bytes and 8 frames for control traffic. Control requests allow a burst of four seconds' quota before rate enforcement disconnects a client. Backpressure packet drops are counted in the relay log at increasing intervals.

## Windows client

Extract the ZIP and open an administrator PowerShell session in the extracted directory. Supply a trusted relay hostname and the token file:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-LanLink.ps1 `
  -RelayHost relay.example.com -AuthenticationTokenFile C:\secure\auth.token
```

The default locations are `C:\Program Files\LanLink` for programs and `C:\ProgramData\LanLink` for persistent configuration, identity, token, and logs. The installer registers the automatic Windows service, a Start Menu shortcut, and firewall rules for relay communication and virtual IPv4 traffic. Open LanLink from the Start Menu, create a network, and have its owner approve joining devices. Diagnostics show the active transport and connection state.

LanLink's own executables and scripts are unsigned. The package includes the official signed Wintun DLL and the runtime libraries required by the client. Only virtual addresses in `10.77.0.0/16` are covered by the virtual LAN inbound firewall rule.

## Upgrade and uninstall

Extract a new package outside the installed program directory. Rerun its installer without the first-install credential options; use the same program and state directories when customized. Upgrades preserve device identities, credentials, network membership, and relay leases. The relay installer restores the previous installation if startup fails.

Uninstall the relay with `sudo bash /opt/lanlink/uninstall-relay.sh`. Uninstall the client from Windows Installed Apps or run `C:\Program Files\LanLink\Uninstall-LanLink.ps1` in administrator PowerShell. Ordinary uninstall preserves persistent state. Use the explicit relay `--remove-data` or Windows `-RemoveData` option only when you intend to delete credentials, identities, and stored network state.
