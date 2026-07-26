#!/usr/bin/env python3
"""Issue #2057: side-effect primitives must inherit capability enforcement.

Scans evaluator_primitives*.cpp for public ``add("name", …)`` registrations
whose names look effectful (mutate / ffi / network / render / exec / file
write / agent self-mod). Each such registration must show a security
coverage marker in the same translation unit:

  - ``add_mutate(``          — mutate family wrapper (#2052)
  - ``require_effect(``      — production entry (#2072)
  - ``check_and_record_effect(``
  - ``AURA_SIDE_EFFECT_PRIM`` — documented pattern token
  - ``security_exempt`` / ``SECURITY_EXEMPT`` — documented exempt
  - ``effect_enforced_in_body`` — PrimMeta body-enforced flag

Also accepts per-name allowlist entries in
``tests/side-effect-security-allowlist.txt`` (one name per line, ``#`` comments).

Usage:
  python3 scripts/check_side_effect_security.py
  python3 scripts/check_side_effect_security.py --strict   # exit 1 on violations

Exit 0 = OK (or report-only without --strict), 1 = violation under --strict.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRIM_GLOB = "src/compiler/evaluator_primitives*.cpp"
ALLOWLIST_PATH = ROOT / "tests" / "side-effect-security-allowlist.txt"

ADD_RE = re.compile(r'(?:^|[^\w.])add\(\s*"([^"]+)"')
# Coverage markers that prove the TU enforces capability checks for side effects.
COVERAGE_MARKERS = (
    "add_mutate(",
    "require_effect(",
    "check_and_record_effect(",
    "AURA_SIDE_EFFECT_PRIM",
    "security_exempt",
    "SECURITY_EXEMPT",
    "effect_enforced_in_body",
    "required_effects",  # PrimMeta stamp (RENDER_PRIMITIVE_META / #2136)
    "RENDER_PRIMITIVE_META",  # auto stamps kEffectRender (#2136)
)

# High-risk side-effect surface that MUST show coverage markers in the same
# TU (Issue #2057). Commercial verticals (agent / strategy / synthesize /
# auto-evolve / tcp / git) are tracked via allowlist until wired through
# require_effect; the gate still catches new mutate/ffi/render/exec/file
# registrations without enforcement.
# Issue #2136: tui: / terminal-present / c-render are Render-gated.
SIDE_EFFECT_PREFIXES = (
    "mutate:",
    "mutate-",
    "ffi:",
    "ffi-",
    "render3d:",
    "render:",
    "tui:",
    "terminal-present",
    "c-render-",
    "file:write",
    "sys-write",
    "sys-open",
    "sys-exec",
    "exec:",
    "exec-",
    "syscall",
)
SIDE_EFFECT_EXACT = frozenset({"write-file", "c-present-batch", "c-ansi-emit"})


def is_side_effect_name(name: str) -> bool:
    if name in SIDE_EFFECT_EXACT:
        return True
    return any(name.startswith(p) for p in SIDE_EFFECT_PREFIXES)


def load_allowlist() -> set[str]:
    if not ALLOWLIST_PATH.exists():
        return set()
    out: set[str] = set()
    for line in ALLOWLIST_PATH.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        out.add(line)
    return out


def file_has_coverage(text: str) -> bool:
    return any(m in text for m in COVERAGE_MARKERS)


def scan() -> list[tuple[str, str, int]]:
    """Return list of (path, name, line) violations."""
    allow = load_allowlist()
    violations: list[tuple[str, str, int]] = []
    for path in sorted((ROOT / "src" / "compiler").glob("evaluator_primitives*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        covered_tu = file_has_coverage(text)
        for i, line in enumerate(text.splitlines(), start=1):
            for m in ADD_RE.finditer(line):
                name = m.group(1)
                if not is_side_effect_name(name):
                    continue
                if name in allow:
                    continue
                if covered_tu:
                    # TU has a coverage marker — all effectful adds in this
                    # file are considered covered (add_mutate / require_effect
                    # wrappers live in the same register_* function).
                    continue
                violations.append((str(path.relative_to(ROOT)), name, i))
    return violations


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 on violations (default for ./build.py gate)",
    )
    args = ap.parse_args()

    violations = scan()
    if not violations:
        print("OK: side-effect security coverage (Issue #2057) — no uncovered effectful prims")
        return 0

    print("FAIL: side-effect primitives without security coverage (Issue #2057):")
    for path, name, line in violations:
        print(f"  + {name}  [{path}:{line}]")
    print(
        "\nEvery effectful prim must use add_mutate / require_effect /\n"
        "check_and_record_effect, or mark security_exempt with a reason.\n"
        "See src/compiler/security_side_effect.hh.\n"
        "To allowlist with justification, add the name to\n"
        f"  {ALLOWLIST_PATH.relative_to(ROOT)}"
    )
    return 1 if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
