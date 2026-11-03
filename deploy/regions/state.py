import argparse
from contextlib import closing
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
import time
from common import digest, regular, require, settings

FILES = {
    "config": ("etc/lanlink/relay.conf", None, 0o640),
    "certificate": ("etc/lanlink/server.cert", "tls_certificate_file", 0o640),
    "private_key": ("etc/lanlink/server.key", "tls_private_key_file", 0o640),
    "auth_token": ("etc/lanlink/auth.token", "auth_token_file", 0o640),
    "database": ("var/lib/lanlink/relay.db", "relay_database_file", 0o600),
    "health_identity": ("var/lib/lanlink/relay-health.identity", "relay_health_identity_file", 0o600),
}


def layout(config):
    regional = bool(config.get("relay_upstream_host"))
    names = {"config", "certificate", "private_key"}
    if regional:
        require(config.get("relay_id") and config.get("relay_authority") and config.get("relay_health_identity_file"), "regional identity is incomplete")
    else:
        names.update(("auth_token", "database"))
    if config.get("relay_health_identity_file"):
        names.add("health_identity")
    for name in names - {"config"}:
        relative, field, _ = FILES[name]
        require(config.get(field) == "/" + relative, "state tool requires installed canonical paths: " + field)
    return names, "region" if regional else "authority"


def private_write(path, data):
    with Path(path).open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    Path(path).chmod(0o600)


def snapshot(source, destination, timeout):
    deadline = time.monotonic() + timeout
    def progress(status, remaining, total):
        if time.monotonic() > deadline:
            raise TimeoutError("SQLite backup deadline exceeded")
    with closing(sqlite3.connect(regular(source).as_uri() + "?mode=ro", uri=True, timeout=5)) as incoming:
        with closing(sqlite3.connect(str(destination), timeout=5)) as outgoing:
            incoming.backup(outgoing, pages=256, progress=progress, sleep=0.05)
            require(outgoing.execute("PRAGMA journal_mode=DELETE").fetchone()[0] == "delete", "backup journal was not consolidated")
            require(outgoing.execute("PRAGMA integrity_check").fetchall() == [("ok",)], "SQLite snapshot integrity check failed")
    destination.chmod(0o600)


