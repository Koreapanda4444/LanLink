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
import sys



MAC = sys.platform == "darwin"

def layout(root):
    if MAC:
        return (root / "Library/Application Support/LanLink", root / "Library/Preferences/LanLink/client.conf",
            root / "Library/Application Support/LanLinkData", root / "Library/Logs/LanLink",
            root / "Library/LaunchDaemons/com.lanlink.client.plist", root / "Applications/LanLink.app")
    return (root / "opt/lanlink", root / "etc/lanlink/client.conf", root / "var/lib/lanlink", root / "var/log/lanlink",
        root / "etc/systemd/system/lanlink-client.service", root / "usr/share/applications/lanlink.desktop")

def run(arguments, check=True):
    return subprocess.run(arguments, check=check, timeout=30, capture_output=True)

def active():
    if MAC:
        return run(["/bin/launchctl", "print", "system/com.lanlink.client"], check=False).returncode == 0
    return run(["/usr/bin/systemctl", "is-active", "--quiet", "lanlink-client.service"], check=False).returncode == 0

def stop_service():
    if MAC:
        run(["/bin/launchctl", "bootout", "system/com.lanlink.client"])
    else:
        run(["/usr/bin/systemctl", "stop", "lanlink-client.service"])

def reload_service():
    if not MAC:
        run(["/usr/bin/systemctl", "daemon-reload"])

def start_service(unit):
    if MAC:
        run(["/bin/launchctl", "bootstrap", "system", str(unit)])
    else:
        run(["/usr/bin/systemctl", "enable", "--now", "lanlink-client.service"])

def access_group(user):
    try:
        grp.getgrnam("lanlink")
    except KeyError:
        if MAC:
            used = {group.gr_gid for group in grp.getgrall()}
            number = next(value for value in range(450, 500) if value not in used)
            run(["/usr/bin/dscl", ".", "-create", "/Groups/lanlink"])
            run(["/usr/bin/dscl", ".", "-create", "/Groups/lanlink", "PrimaryGroupID", str(number)])
            run(["/usr/bin/dscl", ".", "-create", "/Groups/lanlink", "RealName", "LanLink local control"])
            run(["/usr/bin/dscl", ".", "-create", "/Groups/lanlink", "Password", "*"])
        else:
            run(["/usr/sbin/groupadd", "--system", "lanlink"])
    if user:
        if MAC:
            run(["/usr/sbin/dseditgroup", "-o", "edit", "-a", user, "-t", "user", "lanlink"])
        else:
            run(["/usr/sbin/usermod", "-a", "-G", "lanlink", "--", user])

def check_desktop(prefix, desktop):
    if MAC and (desktop.exists() or desktop.is_symlink()):
        if not desktop.is_symlink() or Path(os.readlink(desktop)) != prefix / "LanLink.app":
            raise ValueError("Existing /Applications/LanLink.app is not this managed installation")

def save_desktop(desktop):
    if MAC:
        return os.readlink(desktop) if desktop.is_symlink() else None
    return desktop.read_bytes() if desktop.exists() else None

def install_desktop(prefix, desktop):
    if MAC:
        desktop.unlink(missing_ok=True)
        desktop.symlink_to(prefix / "LanLink.app", target_is_directory=True)
    else:
        shutil.copyfile(prefix / "lanlink.desktop", desktop)
        desktop.chmod(0o644)

def restore_desktop(desktop, previous):
    if MAC:
        desktop.unlink(missing_ok=True)
        if previous is not None:
            desktop.symlink_to(previous, target_is_directory=True)
    elif previous is None:
        desktop.unlink(missing_ok=True)
    else:
        desktop.write_bytes(previous)

