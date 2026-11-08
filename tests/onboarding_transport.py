import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="lanlink-onboarding-") as directory:
    Path(directory).chmod(0o700)
    for _ in range(32):
        with socket.socket() as tcp, socket.socket(type=socket.SOCK_DGRAM) as udp:
            tcp.bind(("127.0.0.1", 0))
            port = tcp.getsockname()[1]
            try:
                udp.bind(("127.0.0.1", port))
            except OSError:
                continue
            break
    else:
        raise RuntimeError("No free TCP/UDP fixture port")
    subprocess.run([binary, directory, str(port)], check=True, timeout=40)
