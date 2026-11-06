import argparse
import hashlib
import grp
import json
import os
from pathlib import Path
import pwd
import re
import signal
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("package", type=Path)
parser.add_argument("--user", required=True)
args = parser.parse_args()
assert os.geteuid() == 0 and os.environ.get("GITHUB_ACTIONS") == "true", "Native installation fixture requires an isolated CI runner"
source = args.package.resolve()
metadata = json.loads((source / "build-info.json").read_text())
mac = sys.platform == "darwin"
if mac:
    prefix = Path("/Library/Application Support/LanLink")
    config = Path("/Library/Preferences/LanLink/client.conf")
    state = Path("/Library/Application Support/LanLinkData")
    logs = Path("/Library/Logs/LanLink")
    unit = Path("/Library/LaunchDaemons/com.lanlink.client.plist")
    desktop = Path("/Applications/LanLink.app")
else:
    prefix = Path("/opt/lanlink")
    config = Path("/etc/lanlink/client.conf")
    state = Path("/var/lib/lanlink")
    logs = Path("/var/log/lanlink")
    unit = Path("/etc/systemd/system/lanlink-client.service")
    desktop = Path("/usr/share/applications/lanlink.desktop")
assert all(not item.exists() and not item.is_symlink() for item in (prefix, config.parent, state, logs, unit, desktop)), "A pre-existing installation must not be touched"
user = pwd.getpwnam(args.user)
assert user.pw_uid != 0

def command(arguments, check=True, **kwargs):
    result = subprocess.run(arguments, check=False, text=True, capture_output=True, timeout=30, **kwargs)
    if check and result.returncode:
        raise RuntimeError(f"{arguments[0]} failed ({result.returncode}): {result.stdout}\n{result.stderr}")
    return result

def pid():
    if mac:
        result = command(["/bin/launchctl", "print", "system/com.lanlink.client"], check=False)
        match = re.search(r"\bpid\s*=\s*(\d+)", result.stdout)
        return int(match[1]) if match else None
    result = command(["/usr/bin/systemctl", "show", "--property=MainPID", "--value", "lanlink-client.service"], check=False)
    return int(result.stdout.strip() or "0") or None

def start():
    if mac:
        sys.path.insert(0, str(source))
        import client_setup
        client_setup.start_service(unit)
    else:
        command(["/usr/bin/systemctl", "start", "lanlink-client.service"])

def ready(previous=None):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        current = pid()
        if current and current != previous:
            result = command([str(prefix / "bin/lanlink-ui-cli"), "status"], check=False)
            if result.returncode == 0 and "authenticated:" in result.stdout:
                return current
        time.sleep(0.1)
    raise AssertionError("Installed native service did not become ready")

def as_user(account):
    groups = []
    if account.pw_name == args.user:
        control_group = grp.getgrnam("lanlink").gr_gid
        assert control_group in os.getgrouplist(account.pw_name, account.pw_gid), "Installer did not enroll the control user"
        # Darwin limits setgroups even when directory services report more groups.
        # Only the control group is needed to exercise this permission boundary.
        groups = [account.pw_gid, control_group]
    return command([str(prefix / "bin/lanlink-ui-cli"), "status"], check=False,
        user=account.pw_uid, group=account.pw_gid, extra_groups=groups)

with tempfile.TemporaryDirectory(prefix="lanlink-native-install-") as directory:
    token = Path(directory) / "auth.token"
    token.write_text("lanlink-native-unit-isolated-fixture-token-2026\n")
    token.chmod(0o600)
    install = [sys.executable, str(source / "install-client.py")]
    uninstall = [sys.executable, str(source / "uninstall-client.py")]
    try:
        command(install + ["--user", args.user])
        first = ready()
        print("Installed unconfigured service ready", flush=True)
        status = command([str(prefix / "bin/lanlink-ui-cli"), "status"])
        assert "server configuration: unconfigured" in status.stdout, status.stdout
        assert not (state / "auth.token").exists() and not (state / "device.identity").exists()
        os.kill(first, signal.SIGKILL)
        ready(previous=first)
        command(install + ["--no-start"])
        # App-owned registration is outside the installer. This existing native
        # fixture still exercises the configured transport/adapter lifecycle.
        (state / "auth.token").write_bytes(token.read_bytes())
        (state / "auth.token").chmod(0o600)
        with config.open("a") as output:
            output.write("relay_host=127.0.0.1\nrelay_port=9\n")
        start()
        first = ready()
        control = grp.getgrnam("lanlink").gr_gid
        socket_info = Path("/var/run/lanlink/control.sock").stat()
        assert socket_info.st_gid == control and socket_info.st_mode & 0o777 == 0o660, "Service control socket must be group-restricted"
        allowed = as_user(user)
        assert allowed.returncode == 0 and "authenticated:" in allowed.stdout, allowed.stderr
        print("Authorized control user passed", flush=True)
        denied = as_user(pwd.getpwnam("nobody"))
        assert denied.returncode != 0 and "Permission denied" in denied.stderr, denied.stderr
        print("Unauthorized control user denied", flush=True)
        identity = state / "device.identity"
        protected = (config, state / "auth.token", identity)
        before = {path: hashlib.sha256(path.read_bytes()).digest() for path in protected}
        assert all(path.stat().st_mode & 0o077 == 0 for path in protected)
        os.kill(first, signal.SIGKILL)
        assert ready(previous=first) != first
        assert all(hashlib.sha256(path.read_bytes()).digest() == value for path, value in before.items())
        print("Crash restart and identity preservation passed", flush=True)
        command(install + ["--no-start"])
        if mac:
            unloaded = command(["/bin/launchctl", "print", "system/com.lanlink.client"], check=False)
            assert unloaded.returncode != 0, "Upgrade with --no-start left the launchd job loaded: " + unloaded.stdout
            command(["/usr/bin/plutil", "-lint", str(unit)])
        assert all(hashlib.sha256(path.read_bytes()).digest() == value for path, value in before.items())
        start()
        ready()
        command(uninstall)
        assert not prefix.exists() and not unit.exists() and not desktop.exists() and not desktop.is_symlink()
        assert all(path.exists() for path in protected)
        command(install + ["--no-start"])
        command(uninstall + ["--purge"])
        assert all(not path.exists() for path in (prefix, config.parent, state, logs, unit, desktop))
        assert not Path("/var/run/lanlink/control.sock").exists()
    except BaseException:
        if logs.exists():
            for file in logs.glob("*.log"):
                print(file.name, file.read_text(errors="replace")[-8000:])
        if not mac:
            print(command(["/usr/bin/systemctl", "status", "lanlink-client.service"], check=False).stdout)
        raise
    finally:
        marker = prefix / "build-info.json"
        if marker.exists() and json.loads(marker.read_text()).get("source_commit") == metadata["source_commit"]:
            command(uninstall + ["--purge"], check=False)
        for folder in (config.parent, state, logs):
            if folder.exists() and not any(folder.iterdir()):
                folder.rmdir()
print("Native installed client: system service, group access, unauthorized-user denial, crash restart, identity-preserving upgrade and remove/purge passed")
