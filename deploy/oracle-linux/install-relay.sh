#!/usr/bin/env bash
set -euo pipefail
umask 077

package_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=/
certificate=
private_key=
token_file=
port=
bind_host=
firewall_zone=
start=1
firewall=1
while (( $# )); do
    case "$1" in
        --root|--certificate|--private-key|--token-file|--port|--bind|--firewall-zone)
            (( $# >= 2 )) || { printf 'Missing option value\n' >&2; exit 1; }
            case "$1" in
                --root) root=$2 ;;
                --certificate) certificate=$2 ;;
                --private-key) private_key=$2 ;;
                --token-file) token_file=$2 ;;
                --port) port=$2 ;;
                --bind) bind_host=$2 ;;
                --firewall-zone) firewall_zone=$2 ;;
            esac
            shift 2 ;;
        --no-start) start=0; shift ;;
        --no-firewall) firewall=0; shift ;;
        *) printf 'Usage: install-relay.sh [--certificate PEM --private-key PEM --token-file FILE] [--port PORT] [--bind IP] [--firewall-zone ZONE] [--no-start] [--no-firewall] [--root STAGING_DIRECTORY]\n' >&2; exit 1 ;;
    esac
done
[[ $root = /* && $root != *$'\n'* ]] || { printf 'The staging root must be an absolute path\n' >&2; exit 1; }
root=$(realpath -m -- "$root")
prefix=${root%/}
program_directory="$prefix/opt/lanlink"
config_directory="$prefix/etc/lanlink"
unit_path="$prefix/etc/systemd/system/lanlink-relay.service"
offline=0
[[ -z $prefix ]] || offline=1
if (( !offline )); then
    (( EUID == 0 )) || { printf 'Run the installer as root\n' >&2; exit 1; }
    source /etc/os-release
    [[ $ID == ol && ${VERSION_ID%%.*} == 9 ]] || { printf 'This installer targets Oracle Linux 9; use --root for offline staging\n' >&2; exit 1; }
    command -v systemctl >/dev/null
    if (( firewall )); then
        firewall-cmd --state >/dev/null || { printf 'firewalld must be running, or specify --no-firewall\n' >&2; exit 1; }
        firewall_zone=${firewall_zone:-$(firewall-cmd --get-default-zone)}
        [[ $firewall_zone =~ ^[a-zA-Z0-9_-]+$ ]] || exit 1
    fi
fi
[[ $package_directory != "$program_directory" ]] || { printf 'Extract the new package outside the installation directory\n' >&2; exit 1; }
for file in bin/lanlink-relay lanlink-relay.service uninstall-relay.sh relay-version.txt; do
    [[ -f "$package_directory/$file" ]] || { printf 'Incomplete relay package: %s\n' "$file" >&2; exit 1; }
done
[[ ! -L $program_directory && ! -L $config_directory ]] || { printf 'Installation directories cannot be symlinks\n' >&2; exit 1; }
[[ ! -e $program_directory || -f "$program_directory/.lanlink-installation" ]] || { printf 'The target is not a LanLink installation\n' >&2; exit 1; }
certificate=${certificate:-"$config_directory/server.cert"}
private_key=${private_key:-"$config_directory/server.key"}
token_file=${token_file:-"$config_directory/auth.token"}
for source_file in "$certificate" "$private_key" "$token_file"; do
    [[ -f $source_file ]] || { printf 'Certificate, private key and token files are required for the first installation\n' >&2; exit 1; }
done
python3 - "$token_file" <<'PY'
import pathlib,sys
raw=pathlib.Path(sys.argv[1]).read_bytes()
token=raw.rstrip(b'\r\n')
if len(raw)>4096 or len(token)<32 or any(b<33 or b>126 for b in token):
    sys.exit('Authentication token must contain 32 to 4096 printable ASCII bytes')
PY
openssl x509 -in "$certificate" -checkend 0 -noout >/dev/null
if [[ -f $config_directory/relay.conf ]]; then
    read_setting() {
        awk -F= -v key="$1" '{gsub(/^[[:space:]]+|[[:space:]]+$/, "", $1); if ($1==key) {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2}}' "$config_directory/relay.conf"
    }
    port=${port:-$(read_setting relay_port)}
    bind_host=${bind_host:-$(read_setting listen_host)}
fi
port=${port:-4433}
bind_host=${bind_host:-0.0.0.0}
[[ $port =~ ^[0-9]{1,5}$ ]] && (( 10#$port >= 1 && 10#$port <= 65535 )) || { printf 'Invalid relay port\n' >&2; exit 1; }
port=$((10#$port))
[[ $bind_host =~ ^[a-zA-Z0-9.:_-]+$ ]] || { printf 'Invalid bind address\n' >&2; exit 1; }
install -d -m 0755 "$prefix/opt" "$prefix/etc/systemd/system"
backup=$(mktemp -d "$prefix/opt/.lanlink-backup.XXXXXX")
staging=$(mktemp -d "$prefix/opt/.lanlink-stage.XXXXXX")
success=0
changed=0
was_active=0
was_enabled=0
cleanup() {
    code=$?
    if (( !success && changed )); then
        if (( !offline && firewall )) && [[ -f $backup/new-firewall.rules ]]; then
            while read -r zone rule; do
                firewall-cmd --permanent --zone="$zone" --remove-port="$rule" >/dev/null || true
            done < "$backup/new-firewall.rules"
        fi
        if (( !offline && firewall )) && [[ -f $backup/removed-firewall.rules ]]; then
            while read -r zone rule; do
                firewall-cmd --permanent --zone="$zone" --add-port="$rule" >/dev/null || true
            done < "$backup/removed-firewall.rules"
        fi
        if (( !offline && firewall )); then firewall-cmd --reload >/dev/null || true; fi
        if (( !offline )); then systemctl stop lanlink-relay.service || true; fi
        if (( !offline && !was_enabled )); then systemctl disable lanlink-relay.service || true; fi
        rm -rf -- "$program_directory"
        [[ ! -d $backup/program ]] || mv -- "$backup/program" "$program_directory"
        rm -rf -- "$config_directory"
        [[ ! -d $backup/config ]] || mv -- "$backup/config" "$config_directory"
        if [[ -f $backup/unit ]]; then cp -a -- "$backup/unit" "$unit_path"; else rm -f -- "$unit_path"; fi
        if (( !offline )); then
            systemctl daemon-reload || true
            (( !was_active )) || systemctl start lanlink-relay.service || true
        fi
    fi
    rm -rf -- "$staging" "$backup"
    return "$code"
}
trap cleanup EXIT
openssl x509 -in "$certificate" -pubkey -noout > "$backup/cert.pub"
openssl pkey -in "$private_key" -passin pass: -pubout > "$backup/key.pub"
cmp -s "$backup/cert.pub" "$backup/key.pub" || { printf 'Certificate and private key do not match\n' >&2; exit 1; }
cp -a -- "$package_directory/." "$staging/"
printf '%s\n' "$(cat "$package_directory/relay-version.txt")" > "$staging/.lanlink-installation"
chmod 0755 "$staging" "$staging/bin" "$staging/bin/lanlink-relay"
chmod -R go-w "$staging"
dependency_report=$(LD_LIBRARY_PATH="$staging/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$staging/bin/lanlink-relay")
[[ $dependency_report != *'not found'* ]] || { printf 'Relay runtime dependencies are missing\n' >&2; exit 1; }
[[ ! -d $config_directory ]] || cp -a -- "$config_directory" "$backup/config"
[[ ! -f $unit_path ]] || cp -a -- "$unit_path" "$backup/unit"
if (( !offline )); then
    id lanlink >/dev/null 2>&1 || useradd --system --user-group --home-dir /var/lib/lanlink --shell /sbin/nologin lanlink
    if systemctl is-enabled --quiet lanlink-relay.service; then was_enabled=1; fi
    if systemctl is-active --quiet lanlink-relay.service; then
        was_active=1
        systemctl stop lanlink-relay.service
    fi
fi
if [[ -d $program_directory ]]; then
    mv -- "$program_directory" "$backup/program"
    changed=1
fi
mv -- "$staging" "$program_directory"
changed=1
install -d -m 0750 "$config_directory" "$prefix/var/lib/lanlink" "$prefix/var/log/lanlink"
copy_secret() {
    source_file=$1
    target_file="$config_directory/$2"
    if [[ $(realpath -- "$source_file") != "$(realpath -m -- "$target_file")" ]]; then
        install -m 0640 -- "$source_file" "$target_file"
    fi
    chmod 0640 -- "$target_file"
}
copy_secret "$certificate" server.cert
copy_secret "$private_key" server.key
copy_secret "$token_file" auth.token
python3 - "$config_directory/relay.conf" "$port" "$bind_host" <<'PY'
import pathlib,sys
path=pathlib.Path(sys.argv[1])
settings={}
if path.is_file():
    for line in path.read_text().splitlines():
        if line.strip() and not line.lstrip().startswith('#'):
            key,value=line.split('=',1)
            if key.strip() in settings:
                sys.exit('Duplicate relay configuration key')
            settings[key.strip()]=value.strip()
settings.update(listen_host=sys.argv[3],relay_port=sys.argv[2],
    tls_certificate_file='/etc/lanlink/server.cert',tls_private_key_file='/etc/lanlink/server.key',
    auth_token_file='/etc/lanlink/auth.token',relay_database_file='/var/lib/lanlink/relay.db',
    log_directory='/var/log/lanlink')
path.write_text(''.join(f'{key}={value}\n' for key,value in settings.items()))
PY
chmod 0640 "$config_directory/relay.conf"
install -m 0644 "$program_directory/lanlink-relay.service" "$unit_path"
if (( !offline )); then
    chown -R root:root "$program_directory"
    chown -R root:lanlink "$config_directory"
    chown -R lanlink:lanlink "$prefix/var/lib/lanlink" "$prefix/var/log/lanlink"
    if command -v restorecon >/dev/null; then restorecon -RF "$program_directory" "$config_directory" /var/lib/lanlink /var/log/lanlink "$unit_path"; fi
    systemctl daemon-reload
    systemctl enable lanlink-relay.service
    if (( start )); then
        systemctl start lanlink-relay.service
        sleep 1
        systemctl is-active --quiet lanlink-relay.service
    fi
    if (( firewall )); then
        touch "$config_directory/firewall.rules"
        while read -r zone rule; do
            [[ $zone =~ ^[a-zA-Z0-9_-]+$ && $rule =~ ^[0-9]+/(tcp|udp)$ ]] || { printf 'Invalid firewall ownership record\n' >&2; exit 1; }
            if [[ $zone != "$firewall_zone" || ( $rule != "$port/tcp" && $rule != "$port/udp" ) ]]; then
                if firewall-cmd --permanent --zone="$zone" --query-port="$rule" >/dev/null; then
                    firewall-cmd --permanent --zone="$zone" --remove-port="$rule"
                    printf '%s %s\n' "$zone" "$rule" >> "$backup/removed-firewall.rules"
                fi
            else
                printf '%s %s\n' "$zone" "$rule" >> "$backup/current-firewall.rules"
            fi
        done < "$config_directory/firewall.rules"
        : > "$config_directory/firewall.rules"
        if [[ -f $backup/current-firewall.rules ]]; then
            cat "$backup/current-firewall.rules" > "$config_directory/firewall.rules"
        fi
        for protocol in tcp udp; do
            if ! firewall-cmd --permanent --zone="$firewall_zone" --query-port="$port/$protocol" >/dev/null; then
                firewall-cmd --permanent --zone="$firewall_zone" --add-port="$port/$protocol"
                printf '%s %s\n' "$firewall_zone" "$port/$protocol" >> "$backup/new-firewall.rules"
                printf '%s %s\n' "$firewall_zone" "$port/$protocol" >> "$config_directory/firewall.rules"
            fi
        done
        sort -u "$config_directory/firewall.rules" -o "$config_directory/firewall.rules"
        firewall-cmd --reload
    fi
fi
success=1
printf 'LanLink relay installed. TCP/UDP port: %s. Persistent data: /var/lib/lanlink\n' "$port"