def install_main():
    parser = argparse.ArgumentParser(description="Install the LanLink desktop client")
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
    required = ["bin/lanlink-service", "bin/lanlink-ui-cli", "LanLink.app/Contents/MacOS/LanLink" if MAC else "bin/lanlink-ui"]
    for name in required:
        if not (source / name).is_file():
            raise ValueError(f"Incomplete client package: {name}")
    if metadata["system"] != ("Darwin" if MAC else "Linux"):
        raise ValueError("Package does not match this operating system")
    prefix, config, state, logs, unit, desktop = layout(root)
    check_desktop(prefix, desktop)
    for target in (prefix, config, state, logs, unit, desktop):
        current = target.parent if MAC and target == desktop else target
        while current != root:
            if current.is_symlink():
                raise ValueError(f"Refusing a symbolic installation path: {current}")
            current = current.parent
    if prefix.exists() and not (prefix / "build-info.json").is_file():
        raise ValueError("Existing client program directory is not a LanLink installation")
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
        access_group(args.user)
    temporary = Path(tempfile.mkdtemp(prefix=".lanlink-new-", dir=prefix.parent))
    backup = temporary.with_name(temporary.name + "-old")
    previous_unit = unit.read_bytes() if unit.exists() else None
    previous_desktop = save_desktop(desktop)
    created = []
    installed = False
    was_active = live and active()
    try:
        shutil.copytree(source, temporary, dirs_exist_ok=True, symlinks=True, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        temporary.chmod(0o755)
        for item in temporary.rglob("*"):
            if not item.is_symlink():
                item.chmod(0o755 if item.is_dir() or item.stat().st_mode & 0o111 else 0o644)
        if was_active:
            stop_service()
        if prefix.exists():
            prefix.rename(backup)
        temporary.rename(prefix)
        installed = True
        shutil.copyfile(prefix / ("com.lanlink.client.plist" if MAC else "lanlink-client.service"), unit)
        unit.chmod(0o644)
        if live and MAC:
            os.chown(unit, 0, 0)
        install_desktop(prefix, desktop)
        if token is not None:
            target = state / "auth.token"
            with target.open("xb") as output:
                created.append(target)
                os.fchmod(output.fileno(), 0o600)
                output.write(token)
            with config.open("x") as output:
                created.append(config)
                os.fchmod(output.fileno(), 0o600)
                _, _, host_state, host_logs, _, _ = layout(Path("/"))
                output.write(f"relay_host={args.relay}\nrelay_port={args.port}\n"
                    f"auth_token_file={host_state / 'auth.token'}\n"
                    f"device_identity_file={host_state / 'device.identity'}\n"
                    f"log_directory={host_logs}\n")
        if live:
            reload_service()
            if config.exists() and not args.no_start:
                start_service(unit)
        if backup.exists():
            shutil.rmtree(backup)
        print(f"Installed LanLink {metadata['version']} in {prefix}")
        if not config.exists():
            print(f"Create {config} using INSTALL.md before starting the service.")
        if args.user and live:
            print("Sign out and back in to activate control group membership.")
    except BaseException:
        if installed:
            shutil.rmtree(prefix)
        if backup.exists():
            backup.rename(prefix)
        if previous_unit is None:
            unit.unlink(missing_ok=True)
        else:
            unit.write_bytes(previous_unit)
        restore_desktop(desktop, previous_desktop)
        for target in created:
            target.unlink(missing_ok=True)
        if live:
            reload_service()
            if was_active:
                start_service(unit)
        raise
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def uninstall_main():
    parser = argparse.ArgumentParser(description="Remove the LanLink desktop client")
    parser.add_argument("--root", default="/")
    parser.add_argument("--purge", action="store_true", help="Also delete this client's configuration and device identity")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    live = root == Path("/")
    if live and os.geteuid() != 0:
        parser.error("Run the uninstaller with sudo")
    prefix, config, state, logs, unit, desktop = layout(root)
    check_desktop(prefix, desktop)
    if prefix.is_symlink() or not (prefix / "build-info.json").is_file():
        raise ValueError("No managed LanLink installation found")
    for target in (prefix, config, state, logs, unit, desktop):
        current = target.parent if MAC and target == desktop else target
        while current != root:
            if current.is_symlink():
                raise ValueError("Refusing a symbolic installation path")
            current = current.parent
    if live:
        if MAC:
            if active():
                stop_service()
        else:
            run(["/usr/bin/systemctl", "disable", "--now", "lanlink-client.service"])
    unit.unlink(missing_ok=True)
    desktop.unlink(missing_ok=True)
    shutil.rmtree(prefix)
    if live:
        reload_service()
    if args.purge:
        for target in (config.parent, state, logs):
            if target.exists():
                shutil.rmtree(target)
    print("LanLink removed; configuration and device identity " + ("deleted." if args.purge else "preserved."))
