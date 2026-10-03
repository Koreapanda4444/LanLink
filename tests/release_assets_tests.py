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
        result["lib/libmsquic.so.2.4.8"] = linux_binary()
    result["package-version.txt" if kind == "windows" else "relay-version.txt"] = b"0.1.0\n"
    result["build-info.json"] = json.dumps({
        "version": "0.1.0", "source_commit": COMMIT, "source_clean": True,
        "protocol_version": 5, "system": "Windows" if kind == "windows" else "Linux",
        "distribution": "Windows" if kind == "windows" else "oraclelinux9", "architecture": "x64",
        "vcpkg_manifest": {"builtin-baseline": prepare.BASELINE, "version-string": "0.1.0"},
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


class ReleaseAssetsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="lanlink-release-tests-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)

    def test_release_provenance_and_checksums(self):
        for kind in prepare.ARCHIVES:
            write_package(self.directory, kind)
        manifest = prepare.prepare(self.directory, COMMIT)
        self.assertEqual(manifest["source_commit"], COMMIT)
        self.assertEqual(len(manifest["assets"]), 2)
        self.assertEqual(len((self.directory / "SHA256SUMS").read_text().splitlines()), 4)
        self.assertEqual(prepare.verify(self.directory, COMMIT), manifest)
        with (self.directory / prepare.ARCHIVES["windows"]).open("ab") as destination:
            destination.write(b"changed after validation")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            prepare.verify(self.directory, COMMIT)

    def test_package_metadata_mismatch(self):
        for field, value in (("source_commit", "b" * 40), ("version", "0.2.0"),
                             ("source_clean", False), ("architecture", "arm64"),
                             ("protocol_version", 4), ("distribution", "ubuntu")):
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
        with self.assertRaisesRegex(ValueError, "two supported archives"):
            prepare.prepare(self.directory, COMMIT)

    def test_optional_top_directory(self):
        for kind in prepare.ARCHIVES:
            path = write_package(self.directory, kind, root="LanLink-package")
            self.assertEqual(prepare.inspect_package(path, kind, COMMIT)["build"]["version"], "0.1.0")

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
        prepare.prepare(self.directory, COMMIT)
        with mock.patch.object(publish, "gh", return_value='{"nameWithOwner":"owner/repository"}') as cli, \
             mock.patch.object(publish, "api", return_value={"object": {"type": "commit", "sha": "b" * 40}}):
            with self.assertRaisesRegex(ValueError, "different commit"):
                publish.main(self.directory, COMMIT)
            self.assertEqual(cli.call_count, 1)

    def test_publication_preserves_unrelated_draft(self):
        for kind in prepare.ARCHIVES:
            write_package(self.directory, kind)
        prepare.prepare(self.directory, COMMIT)
        with mock.patch.object(publish, "gh", return_value='{"nameWithOwner":"owner/repository"}') as cli, \
             mock.patch.object(publish, "api", side_effect=[None, {"draft": True, "target_commitish": "b" * 40}]):
            with self.assertRaisesRegex(ValueError, "another commit"):
                publish.main(self.directory, COMMIT)
            self.assertEqual(cli.call_count, 1)


if __name__ == "__main__":
    unittest.main()
