#!/usr/bin/env python3
import argparse
import grp
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description="Install the LanLink Linux client")
    parser.add_argument("--root", default="/", help="Staging root; services and user groups are untouched")
    parser.add_argument("--relay")
    parser.add_argument("--port", type=int, default=4433)
    parser.add_argument("--token-file", type=Path)
    parser.add_argument("--user", help="Existing user to add to the local control group")
    parser.add_argument("--no-start", action="store_true")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    live = root == Path("/")
    if live and os.geteuid() != 0:
        parser.error("Run the installer with sudo")
    if args.relay and (not re.fullmatch(r"[A-Za-z0-9.:-]{1,253}", args.relay) or not 1 <= args.port <= 65535):
        parser.error("Invalid relay host or port")
    if bool(args.relay) != bool(args.token_file):
        parser.error("--relay and --token-file must be provided together")
    if args.user:
        pwd.getpwnam(args.user)
    source = Path(__file__).resolve().parent
    metadata = json.loads((source / "build-info.json").read_text())
    for name in ("lanlink-service", "lanlink-ui", "lanlink-ui-cli"):
        if not (source / "bin" / name).is_file():
            raise ValueError(f"Incomplete client package: {name}")
    prefix = root / "opt/lanlink"
    config = root / "etc/lanlink/client.conf"
    state = root / "var/lib/lanlink"
    logs = root / "var/log/lanlink"
    unit = root / "etc/systemd/system/lanlink-client.service"
    desktop = root / "usr/share/applications/lanlink.desktop"
    for target in (prefix, config, state, logs, unit, desktop):
        current = target
        while current != root:
            if current.is_symlink():
                raise ValueError(f"Refusing a symbolic installation path: {current}")
            current = current.parent
    if prefix.exists() and not (prefix / "build-info.json").is_file():
        raise ValueError("Existing /opt/lanlink is not a LanLink installation")
    if prefix.is_relative_to(source):
        raise ValueError("The installation target must be outside the package")
    for link in source.rglob("*"):
        if link.is_symlink() and not link.resolve().is_relative_to(source):
            raise ValueError("Runtime library link escapes the package")
    token = None
    if args.token_file:
        token = args.token_file.read_bytes()
        if not 32 <= len(token.strip()) <= 4096:
            raise ValueError("Authentication token must contain 32 to 4096 bytes")
        if config.exists():
            raise ValueError("Configuration already exists; upgrade without --relay to preserve it")
        if (state / "auth.token").exists():
            raise ValueError("Existing token must be preserved; restore client.conf before installing")
    for directory in (prefix.parent, config.parent, state, logs, unit.parent, desktop.parent):
        directory.mkdir(parents=True, exist_ok=True)
    if live:
        for directory in (config.parent, state, logs):
            directory.chmod(0o700)
        try:
            grp.getgrnam("lanlink")
        except KeyError:
            subprocess.run(["/usr/sbin/groupadd", "--system", "lanlink"], check=True)
        if args.user:
            subprocess.run(["/usr/sbin/usermod", "-a", "-G", "lanlink", "--", args.user], check=True)
    temporary = Path(tempfile.mkdtemp(prefix=".lanlink-new-", dir=prefix.parent))
    backup = temporary.with_name(temporary.name + "-old")
    previous_unit = unit.read_bytes() if unit.exists() else None
    previous_desktop = desktop.read_bytes() if desktop.exists() else None
    created = []
    installed = False
    was_active = live and subprocess.run(["/usr/bin/systemctl", "is-active", "--quiet", "lanlink-client.service"]).returncode == 0
    try:
        shutil.copytree(source, temporary, dirs_exist_ok=True, symlinks=True)
        for item in temporary.rglob("*"):
            if not item.is_symlink():
                item.chmod(item.stat().st_mode & ~0o022)
        if was_active:
            subprocess.run(["/usr/bin/systemctl", "stop", "lanlink-client.service"], check=True)
        if prefix.exists():
            prefix.rename(backup)
        temporary.rename(prefix)
        installed = True
        shutil.copyfile(prefix / "lanlink-client.service", unit)
        unit.chmod(0o644)
        shutil.copyfile(prefix / "lanlink.desktop", desktop)
        desktop.chmod(0o644)
        if token is not None:
            target = state / "auth.token"
            with target.open("xb") as output:
                created.append(target)
                os.fchmod(output.fileno(), 0o600)
                output.write(token)
            with config.open("x") as output:
                created.append(config)
                os.fchmod(output.fileno(), 0o600)
                output.write(f"relay_host={args.relay}\nrelay_port={args.port}\n"
                    "auth_token_file=/var/lib/lanlink/auth.token\n"
                    "device_identity_file=/var/lib/lanlink/device.identity\n"
                    "log_directory=/var/log/lanlink\n")
        if live:
            subprocess.run(["/usr/bin/systemctl", "daemon-reload"], check=True)
            if config.exists() and not args.no_start:
                subprocess.run(["/usr/bin/systemctl", "enable", "--now", "lanlink-client.service"], check=True)
        if backup.exists():
            shutil.rmtree(backup)
        print(f"Installed LanLink {metadata['version']} in {prefix}")
        if not config.exists():
            print("Create /etc/lanlink/client.conf using INSTALL.md before starting the service.")
        if args.user and live:
            print("Sign out and back in to activate control group membership.")
    except BaseException:
        if installed:
            shutil.rmtree(prefix)
        if backup.exists():
            backup.rename(prefix)
        for target, content in ((unit, previous_unit), (desktop, previous_desktop)):
            if content is None:
                target.unlink(missing_ok=True)
            else:
                target.write_bytes(content)
        for target in created:
            target.unlink(missing_ok=True)
        if live:
            subprocess.run(["/usr/bin/systemctl", "daemon-reload"])
            if was_active:
                subprocess.run(["/usr/bin/systemctl", "start", "lanlink-client.service"])
        raise
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
