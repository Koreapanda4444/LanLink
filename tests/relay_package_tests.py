import hashlib
import os
import pathlib
import shutil
import signal
import socket
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(arguments, accepted=True, **kwargs):
    result = subprocess.run(arguments, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=30, **kwargs)
    if accepted:
        require(result.returncode == 0, result.stdout)
    else:
        require(result.returncode != 0, "Invalid installer input was accepted")
    return result


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_relay(root, workspace, port):
    configuration = root / "etc/lanlink/relay.conf"
    runtime_configuration = workspace / "runtime.conf"
    settings = {}
    for line in configuration.read_text().splitlines():
        key, value = line.split("=", 1)
        settings[key] = str(root / value.lstrip("/")) if value.startswith("/") else value
    runtime_configuration.write_text("".join(f"{key}={value}\n" for key, value in settings.items()))
    environment = os.environ.copy()
    environment.pop("LD_LIBRARY_PATH", None)
    executable = root / "opt/lanlink/bin/lanlink-relay"
    dependency_report = run(["ldd", str(executable)], env=environment).stdout
    require("not found" not in dependency_report, dependency_report)
    msquic_paths = [pathlib.Path(line.partition("=> ")[2].rsplit(" (", 1)[0].strip()).resolve()
                   for line in dependency_report.splitlines() if "libmsquic" in line and "=> " in line]
    require(msquic_paths and all(path.parent == root / "opt/lanlink/lib" for path in msquic_paths),
            "MsQuic must resolve from the installed package without LD_LIBRARY_PATH")
    with (workspace / "process.log").open("w+") as output:
        process = subprocess.Popen([str(executable), str(runtime_configuration)],
                                   cwd=root / "var/lib/lanlink", env=environment,
                                   stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    output.seek(0)
                    raise AssertionError(output.read())
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                raise AssertionError("The packaged relay did not open its TCP listener")
            process.send_signal(signal.SIGTERM)
            require(process.wait(timeout=10) == 0, "The relay must exit cleanly on SIGTERM")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
    require("stopped" in (root / "var/log/lanlink/relay.log").read_text(),
            "The relay must flush its stop event before exiting")


def test_package(archive):
    with tempfile.TemporaryDirectory(prefix="lanlink-relay-package-") as temporary:
        workspace = pathlib.Path(temporary)
        package = workspace / "package files"
        package.mkdir()
        run(["tar", "-xzf", str(archive), "-C", str(package)])
        installer = package / "install-relay.sh"
        root = workspace / "staging root"
        certificate = workspace / "server | cert.pem"
        private_key = workspace / "server | key.pem"
        token = workspace / "auth | token"
        run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
             "ec_paramgen_curve:P-256", "-nodes", "-days", "1", "-subj", "/CN=127.0.0.1",
             "-addext", "subjectAltName=IP:127.0.0.1", "-keyout", str(private_key),
             "-out", str(certificate)])
        token.write_text("too-short\n")
        arguments = ["bash", str(installer), "--root", str(root),
                     "--certificate", str(certificate), "--private-key", str(private_key),
                     "--token-file", str(token), "--bind", "127.0.0.1"]
        run(arguments, accepted=False)
        token.write_bytes(b"\x80" * 32)
        run(arguments, accepted=False)
        token.write_text("lanlink-package-test-token-0123456789\n")
        run(arguments + ["--port", "65536"], accepted=False)
        run(arguments + ["--port", "0"], accepted=False)
        run(arguments + ["--bind", "127.0.0.1\nrelay_port=1"], accepted=False)
        wrong_key = workspace / "wrong.key"
        run(["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt",
             "ec_paramgen_curve:P-256", "-out", str(wrong_key)])
        run(arguments + ["--private-key", str(wrong_key)], accepted=False)
        require(not (root / "opt/lanlink").exists(),
                "Rejected input must not leave an installed relay")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        run(arguments + ["--port", str(port)])
        configuration = root / "etc/lanlink/relay.conf"
        require(f"relay_port={port}\n" in configuration.read_text(), "The configured port was lost")
        configuration.write_text(configuration.read_text() + "log_max_files=5\n")
        for name in ("server.cert", "server.key", "auth.token", "relay.conf"):
            require(stat.S_IMODE((root / "etc/lanlink" / name).stat().st_mode) == 0o640,
                    "Configuration and credentials must use mode 0640")
        for name in ("etc/lanlink", "var/lib/lanlink", "var/log/lanlink"):
            require(stat.S_IMODE((root / name).stat().st_mode) == 0o750,
                    "Persistent directories must use mode 0750")
        database = root / "var/lib/lanlink/relay.db"
        with sqlite3.connect(str(database)) as connection:
            connection.execute("CREATE TABLE package_fixture (value TEXT)")
            connection.execute("INSERT INTO package_fixture VALUES ('persistent relay state')")
        log_marker = root / "var/log/lanlink/persistent.log"
        log_marker.write_text("persistent log fixture\n")
        preserved = [configuration, root / "etc/lanlink/server.cert", root / "etc/lanlink/server.key",
                     root / "etc/lanlink/auth.token", database, log_marker]
        hashes = [digest(path) for path in preserved]
        run(["bash", str(installer), "--root", str(root)])
        require([digest(path) for path in preserved] == hashes,
                "An upgrade must preserve credentials, port, tuning, database and logs")
        require(not list((root / "opt").glob(".lanlink-*")), "An upgrade left staging directories")
        unit = root / "etc/systemd/system/lanlink-relay.service"
        unit_content = unit.read_text()
        for setting in ("User=lanlink\n", "Group=lanlink\n", "Restart=on-failure\n",
                        "WantedBy=multi-user.target\n", "ProtectSystem=strict\n"):
            require(setting in unit_content, "The systemd unit lost its service policy")
        if shutil.which("systemd-analyze"):
            validation_unit = workspace / "lanlink-relay.service"
            validation_unit.write_text(unit_content.replace(
                "WorkingDirectory=/var/lib/lanlink", f"WorkingDirectory={root}/var/lib/lanlink").replace(
                "ExecStart=/opt/lanlink/bin/lanlink-relay /etc/lanlink/relay.conf",
                f'ExecStart="{root}/opt/lanlink/bin/lanlink-relay" "{configuration}"'))
            run(["systemd-analyze", "verify", str(validation_unit)])
        installed_binary = root / "opt/lanlink/bin/lanlink-relay"
        rollback_files = [installed_binary, unit, *preserved]
        upgrade_package = workspace / "upgrade files"
        shutil.copytree(package, upgrade_package)
        with (upgrade_package / "bin/lanlink-relay").open("ab") as binary:
            binary.write(b"\x00package-upgrade-fixture")
        configuration.write_text(configuration.read_text() + "invalid configuration line\n")
        rollback_hashes = [digest(path) for path in rollback_files]
        run(["bash", str(upgrade_package / "install-relay.sh"), "--root", str(root)], accepted=False)
        require([digest(path) for path in rollback_files] == rollback_hashes,
                "Failed upgrade must restore the prior program, unit, configuration and secrets")
        configuration.write_text(configuration.read_text().removesuffix("invalid configuration line\n"))
        run_relay(root, workspace, port)
        run_relay(root, workspace, port)
        with sqlite3.connect(str(database)) as connection:
            require(connection.execute("SELECT value FROM package_fixture").fetchone()[0]
                    == "persistent relay state", "Relay restarts must preserve the database")
        uninstaller = root / "opt/lanlink/uninstall-relay.sh"
        run(["bash", str(uninstaller), "--root", str(root)])
        require(not (root / "opt/lanlink").exists() and not unit.exists(),
                "Uninstall must remove the program and unit")
        require(all(path.exists() for path in preserved), "Uninstall must preserve persistent data")
        run(["bash", str(installer), "--root", str(root)])
        run(["bash", str(uninstaller), "--root", str(root), "--remove-data"])
        require(all(not (root / name).exists() for name in
                    ("opt/lanlink", "etc/lanlink", "var/lib/lanlink", "var/log/lanlink")),
                "Explicit data removal must remove the LanLink state")
        print("Relay package validation, install, upgrade, rollback, restart and uninstall passed.")


if __name__ == "__main__":
    require(len(sys.argv) == 2, "Usage: relay_package_tests.py PACKAGE.tar.gz")
    test_package(pathlib.Path(sys.argv[1]).resolve(strict=True))
