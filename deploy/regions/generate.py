import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
from urllib.parse import urlsplit
from common import digest, require

VERSION = "1.0.0"
LIMITS = {
    "relay_send_queue_bytes", "relay_send_queue_frames", "relay_control_requests_per_second",
    "relay_health_requests_per_second", "relay_device_bytes_per_second", "relay_network_bytes_per_second",
    "relay_authority_bytes_per_second", "relay_device_deliveries_per_second",
    "relay_network_deliveries_per_second", "relay_authority_deliveries_per_second",
}


def run(arguments):
    result = subprocess.run([str(v) for v in arguments], capture_output=True, text=True, timeout=30)
    require(result.returncode == 0, result.stderr.strip() or "deployment command failed")
    return result.stdout.strip()


def tag(value, limit=64):
    require(isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]{0," + str(limit - 1) + "}", value), "invalid inventory identifier")
    return value


def integer(value, maximum):
    require(type(value) is int and 1 <= value <= maximum, "invalid inventory number")
    return value


def generate(inventory, output, relay, signer, identity):
    require(inventory.stat().st_size <= 65536, "inventory exceeds 64 KiB")
    def unique(pairs):
        value = {}
        for key, item in pairs:
            require(key not in value, "duplicate inventory field")
            value[key] = item
        return value
    spec = json.loads(inventory.read_text(encoding="utf-8"), object_pairs_hook=unique)
    require(isinstance(spec, dict) and set(spec) <= {"authority", "revision", "directory_url", "nodes", "limits"}, "unknown inventory field")
    authority = tag(spec["authority"])
    revision = integer(spec["revision"], 2**64 - 1)
    url = urlsplit(spec["directory_url"])
    require(url.scheme == "https" and url.hostname and not url.username and not url.password and not url.fragment and not url.query, "directory URL must use HTTPS without credentials, query or fragment")
    require(len(spec["directory_url"]) <= 2048 and "\\" not in spec["directory_url"] and
            not any(c.isspace() or ord(c) < 32 or ord(c) == 127 for c in spec["directory_url"]), "invalid directory URL")
    nodes = spec["nodes"]
    require(isinstance(nodes, list) and 2 <= len(nodes) <= 33, "inventory requires one authority and 1 to 32 regional fronts")
    limits = spec.get("limits", {})
    require(isinstance(limits, dict) and set(limits) <= LIMITS, "unknown relay limit")
    for number in limits.values():
        integer(number, 2**32 - 1)
    ids, addresses, homes = set(), set(), []
    for node in nodes:
        require(isinstance(node, dict) and set(node) <= {"id", "role", "host", "port", "region", "capacity", "health_public_key", "listen_host", "advertise"}, "unknown node field")
        tag(node["id"]); tag(node["host"], 253); tag(node["region"], 32)
        integer(node["port"], 65535); integer(node["capacity"], 4096)
        require(node["role"] in ("authority", "region"), "invalid node role")
        require(re.fullmatch(r"[0-9a-fA-F]{64}", node["health_public_key"]) and int(node["health_public_key"], 16), "replace the health public-key placeholder with the node's real key")
        ipaddress.ip_address(node.get("listen_host", "0.0.0.0"))
        require(type(node.get("advertise", node["role"] == "region")) is bool, "advertise must be a boolean")
        require(node["id"] not in ids and (node["host"].lower(), node["port"]) not in addresses, "duplicate node ID or endpoint")
        ids.add(node["id"]); addresses.add((node["host"].lower(), node["port"]))
        if node["role"] == "authority":
            homes.append(node)
    require(len(homes) == 1, "exactly one persistent authority is required")
    home = homes[0]
    advertised = [n for n in nodes if n.get("advertise", n["role"] == "region")]
    require(1 <= len(advertised) <= 32, "directory must advertise 1 to 32 nodes")
    require(not output.exists() and not output.is_symlink(), "output directory already exists; use a new rollout directory")
    require(not identity.resolve().is_relative_to(output.resolve()), "keep the private directory signing identity outside deployment output")
    require(run([relay, "--version"]) == VERSION and run([signer, "--version"]) == VERSION, "deployment tools must match version " + VERSION)
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".lanlink-deploy-", dir=output.parent))
    try:
        for node in nodes:
            target = stage / node["id"]
            target.mkdir(mode=0o750)
            config = {
                "listen_host": node.get("listen_host", "0.0.0.0"), "relay_port": node["port"],
                "tls_certificate_file": "/etc/lanlink/server.cert", "tls_private_key_file": "/etc/lanlink/server.key",
                "auth_token_file": "/etc/lanlink/auth.token", "relay_database_file": "/var/lib/lanlink/relay.db",
                "log_directory": "/var/log/lanlink", "relay_id": node["id"], "relay_authority": authority,
                "relay_health_identity_file": "/var/lib/lanlink/relay-health.identity",
                "relay_max_connections": node["capacity"], "relay_max_tls_handshakes": min(16, node["capacity"]),
            }
            config.update(limits)
            if node["role"] == "region":
                config.update(relay_upstream_host=home["host"], relay_upstream_port=home["port"],
                              relay_upstream_id=home["id"], relay_upstream_capacity=home["capacity"],
                              relay_upstream_health_public_key=home["health_public_key"])
            path = target / "relay.conf"
            path.write_text("".join(f"{key}={value}\n" for key, value in config.items()), encoding="utf-8")
            path.chmod(0o640)
            (target / "health-public-key.txt").write_text(node["health_public_key"].lower() + "\n", encoding="ascii")
            run([relay, "--check-config", path])
        arguments = [signer, "--regional", "self_hosted", str(revision), identity, stage / "relays.bin"]
        for node in advertised:
            arguments.extend([node["id"], node["host"], str(node["port"]), node["region"], str(node["capacity"]), authority, node["health_public_key"]])
        public = run(arguments)
        require(re.fullmatch(r"directory_public_key=[0-9a-f]{64}", public), "unexpected directory signer output")
        public_key = public.split("=", 1)[1]
        run([signer, "--verify", public_key, stage / "relays.bin"])
        data = (stage / "relays.bin").read_bytes()
        require(data[:4] == b"LLD2" and len(data) <= 16384 and data[28] == 2, "invalid self-hosted catalog")
        issued, expires, signed_revision = struct.unpack_from(">QQQ", data, 4)
        require(signed_revision == revision, "catalog revision mismatch")
        (stage / "client-bootstrap.conf").write_text(
            f"bootstrap_mode=self_hosted\ndirectory_url={spec['directory_url']}\ndirectory_public_key={public_key}\n", encoding="utf-8")
        hashes = {str(p.relative_to(stage)): digest(p) for p in sorted(stage.rglob("*")) if p.is_file()}
        manifest = {"schema": 1, "version": VERSION, "authority": authority, "revision": revision,
                    "issued_ms": issued, "expires_ms": expires, "directory_public_key": public_key,
                    "inventory_sha256": hashlib.sha256(json.dumps(spec, sort_keys=True, separators=(",", ":")).encode()).hexdigest(),
                    "rollout_order": [home["id"]] + [n["id"] for n in nodes if n["role"] == "region"], "files": hashes}
        (stage / "deployment.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        hashes["deployment.json"] = digest(stage / "deployment.json")
        (stage / "SHA256SUMS").write_text("".join(f"{value}  {name}\n" for name, value in sorted(hashes.items())), encoding="utf-8")
        require(not output.exists(), "output appeared during generation")
        os.rename(stage, output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    print(f"Prepared {len(nodes)} nodes; catalog revision {revision}, expires_ms={expires}. No servers were contacted.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Prepare a self-hosted regional rollout without contacting servers")
    parser.add_argument("inventory", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--relay", type=Path, required=True)
    parser.add_argument("--signer", type=Path, required=True)
    parser.add_argument("--identity", type=Path, required=True, help="persistent offline directory signing identity")
    args = parser.parse_args()
    try:
        generate(args.inventory.resolve(strict=True), args.output.absolute(), args.relay.resolve(strict=True), args.signer.resolve(strict=True), args.identity.absolute())
    except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + "\n")
