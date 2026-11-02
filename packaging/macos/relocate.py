from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
app = root / "LanLink.app"
frameworks = app / "Contents/Frameworks"
frameworks.mkdir(parents=True, exist_ok=True)
for library in (root / "lib").iterdir():
    if library.is_file():
        shutil.copy2(library, frameworks / library.name)

def native(path):
    if not path.is_file() or path.is_symlink():
        return False
    with path.open("rb") as stream:
        return stream.read(4) in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")

def run(*arguments):
    return subprocess.check_output(arguments, text=True, stderr=subprocess.STDOUT)

def version(value):
    parts = tuple(map(int, value.split(".")))
    return (parts + (0, 0, 0))[:3]

declared_minimum = plistlib.loads((app / "Contents/Info.plist").read_bytes())["LSMinimumSystemVersion"]
for binary in (path for path in root.rglob("*") if native(path)):
    minimums = []
    for block in re.split(r"(?=Load command \d+)", run("/usr/bin/otool", "-l", str(binary))):
        if re.search(r"(?m)^\s*cmd\s+LC_(?:BUILD_VERSION|VERSION_MIN_MACOSX)\s*$", block):
            value = re.search(r"(?m)^\s*(?:minos|version)\s+(\d+(?:\.\d+){0,2})\s*$", block)
            if not value:
                raise ValueError(f"Cannot inspect native deployment target: {binary}")
            minimums.append(value[1])
    if not minimums or any(version(value) > version(declared_minimum) for value in minimums):
        raise ValueError(f"{binary.relative_to(root)} requires macOS {minimums}, above declared {declared_minimum}")

binaries = [path for path in root.rglob("*") if native(path)]
for binary in binaries:
    for line in run("/usr/bin/otool", "-L", str(binary)).splitlines()[1:]:
        dependency = line.rsplit(" (compatibility version", 1)[0].strip()
        if dependency.startswith(("/System/Library/", "/usr/lib/")):
            continue
        name = Path(dependency).name
        if not (root / "lib" / name).is_file():
            raise ValueError(f"Unbundled native dependency: {name}")
        if dependency.startswith("/"):
            run("/usr/bin/install_name_tool", "-change", dependency, "@rpath/" + name, str(binary))
    if binary.suffix == ".dylib":
        run("/usr/bin/install_name_tool", "-id", "@rpath/" + binary.name, str(binary))
    inside = binary.is_relative_to(app)
    rpath = "@loader_path/../Frameworks" if inside else "@loader_path/../lib"
    if f"path {rpath} (offset" not in run("/usr/bin/otool", "-l", str(binary)):
        run("/usr/bin/install_name_tool", "-add_rpath", rpath, str(binary))
    run("/usr/bin/codesign", "--force", "--sign", "-", str(binary))
run("/usr/bin/codesign", "--force", "--sign", "-", str(app))
run("/usr/bin/codesign", "--verify", "--deep", "--strict", str(app))
print(f"Verified all native deployment targets <= macOS {declared_minimum}, relocated libraries and verified the ad hoc application signature")
