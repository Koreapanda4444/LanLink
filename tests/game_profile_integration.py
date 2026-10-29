import argparse
import pathlib
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=pathlib.Path, required=True)
    parser.add_argument("--maps", type=pathlib.Path, required=True)
    parser.add_argument("--probe", type=pathlib.Path, required=True)
    args = parser.parse_args()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as allocation:
        allocation.bind(("127.0.0.1", 0))
        port = allocation.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="lanlink-teeworlds-") as name:
        root = pathlib.Path(name)
        (root / "storage.cfg").write_text(f"add_path $CURRENTDIR\nadd_path {args.maps.resolve().parent}\n", encoding="utf-8")
        config = root / "server.cfg"
        config.write_text(f'sv_register 0\nbindaddr 127.0.0.1\nsv_port {port}\nsv_map dm1\nsv_name "LanLink compatibility verifier"\n', encoding="utf-8")
        with (root / "server.log").open("w+") as log:
            server = subprocess.Popen([str(args.server.resolve()), "-f", config.name], cwd=root, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 12
                while time.monotonic() < deadline:
                    if server.poll() is not None:
                        raise RuntimeError("Teeworlds server exited before the query")
                    result = subprocess.run([str(args.probe.resolve()), "127.0.0.1", str(port), "127.0.0.1"], capture_output=True, text=True, timeout=6)
                    if result.returncode == 0:
                        if "0.7.5" not in result.stdout or "LanLink compatibility verifier" not in result.stdout:
                            raise RuntimeError("unexpected game server identity")
                        print(result.stdout.strip())
                        return
                    time.sleep(0.1)
                raise RuntimeError("game query timed out: " + result.stderr)
            except Exception:
                log.flush()
                print((root / "server.log").read_text(encoding="utf-8", errors="replace")[-5000:])
                raise
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)


if __name__ == "__main__":
    main()
