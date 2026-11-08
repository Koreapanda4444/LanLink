import hashlib
import json
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


def shared_port(used=()):
    for _ in range(128):
        with socket.socket() as tcp:
            tcp.bind(("127.0.0.1", 0))
            port = tcp.getsockname()[1]
            if port in used:
                continue
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                try:
                    udp.bind(("127.0.0.1", port))
                except OSError:
                    continue
            return port
    raise RuntimeError("No shared TCP/UDP test port available")


def test_regional_tools(package, workspace, certificate, private_key, token):
    relay = package / "bin/lanlink-relay"
    signer = package / "bin/lanlink-directory-sign"
    generator = package / "self-hosting/generate.py"
    state = package / "self-hosting/state.py"
    version = (package / "relay-version.txt").read_text().strip()
    require(run([str(relay), "--version"]).stdout.strip() == version and run([str(signer), "--version"]).stdout.strip() == version, "packaged deployment tools have inconsistent versions")
    roots, nodes, used = {}, [], set()
    for name, role in (("home", "authority"), ("near", "region"), ("far", "region")):
        root = workspace / (name + " regional root")
        roots[name] = root
        health = root / "var/lib/lanlink/relay-health.identity"
        config = workspace / (name + "-identity.conf")
        config.write_text(f"tls_certificate_file={certificate}\ntls_private_key_file={private_key}\nrelay_id={name}\nrelay_authority=package-home\nrelay_health_identity_file={health}\n")
        run([str(relay), "--check-config", str(config)])
        require(not health.exists(), "configuration checks must not create identities or start listeners")
        key = run([str(relay), "--health-key", str(config)]).stdout.strip()
        port = shared_port(used);used.add(port)
        nodes.append(dict(id=name, role=role, host="127.0.0.1", port=port, region=name, capacity=32 if role == "authority" else 16,
                          health_public_key=key, listen_host="127.0.0.1"))
    inventory = workspace / "inventory.json"
    spec = dict(authority="package-home", revision=7, directory_url="https://directory.example.com/v1/relays", nodes=nodes)
    inventory.write_text(json.dumps(spec))
    output = workspace / "rollout"
    identity = workspace / "offline-directory.identity"
    arguments = [sys.executable, str(generator), str(inventory), str(output), "--relay", str(relay), "--signer", str(signer), "--identity", str(identity)]
    run(arguments)
    manifest = json.loads((output / "deployment.json").read_text())
    require(manifest["version"] == version and manifest["revision"] == 7 and manifest["rollout_order"] == ["home", "near", "far"], "deployment provenance or ordering mismatch")
    for name, expected in manifest["files"].items():
        require(digest(output / name) == expected, "deployment checksum mismatch")
    require(not any(p.suffix in (".identity", ".token", ".key", ".db") for p in output.rglob("*") if p.is_file()), "rollout contains private state")
    run([str(signer), "--verify", manifest["directory_public_key"], str(output / "relays.bin")])
    original = digest(output / "relays.bin")
    run(arguments, accepted=False)
    require(digest(output / "relays.bin") == original, "existing rollout was overwritten")
    second = workspace / "rollout-second"
    repeated = list(arguments);repeated[3] = str(second);run(repeated)
    for node in nodes:
        require((output / node["id"] / "relay.conf").read_bytes() == (second / node["id"] / "relay.conf").read_bytes(), "same inventory did not reproduce node configuration")
    invalid = json.loads(json.dumps(spec));invalid["nodes"][1]["id"] = "../escape"
    bad_inventory = workspace / "invalid-inventory.json";bad_inventory.write_text(json.dumps(invalid))
    rejected = list(arguments);rejected[2] = str(bad_inventory);rejected[3] = str(workspace / "rejected-rollout")
    run(rejected, accepted=False)
    require(not (workspace / "rejected-rollout").exists(), "invalid inventory left a partial rollout")
    invalid["nodes"][1]["id"] = "near"
    invalid["directory_url"] = "https://directory.example.com/" + "x" * 2048
    bad_inventory.write_text(json.dumps(invalid))
    run(rejected, accepted=False)
    require(not (workspace / "rejected-rollout").exists(), "unusable bootstrap URL left a partial rollout")
    private_output = workspace / "private-in-output"
    private_arguments = list(arguments)
    private_arguments[3] = str(private_output)
    private_arguments[-1] = str(private_output / "directory.identity")
    run(private_arguments, accepted=False)
    require(not private_output.exists(), "private signing key was written inside deployment output")
    corrupt = workspace / "corrupt-catalog.bin"
    data = bytearray((output / "relays.bin").read_bytes());data[30] ^= 1;corrupt.write_bytes(data)
    run([str(signer), "--verify", manifest["directory_public_key"], str(corrupt)], accepted=False)
    pin = output / "near/health-public-key.txt"
    saved_pin = pin.read_text();pin.write_text("11" * 32 + "\n")
    region_args = ["bash", str(package / "install-relay.sh"), "--root", str(roots["near"]), "--node-config", str(output / "near/relay.conf"),
                   "--certificate", str(certificate), "--private-key", str(private_key)]
    private_hash = digest(roots["near"] / "var/lib/lanlink/relay-health.identity")
    run(region_args, accepted=False)
    require(not (roots["near"] / "opt/lanlink").exists() and digest(roots["near"] / "var/lib/lanlink/relay-health.identity") == private_hash, "pin mismatch modified installation or identity")
    pin.write_text(saved_pin)
    run(region_args + ["--token-file", str(token)], accepted=False)
    for node in nodes:
        args = ["bash", str(package / "install-relay.sh"), "--root", str(roots[node["id"]]), "--node-config", str(output / node["id"] / "relay.conf"),
                "--certificate", str(certificate), "--private-key", str(private_key)]
        if node["role"] == "authority":
            args.extend(["--token-file", str(token)])
        run(args)
        root = roots[node["id"]]
        preserved = [root / "etc/lanlink/relay.conf", root / "var/lib/lanlink/relay-health.identity"]
        before = [digest(p) for p in preserved]
        run(["bash", str(package / "install-relay.sh"), "--root", str(root)])
        require([digest(p) for p in preserved] == before, "regional upgrade changed config or node identity")
        if node["role"] == "region":
            require(not (root / "etc/lanlink/auth.token").exists() and not (root / "var/lib/lanlink/relay.db").exists(), "regional installation copied central credentials or database")
    home_config = roots["home"] / "etc/lanlink/relay.conf"
    home_hash = digest(home_config)
    run(["bash", str(package / "install-relay.sh"), "--root", str(roots["home"]), "--node-config", str(output / "near/relay.conf")], accepted=False)
    require(digest(home_config) == home_hash, "role-changing installation modified the authority")
    environment = dict(os.environ, SSL_CERT_FILE=str(certificate))
    processes, logs = {}, []
    def launch(node):
        root = roots[node["id"]]
        config = workspace / (node["id"] + "-runtime.conf")
        settings = {}
        for line in (root / "etc/lanlink/relay.conf").read_text().splitlines():
            key, value = line.split("=", 1)
            settings[key] = str(root / value.lstrip("/")) if value.startswith("/") else value
        config.write_text("".join(f"{key}={value}\n" for key, value in settings.items()))
        log = (workspace / (node["id"] + "-regional-process.log")).open("a+");logs.append(log)
        process = subprocess.Popen([str(root / "opt/lanlink/bin/lanlink-relay"), str(config)], env=environment, stdout=log, stderr=subprocess.STDOUT)
        processes[node["id"]] = process
    def stop(name):
        process = processes.pop(name, None)
        if process and process.poll() is None:
            process.terminate()
            try:
                require(process.wait(timeout=10) == 0, "regional process did not stop cleanly")
            except subprocess.TimeoutExpired:
                process.kill();process.wait(timeout=5);raise
    def ready(selected, expected=True):
        deadline = time.monotonic() + 14
        while time.monotonic() < deadline:
            valid = True
            for node in selected:
                result = subprocess.run([str(relay), "--probe", node["host"], str(node["port"]), node["id"], "package-home", str(node["capacity"]), node["health_public_key"]],
                                        env=environment, text=True, capture_output=True, timeout=8)
                valid &= result.returncode == (0 if expected else 2)
            if valid:
                return
            time.sleep(0.1)
        raise AssertionError("packaged regional readiness or failure recovery did not converge")
    backup = workspace / "home-private-backup"
    writer = None
    try:
        for node in nodes:
            launch(node)
        ready(nodes)
        require(all(not (roots[n["id"]] / "var/lib/lanlink/relay.db").exists() for n in nodes[1:]), "regional process created an independent authority database")
        writer = sqlite3.connect(str(roots["home"] / "var/lib/lanlink/relay.db"))
        writer.execute("PRAGMA journal_mode=WAL")
        writer.execute("CREATE TABLE regional_fixture (value TEXT)")
        writer.execute("INSERT INTO regional_fixture VALUES ('snapshot before mutation')");writer.commit()
        require((roots["home"] / "var/lib/lanlink/relay.db-wal").stat().st_size > 0, "WAL snapshot fixture was not live")
        run([sys.executable, str(state), "backup", str(home_config), str(backup), "--root", str(roots["home"])])
        run([sys.executable, str(state), "verify", str(backup)])
        require(stat.S_IMODE(backup.stat().st_mode) == 0o700 and all(stat.S_IMODE(p.stat().st_mode) == 0o600 for p in backup.rglob("*") if p.is_file()), "private backup permissions are too broad")
        writer.execute("UPDATE regional_fixture SET value='changed after backup'");writer.commit();writer.close();writer = None
        region_backup = workspace / "region-private-backup"
        run([sys.executable, str(state), "backup", str(roots["near"] / "etc/lanlink/relay.conf"), str(region_backup), "--root", str(roots["near"])])
        require(not ({"database", "auth_token"} & set(json.loads((region_backup / "manifest.json").read_text())["files"])), "regional backup contains authority state")
        stop("home");ready(nodes[1:], False)
        launch(nodes[0]);ready(nodes)
        stop("near");launch(nodes[1]);ready(nodes)
    finally:
        if writer:
            writer.close()
        for name in list(processes):
            stop(name)
        for log in logs:
            log.close()
    restored = workspace / "recovered authority"
    run([sys.executable, str(state), "restore", str(backup), "--root", str(restored)], accepted=False)
    require(not restored.exists(), "unfenced restore wrote state")
    run([sys.executable, str(state), "restore", str(backup), "--root", str(restored), "--fenced"])
    with sqlite3.connect(str(restored / "var/lib/lanlink/relay.db")) as database:
        require(database.execute("SELECT value FROM regional_fixture").fetchone()[0] == "snapshot before mutation", "online WAL snapshot was not restored consistently")
    require(digest(restored / "var/lib/lanlink/relay-health.identity") == digest(roots["home"] / "var/lib/lanlink/relay-health.identity"), "restore changed the signed node identity")
    before = digest(restored / "var/lib/lanlink/relay.db")
    run([sys.executable, str(state), "restore", str(backup), "--root", str(restored), "--fenced"], accepted=False)
    require(digest(restored / "var/lib/lanlink/relay.db") == before, "restore overwrote existing state")
    run(["bash", str(package / "install-relay.sh"), "--root", str(restored)])
    run_relay(restored, workspace, nodes[0]["port"])
    damaged = workspace / "damaged-backup";shutil.copytree(backup, damaged)
    (damaged / "payload/auth_token").write_text("tampered private state")
    run([sys.executable, str(state), "verify", str(damaged)], accepted=False)
    run([sys.executable, str(state), "restore", str(damaged), "--root", str(workspace / "rejected-restore"), "--fenced"], accepted=False)
    require(not (workspace / "rejected-restore").exists(), "corrupt backup modified restore target")
    print("Generated catalogs, real node pins, three packaged processes, recovery, online WAL snapshot and fenced restore passed.")


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
        port = shared_port()
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
        for setting in ("User=lanlink-relay\n", "Group=lanlink-relay\n", "Restart=on-failure\n",
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
        test_regional_tools(package, workspace, certificate, private_key, token)
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
