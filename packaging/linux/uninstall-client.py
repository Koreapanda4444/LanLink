#!/usr/bin/env python3
import argparse
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description="Remove the LanLink Linux client")
parser.add_argument("--root", default="/")
parser.add_argument("--purge", action="store_true", help="Also delete this client's configuration and device identity")
args = parser.parse_args()
root = Path(args.root).resolve()
live = root == Path("/")
if live and os.geteuid() != 0:
    parser.error("Run the uninstaller with sudo")
prefix = root / "opt/lanlink"
if prefix.is_symlink() or not (prefix / "build-info.json").is_file():
    raise ValueError("No managed LanLink installation found")
for relative in ("opt/lanlink", "etc/systemd/system/lanlink-client.service", "usr/share/applications/lanlink.desktop", "etc/lanlink", "var/lib/lanlink", "var/log/lanlink"):
    current = root / relative
    while current != root:
        if current.is_symlink():
            raise ValueError("Refusing a symbolic installation path")
        current = current.parent
if live:
    subprocess.run(["/usr/bin/systemctl", "disable", "--now", "lanlink-client.service"], check=True)
for relative in ("etc/systemd/system/lanlink-client.service", "usr/share/applications/lanlink.desktop"):
    target = root / relative
    if target.is_symlink():
        raise ValueError("Refusing a symbolic installation path")
    target.unlink(missing_ok=True)
shutil.rmtree(prefix)
if live:
    subprocess.run(["/usr/bin/systemctl", "daemon-reload"], check=True)
if args.purge:
    for relative in ("etc/lanlink", "var/lib/lanlink", "var/log/lanlink"):
        target = root / relative
        if target.is_symlink():
            raise ValueError("Refusing a symbolic state path")
        if target.exists():
            shutil.rmtree(target)
print("LanLink removed; configuration and device identity " + ("deleted." if args.purge else "preserved."))
