import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import zipfile

LABELS = ("windows", "linux", "macos")
STAGE, PLATFORM = sys.argv[1:3]
assert PLATFORM in LABELS
ROOT = Path(os.environ["RUNNER_TEMP"]) / f"lanlink-interop-{PLATFORM}-{os.environ['GITHUB_RUN_ATTEMPT']}"
PREFIX = f"LanLink-interop-{os.environ['GITHUB_RUN_ATTEMPT']}"
REPOSITORY, RUN = os.environ["GITHUB_REPOSITORY"], os.environ["GITHUB_RUN_ID"]

def api(endpoint, binary=False):
    result = subprocess.run(["gh", "api", f"repos/{REPOSITORY}/{endpoint}"], check=True, capture_output=True)
    return result.stdout if binary else json.loads(result.stdout)

def wait(marker):
    deadline = time.monotonic() + 60
    while not (ROOT / marker).exists():
        if (ROOT / "failed.txt").exists():
            raise RuntimeError((ROOT / "failed.txt").read_text())
        if time.monotonic() > deadline:
            raise TimeoutError(f"native peer did not reach {marker}")
        time.sleep(0.05)

def collect(kind):
    expected = {f"{PREFIX}-{kind}-{label}": label for label in LABELS}
    destination = ROOT / ("keys" if kind == "keys" else "incoming")
    destination.mkdir(exist_ok=True)
    deadline = time.monotonic() + 20 * 60
    while time.monotonic() < deadline:
        run = api(f"actions/runs/{RUN}")
        assert run["head_sha"] == os.environ["GITHUB_SHA"], "workflow provenance changed"
        artifacts = api(f"actions/runs/{RUN}/artifacts?per_page=100")["artifacts"]
        found = {item["name"]: item for item in artifacts if item["name"] in expected and not item["expired"]}
        if len(found) == len(expected):
            break
        jobs = api(f"actions/runs/{RUN}/attempts/{os.environ['GITHUB_RUN_ATTEMPT']}/jobs?per_page=100")["jobs"]
        failed = [job["name"] for job in jobs if job["conclusion"] in ("failure", "cancelled", "timed_out")]
        if failed:
            raise RuntimeError("Cross-platform exchange stopped after a failed CI job: " + ", ".join(failed))
        if int(os.environ["GITHUB_RUN_ATTEMPT"]) > 1:
            names = {job["name"] for job in jobs}
            if not {"windows-latest", "ubuntu-latest", "macOS native client"} <= names:
                raise RuntimeError("Rerun all jobs: the ephemeral key exchange requires all three native peers")
        if (ROOT / "failed.txt").exists():
            raise RuntimeError((ROOT / "failed.txt").read_text())
        time.sleep(5)
    else:
        raise TimeoutError("waiting for all three native platform artifacts")
    for name, item in found.items():
        assert item["size_in_bytes"] <= 2 * 1024 * 1024
        label = expected[name]
        archive = api(f"actions/artifacts/{item['id']}/zip", binary=True)
        assert len(archive) <= 2 * 1024 * 1024
        with zipfile.ZipFile(io.BytesIO(archive)) as source:
            entries = source.infolist()
            assert len(entries) == (1 if kind == "keys" else 8)
            for entry in entries:
                assert entry.file_size <= 65536 and not entry.is_dir()
                assert (entry.external_attr >> 16) & 0o170000 != 0o120000
                if kind == "keys":
                    assert entry.filename == f"{label}.state"
                else:
                    match = re.fullmatch(r"(windows|linux|macos)-to-(windows|linux|macos)\.(key|packet|next|proof)", entry.filename)
                    assert match and match[1] == label and match[1] != match[2]
                target = destination / entry.filename
                assert not target.exists(), "duplicate cross-platform artifact entry"
                target.write_bytes(source.read(entry))
    if kind == "keys":
        (ROOT / "keys.ready").touch()
        wait("outgoing.ready")
    else:
        (ROOT / "messages.ready").touch()
        wait("verified.ready")
        print((ROOT / "process.log").read_text())
        print(f"Verified {PLATFORM} against both other native operating systems without exporting private or group keys")

if STAGE == "start":
    ROOT.mkdir(mode=0o700)
    binary = Path(sys.argv[3]).resolve()
    with (ROOT / "process.log").open("w") as log:
        process = subprocess.Popen([str(binary), str(ROOT), PLATFORM], stdin=subprocess.DEVNULL, stdout=log, stderr=log)
    (ROOT / "process.pid").write_text(str(process.pid))
    with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as environment:
        environment.write(f"LANLINK_INTEROP_ROOT={ROOT}\n")
    wait("public.ready")
elif STAGE == "keys":
    collect("keys")
elif STAGE == "verify":
    collect("packets")
elif STAGE == "stop":
    if ROOT.exists():
        (ROOT / "stop").touch()
        if (ROOT / "process.log").exists():
            print((ROOT / "process.log").read_text())
else:
    raise ValueError("invalid interop stage")