def backup(config_path, output, root, timeout):
    require(1 <= timeout <= 300, "backup timeout must be between 1 and 300 seconds")
    require(not output.exists() and not output.is_symlink(), "backup output already exists")
    config = settings(regular(config_path))
    names, role = layout(config)
    sources = {name: config_path if name == "config" else root / FILES[name][0] for name in names}
    captured = {}
    for name, source in sources.items():
        if name != "database":
            require(regular(source).stat().st_size <= 1024 * 1024, "configuration or credential file exceeds 1 MiB")
            captured[name] = source.read_bytes()
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".lanlink-backup-", dir=output.parent))
    try:
        payload = stage / "payload"
        payload.mkdir(mode=0o700)
        for name, data in captured.items():
            private_write(payload / name, data)
        if "database" in names:
            snapshot(sources["database"], payload / "database", timeout)
        for name, data in captured.items():
            require(regular(sources[name]).read_bytes() == data, "configuration or credentials changed during backup; retry after rotation completes")
        version_path = root / "opt/lanlink/relay-version.txt"
        version = regular(version_path).read_text().strip() if version_path.exists() else "uninstalled"
        manifest = {"schema": 1, "created_ms": int(time.time() * 1000), "version": version,
                    "role": role, "relay_id": config.get("relay_id", ""), "authority": config.get("relay_authority", ""),
                    "files": {name: {"sha256": digest(payload / name), "size": (payload / name).stat().st_size} for name in sorted(names)}}
        private_write(stage / "manifest.json", (json.dumps(manifest, indent=2) + "\n").encode())
        require(not output.exists(), "backup output appeared during creation")
        os.rename(stage, output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    print("Created private state snapshot: " + str(output))


def verify(directory):
    manifest_file = regular(directory / "manifest.json")
    require(manifest_file.stat().st_size <= 1024 * 1024, "oversized backup manifest")
    manifest = json.loads(manifest_file.read_text())
    require(isinstance(manifest, dict), "invalid backup manifest")
    require(manifest.get("schema") == 1 and isinstance(manifest.get("files"), dict), "invalid backup manifest")
    config = settings(regular(directory / "payload/config"))
    names, role = layout(config)
    require(set(manifest["files"]) == names and {p.name for p in (directory / "payload").iterdir()} == names, "unexpected backup entries")
    require(manifest.get("role") == role and manifest.get("relay_id") == config.get("relay_id", "") and
            manifest.get("authority") == config.get("relay_authority", ""), "backup identity metadata mismatch")
    for name, metadata in manifest["files"].items():
        path = regular(directory / "payload" / name)
        require(path.stat().st_size == metadata["size"] and digest(path) == metadata["sha256"], "backup checksum mismatch: " + name)
    if "database" in names:
        with closing(sqlite3.connect((directory / "payload/database").as_uri() + "?mode=ro", uri=True)) as database:
            require(database.execute("PRAGMA integrity_check").fetchall() == [("ok",)], "backup database is corrupt")
            require(database.execute("PRAGMA journal_mode").fetchone()[0] == "delete", "backup contains an unresolved WAL journal")
    return manifest


def restore(directory, root, fenced):
    require(fenced, "stop the relay and fence every writer before passing --fenced")
    manifest = verify(directory)
    if root == Path("/"):
        require(os.geteuid() == 0, "live restore requires root")
        status = subprocess.run(["systemctl", "is-active", "lanlink-relay.service"], capture_output=True, text=True, timeout=10)
        require(status.stdout.strip() in ("inactive", "failed", "unknown"), "relay service must be stopped before restore")
    targets = [root / "etc/lanlink", root / "var/lib/lanlink"]
    require(all(not p.exists() and not p.is_symlink() for p in targets), "move the previous configuration and state directories to a private rollback location first")
    stages, published = [], []
    try:
        for target in targets:
            require(not any(p.is_symlink() for p in (target.parent, *target.parents)), "restore path contains a symbolic link")
            target.parent.mkdir(parents=True, exist_ok=True)
            stage = Path(tempfile.mkdtemp(prefix=".lanlink-restore-", dir=target.parent))
            stages.append((stage, target))
        for name, metadata in manifest["files"].items():
            relative, _, mode = FILES[name]
            index = 0 if relative.startswith("etc/") else 1
            target = stages[index][0] / Path(relative).name
            shutil.copyfile(regular(directory / "payload" / name), target)
            target.chmod(mode)
            require(digest(target) == metadata["sha256"], "backup changed while preparing restore")
        if root == Path("/"):
            import pwd
            try:
                account = pwd.getpwnam("lanlink")
            except KeyError:
                account = None
            if account:
                for index, (stage, _) in enumerate(stages):
                    os.chown(stage, 0 if index == 0 else account.pw_uid, account.pw_gid)
                    for path in stage.iterdir():
                        os.chown(path, 0 if index == 0 else account.pw_uid, account.pw_gid)
        for stage, target in stages:
            stage.chmod(0o750)
            require(not target.exists() and not target.is_symlink(), "restore target appeared during preparation")
            os.rename(stage, target)
            published.append((stage, target))
    except Exception:
        for stage, target in reversed(published):
            os.rename(target, stage)
        raise
    finally:
        for stage, _ in stages:
            if stage.exists():
                shutil.rmtree(stage)
    print("Restored state. Validate and install the matching package before starting the relay.")


if __name__ == "__main__":
    os.umask(0o077)
    parser = argparse.ArgumentParser(description="Private SQLite snapshots and fenced restore into empty state directories")
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("backup")
    create.add_argument("config", type=Path)
    create.add_argument("output", type=Path)
    create.add_argument("--timeout", type=float, default=30)
    create.add_argument("--root", type=Path, default=Path("/"))
    check = commands.add_parser("verify")
    check.add_argument("backup", type=Path)
    recover = commands.add_parser("restore")
    recover.add_argument("backup", type=Path)
    recover.add_argument("--root", type=Path, default=Path("/"))
    recover.add_argument("--fenced", action="store_true")
    args = parser.parse_args()
    try:
        if args.command == "verify":
            verify(args.backup.absolute())
            print("Backup checksums and SQLite snapshot verified")
        else:
            root = args.root.absolute()
            require(not any(p.is_symlink() for p in (root, *root.parents)), "state root contains a symbolic link")
            root = root.resolve()
            if args.command == "backup":
                backup(args.config.absolute(), args.output.absolute(), root, args.timeout)
            else:
                restore(args.backup.absolute(), root, args.fenced)
    except (ValueError, KeyError, TypeError, OSError, sqlite3.Error, subprocess.SubprocessError, TimeoutError) as error:
        parser.exit(1, str(error) + "\n")
