import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve()
metadata = json.loads((source / "build-info.json").read_text())
assert metadata["source_clean"] and len(metadata["source_commit"]) == 40
mac = sys.platform == "darwin"
binaries = [source / "bin/lanlink-service", source / "bin/lanlink-ui-cli", source / "LanLink.app/Contents/MacOS/LanLink" if mac else source / "bin/lanlink-ui"]
for path in binaries:
    assert path.is_file() and os.access(path, os.X_OK)
    if sys.platform == "linux":
        assert path.read_bytes()[:4] == b"\x7fELF"
        dependencies = subprocess.run(["ldd", str(path)], text=True, capture_output=True, check=True)
        assert "not found" not in dependencies.stdout
assert subprocess.check_output([str(source / "bin/lanlink-service"), "--version"], text=True).strip() == metadata["version"]
with tempfile.TemporaryDirectory(prefix="lanlink-install-") as directory:
    root = Path(directory) / "root"
    other_root = Path(directory) / "other-component"
    other_program = other_root / ("Library/Application Support/LanLink" if mac else "opt/lanlink-client")
    (other_program / "bin").mkdir(parents=True)
    (other_program / "build-info.json").write_text(json.dumps(metadata))
    relay = other_program / "bin/lanlink-relay"
    relay.write_bytes(b"existing relay must be preserved")
    (other_program / ".lanlink-installation").write_text(metadata["version"])
    refused = subprocess.run([sys.executable, str(source / "install-client.py"), "--root", str(other_root), "--no-start"], capture_output=True)
    assert refused.returncode != 0 and b"not a LanLink client installation" in refused.stderr
    refused_remove = subprocess.run([sys.executable, str(source / "uninstall-client.py"), "--root", str(other_root), "--purge"], capture_output=True)
    assert refused_remove.returncode != 0 and b"No managed LanLink client installation" in refused_remove.stderr
    assert relay.read_bytes() == b"existing relay must be preserved"
    assert (other_program / ".lanlink-installation").read_text() == metadata["version"]
    assert not (other_program / "bin/lanlink-service").exists()
    token = Path(directory) / "input.token"
    token.write_text("lanlink-isolated-installer-test-token-2026\n")
    install = [sys.executable, str(source / "install-client.py"), "--root", str(root), "--no-start"]
    uninstall = [sys.executable, str(source / "uninstall-client.py"), "--root", str(root)]
    # A colocated relay uses different program/config/state paths and survives client installation/removal.
    relay_files=[]
    if not mac:
        for name in ("opt/lanlink/bin/lanlink-relay","etc/lanlink/relay.conf","var/lib/lanlink/relay.db","var/log/lanlink/server.log"):
            path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b"relay-owned state");relay_files.append(path)
    subprocess.run(install, check=True)
    assert all(p.read_bytes()==b"relay-owned state" for p in relay_files)
    config = root / ("Library/Preferences/LanLink/client.conf" if mac else "etc/lanlink-client/client.conf")
    state = root / ("Library/Application Support/LanLinkData" if mac else "var/lib/lanlink-client")
    assert "relay_host=" not in config.read_text(), "Fresh install must not select a server"
    assert not (state / "auth.token").exists(), "Installer must not invent server credentials"
    # Simulate app-owned connection data; reinstall must leave it byte-for-byte.
    with config.open("a") as output:
        output.write("relay_host=127.0.0.1\nrelay_port=9\n")
    (state / "auth.token").write_bytes(token.read_bytes())
    (state / "auth.token").chmod(0o600)
    identity = state / "device.identity"
    identity.write_bytes(b"isolated device identity fixture")
    protected = (config, identity, state / "auth.token")
    before = {p: hashlib.sha256(p.read_bytes()).digest() for p in protected}
    assert config.stat().st_mode & 0o077 == 0
    assert protected[-1].stat().st_mode & 0o077 == 0
    subprocess.run(install, check=True)
    assert all(hashlib.sha256(p.read_bytes()).digest() == value for p, value in before.items())
    installed = root / ("Library/Application Support/LanLink" if mac else "opt/lanlink-client")
    assert subprocess.check_output([str(installed / "bin/lanlink-service"), "--version"], text=True).strip() == metadata["version"]
    previous_binary = hashlib.sha256((installed / "bin/lanlink-service").read_bytes()).digest()
    broken = Path(directory) / "broken"
    shutil.copytree(source, broken, symlinks=True)
    (broken / ("com.lanlink.client.plist" if mac else "lanlink.desktop")).unlink()
    failure = subprocess.run([sys.executable, str(broken / "install-client.py"), "--root", str(root), "--no-start"], capture_output=True)
    assert failure.returncode != 0
    assert hashlib.sha256((installed / "bin/lanlink-service").read_bytes()).digest() == previous_binary
    assert all(hashlib.sha256(p.read_bytes()).digest() == value for p, value in before.items())
    assert not list(installed.parent.glob(".lanlink-new-*"))
    subprocess.run(uninstall, check=True)
    assert not installed.exists() and all(p.exists() for p in protected)
    subprocess.run(install, check=True)
    subprocess.run(uninstall + ["--purge"], check=True)
    assert all(p.read_bytes()==b"relay-owned state" for p in relay_files)
    assert not config.exists() and not identity.exists()
print("Portable package: native binaries, dependencies, install, upgrade preservation and explicit purge passed")
