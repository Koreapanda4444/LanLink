import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())
def request(path, command):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(3)
        peer.connect(str(path))
        peer.sendall(b"LLCP" + bytes((1, command, 0, 0)) + struct.pack("<I", 0))
        data = b""
        while len(data) < 12:
            part = peer.recv(12 - len(data))
            assert part, "incomplete IPC header"
            data += part
        length = struct.unpack("<I", data[8:12])[0]
        assert data[:4] == b"LLCP" and data[5:8] == bytes((command, 1, 0)) and length <= 524288
        body = b""
        while len(body) < length:
            part = peer.recv(length - len(body))
            assert part, "incomplete IPC body"
            body += part
        return body

with tempfile.TemporaryDirectory(prefix="lanlink-service-") as directory:
    root = Path(directory)
    root.chmod(0o700)
    (root / "token").write_text("lanlink-isolated-service-test-token\n")
    (root / "token").chmod(0o600)
    configuration = root / "client.conf"
    configuration.write_text(f"relay_host=127.0.0.1\nrelay_port=9\nauth_token_file={root / 'token'}\ndevice_identity_file={root / 'identity'}\nlog_directory={root / 'logs'}\n")
    endpoint = root / "control.sock"
    environment = dict(os.environ, LANLINK_CONTROL_SOCKET=str(endpoint))
    for mode in ("signal", "ipc"):
        with (root / "process.log").open("w+") as log:
            process = subprocess.Popen([binary, "--console", str(configuration)], env=environment, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while not endpoint.exists():
                    assert process.poll() is None, "service exited before IPC readiness"
                    assert time.monotonic() < deadline, "service startup timed out"
                    time.sleep(0.05)
                assert len(request(endpoint, 1)) == 8
                if mode == "signal": process.send_signal(signal.SIGTERM)
                else: assert request(endpoint, 9) == b""
                assert process.wait(timeout=10) == 0
                assert not endpoint.exists(), "stale control socket after shutdown"
            except BaseException:
                log.seek(0)
                print(log.read())
                raise
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
    assert (root / "identity").exists()
    assert (root / "identity").stat().st_mode & 0o077 == 0
print("Native service: foreground startup, disconnected IPC, SIGTERM, IPC stop, identity permissions and restart passed")
