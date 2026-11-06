"""Publish the explicitly approved, already-verified 1.0.0 candidate only."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from prepare import ARCHIVES, INSTALLER, PACKAGES, inspect_installer, inspect_native_package, inspect_package, package_names, require, verify

REPOSITORY = "Koreapanda4444/LanLink"
SOURCE = "4f819c66786ee9d59593e793819dd228c78814ae"
RUN = 42137323832
TAG = "v1.0.0"


def gh(*arguments):
    return subprocess.run(["gh", *map(str, arguments)], check=True, capture_output=True, text=True).stdout


def api(path):
    result = subprocess.run(["gh", "api", f"repos/{REPOSITORY}/{path}"], capture_output=True, text=True)
    if result.returncode:
        require("HTTP 404" in result.stderr, result.stderr.strip())
        return None
    return json.loads(result.stdout)


def validate(directory):
    manifest = verify(directory, SOURCE, native=True)
    require(manifest["version"] == "1.0.0" and len(manifest["assets"]) == 5, "unexpected release candidate")
    for kind, name in ARCHIVES.items():
        inspect_package(directory / name, kind, SOURCE)
    inspect_installer(directory / INSTALLER, SOURCE)
    for name in package_names(directory, True) - PACKAGES:
        inspect_native_package(directory / name, SOURCE)
    names = {asset["name"] for asset in manifest["assets"]} | {"INSTALL.md", "SHA256SUMS", "release-manifest.json"}
    require({path.name for path in directory.iterdir()} == names, "unexpected publication file")
    require(all(path.is_file() and not path.is_symlink() for path in directory.iterdir()), "invalid publication file")
    return names


def main(directory, publish=False):
    directory = Path(directory).resolve()
    names = validate(directory)
    notes = Path(__file__).with_name("1.0.0-prerelease.md")
    require(notes.is_file(), "prerelease notes missing")
    if not publish:
        print(f"Verified candidate {SOURCE}: {len(names)} files, 5 packages; no publication requested")
        return
    run = api(f"actions/runs/{RUN}")
    require(run and run["head_sha"] == SOURCE and run["conclusion"] == "success", "source CI is not successful")
    reference = api(f"git/ref/tags/{TAG}")
    if reference:
        target = reference["object"]
        for _ in range(4):
            if target["type"] == "commit":
                break
            require(target["type"] == "tag", "unexpected tag type")
            target = api(f"git/tags/{target['sha']}")["object"]
        require(target["type"] == "commit" and target["sha"] == SOURCE, "existing tag targets different source")
    release = api(f"releases/tags/{TAG}")
    if release:
        require(release["prerelease"], "refusing to replace an existing stable release")
        require(reference or release["target_commitish"] == SOURCE, "draft targets another source")
        require({asset["name"] for asset in release["assets"]} <= names, "existing release contains unrelated assets")
    if release is None:
        gh("release", "create", TAG, "--repo", REPOSITORY, "--draft", "--prerelease", "--latest=false",
           "--target", SOURCE, "--title", "LanLink 1.0.0 (Pre-release)", "--notes-file", notes,
           *[directory / name for name in sorted(names)])
    elif release["draft"]:
        gh("release", "upload", TAG, "--repo", REPOSITORY, "--clobber", *[directory / name for name in sorted(names)])
    with tempfile.TemporaryDirectory(prefix="lanlink-published-candidate-") as temporary:
        gh("release", "download", TAG, "--repo", REPOSITORY, "--dir", temporary)
        downloaded = Path(temporary)
        validate(downloaded)
        for name in names:
            require(hashlib.sha256((directory / name).read_bytes()).digest() ==
                    hashlib.sha256((downloaded / name).read_bytes()).digest(), "uploaded asset differs: " + name)
    if release is None or release["draft"]:
        gh("release", "edit", TAG, "--repo", REPOSITORY, "--draft=false", "--prerelease", "--latest=false", "--notes-file", notes)
    published = api(f"releases/tags/{TAG}")
    require(published and not published["draft"] and published["prerelease"], "prerelease was not published")
    require({asset["name"] for asset in published["assets"]} == names, "published assets are incomplete")
    reference = api(f"git/ref/tags/{TAG}")
    require(reference and reference["object"]["type"] == "commit" and reference["object"]["sha"] == SOURCE, "published tag mismatch")
    print("Published verified prerelease: " + published["html_url"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--publish", action="store_true")
    arguments = parser.parse_args()
    main(arguments.directory, arguments.publish)
