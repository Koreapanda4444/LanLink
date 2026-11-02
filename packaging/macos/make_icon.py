import shutil
import subprocess
import sys
from pathlib import Path

source, output = map(Path, sys.argv[1:])
icons = output.with_suffix(".iconset")
icons.mkdir(parents=True, exist_ok=True)
for size in (16, 32, 128, 256, 512):
    for scale in (1, 2):
        name = f"icon_{size}x{size}" + ("@2x" if scale == 2 else "") + ".png"
        subprocess.run(["/usr/bin/sips", "-z", str(size * scale), str(size * scale), str(source), "--out", str(icons / name)], check=True, stdout=subprocess.DEVNULL)
subprocess.run(["/usr/bin/iconutil", "-c", "icns", str(icons), "-o", str(output)], check=True)
shutil.rmtree(icons)
