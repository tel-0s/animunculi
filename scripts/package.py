"""Assemble the playtest release under dist/:

  dist/Animunculi/                 MO2-installable mod folder (SKSE plugin, INI, Papyrus API)
  dist/Animunculi-<ver>.zip        the same, zipped
  dist/animunculi_mcp-<ver>-py3-none-any.whl   the MCP server (if `uv build` is available)

Run after building (build.ps1 does everything in order).
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / "dist"
STAGE = DIST / "Animunculi"


def version() -> str:
    m = re.search(r"project\(Animunculi VERSION ([0-9.]+)", (ROOT / "CMakeLists.txt").read_text())
    return m.group(1) if m else "0.0.0"


def main() -> int:
    ver = version()
    dll = ROOT / "build/vs2026/Release/Animunculi.dll"
    if not dll.exists():
        print("build the plugin first (build.ps1)", file=sys.stderr)
        return 1
    plugins = STAGE / "SKSE/Plugins"
    plugins.mkdir(parents=True, exist_ok=True)
    for src in (dll, dll.with_suffix(".pdb"), ROOT / "data/SKSE/Plugins/Animunculi.ini"):
        shutil.copy2(src, plugins / src.name)
    pex = ROOT / "data/Scripts/Animunculi.pex"
    if not pex.exists():
        print("compile the Papyrus script first (scripts/compile_papyrus.py)", file=sys.stderr)
        return 1
    (STAGE / "Scripts/Source").mkdir(parents=True, exist_ok=True)
    shutil.copy2(pex, STAGE / "Scripts" / pex.name)
    shutil.copy2(ROOT / "data/Scripts/Source/Animunculi.psc", STAGE / "Scripts/Source/Animunculi.psc")

    archive = DIST / f"Animunculi-{ver}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(STAGE.rglob("*")):
            if f.is_file():
                z.write(f, f.relative_to(STAGE).as_posix())
    print(f"mod archive: {archive} ({archive.stat().st_size // 1024} KB)")

    uv = shutil.which("uv")
    if uv:
        result = subprocess.run([uv, "build", "--wheel", "--out-dir", str(DIST)], cwd=ROOT / "mcp",
                                capture_output=True, text=True)
        if result.returncode == 0:
            for whl in DIST.glob("animunculi_mcp-*.whl"):
                print(f"mcp wheel:   {whl}")
        else:
            print(f"uv build failed (the MCP can still run from source):\n{result.stderr[-800:]}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
