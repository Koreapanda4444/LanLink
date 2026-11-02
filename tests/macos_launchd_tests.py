import os
from pathlib import Path
import plistlib
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())
assert sys.platform == "darwin" and os.geteuid() == 0
label = f"com.lanlink.client.test.{os.getpid()}"
def status(endpoint):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(2)
        peer.connect(str(endpoint))
        peer.sendall(b"LLCP\x01\x01\x00\x00" + struct.pack("<I", 0))
        data = b""
        while len(data) < 20:
            part = peer.recv(20 - len(data))
            assert part, "launchd service returned an incomplete status"
            data += part
        assert data[:12] == b"LLCP\x01\x01\x01\x00" + struct.pack("<I", 8)

def wait_ready(endpoint, previous=None):
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        result = subprocess.run(["/bin/launchctl", "print", "system/" + label], capture_output=True, text=True)
        match = re.search(r"\bpid\s*=\s*(\d+)", result.stdout)
        pid = int(match[1]) if match else None
        if pid and pid != previous:
            try:
                status(endpoint)
                return pid
            except OSError:
                pass
        time.sleep(0.1)
    raise AssertionError("launchd client did not become ready")

with tempfile.TemporaryDirectory(prefix="lanlink-launchd-") as directory:
    root = Path(directory)
    root.chmod(0o700)
    (root / "token").write_text("lanlink-isolated-launchd-fixture-token\n")
    (root / "token").chmod(0o600)
    config = root / "client.conf"
    config.write_text(f"relay_host=127.0.0.1\nrelay_port=9\nauth_token_file={root / 'token'}\ndevice_identity_file={root / 'identity'}\nlog_directory={root / 'logs'}\n")
    endpoint = root / "control.sock"
    settings = plistlib.loads(Path("deploy/macos/com.lanlink.client.plist").read_bytes())
    settings.update(Label=label, ProgramArguments=[binary, "--console", str(config)],
        EnvironmentVariables={"LANLINK_CONTROL_SOCKET": str(endpoint)},
        StandardOutPath=str(root / "process.log"), StandardErrorPath=str(root / "process.log"))
    fixture = root / "job.plist"
    fixture.write_bytes(plistlib.dumps(settings))
    fixture.chmod(0o600)
    loaded = False
    try:
        subprocess.run(["/bin/launchctl", "bootstrap", "system", str(fixture)], check=True)
        loaded = True
        first = wait_ready(endpoint)
        os.kill(first, signal.SIGKILL)
        second = wait_ready(endpoint, previous=first)
        assert second != first
        subprocess.run(["/bin/launchctl", "bootout", "system/" + label], check=True)
        loaded = False
        deadline = time.monotonic() + 10
        while endpoint.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert not endpoint.exists(), "launchd removal left the control socket"
        assert (root / "identity").stat().st_mode & 0o077 == 0
    except BaseException:
        if (root / "process.log").exists():
            print((root / "process.log").read_text())
        raise
    finally:
        if loaded:
            subprocess.run(["/bin/launchctl", "bootout", "system/" + label])
print("launchd: native daemon startup, authenticated local IPC, crash restart, private identity and unload cleanup passed")
