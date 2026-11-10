#!/usr/bin/env python3
"""Issue #5132: stdlib surface consistency (INDEX / export / define) + drift gate.

Checks:
  AC1  every module the INDEX registry lists has lib/std/<name>.aura
  AC2  every name a module exports resolves at runtime once required
       (needs build/aura; skipped with a note when it is absent)
  AC3  INDEX carries no stale numeric claim about the -stats surface

Usage:
  python3 scripts/coverage/checks/check_stdlib_surface_drift_5132.py
  python3 ... --no-live     # AC1 + AC3 only
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
STD = ROOT / "lib" / "std"


def strip_line_comments(src: str) -> str:
    out = []
    for line in src.splitlines():
        out.append("" if line.lstrip().startswith(";") else line)
    return chr(10).join(out)


def exported_names(path: Path) -> list[str]:
    src = strip_line_comments(path.read_text(errors="replace"))
    m = re.search(r"\(export([^)]*)\)", src)
    if not m:
        return []
    return [n for n in m.group(1).split() if n and not n.startswith(";")]


def indexed_modules() -> list[str]:
    src = strip_line_comments((STD / "INDEX.aura").read_text(errors="replace"))
    start = src.find("(define stdlib-registry")
    if start >= 0:
        src = src[start:]
    names = re.findall(r'\n\s*\(list\s+"([a-z0-9-]+)"', src)
    seen = []
    for n in names:
        if n not in seen:
            seen.append(n)
    return seen


def live_missing(exports_by_module: dict[str, list[str]]) -> tuple[list[tuple[str, str]], str]:
    aura = ROOT / "build" / "aura"
    if not aura.exists():
        return [], "no build/aura"
    prog = []
    for mod, names in exports_by_module.items():
        if not names:
            continue
        prog.append('(require "std/' + mod + '" all:)')
        for n in names:
            call = "(try (begin " + n + " (quote ok)) (catch (e) (quote unbound)))"
            prog.append('(display "' + mod + " " + n + ' " ' + call + ")")
            prog.append("(newline)")
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as fh:
        fh.write(chr(10).join(prog) + chr(10))
        path = fh.name
    env = {
        "AURA_PATH": str(ROOT / "lib"),
        "AURA_SANDBOX": "off",
        "PATH": "/usr/bin:/bin",
        "HOME": "/tmp",
    }
    r = subprocess.run([str(aura), path], capture_output=True, text=True, timeout=600, env=env)
    missing = []
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "unbound":
            missing.append((parts[0], parts[1]))
    if r.returncode != 0 and not missing:
        tail = (r.stderr or "")[-300:]
        return [], "probe failed rc=" + str(r.returncode) + ": " + tail
    return missing, "live"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-live", action="store_true")
    args = ap.parse_args()

    fails = 0
    indexed = indexed_modules()
    files = sorted(p.stem for p in STD.glob("*.aura"))
    print("INDEX entries: " + str(len(indexed)) + " | module files: " + str(len(files)))

    missing_files = [m for m in indexed if not (STD / (m + ".aura")).exists()]
    if missing_files:
        print("AC1 FAIL: indexed without a module file: " + ", ".join(missing_files))
        fails += 1
    else:
        print("AC1 ok: every indexed module exists")

    exports_by_module = {m: exported_names(STD / (m + ".aura")) for m in files}
    if args.no_live:
        print("AC2 skipped (--no-live)")
    else:
        missing, how = live_missing(exports_by_module)
        if how == "live":
            if missing:
                print("AC2 FAIL: " + str(len(missing)) + " exported name(s) do not resolve:")
                for mod, name in missing[:40]:
                    print("   " + mod + ": " + name)
                fails += 1
            else:
                total = sum(len(v) for v in exports_by_module.values())
                print("AC2 ok: " + str(total) + " exported names resolve across " + str(len(files)) + " modules")
        else:
            print("AC2 skipped: " + how)

    idx_src = (STD / "INDEX.aura").read_text(errors="replace")
    claims = re.findall(r"\d+\s+\*-stats primitives", idx_src)
    if claims:
        print("AC3 FAIL: INDEX still states a fixed stats count (" + claims[0] + "); use the runtime count")
        fails += 1
    else:
        print("AC3 ok: INDEX states no fixed -stats count")

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
