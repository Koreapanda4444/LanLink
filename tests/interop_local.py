from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

labels = ("windows", "linux", "macos")
with tempfile.TemporaryDirectory(prefix="lanlink-interop-local-") as directory:
    root = Path(directory)
    processes, logs = [], []
    def wait(marker):
        deadline = time.monotonic() + 20
        while not all((root / label / marker).exists() for label in labels):
            for label, process in zip(labels, processes):
                if (root / label / "failed.txt").exists():
                    raise RuntimeError((root / label / "failed.txt").read_text())
                assert process.poll() is None, f"local peer {label} exited before {marker}"
            assert time.monotonic() < deadline, f"local exchange timed out at {marker}"
            time.sleep(0.02)
    try:
        for label in labels:
            own = root / label
            own.mkdir(mode=0o700)
            log = (own / "process.log").open("w+")
            logs.append(log)
            processes.append(subprocess.Popen([sys.argv[1], str(own), label, "--local-fixture"], stdout=log, stderr=log))
        wait("public.ready")
        for label in labels:
            own = root / label
            (own / "keys").mkdir()
            for producer in labels:
                shutil.copyfile(root / producer / "public" / f"{producer}.state", own / "keys" / f"{producer}.state")
            (own / "keys.ready").touch()
        wait("outgoing.ready")
        for label in labels:
            own = root / label
            (own / "incoming").mkdir()
            for producer in labels:
                for file in (root / producer / "outgoing").iterdir():
                    shutil.copyfile(file, own / "incoming" / file.name)
            (own / "messages.ready").touch()
        for process in processes:
            assert process.wait(timeout=20) == 0
        assert all((root / label / "verified.ready").exists() for label in labels)
    except BaseException:
        for log in logs:
            log.flush()
            log.seek(0)
            print(log.read())
        raise
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
        for log in logs:
            log.close()
print("Local native fixture: three processes completed all six ordered cryptographic exchanges; foreign OS coverage requires the CI exchange")
