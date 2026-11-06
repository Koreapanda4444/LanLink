import io
import json
from pathlib import Path
import struct
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "release"))
import prepare
import publish


COMMIT = "a" * 40


def windows_binary(machine=0x8664):
    data = bytearray(160)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<H", data, 132, machine)
    return bytes(data)


def linux_binary(machine=62):
    data = bytearray(64)
    data[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", data, 18, machine)
    return bytes(data)


def fixture_files(kind):
    result = {name: b"package fixture\n" for name in prepare.REQUIRED[kind]}
    for name in result:
        if name.endswith((".exe", ".dll")):
            result[name] = windows_binary()
    if kind == "relay":
        result["bin/lanlink-relay"] = linux_binary()
        result["bin/lanlink-directory-sign"] = linux_binary()
        result["lib/libmsquic.so.2.4.8"] = linux_binary()
    result["package-version.txt" if kind == "windows" else "relay-version.txt"] = (prepare.VERSION + "\n").encode()
    result["build-info.json"] = json.dumps({
        "version": prepare.VERSION, "source_commit": COMMIT, "source_clean": True,
        "protocol_version": 5, "system": "Windows" if kind == "windows" else "Linux",
        "distribution": "Windows" if kind == "windows" else "linux", "architecture": "x64",
        "vcpkg_manifest": {"builtin-baseline": prepare.BASELINE, "version-string": prepare.VERSION},
        "dependency_policy": prepare.DEPENDENCY_POLICY, "msquic_port_sha256": prepare.QUIC_PORT_SHA256,
    }).encode()
    return result


def write_package(directory, kind, files=None, links=None, root=""):
    files = fixture_files(kind) if files is None else files
    path = directory / prepare.ARCHIVES[kind]
    def archive_name(name):
        return f"{root}/{name}" if root else name
    if kind == "windows":
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in files.items():
                archive.writestr(archive_name(name), data)
    else:
        with tarfile.open(path, "w:gz") as archive:
            for name, data in files.items():
                entry = tarfile.TarInfo(archive_name(name))
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
            for name, target in (links or {"lib/libmsquic.so.2": "libmsquic.so.2.4.8"}).items():
                entry = tarfile.TarInfo(archive_name(name))
                entry.type = tarfile.SYMTYPE
                entry.linkname = target
                archive.addfile(entry)
    return path


def write_installer(directory, commit=COMMIT):
    data = bytearray(windows_binary(0x014c))
    major, minor, patch = map(int, prepare.VERSION.split("."))
    data.extend(struct.pack("<IIII", 0xfeef04bd, 0x00010000, (major << 16) | minor, patch << 16))
    data.extend(commit.encode("utf-16-le"))
    (directory / prepare.INSTALLER).write_bytes(data)



def native_files(system, architecture):
    required = {"build-info.json", "INSTALL.md", "client_setup.py", "install-client.py", "uninstall-client.py",
                "bin/lanlink-service", "bin/lanlink-ui-cli"}
    required |= {f"licenses/{name}.txt" for name in ("msquic", "openssl", "sqlite3", "imgui", "sdl3", "libpng", "zlib")}
    if system == "Linux":
        required |= {"bin/lanlink-ui", "share/lanlink/lanlink.png", "lanlink.desktop", "lanlink-client.service",
                     "lib/libmsquic.so.2"}
        binaries = ("bin/lanlink-service", "bin/lanlink-ui-cli", "bin/lanlink-ui", "lib/libmsquic.so.2")
        binary = linux_binary()
    else:
        required |= {"LanLink.app/Contents/MacOS/LanLink", "LanLink.app/Contents/Info.plist",
                     "LanLink.app/Contents/Resources/LanLink.icns", "LanLink.app/Contents/Resources/lanlink.png",
                     "com.lanlink.client.plist", "lib/libmsquic.dylib"}
        binaries = ("bin/lanlink-service", "bin/lanlink-ui-cli", "LanLink.app/Contents/MacOS/LanLink", "lib/libmsquic.dylib")
        data = bytearray(64)
        data[:4] = b"\xcf\xfa\xed\xfe"
        struct.pack_into("<I", data, 4, 0x0100000c if architecture == "arm64" else 0x01000007)
        binary = bytes(data)
    files = {name: b"native release fixture\n" for name in required}
    for name in binaries:
        files[name] = binary
    files["build-info.json"] = json.dumps({
        "version": prepare.VERSION, "source_commit": COMMIT, "source_clean": True,
        "protocol_version": 5, "system": system, "architecture": architecture,
        "vcpkg_manifest": {"builtin-baseline": prepare.BASELINE, "version-string": prepare.VERSION},
        "dependency_policy": prepare.DEPENDENCY_POLICY, "msquic_port_sha256": prepare.QUIC_PORT_SHA256,
    }).encode()
    if system == "Darwin":
        files["LanLink.app/Contents/Info.plist"] = prepare.plistlib.dumps({
            "CFBundleIdentifier": "com.lanlink.desktop", "CFBundleShortVersionString": prepare.VERSION})
    return files


def write_native(directory, system, architecture, files=None):
    files = native_files(system, architecture) if files is None else files
    name = f"LanLink-{prepare.VERSION}-{'linux' if system == 'Linux' else 'macos'}-{architecture}"
    if system == "Linux":
        path = directory / (name + ".tar.gz")
        with tarfile.open(path, "w:gz") as archive:
            for name, data in files.items():
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
    else:
        path = directory / (name + ".zip")
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in files.items():
                archive.writestr(name, data)
    return path


def write_native_pair(directory):
    write_native(directory, "Linux", "x64")
    write_native(directory, "Darwin", "arm64")


class ReleaseAssetsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="lanlink-release-tests-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        write_installer(self.directory)

    def test_release_provenance_and_checksums(self):
        for kind in prepare.ARCHIVES:
            write_package(self.directory, kind)
        manifest = prepare.prepare(self.directory, COMMIT)
        self.assertEqual(manifest["source_commit"], COMMIT)
        self.assertEqual(len(manifest["assets"]), 3)
        self.assertEqual(len((self.directory / "SHA256SUMS").read_text().splitlines()), 5)
        self.assertEqual(prepare.verify(self.directory, COMMIT), manifest)
        with (self.directory / prepare.ARCHIVES["windows"]).open("ab") as destination:
            destination.write(b"changed after validation")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            prepare.verify(self.directory, COMMIT)

    def test_installer_provenance(self):
        self.assertEqual(prepare.inspect_installer(self.directory / prepare.INSTALLER, COMMIT)["build"]["version"], prepare.VERSION)
        with self.assertRaisesRegex(ValueError, "source commit"):
            prepare.inspect_installer(self.directory / prepare.INSTALLER, "b" * 40)
        (self.directory / prepare.INSTALLER).write_bytes(b"not an executable")
        with self.assertRaisesRegex(ValueError, "invalid Windows installer"):
            prepare.inspect_installer(self.directory / prepare.INSTALLER, COMMIT)

    def test_package_metadata_mismatch(self):
        for field, value in (("source_commit", "b" * 40), ("version", "99.0.0"),
                             ("source_clean", False), ("architecture", "arm64"),
                             ("protocol_version", 4), ("distribution", "ubuntu"),
                             ("dependency_policy", {}), ("msquic_port_sha256", "0" * 64)):
            with self.subTest(field=field):
                files = fixture_files("relay")
                info = json.loads(files["build-info.json"])
                info[field] = value
                files["build-info.json"] = json.dumps(info).encode()
                path = write_package(self.directory, "relay", files)
                with self.assertRaises(ValueError):
                    prepare.inspect_package(path, "relay", COMMIT)

    def test_wrong_binary_architecture(self):
        for kind in prepare.ARCHIVES:
            with self.subTest(kind=kind):
                files = fixture_files(kind)
                if kind == "windows":
                    files["bin/wintun.dll"] = windows_binary(0x014c)
                else:
                    files["bin/lanlink-relay"] = linux_binary(183)
                with self.assertRaisesRegex(ValueError, "x64"):
                    prepare.inspect_package(write_package(self.directory, kind, files), kind, COMMIT)

    def test_missing_runtime_and_license(self):
        for name in ("bin/msquic.dll", "licenses/Wintun.txt", "build-info.json"):
            files = fixture_files("windows")
            del files[name]
            with self.assertRaisesRegex(ValueError, "incomplete"):
                prepare.inspect_package(write_package(self.directory, "windows", files), "windows", COMMIT)

    def test_private_state_and_unsafe_paths(self):
        for name in ("data/auth.token", "auth.token", "server.key", "../outside.txt", "C:/outside.txt"):
            with self.subTest(name=name):
                files = fixture_files("windows")
                files[name] = b"private fixture"
                with self.assertRaises(ValueError):
                    prepare.inspect_package(write_package(self.directory, "windows", files), "windows", COMMIT)

    def test_runtime_library_links(self):
        path = write_package(self.directory, "relay")
        self.assertEqual(prepare.inspect_package(path, "relay", COMMIT)["build"]["system"], "Linux")
        for target in ("../../private.key", "/usr/lib/libmsquic.so", "missing.so", "libmsquic.so.2"):
            path = write_package(self.directory, "relay", links={"lib/libmsquic.so.2": target})
            with self.assertRaises(ValueError):
                prepare.inspect_package(path, "relay", COMMIT)

    def test_partial_release_is_rejected(self):
        write_package(self.directory, "windows")
        with self.assertRaisesRegex(ValueError, "three supported packages"):
            prepare.prepare(self.directory, COMMIT)

    def test_optional_top_directory(self):
        for kind in prepare.ARCHIVES:
            path = write_package(self.directory, kind, root="LanLink-package")
            self.assertEqual(prepare.inspect_package(path, kind, COMMIT)["build"]["version"], prepare.VERSION)

    def test_checksum_list_cannot_drop_a_package(self):
        for kind in prepare.ARCHIVES:
            write_package(self.directory, kind)
        prepare.prepare(self.directory, COMMIT)
        path = self.directory / "SHA256SUMS"
        path.write_text("\n".join(path.read_text().splitlines()[1:]) + "\n")
        with self.assertRaisesRegex(ValueError, "incomplete"):
            prepare.verify(self.directory, COMMIT)

    def test_publication_preserves_existing_tag(self):
        for kind in prepare.ARCHIVES:
            write_package(self.directory, kind)
        write_native_pair(self.directory)
        prepare.prepare(self.directory, COMMIT, native=True)
        with mock.patch.object(publish, "gh", return_value='{"nameWithOwner":"owner/repository"}') as cli, \
             mock.patch.object(publish, "api", return_value={"object": {"type": "commit", "sha": "b" * 40}}):
            with self.assertRaisesRegex(ValueError, "different commit"):
                publish.main(self.directory, COMMIT)
            self.assertEqual(cli.call_count, 1)

    def test_publication_preserves_unrelated_draft(self):
        for kind in prepare.ARCHIVES:
            write_package(self.directory, kind)
        write_native_pair(self.directory)
        prepare.prepare(self.directory, COMMIT, native=True)
        with mock.patch.object(publish, "gh", return_value='{"nameWithOwner":"owner/repository"}') as cli, \
             mock.patch.object(publish, "api", side_effect=[None, {"draft": True, "target_commitish": "b" * 40}]):
            with self.assertRaisesRegex(ValueError, "another commit"):
                publish.main(self.directory, COMMIT)
            self.assertEqual(cli.call_count, 1)


    def test_native_release_manifest_and_checksums(self):
        for kind in prepare.ARCHIVES:
            write_package(self.directory, kind)
        write_native_pair(self.directory)
        manifest = prepare.prepare(self.directory, COMMIT, native=True)
        self.assertEqual(len(manifest["assets"]), 5)
        self.assertEqual(len((self.directory / "SHA256SUMS").read_text().splitlines()), 7)
        self.assertEqual(prepare.verify(self.directory, COMMIT, native=True), manifest)
        (self.directory / f"LanLink-{prepare.VERSION}-macos-arm64.zip").unlink()
        with self.assertRaises((ValueError, FileNotFoundError)):
            prepare.verify(self.directory, COMMIT, native=True)

    def test_native_release_requires_both_platforms(self):
        for kind in prepare.ARCHIVES:
            write_package(self.directory, kind)
        write_native(self.directory, "Linux", "x64")
        with self.assertRaisesRegex(ValueError, "requires Linux and macOS"):
            prepare.prepare(self.directory, COMMIT, native=True)

    def test_native_wrong_architecture_and_provenance(self):
        for system, architecture, binary in (
                ("Linux", "x64", "bin/lanlink-service"),
                ("Darwin", "arm64", "LanLink.app/Contents/MacOS/LanLink")):
            with self.subTest(system=system):
                files = native_files(system, architecture)
                files[binary] = windows_binary()
                path = write_native(self.directory, system, architecture, files)
                with self.assertRaisesRegex(ValueError, "ELF x64|architecture"):
                    prepare.inspect_native_package(path, COMMIT)
                path = write_native(self.directory, system, architecture)
                with self.assertRaisesRegex(ValueError, "provenance"):
                    prepare.inspect_native_package(path, "b" * 40)

    def test_native_missing_license_and_private_state(self):
        for name in ("licenses/sdl3.txt", "licenses/libpng.txt"):
            files = native_files("Darwin", "arm64")
            del files[name]
            with self.assertRaisesRegex(ValueError, "incomplete"):
                prepare.inspect_native_package(write_native(self.directory, "Darwin", "arm64", files), COMMIT)
        files = native_files("Linux", "x64")
        files["auth.token"] = b"must never ship"
        with self.assertRaisesRegex(ValueError, "private state"):
            prepare.inspect_native_package(write_native(self.directory, "Linux", "x64", files), COMMIT)


if __name__ == "__main__":
    unittest.main()
