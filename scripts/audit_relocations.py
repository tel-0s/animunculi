"""Fail the build if Animunculi.dll links a CommonLibSSE-NG function that has no address for SE or AE.

CommonLib sometimes ships relocations like RELOCATION_ID(48461, 0): the AE id is 0, so calling the
function on 1.6.x jumps to address 0 (that is exactly how Actor::IsInBleedout crashed the first
playtest). This script checks:

  1. every CommonLib function the linker pulled into the DLL (from the /MAP file), by finding its
     definition in CommonLib's .cpp sources and scanning its body for RELOCATION_ID/VariantID with a
     0 SE or AE id, or unguarded REL::ID / REL::Offset;
  2. header-inline functions with such relocations whose names our sources call (these are inlined
     into our own objects and may not appear in the map).

Usage: python scripts/audit_relocations.py [path/to/Animunculi.map]
CommonLib's sources are found through the CMake cache (COMMONLIB_DIR), or the COMMONLIB_DIR /
ANIMUNCULI_CS_ROOT environment variables.
"""
from __future__ import annotations

import collections
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAP = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/vs2026/Release/Animunculi.map"


def commonlib_dir() -> Path:
    if os.environ.get("COMMONLIB_DIR"):
        return Path(os.environ["COMMONLIB_DIR"])
    cache = ROOT / "build/vs2026/CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("COMMONLIB_DIR:"):
                return Path(line.split("=", 1)[1])
    if os.environ.get("ANIMUNCULI_CS_ROOT"):
        return Path(os.environ["ANIMUNCULI_CS_ROOT"]) / "extern/CommonLibSSE-NG"
    sys.exit("audit: cannot find CommonLibSSE-NG (configure with CMake first, or set COMMONLIB_DIR)")


CL = commonlib_dir()

REL = re.compile(r"(RELOCATION_ID|RelocationID|VariantID)\(\s*([0-9xa-fA-F]+)\s*,\s*([0-9xa-fA-F]+)")
ZERO = ("0", "0x0")


def missing_ids(text: str) -> list[str]:
    issues = []
    for m in REL.finditer(text):
        if m.group(3) in ZERO:
            issues.append(f"no AE address: {m.group(0)})")
        if m.group(2) in ZERO:
            issues.append(f"no SE address: {m.group(0)})")
    guarded = any(k in text for k in ("IsAE", "IsSE", "IsVR"))
    if not guarded:
        issues += [f"unguarded {m.group(0)}" for m in re.finditer(r"REL::ID\(\s*\d+\s*\)", text)]
        issues += [f"fixed {m.group(0)}" for m in re.finditer(r"REL::Offset\(\s*0x[0-9a-fA-F]+\s*\)", text)]
    return issues


def linked_functions() -> set[tuple[str, str]]:
    out = set()
    for line in MAP.read_text(encoding="latin-1").splitlines():
        parts = line.split()
        if len(parts) < 2 or not parts[1].startswith("?"):
            continue
        dec = parts[1]
        if dec.startswith("??0"):
            scope = dec[3:].split("@@")[0].split("@")
            func = scope[0]
        elif dec.startswith("??"):
            continue
        else:
            names = dec[1:].split("@@")[0].split("@")
            func, scope = names[0], names[1:]
        if scope and (scope[-1] in ("RE", "REL", "SKSE") or "RE" in scope):
            out.add((scope[0], func))  # (innermost class, function)
    return out


def cpp_definitions() -> dict[tuple[str, str], list[str]]:
    defs: dict[tuple[str, str], list[str]] = collections.defaultdict(list)
    for f in CL.glob("src/**/*.cpp"):
        lines = f.read_text(encoding="utf-8", errors="ignore").splitlines()
        for i, line in enumerate(lines):
            m = re.match(r"^\s*[\w:<>\*&,\s\[\]]*?\b([A-Za-z_]\w*)::([A-Za-z_~]\w*)\s*\(", line)
            if not m or line.lstrip().startswith(("return", "//", "static REL", "using", "if", "auto", "func")):
                continue
            depth, started, body = 0, False, []
            for bl in lines[i:i + 150]:
                body.append(bl)
                depth += bl.count("{") - bl.count("}")
                started |= "{" in bl
                if started and depth <= 0:
                    break
            defs[(m.group(1), m.group(2))].append(f"{f.relative_to(CL)}:{i + 1}\n" + "\n".join(body))
    return defs


def main() -> int:
    if not MAP.exists():
        print(f"map file not found: {MAP} (link with /MAP)", file=sys.stderr)
        return 2
    problems = []
    defs = cpp_definitions()
    linked = linked_functions()
    for cls, func in sorted(linked):
        for d in defs.get((cls, func), []):
            where, body = d.split("\n", 1)
            for issue in missing_ids(body):
                problems.append(f"{cls}::{func} [{where}] {issue}")

    ours = "\n".join(p.read_text(encoding="utf-8") for p in (ROOT / "src").rglob("*") if p.suffix in (".cpp", ".h"))
    for f in (CL / "include").rglob("*.h"):
        lines = f.read_text(encoding="utf-8", errors="ignore").splitlines()
        for i, line in enumerate(lines):
            if not missing_ids(line) or "REL::ID" in line:
                continue
            fn = None
            for j in range(i, max(0, i - 15), -1):
                mm = re.search(r"\b([A-Za-z_]\w*)\s*\([^()]*\)\s*(const)?\s*(noexcept)?\s*$", lines[j].split("//")[0])
                if mm and not re.search(r"\b(if|for|while|return|func|Relocation)\b", lines[j]):
                    fn = mm.group(1)
                    break
            if fn and fn != "GetSingleton" and re.search(r"(->|\.)" + re.escape(fn) + r"\s*[<(]", ours):
                problems.append(f"{f.stem}::{fn} [include/{f.relative_to(CL / 'include')}:{i + 1}] (header inline, called by name) "
                                + "; ".join(missing_ids(line)))

    print(f"relocation audit: {len(linked)} linked CommonLib functions checked")
    for p in problems:
        print(f"  PROBLEM {p}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
