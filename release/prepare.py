import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import shutil
import struct
import tarfile
import zipfile


VERSION = "0.1.0"
BASELINE = "a1cae005c39be7b18ba319fced856b68d7276271"
ARCHIVES = {
    "windows": f"LanLink-{VERSION}-windows-x64.zip",
    "relay": f"LanLink-relay-{VERSION}-oraclelinux9-x86_64.tar.gz",
}
COMMON = {"build-info.json", "INSTALL.md", "RELEASE-NOTES.md"}
REQUIRED = {
    "windows": COMMON | {"bin/lanlink-service.exe", "bin/lanlink-ui.exe",
        "bin/lanlink-ui-cli.exe", "bin/wintun.dll", "bin/msquic.dll",
        "Install-LanLink.ps1", "Uninstall-LanLink.ps1", "ClientSetup.psm1",
        "package-version.txt", "licenses/Wintun.txt", "licenses/msquic.txt",
        "licenses/openssl.txt", "licenses/sqlite3.txt", "licenses/imgui.txt"},
    "relay": COMMON | {"bin/lanlink-relay", "install-relay.sh", "uninstall-relay.sh",
        "lanlink-relay.service", "relay-version.txt", "licenses/msquic.txt",
        "licenses/openssl.txt", "licenses/sqlite3.txt"},
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def safe_name(name):
    path = PurePosixPath(name)
    require(name and not path.is_absolute() and ".." not in path.parts and
            "\\" not in name and ":" not in name, "unsafe archive path")
    return path


def read_archive(path):
    files = {}
    links = {}
    total = 0

    def add(name, data=None, link=None, size=0):
        nonlocal total
        normalized = str(safe_name(name))
        require(normalized not in files and normalized not in links, "duplicate archive entry")
        require(size <= 64 * 1024 * 1024, "oversized package entry")
        total += size
        require(total <= 128 * 1024 * 1024 and len(files) + len(links) < 4096,
                "package exceeds release limits")
        if link is not None:
            safe_name(link)
            target = posixpath.normpath(posixpath.join(posixpath.dirname(normalized), link))
            require(PurePosixPath(target).parent == PurePosixPath(normalized).parent,
                    "unsafe runtime library link")
            links[normalized] = target
        else:
            files[normalized] = data()

    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            for entry in archive.infolist():
                safe_name(entry.filename)
                if entry.is_dir():
                    continue
                require((entry.external_attr >> 16) & 0o170000 != 0o120000,
                        "client archive contains a symbolic link")
                add(entry.filename, lambda entry=entry: archive.read(entry), size=entry.file_size)
    else:
        with tarfile.open(path, "r:gz") as archive:
            for entry in archive:
                safe_name(entry.name)
                if entry.isdir():
                    continue
                if entry.issym():
                    add(entry.name, link=entry.linkname)
                else:
                    require(entry.isfile(), "relay archive contains a special file")
                    add(entry.name, lambda entry=entry: archive.extractfile(entry).read(), size=entry.size)
    require(files, "empty package")
    root = None
    if not any(len(PurePosixPath(name).parts) == 1 for name in files):
        roots = {PurePosixPath(name).parts[0] for name in set(files) | set(links)}
        require(len(roots) == 1, "package must be flat or have one top directory")
        root = next(iter(roots))
    for target in links.values():
        seen = set()
        while target in links:
            require(target not in seen, "cyclic runtime library link")
            seen.add(target)
            target = links[target]
        require(target in files, "runtime library link has no target")
    def relative(name):
        return str(PurePosixPath(name).relative_to(root)) if root else name

    result = {relative(name): data for name, data in files.items()}
    links = {relative(name): relative(target) for name, target in links.items()}
    names = set(result) | set(links)
    require(len({name.casefold() for name in names}) == len(names), "case-colliding package entries")
    require(all(PurePosixPath(name).parts[0] == "lib" for name in links),
            "symbolic links must be runtime libraries")
    for name in names:
        parts = PurePosixPath(name).parts
        require(not ({"data", "logs", ".git", ".vcpkg", "tests"} & set(parts)) and
                PurePosixPath(name).suffix not in {".db", ".log", ".pem", ".key", ".identity", ".token"},
                "package contains private state or build files")
    return result, links


def inspect_package(path, kind, commit, require_clean=True, distribution=None, require_licenses=True):
    files, links = read_archive(path)
    required = REQUIRED[kind]
    if not require_licenses:
        required = {name for name in required if not name.startswith("licenses/")}
    require(required <= files.keys(), f"incomplete {kind} package: {sorted(required - files.keys())}")
    require(all(files[name] for name in required), "empty required package file")
    metadata = json.loads(files["build-info.json"])
    require(metadata["version"] == VERSION and metadata["source_commit"] == commit,
            "package version or source commit mismatch")
    require(not require_clean or metadata["source_clean"] is True, "package source tree was dirty")
    require(metadata["architecture"] == "x64" and metadata["protocol_version"] == 5,
            "package architecture or protocol mismatch")
    require(metadata["vcpkg_manifest"]["builtin-baseline"] == BASELINE and
            metadata["vcpkg_manifest"]["version-string"] == VERSION, "dependency baseline mismatch")
    if kind == "windows":
        require(metadata["system"] == "Windows", "client was not built on Windows")
        version_file = "package-version.txt"
        for name, data in files.items():
            if name.endswith((".exe", ".dll")):
                require(len(data) >= 64 and data[:2] == b"MZ", "invalid Windows binary")
                offset = struct.unpack_from("<I", data, 60)[0]
                require(offset <= len(data) - 6 and data[offset:offset + 4] == b"PE\0\0" and
                        struct.unpack_from("<H", data, offset + 4)[0] == 0x8664,
                        "Windows binary is not x64")
    else:
        require(metadata["system"] == "Linux" and metadata["distribution"] ==
                (distribution or "oraclelinux9"), "relay distribution mismatch")
        version_file = "relay-version.txt"
        binary = files["bin/lanlink-relay"]
        require(len(binary) >= 20 and binary[:6] == b"\x7fELF\x02\x01" and
                struct.unpack_from("<H", binary, 18)[0] == 62, "relay binary is not ELF x64")
        require(any("libmsquic.so" in name for name in set(files) | set(links)),
                "relay runtime library is missing")
    require(files[version_file].decode().strip() == VERSION, "installed version file mismatch")
    return {"name": Path(path).name, "sha256": digest(path), "size_bytes": Path(path).stat().st_size,
            "file_count": len(files) + len(links), "build": metadata}


def prepare(directory, commit):
    require(re.fullmatch(r"[0-9a-f]{40}", commit), "a full source commit is required")
    directory = Path(directory)
    found = {p.name for p in directory.iterdir() if p.name.endswith((".zip", ".tar.gz"))}
    require(found == set(ARCHIVES.values()), "release must contain exactly the two supported archives")
    assets = [inspect_package(directory / name, kind, commit) for kind, name in ARCHIVES.items()]
    manifest = {"version": VERSION, "source_commit": commit, "protocol_version": 5,
                "vcpkg_baseline": BASELINE, "assets": assets}
    (directory / "release-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    shutil.copyfile(Path(__file__).with_name("INSTALL.md"), directory / "INSTALL.md")
    checksummed = sorted(list(ARCHIVES.values()) + ["release-manifest.json", "INSTALL.md"])
    (directory / "SHA256SUMS").write_text("".join(
        f"{digest(directory / name)}  {name}\n" for name in checksummed))
    verify(directory, commit)
    return manifest


def verify(directory, commit):
    directory = Path(directory)
    entries = {}
    for line in (directory / "SHA256SUMS").read_text().splitlines():
        checksum, name = line.split("  ", 1)
        require(re.fullmatch(r"[0-9a-f]{64}", checksum) and
                name == PurePosixPath(name).name and name not in entries, "invalid checksum entry")
        entries[name] = checksum
        require(digest(directory / name) == checksum, f"checksum mismatch for {name}")
    require(set(entries) == set(ARCHIVES.values()) | {"release-manifest.json", "INSTALL.md"},
            "release checksum list is incomplete")
    manifest = json.loads((directory / "release-manifest.json").read_text())
    require(manifest["source_commit"] == commit and manifest["version"] == VERSION,
            "published release provenance mismatch")
    require({asset["name"] for asset in manifest["assets"]} == set(ARCHIVES.values()),
            "release manifest is incomplete")
    for asset in manifest["assets"]:
        require(asset["sha256"] == entries[asset["name"]] and
                asset["size_bytes"] == (directory / asset["name"]).stat().st_size,
                "release manifest hash or size mismatch")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--commit", required=True)
    arguments = parser.parse_args()
    result = prepare(arguments.directory, arguments.commit)
    print(f"Validated LanLink {VERSION}, commit {result['source_commit']}, {len(result['assets'])} packages")
