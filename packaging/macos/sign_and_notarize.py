import argparse
import json
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description="Sign and notarize a prepared Mac client with the operator's credentials")
parser.add_argument("package", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--identity", required=True, help="Installed Developer ID Application identity")
parser.add_argument("--notary-profile", required=True, help="Existing notarytool Keychain profile")
args = parser.parse_args()
if sys.platform != "darwin":
    parser.error("Signing requires macOS")
root = args.package.resolve()
app = root / "LanLink.app"
metadata = json.loads((root / "build-info.json").read_text())
for binary in sorted(root.rglob("*"), key=lambda item: len(item.parts), reverse=True):
    if binary.is_symlink() or not binary.is_file():
        continue
    with binary.open("rb") as stream:
        magic = stream.read(4)
    if magic in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
        subprocess.run(["/usr/bin/codesign", "--force", "--options", "runtime", "--timestamp", "--sign", args.identity, str(binary)], check=True)
subprocess.run(["/usr/bin/codesign", "--force", "--options", "runtime", "--timestamp", "--sign", args.identity, str(app)], check=True)
subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)], check=True)
args.output.mkdir(parents=True, exist_ok=True)
image = args.output.resolve() / f"LanLink-{metadata['version']}-macos-{metadata['architecture']}.dmg"
subprocess.run(["/usr/bin/hdiutil", "create", "-volname", "LanLink", "-srcfolder", str(root), "-format", "UDZO", str(image)], check=True)
subprocess.run(["/usr/bin/codesign", "--timestamp", "--sign", args.identity, str(image)], check=True)
subprocess.run(["/usr/bin/xcrun", "notarytool", "submit", str(image), "--keychain-profile", args.notary_profile, "--wait"], check=True)
subprocess.run(["/usr/bin/xcrun", "stapler", "staple", str(image)], check=True)
subprocess.run(["/usr/bin/xcrun", "stapler", "validate", str(image)], check=True)
print(image)
