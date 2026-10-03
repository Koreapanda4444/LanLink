import os
from pathlib import Path
import select
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time


def free_port():
    with socket.socket() as tcp:
        tcp.bind(("127.0.0.1", 0))
        port = tcp.getsockname()[1]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", port))
        return port


class Proxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    block_on_close = False


class Forward(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            with socket.create_connection(("127.0.0.1", self.server.target), timeout=2) as upstream:
                peers = (self.request, upstream)
                for peer in peers:
                    peer.settimeout(2)
                while not self.server.stopping.is_set():
                    readable, _, _ = select.select(peers, [], [], 0.1)
                    for peer in readable:
                        data = peer.recv(16384)
                        if not data:
                            return
                        (upstream if peer is self.request else self.request).sendall(data)
        except OSError:
            return


def stop_process(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=6)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def scenario(openssl, relay_binary, client_binary, mode):
    with tempfile.TemporaryDirectory(prefix="lanlink-e2e-") as directory:
        root = Path(directory)
        subprocess.run([openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt",
                        "ec_paramgen_curve:P-256", "-nodes", "-days", "1", "-subj",
                        "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
                        "-keyout", str(root / "key.pem"), "-out", str(root / "cert.pem")],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        (root / "auth.token").write_text("lanlink-loopback-e2e-fixture-token\n")
        port = free_port()
        (root / "relay.conf").write_text(
            f"listen_host=127.0.0.1\nrelay_port={port}\n"
            f"tls_certificate_file={root / 'cert.pem'}\n"
            f"tls_private_key_file={root / 'key.pem'}\n"
            f"auth_token_file={root / 'auth.token'}\n"
            f"relay_database_file={root / 'relay.db'}\nlog_directory={root / 'logs'}\n")
        environment = dict(os.environ, SSL_CERT_FILE=str(root / "cert.pem"))
        relay = None
        client = None
        proxy = None
        proxy_thread = None
        with (root / "process.log").open("w+") as relay_log, (root / "client.log").open("w+") as client_log:
            def start():
                process = subprocess.Popen([relay_binary, str(root / "relay.conf")],
                                           stdout=relay_log, stderr=subprocess.STDOUT, env=environment)
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError("relay executable exited during startup")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            return process
                    except OSError:
                        time.sleep(0.02)
                stop_process(process)
                raise RuntimeError("relay executable did not listen")

            try:
                relay = start()
                client_port = port
                if mode == "tcp":
                    proxy = Proxy(("127.0.0.1", 0), Forward)
                    proxy.target = port
                    proxy.stopping = threading.Event()
                    client_port = proxy.server_address[1]
                    proxy_thread = threading.Thread(target=proxy.serve_forever, daemon=True)
                    proxy_thread.start()
                client = subprocess.Popen([client_binary, directory, str(client_port), mode],
                                          stdout=client_log, stderr=subprocess.STDOUT, env=environment)
                completed = 0
                deadline = time.monotonic() + 80
                while client.poll() is None:
                    if time.monotonic() > deadline:
                        raise RuntimeError(f"{mode} client scenario timed out")
                    request = root / "request"
                    if request.exists():
                        sequence, command = request.read_text().split()
                        if int(sequence) > completed:
                            if command == "stop":
                                stop_process(relay)
                                if relay.returncode != 0:
                                    raise RuntimeError("relay did not stop cleanly")
                                relay = None
                            elif command == "start":
                                relay = start()
                            else:
                                raise RuntimeError("invalid restart command")
                            completed = int(sequence)
                            (root / "completed").write_text(str(completed))
                    if relay is not None and relay.poll() is not None:
                        raise RuntimeError("relay crashed during the client scenario")
                    time.sleep(0.02)
                client_log.seek(0)
                print(client_log.read(), end="", flush=True)
                if client.returncode != 0:
                    for path in sorted((root / "logs").glob("*.log")) + sorted(root.glob("owner.log")):
                        print(path.name, path.read_text()[-6000:], flush=True)
                    raise RuntimeError(f"{mode} client scenario failed ({client.returncode})")
                if completed != 2:
                    raise RuntimeError("relay restart scenario was not completed")
            finally:
                stop_process(client)
                stop_process(relay)
                if proxy is not None:
                    proxy.stopping.set()
                    proxy.shutdown()
                    proxy.server_close()
                    proxy_thread.join(timeout=2)
                relay_log.seek(0)
                output = relay_log.read()
                if "lanlink-relay:" in output:
                    print(output, flush=True)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: relay_e2e.py openssl relay-binary client-binary")
    for mode in ("quic", "tcp"):
        scenario(*sys.argv[1:], mode)


if __name__ == "__main__":
    main()
