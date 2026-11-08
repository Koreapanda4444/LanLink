"""Publish an explicit preview request using an existing successful native build."""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from prepare import (ARCHIVES, INSTALLER, VERSION, inspect_installer,
                     inspect_native_package, inspect_package, require, verify)


def gh(*args):
    return subprocess.run(["gh", *map(str, args)], check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def api(path):
    return json.loads(gh("api", path))


def validate(directory, commit):
    manifest = verify(directory, commit, native=True)
    require(len(manifest["assets"]) == 5, "preview requires all five native packages")
    require({p.name for p in directory.iterdir()} ==
            {a["name"] for a in manifest["assets"]} |
            {"SHA256SUMS", "release-manifest.json", "INSTALL.md"},
            "unexpected release files")
    for asset in manifest["assets"]:
        name = asset["name"]
        if name == INSTALLER:
            actual = inspect_installer(directory / name, commit)
        elif name in ARCHIVES.values():
            kind = next(k for k, v in ARCHIVES.items() if v == name)
            actual = inspect_package(directory / name, kind, commit)
        else:
            actual = inspect_native_package(directory / name, commit)
        require(actual == asset, "package inspection differs from the build manifest")
    return manifest


def main():
    request = json.loads(Path(__file__).with_name("preview.json").read_text())
    tag, commit, run_id = (request[k] for k in ("tag", "source_commit", "run_id"))
    require(request.get("schema") == 1 and
            re.fullmatch(r"v" + re.escape(VERSION) + r"-preview\.[1-9][0-9]*", tag),
            "only versioned preview tags are permitted; stable readiness gates remain separate")
    require(re.fullmatch(r"[0-9a-f]{40}", commit) and type(run_id) is int and run_id > 0,
            "invalid source build request")
    repo = os.environ["GH_REPO"]
    base = f"repos/{repo}"
    run = api(f"{base}/actions/runs/{run_id}")
    require(run["head_sha"] == commit and run["head_branch"] == "main" and
            run["event"] == "push" and run["path"] == ".github/workflows/build.yml" and
            run["status"] == "completed" and run["conclusion"] == "success",
            "source run is not a successful main build for the requested commit")
    required = {"windows-latest", "ubuntu-latest", "macOS native client",
                "Linux x64 relay (glibc 2.34)", "Relay on Ubuntu 22.04",
                "Relay on Debian 12", f"Verify {VERSION} release packages"}
    jobs = api(f"{base}/actions/runs/{run_id}/jobs?filter=latest&per_page=100")["jobs"]
    require(required <= {j["name"] for j in jobs if j["conclusion"] == "success"},
            "required native build/package jobs have not all succeeded")
    require(api(f"{base}/compare/{commit}...main")["status"] in ("ahead", "identical"),
            "source commit is no longer an ancestor of main")
    existing = {r["tag_name"]: r for r in api(f"{base}/releases?per_page=100")}.get(tag)
    with tempfile.TemporaryDirectory(prefix="lanlink-preview-") as temporary:
        directory = Path(temporary) / "assets"
        gh("run", "download", run_id, "--name", f"LanLink-{VERSION}-release", "--dir", directory)
        manifest = validate(directory, commit)
        if existing is not None:
            require(existing["prerelease"], "existing release is not a preview")
            require(existing["target_commitish"] == commit, "existing preview targets another source")
            if not existing["draft"]:
                downloaded = Path(temporary) / "published"
                gh("release", "download", tag, "--dir", downloaded)
                require(validate(downloaded, commit) == manifest, "existing preview content differs")
                print(f"Verified published preview: {existing['html_url']}")
                return
        else:
            notes = Path(temporary) / "notes.md"
            body = Path(__file__).with_name(f"{VERSION}.md").read_text()
            body += f"\n소스: `{commit}` · [빌드·패키지 검증](https://github.com/{repo}/actions/runs/{run_id})\n"
            notes.write_text(body)
            # Refuse to reuse any unrelated tag, even if no Release currently exists.
            refs = api(f"{base}/git/matching-refs/tags/{tag}")
            require(not any(r["ref"] == f"refs/tags/{tag}" for r in refs),
                    "preview tag already exists without its release")
            gh("release", "create", tag, "--draft", "--prerelease", "--latest=false",
               "--target", commit, "--title", f"LanLink {tag.removeprefix('v')} (Pre-release)",
               "--notes-file", notes)
        gh("release", "upload", tag, *sorted(directory.iterdir()), "--clobber")
        downloaded = Path(temporary) / "draft"
        gh("release", "download", tag, "--dir", downloaded)
        require(validate(downloaded, commit) == manifest, "uploaded preview content differs")
        gh("release", "edit", tag, "--draft=false", "--prerelease", "--latest=false")
        published = api(f"{base}/releases/tags/{tag}")
        require(not published["draft"] and published["prerelease"] and
                len(published["assets"]) == len(manifest["assets"]) + 3,
                "preview publication is incomplete")
        require(api(f"{base}/git/ref/tags/{tag}")["object"]["sha"] == commit,
                "published tag source differs from verified packages")
        print(f"Published {published['html_url']}")


if __name__ == "__main__":
    main()
