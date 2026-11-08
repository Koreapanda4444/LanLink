#!/usr/bin/env python3
import argparse
import configparser
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
import time



MAC = sys.platform == "darwin"

def layout(root):
    if MAC:
        return (root / "Library/Application Support/LanLink", root / "Library/Preferences/LanLink/client.conf",
            root / "Library/Application Support/LanLinkData", root / "Library/Logs/LanLink",
            root / "Library/LaunchDaemons/com.lanlink.client.plist", root / "Applications/LanLink.app")
    return (root / "opt/lanlink-client", root / "etc/lanlink-client/client.conf", root / "var/lib/lanlink-client", root / "var/log/lanlink-client",
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
        deadline = time.monotonic() + 20
        while active():
            if time.monotonic() >= deadline:
                raise RuntimeError("launchd did not finish unloading the LanLink service")
            time.sleep(0.1)
    else:
        run(["/usr/bin/systemctl", "stop", "lanlink-client.service"])

def reload_service():
    if not MAC:
        run(["/usr/bin/systemctl", "daemon-reload"])

def start_service(unit):
    if MAC:
        deadline = time.monotonic() + 10
        while True:
            result = run(["/bin/launchctl", "bootstrap", "system", str(unit)], check=False)
            if result.returncode == 0:
                break
            # launchd can still be completing a successful bootout. Retry only
            # its transient EIO response while no job is registered.
            if result.returncode != 5 or active() or time.monotonic() >= deadline:
                raise RuntimeError("launchd bootstrap failed: " + result.stderr.decode(errors="replace").strip())
            time.sleep(0.1)
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

def mime_default(root, install):
    path=root / "usr/share/applications/mimeapps.list"
    previous=path.read_bytes() if path.exists() else None
    parser=configparser.ConfigParser(interpolation=None,strict=True)
    parser.optionxform=str
    if previous:parser.read_string(previous.decode())
    section="Default Applications";key="x-scheme-handler/lanlink"
    if install:
        if not parser.has_section(section):parser.add_section(section)
        if not parser.has_option(section,key):parser[section][key]="lanlink.desktop;"
    elif parser.has_option(section,key) and parser[section][key]=="lanlink.desktop;":
        parser.remove_option(section,key)
    else:return previous
    path.parent.mkdir(parents=True,exist_ok=True)
    with path.open("w") as f:parser.write(f,space_around_delimiters=False)
    path.chmod(0o644)
    return previous


def install_main():
    parser = argparse.ArgumentParser(description="Install the LanLink desktop client")
    parser.add_argument("--root", default="/", help="Staging root; services and user groups are untouched")
    parser.add_argument("--user", help="Existing user to add to the local control group")
    parser.add_argument("--no-start", action="store_true")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    live = root == Path("/")
    if live and os.geteuid() != 0:
        parser.error("Run the installer with sudo")
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
    if prefix.exists() and (not (prefix / "build-info.json").is_file() or
            not all((prefix / name).is_file() for name in ("bin/lanlink-service", "bin/lanlink-ui-cli"))):
        raise ValueError("Existing program directory is not a LanLink client installation; it must not replace a relay or another component")
    if prefix.is_relative_to(source):
        raise ValueError("The installation target must be outside the package")
    for link in source.rglob("*"):
        if link.is_symlink() and not link.resolve().is_relative_to(source):
            raise ValueError("Runtime library link escapes the package")
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
    mime_path=root / "usr/share/applications/mimeapps.list"
    previous_mime=mime_path.read_bytes() if not MAC and mime_path.exists() else None
    mime_changed=False
    created = []
    installed = False
    was_active = live and active()
    start_attempted = False
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
        if not MAC:
            mime_changed=True
            mime_default(root,True)
        if not config.exists():
            with config.open("x") as output:
                created.append(config)
                os.fchmod(output.fileno(), 0o600)
                _, _, host_state, host_logs, _, _ = layout(Path("/"))
                output.write("# Server connections are configured separately in LanLink.\n"
                    f"auth_token_file={host_state / 'auth.token'}\n"
                    f"device_identity_file={host_state / 'device.identity'}\n"
                    f"log_directory={host_logs}\n")
        if live:
            reload_service()
            if not args.no_start:
                start_attempted = True
                start_service(unit)
        if backup.exists():
            shutil.rmtree(backup)
        print(f"Installed LanLink {metadata['version']} in {prefix}")
        print("Open LanLink. The service can wait without a registered server.")
        if args.user and live:
            print("Sign out and back in to activate control group membership.")
    except BaseException:
        if start_attempted and active():
            stop_service()
        if installed:
            shutil.rmtree(prefix)
        if backup.exists():
            backup.rename(prefix)
        if previous_unit is None:
            unit.unlink(missing_ok=True)
        else:
            unit.write_bytes(previous_unit)
        restore_desktop(desktop, previous_desktop)
        if mime_changed:
            if previous_mime is None:mime_path.unlink(missing_ok=True)
            else:mime_path.write_bytes(previous_mime)
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
    if prefix.is_symlink() or not (prefix / "build-info.json").is_file() or not all(
            (prefix / name).is_file() for name in ("bin/lanlink-service", "bin/lanlink-ui-cli")):
        raise ValueError("No managed LanLink client installation found")
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
    if not MAC:mime_default(root,False)
    shutil.rmtree(prefix)
    if live:
        reload_service()
    if args.purge:
        for target in (config.parent, state, logs):
            if target.exists():
                shutil.rmtree(target)
    print("LanLink removed; configuration and device identity " + ("deleted." if args.purge else "preserved."))
