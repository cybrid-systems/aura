#!/usr/bin/env python3
"""Issue #4837: print the ninja object targets for the Clang module build.

The Clang configure/build exists only to produce module BMIs and a
compile_commands.json that clang-tidy can read for src/core / src/compiler
module interface units. Entries in scripts/clang_tidy_module_skip.txt are
documented exclusions (currently none).
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKIP_PATH = Path(__file__).resolve().parent / "clang_tidy_module_skip.txt"
PAT = re.compile(r"^(CMakeFiles/aura\.dir/src/(?:core|compiler)/[^:]+\.(?:ixx|cppm)\.o):")


def load_skip() -> set[str]:
    if not SKIP_PATH.is_file():
        return set()
    out: set[str] = set()
    for line in SKIP_PATH.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.add(line)
    return out


def main() -> int:
    build = sys.argv[1] if len(sys.argv) > 1 else "build_clang"
    skip = load_skip()
    proc = subprocess.run(
        ["ninja", "-C", build, "-t", "targets", "all"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.exit(proc.stderr.strip() or "clang_module_targets: ninja -t targets failed")
    targets: set[str] = set()
    for line in proc.stdout.splitlines():
        match = PAT.match(line)
        if not match:
            continue
        target = match.group(1)
        src = target[len("CMakeFiles/aura.dir/") : -2]
        if src in skip:
            continue
        targets.add(target)
    if not targets:
        sys.exit("clang_module_targets: no module object targets found")
    print(" ".join(sorted(targets)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
