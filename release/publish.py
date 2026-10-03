import json
from pathlib import Path
import subprocess
import sys
import tempfile

from prepare import VERSION, require, verify


def gh(*arguments):
    return subprocess.run(["gh", *arguments], check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def api(path):
    result = subprocess.run(["gh", "api", path], text=True, capture_output=True)
    if result.returncode:
        if "HTTP 404" in result.stderr:
            return None
        raise RuntimeError(result.stderr.strip())
    return json.loads(result.stdout)


def main(directory, commit):
    directory = Path(directory)
    verify(directory, commit)
    repository = json.loads(gh("repo", "view", "--json", "nameWithOwner"))["nameWithOwner"]
    tag = f"v{VERSION}"
    reference = api(f"repos/{repository}/git/ref/tags/{tag}")
    if reference is not None:
        target = reference["object"]
        for _ in range(4):
            if target["type"] == "commit":
                break
            require(target["type"] == "tag", "release tag has an unexpected object type")
            target = api(f"repos/{repository}/git/tags/{target['sha']}")["object"]
        require(target["type"] == "commit" and target["sha"] == commit,
                "existing release tag points to a different commit")
    release = api(f"repos/{repository}/releases/tags/{tag}")
    if release is not None and release["draft"] and reference is None:
        require(release["target_commitish"] == commit, "existing draft targets another commit")
    if release is not None and not release["draft"]:
        with tempfile.TemporaryDirectory(prefix="lanlink-published-") as temporary:
            gh("release", "download", tag, "--dir", temporary)
            verify(temporary, commit)
        print(f"Verified existing release: {release['html_url']}")
        return

    assets = sorted(str(path) for path in directory.iterdir() if path.is_file())
    require(len(assets) == 5, "release directory contains unexpected files")
    if release is None:
        gh("release", "create", tag, *assets, "--draft", "--target", commit,
           "--title", f"LanLink {VERSION}", "--notes-file", str(Path(__file__).with_name(f"{VERSION}.md")))
    else:
        gh("release", "upload", tag, *assets, "--clobber")
    with tempfile.TemporaryDirectory(prefix="lanlink-draft-") as temporary:
        gh("release", "download", tag, "--dir", temporary)
        verify(temporary, commit)
    gh("release", "edit", tag, "--draft=false", "--latest")
    published = api(f"repos/{repository}/releases/tags/{tag}")
    require(published is not None and not published["draft"] and len(published["assets"]) == 5,
            "release was not published with all five assets")
    print(f"Published {published['html_url']}")


if __name__ == "__main__":
    require(len(sys.argv) == 3, "usage: publish.py directory commit")
    main(sys.argv[1], sys.argv[2])
