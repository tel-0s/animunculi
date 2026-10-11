"""Compile data/Scripts/Source/*.psc into data/Scripts/*.pex.

Vanilla sources come from the base game's Scripts.zip, overlaid with SKSE's script sources
(which extend Form/Actor/etc.); everything goes into a build-only include folder.

Paths default to a Creation Kit install under SKYRIM_ROOT (the game folder):
  PAPYRUS_SOURCE_ZIP   <SKYRIM_ROOT>/Data/Scripts.zip
  SKSE_SCRIPT_SOURCES  <SKYRIM_ROOT>/Data/Scripts/Source
  PAPYRUS_FLAGS        <SKYRIM_ROOT>/Data/Source/Scripts/TESV_Papyrus_Flags.flg
  PAPYRUS_COMPILER     <SKYRIM_ROOT>/Papyrus Compiler/PapyrusCompiler.exe
Each can be set on its own (e.g. when SKSE lives in a mod manager's folder). Without a compiler
the build keeps the committed data/Scripts/*.pex (exit code 3, reported as a warning).
"""
from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
SKYRIM = Path(os.environ.get("SKYRIM_ROOT", "SKYRIM_ROOT-not-set"))


def setting(name: str, default: Path) -> Path:
    return Path(os.environ[name]) if os.environ.get(name) else default


SOURCE_ZIP = setting("PAPYRUS_SOURCE_ZIP", SKYRIM / "Data/Scripts.zip")
SKSE_SOURCES = setting("SKSE_SCRIPT_SOURCES", SKYRIM / "Data/Scripts/Source")
FLAGS = setting("PAPYRUS_FLAGS", SKYRIM / "Data/Source/Scripts/TESV_Papyrus_Flags.flg")
COMPILER = setting("PAPYRUS_COMPILER", SKYRIM / "Papyrus Compiler/PapyrusCompiler.exe")
SKIPPED = 3

INCLUDE = PROJECT / "build/papyrus-include"
SOURCES = PROJECT / "data/Scripts/Source"
OUTPUT = PROJECT / "data/Scripts"


def scrub_header(pex: Path) -> None:
    """The compiler writes the Windows user and computer names into the .pex header (debug info the
    game ignores). Replace them, so a published script does not carry the builder's machine."""
    data = pex.read_bytes()
    if data[:4] != bytes.fromhex("FA57C0DE"):  # Skyrim's big-endian PEX magic
        raise ValueError(f"{pex} is not a Skyrim .pex file")
    off = 16  # magic, version, game id, compile time
    out = bytearray(data[:off])
    for replacement in (None, b"animunculi", b"build"):  # source file name (kept), user, machine
        (n,) = struct.unpack(">H", data[off:off + 2])
        value = data[off + 2:off + 2 + n] if replacement is None else replacement
        out += struct.pack(">H", len(value)) + value
        off += 2 + n
    out += data[off:]
    pex.write_bytes(bytes(out))


def main() -> int:
    for path, what in ((SOURCE_ZIP, "vanilla Scripts.zip"), (COMPILER, "PapyrusCompiler.exe"), (FLAGS, "flags file")):
        if not path.exists():
            print(f"skipping the Papyrus compile: missing {what} at {path} (set SKYRIM_ROOT or the matching "
                  "variable; see this script's docstring). The committed data/Scripts/*.pex is used.", file=sys.stderr)
            return SKIPPED
    INCLUDE.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(SOURCE_ZIP) as archive:
        for member in archive.namelist():
            if member.lower().endswith(".psc"):
                (INCLUDE / Path(member).name).write_bytes(archive.read(member))
    if SKSE_SOURCES.is_dir():
        for psc in SKSE_SOURCES.glob("*.psc"):
            shutil.copy2(psc, INCLUDE / psc.name)
    else:
        print(f"warning: SKSE script sources not found at {SKSE_SOURCES}", file=sys.stderr)
    for psc in SOURCES.glob("*.psc"):
        shutil.copy2(psc, INCLUDE / psc.name)
    shutil.copy2(FLAGS, INCLUDE / FLAGS.name)

    OUTPUT.mkdir(parents=True, exist_ok=True)
    for psc in sorted(SOURCES.glob("*.psc")):
        print(f"compiling {psc.name}")
        result = subprocess.run([str(COMPILER), str(psc), f"-f={FLAGS.name}", f"-i={INCLUDE}", f"-o={OUTPUT}"],
                                capture_output=True, text=True)
        sys.stdout.write(result.stdout[-2000:])
        if result.returncode != 0:
            sys.stderr.write(result.stderr)
            return result.returncode
        scrub_header(OUTPUT / (psc.stem + ".pex"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
