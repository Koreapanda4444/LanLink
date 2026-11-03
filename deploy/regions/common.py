from pathlib import Path
import hashlib
import subprocess


def require(condition, message):
    if not condition:
        raise ValueError(message)


def settings(path):
    data = Path(path).read_bytes()
    require(len(data) <= 1024 * 1024, "configuration exceeds 1 MiB")
    result = {}
    for line in data.decode("utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        key, separator, value = line.partition("=")
        key, value = key.strip(), value.strip()
        require(separator and key and value and key not in result, "invalid or duplicate configuration setting")
        result[key] = value
    return result


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(data)
    return value.hexdigest()


def regular(path):
    path = Path(path)
    require(path.is_file() and not any(p.is_symlink() for p in (path, *path.parents)), "expected a regular file without symbolic links: " + str(path))
    return path


def validate_node(config_path, installed_path, root, relay):
    import re
    import tempfile
    node = settings(regular(config_path))
    canonical = {"tls_certificate_file": "/etc/lanlink/server.cert", "tls_private_key_file": "/etc/lanlink/server.key",
                 "auth_token_file": "/etc/lanlink/auth.token", "relay_database_file": "/var/lib/lanlink/relay.db",
                 "relay_health_identity_file": "/var/lib/lanlink/relay-health.identity", "log_directory": "/var/log/lanlink"}
    require(all(node.get(key) == value for key, value in canonical.items()), "node configuration must use installed canonical paths")
    require(node.get("relay_id") and node.get("relay_authority"), "node identity is incomplete")
    if installed_path.exists() or installed_path.is_symlink():
        current = settings(regular(installed_path))
        require(bool(current.get("relay_upstream_host")) == bool(node.get("relay_upstream_host")), "changing an existing relay's authority/region role requires a separate migration")
        for key in ("relay_id", "relay_authority"):
            require(not current.get(key) or current[key] == node[key], "node identity cannot change during upgrade")
    public_path = regular(config_path.with_name("health-public-key.txt"))
    require(public_path.stat().st_size <= 128, "invalid public health key")
    public = public_path.read_text(encoding="ascii").strip().lower()
    require(re.fullmatch(r"[0-9a-f]{64}", public) and int(public, 16), "invalid public health key")
    health_file = regular(root / "var/lib/lanlink/relay-health.identity")
    checked = subprocess.run([str(relay), "--check-config", str(config_path)], capture_output=True, text=True, timeout=10)
    require(checked.returncode == 0, checked.stderr.strip() or "node configuration rejected")
    with tempfile.TemporaryDirectory(prefix="lanlink-node-check-") as temporary:
        config = Path(temporary) / "relay.conf"
        node["relay_health_identity_file"] = str(health_file)
        config.write_text("".join(f"{key}={value}\n" for key, value in node.items()), encoding="utf-8")
        config.chmod(0o600)
        checked = subprocess.run([str(relay), "--health-key", str(config)], capture_output=True, text=True, timeout=10)
        require(checked.returncode == 0 and checked.stdout.strip().lower() == public,
                "existing node health identity differs from the deployment public key")


if __name__ == "__main__":
    import sys
    try:
        require(len(sys.argv) == 5, "usage: common.py NODE_CONFIG INSTALLED_CONFIG ROOT RELAY_BINARY")
        validate_node(*(Path(value).absolute() for value in sys.argv[1:]))
    except (ValueError, OSError, UnicodeError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
