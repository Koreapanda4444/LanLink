import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

openssl, signer, probe, server = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="lanlink-directory-") as directory:
    root = Path(directory)
    subprocess.run([openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-days", "1", "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1", "-keyout", str(root / "key.pem"), "-out", str(root / "cert.pem")], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    key = subprocess.check_output([signer, str(root / "identity"), str(root / "catalog"), "test", "127.0.0.1", "4433"], text=True).strip().split("=", 1)[1]
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    process = subprocess.Popen([sys.executable, server, "--catalog", str(root / "catalog"), "--cert", str(root / "cert.pem"), "--key", str(root / "key.pem"), "--port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + 5
        while True:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                    break
            except OSError:
                if time.monotonic() >= deadline or process.poll() is not None:
                    raise RuntimeError("directory did not listen")
                time.sleep(0.02)
        url = f"https://127.0.0.1:{port}/v1/relays"
        environment = dict(os.environ, SSL_CERT_FILE=str(root / "cert.pem"))
        subprocess.run([probe, url, key], env=environment, check=True)
        if subprocess.run([probe, url, key], env=dict(os.environ, SSL_CERT_FILE=str(root / "missing.pem")), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            raise RuntimeError("untrusted HTTPS certificate accepted")
        data = bytearray((root / "catalog").read_bytes())
        data[30] ^= 1
        (root / "catalog").write_bytes(data)
        if subprocess.run([probe, url, key], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            raise RuntimeError("tampered catalog accepted")
        print("HTTPS certificate and catalog integrity passed")
    finally:
        process.terminate()
        process.wait(timeout=5)
