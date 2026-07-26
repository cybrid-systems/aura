#!/usr/bin/env python3
"""Issue #2057 / #2152: side-effect primitives must inherit capability enforcement.

Scans evaluator_primitives*.cpp for public ``add("name", …)`` registrations
whose names look effectful (mutate / ffi / network / render / exec / file
write / agent self-mod). Each such registration must show a security
coverage marker:

  - ``add_mutate(``          — mutate family wrapper (#2052)
  - ``require_effect(``      — production entry (#2072)
  - ``check_and_record_effect(``
  - ``AURA_SIDE_EFFECT_PRIM`` — documented pattern token
  - ``security_exempt`` / ``SECURITY_EXEMPT`` — documented exempt
  - ``effect_enforced_in_body`` — PrimMeta body-enforced flag
  - ``required_effects`` / ``RENDER_PRIMITIVE_META`` — PrimMeta stamp

Issue #2152 strengthens the gate:
  - Allowlist entries MUST include ``# SECURITY_EXEMPT: <reason>``
  - Per-registration local window is preferred; TU-wide markers still
    cover files that share require_effect / add_mutate wrappers
  - Bare side-effect ``add("prefix:…")`` without coverage fails under
    ``--strict`` (defense against novel prim names)

Also accepts per-name allowlist entries in
``tests/side-effect-security-allowlist.txt`` (one name per line, with
``# SECURITY_EXEMPT: reason`` required).

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
ADD_MUTATE_RE = re.compile(r'add_mutate\(\s*"([^"]+)"')
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
    "effective_required_effects",  # #2152 dispatch helper
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

# Issue #2152: allowlist lines must document a reason with this token.
EXEMPT_REASON_RE = re.compile(r"SECURITY_EXEMPT\s*:", re.IGNORECASE)


def is_side_effect_name(name: str) -> bool:
    if name in SIDE_EFFECT_EXACT:
        return True
    return any(name.startswith(p) for p in SIDE_EFFECT_PREFIXES)


def load_allowlist() -> tuple[set[str], list[str]]:
    """Return (names, reason_errors). reason_errors are allowlist lines
    missing SECURITY_EXEMPT: reason (#2152 AC3)."""
    if not ALLOWLIST_PATH.exists():
        return set(), []
    out: set[str] = set()
    reason_errors: list[str] = []
    for lineno, raw in enumerate(ALLOWLIST_PATH.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        # name  # SECURITY_EXEMPT: reason
        if "#" in line:
            name_part, comment = line.split("#", 1)
            name = name_part.strip()
            if not name:
                continue
            if not EXEMPT_REASON_RE.search(comment):
                reason_errors.append(
                    f"{ALLOWLIST_PATH.relative_to(ROOT)}:{lineno}: {name!r} missing '# SECURITY_EXEMPT: <reason>'"
                )
            out.add(name)
        else:
            # bare name without reason comment
            out.add(line)
            reason_errors.append(
                f"{ALLOWLIST_PATH.relative_to(ROOT)}:{lineno}: {line!r} missing '# SECURITY_EXEMPT: <reason>'"
            )
    return out, reason_errors


def file_has_coverage(text: str) -> bool:
    return any(m in text for m in COVERAGE_MARKERS)


def local_window_has_coverage(lines: list[str], line_idx: int, window: int = 80) -> bool:
    """Check a local window around the registration for coverage markers.

    Looks slightly before (meta prep) and after (set_meta / body).
    """
    start = max(0, line_idx - 5)
    end = min(len(lines), line_idx + window)
    chunk = "\n".join(lines[start:end])
    return any(m in chunk for m in COVERAGE_MARKERS)


def scan() -> tuple[list[tuple[str, str, int]], list[str]]:
    """Return (violations, allowlist_reason_errors).

    violations: list of (path, name, line).
    """
    allow, reason_errors = load_allowlist()
    violations: list[tuple[str, str, int]] = []
    for path in sorted((ROOT / "src" / "compiler").glob("evaluator_primitives*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        covered_tu = file_has_coverage(text)
        # Names registered via add_mutate in this TU are always covered.
        mutate_names = set(ADD_MUTATE_RE.findall(text))
        for i, line in enumerate(lines, start=1):
            for m in ADD_RE.finditer(line):
                name = m.group(1)
                if not is_side_effect_name(name):
                    continue
                if name in allow:
                    continue
                if name in mutate_names:
                    continue
                # Prefer local window; fall back to TU-wide for shared wrappers
                # (add_mutate / require_effect helpers living elsewhere in the file).
                if local_window_has_coverage(lines, i - 1) or covered_tu:
                    continue
                violations.append((str(path.relative_to(ROOT)), name, i))
    return violations, reason_errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 on violations (default for ./build.py gate)",
    )
    args = ap.parse_args()

    violations, reason_errors = scan()
    failed = False

    if reason_errors:
        print("FAIL: side-effect allowlist entries without SECURITY_EXEMPT reason (Issue #2152):")
        for err in reason_errors:
            print(f"  + {err}")
        failed = True

    if violations:
        print("FAIL: side-effect primitives without security coverage (Issue #2057/#2152):")
        for path, name, line in violations:
            print(f"  + {name}  [{path}:{line}]")
        print(
            "\nEvery effectful prim must use add_mutate / require_effect /\n"
            "check_and_record_effect / required_effects, or mark\n"
            "security_exempt with SECURITY_EXEMPT: <reason>.\n"
            "See src/compiler/security_side_effect.hh.\n"
            "To allowlist with justification, add the name to\n"
            f"  {ALLOWLIST_PATH.relative_to(ROOT)}\n"
            "  (format: name  # SECURITY_EXEMPT: reason)"
        )
        failed = True

    if not failed:
        print("OK: side-effect security coverage (Issue #2057/#2152) — no uncovered effectful prims")
        return 0

    return 1 if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
