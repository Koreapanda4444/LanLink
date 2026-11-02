import json
from pathlib import Path
import sys
import zipfile

root, output = map(Path, sys.argv[1:])
metadata = json.loads((root / "build-info.json").read_text())
assert metadata["system"] == "Darwin"
output.mkdir(parents=True, exist_ok=True)
archive = output / f"LanLink-{metadata['version']}-macos-{metadata['architecture']}.zip"
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as target:
    for path in sorted(root.rglob("*")):
        if path.is_file():
            target.write(path, str(path.relative_to(root)))
print(archive)
