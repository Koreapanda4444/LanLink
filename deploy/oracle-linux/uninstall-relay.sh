#!/usr/bin/env bash
set -euo pipefail

root=/
remove_data=0
while (( $# )); do
    case "$1" in
        --root) (( $# >= 2 )) || exit 1; root=$2; shift 2 ;;
        --remove-data) remove_data=1; shift ;;
        *) printf 'Usage: uninstall-relay.sh [--remove-data] [--root STAGING_DIRECTORY]\n' >&2; exit 1 ;;
    esac
done
[[ $root = /* && $root != *$'\n'* ]] || exit 1
root=$(realpath -m -- "$root")
prefix=${root%/}
program_directory="$prefix/opt/lanlink"
config_directory="$prefix/etc/lanlink"
[[ -f $program_directory/.lanlink-installation && ! -L $program_directory ]] || { printf 'A LanLink installation marker is required\n' >&2; exit 1; }
if [[ -z $prefix ]]; then
    (( EUID == 0 )) || { printf 'Run the uninstaller as root\n' >&2; exit 1; }
    systemctl disable --now lanlink-relay.service
    if [[ -f $config_directory/firewall.rules ]]; then
        firewall-cmd --state >/dev/null
        while read -r zone rule; do
            [[ $zone =~ ^[a-zA-Z0-9_-]+$ && $rule =~ ^[0-9]+/(tcp|udp)$ ]] || exit 1
            if firewall-cmd --permanent --zone="$zone" --query-port="$rule" >/dev/null; then
                firewall-cmd --permanent --zone="$zone" --remove-port="$rule"
            fi
        done < "$config_directory/firewall.rules"
        firewall-cmd --reload
        rm -f -- "$config_directory/firewall.rules"
    fi
fi
rm -f -- "$prefix/etc/systemd/system/lanlink-relay.service"
rm -rf -- "$program_directory"
if [[ -z $prefix ]]; then systemctl daemon-reload; fi
if (( remove_data )); then
    rm -rf -- "$config_directory" "$prefix/var/lib/lanlink" "$prefix/var/log/lanlink"
fi
printf 'LanLink relay removed. Persistent data is kept unless --remove-data is specified.\n'
