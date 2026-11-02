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
for name in ("lanlink-service", "lanlink-ui", "lanlink-ui-cli"):
    path = source / "bin" / name
    assert path.is_file() and os.access(path, os.X_OK)
    if sys.platform == "linux":
        assert path.read_bytes()[:4] == b"\x7fELF"
        dependencies = subprocess.run(["ldd", str(path)], text=True, capture_output=True, check=True)
        assert "not found" not in dependencies.stdout
assert subprocess.check_output([str(source / "bin/lanlink-service"), "--version"], text=True).strip() == metadata["version"]
with tempfile.TemporaryDirectory(prefix="lanlink-install-") as directory:
    root = Path(directory) / "root"
    token = Path(directory) / "input.token"
    token.write_text("lanlink-isolated-installer-test-token-2026\n")
    install = [sys.executable, str(source / "install-client.py"), "--root", str(root), "--no-start"]
    uninstall = [sys.executable, str(source / "uninstall-client.py"), "--root", str(root)]
    subprocess.run(install + ["--relay", "127.0.0.1", "--token-file", str(token)], check=True)
    config = root / "etc/lanlink/client.conf"
    identity = root / "var/lib/lanlink/device.identity"
    identity.write_bytes(b"isolated device identity fixture")
    protected = (config, identity, root / "var/lib/lanlink/auth.token")
    before = {p: hashlib.sha256(p.read_bytes()).digest() for p in protected}
    assert config.stat().st_mode & 0o077 == 0
    assert protected[-1].stat().st_mode & 0o077 == 0
    subprocess.run(install, check=True)
    assert all(hashlib.sha256(p.read_bytes()).digest() == value for p, value in before.items())
    installed = root / "opt/lanlink"
    assert subprocess.check_output([str(installed / "bin/lanlink-service"), "--version"], text=True).strip() == metadata["version"]
    previous_binary = hashlib.sha256((installed / "bin/lanlink-service").read_bytes()).digest()
    broken = Path(directory) / "broken"
    shutil.copytree(source, broken, symlinks=True)
    (broken / "lanlink.desktop").unlink()
    failure = subprocess.run([sys.executable, str(broken / "install-client.py"), "--root", str(root), "--no-start"], capture_output=True)
    assert failure.returncode != 0
    assert hashlib.sha256((installed / "bin/lanlink-service").read_bytes()).digest() == previous_binary
    assert all(hashlib.sha256(p.read_bytes()).digest() == value for p, value in before.items())
    assert not list((root / "opt").glob(".lanlink-new-*"))
    subprocess.run(uninstall, check=True)
    assert not (root / "opt/lanlink").exists() and all(p.exists() for p in protected)
    subprocess.run(install, check=True)
    subprocess.run(uninstall + ["--purge"], check=True)
    assert not config.exists() and not identity.exists()
print("Portable package: native binaries, dependencies, install, upgrade preservation and explicit purge passed")
