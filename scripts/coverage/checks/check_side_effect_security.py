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

Issue #3524: JIT/FFI dispatch TUs (``ffi_hot_path.hh``,
``aura_jit_bridge.cpp``, ``aura_jit_runtime.cpp``,
``ir_executor_impl.cpp``) are in scope. Callers of ``dispatch_batch`` /
``dispatch_cellgrid`` / ``dispatch_named`` / ``try_cellgrid_present``
must have a ``require_effect(`` predecessor in the local window.
The hot-path header itself is the API/forwarder (skipped). Use
``--dispatch-path`` for fixture TUs (CI never passes it).

Issue #3593: the JIT C ABI prim dispatch (``aura_jit_prim_dispatch`` in
``service.ixx``) must route through the Evaluator choke point
(``invoke_prim_with_telemetry``) with fail-closed owner resolution — no
bare ``(*pfn)(`` outside that routing. The body scan flags a second bare
call or a lost/fail-open routing.

Also accepts per-name allowlist entries in
``tests/side-effect-security-allowlist.txt`` (one name per line, with
``# SECURITY_EXEMPT: reason`` required).

Usage:
  python3 scripts/coverage/checks/check_side_effect_security.py
  python3 scripts/coverage/checks/check_side_effect_security.py --strict   # exit 1 on violations

Exit 0 = OK (or report-only without --strict), 1 = violation under --strict.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PRIM_GLOB = "src/compiler/evaluator_primitives*.cpp"
ALLOWLIST_PATH = ROOT / "tests" / "side-effect-security-allowlist.txt"

# Issue #3524: FFI/JIT/ir_executor TUs that may call the hot-path dispatch.
DISPATCH_TUS = (
    "src/compiler/ffi_hot_path.hh",
    "src/compiler/aura_jit_bridge.cpp",
    "src/compiler/aura_jit_runtime.cpp",
    "src/compiler/ir_executor_impl.cpp",
)
# API + token-forwarders live here; do not flag the definitions.
DISPATCH_SKIP_FILES = frozenset({"src/compiler/ffi_hot_path.hh"})
DISPATCH_CALL_RE = re.compile(r"\b(dispatch_batch|dispatch_cellgrid|dispatch_named|try_cellgrid_present)\s*\(")

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
    "register_render_hot_prim",  # #2217 unified hot render registrar
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
    "render:",
    "file:write",
    "sys-write",
    "sys-open",
    "sys-exec",
    "exec:",
    "exec-",
    "syscall",
)
SIDE_EFFECT_EXACT = frozenset({"write-file"})

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


def scan(path_override: str | None = None) -> tuple[list[tuple[str, str, int]], list[str]]:
    """Return (violations, allowlist_reason_errors).

    violations: list of (path, name, line).
    path_override: optional test fixture path (Issue #2494 AC1 self-test).
    """
    allow, reason_errors = load_allowlist()
    violations: list[tuple[str, str, int]] = []
    if path_override:
        paths = [Path(path_override)]
    else:
        paths = sorted((ROOT / "src" / "compiler").glob("evaluator_primitives*.cpp"))
    for path in paths:
        if not path.is_file():
            continue
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


def scan_jit_dispatch_body(service_path: str | None = None) -> list[str]:
    """Issue #3593: aura_jit_prim_dispatch must route through the Evaluator
    choke point (invoke_prim_with_telemetry) with fail-closed owner
    resolution. Flags: lost routing, fail-open owner check, or growth of a
    second bare (*pfn)( call outside the telemetry lambda.

    Returns list of violation strings.
    """
    path = Path(service_path) if service_path else ROOT / "src" / "compiler" / "service.ixx"
    if not path.is_file():
        return [f"{path}: missing — cannot verify #3593 JIT dispatch routing"]
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    impl_start = None
    for i, line in enumerate(lines):
        if "std::int64_t jit_prim_dispatch_impl(" in line:
            impl_start = i
            break
    if impl_start is None:
        return [f"{path}: jit_prim_dispatch_impl not found (#3593)"]
    # Bound at the first column-0 close brace after the impl (its own close
    # is indented) — marker-independent, survives comment reformatting.
    impl_end = len(lines)
    for j in range(impl_start + 1, len(lines)):
        if lines[j].startswith("}"):
            impl_end = j
            break
    body = lines[impl_start:impl_end]
    issues: list[str] = []
    telemetry = [i for i, ln in enumerate(body) if "invoke_prim_with_telemetry(" in ln]
    bare = [i for i, ln in enumerate(body) if "(*pfn)(" in ln]
    owner_check = [i for i, ln in enumerate(body) if "owner_evaluator(prims)" in ln]
    if not telemetry:
        issues.append("service.ixx: jit_prim_dispatch_impl lost invoke_prim_with_telemetry routing (#3593)")
    if not owner_check:
        issues.append("service.ixx: jit_prim_dispatch_impl lost owner_evaluator fail-closed resolution (#3593)")
    if len(bare) != 1:
        issues.append(
            f"service.ixx: jit_prim_dispatch_impl has {len(bare)} (*pfn)( calls — "
            "exactly one, inside invoke_prim_with_telemetry (#3593)"
        )
    elif telemetry and bare[0] < telemetry[0]:
        issues.append("service.ixx: bare (*pfn)( call precedes invoke_prim_with_telemetry (#3593)")
    # The extern "C" entry and the test hook must both delegate to the impl.
    if text.count("jit_prim_dispatch_impl(prim_id, args, argc)") < 2:
        issues.append(
            "service.ixx: aura_jit_prim_dispatch / aura_test_jit_prim_dispatch "
            "must both delegate to jit_prim_dispatch_impl (#3593)"
        )
    rel = str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)
    return [f"{rel}: {msg}" for msg in issues]


def scan_dispatch_callers(dispatch_path: str | None = None) -> list[tuple[str, str, int]]:
    """Issue #3524: flag dispatch_* calls without require_effect predecessor.

    Returns list of (path, callee, line). Missing production TUs are
    reported as callee='<missing-tu>' line=0.
    """
    violations: list[tuple[str, str, int]] = []
    paths: list[Path] = []
    for rel in DISPATCH_TUS:
        p = ROOT / rel
        if not p.is_file():
            violations.append((rel, "<missing-tu>", 0))
            continue
        paths.append(p)
    if dispatch_path:
        extra = Path(dispatch_path)
        if extra.is_file():
            paths.append(extra)
        else:
            violations.append((str(extra), "<missing-tu>", 0))

    for path in paths:
        try:
            rel = str(path.resolve().relative_to(ROOT.resolve()))
        except ValueError:
            rel = str(path)
        if rel in DISPATCH_SKIP_FILES and not (dispatch_path and Path(dispatch_path).resolve() == path.resolve()):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        for i, line in enumerate(lines):
            m = DISPATCH_CALL_RE.search(line)
            if not m:
                continue
            callee = m.group(1)
            # require_effect is the mandated predecessor (#3524). PrimMeta
            # markers do not count for dispatch_* (trust-the-caller hole).
            start = max(0, i - 80)
            chunk = "\n".join(lines[start : i + 1])
            if "require_effect(" in chunk:
                continue
            violations.append((rel, callee, i + 1))
    return violations


def scan_infer_mse_bits(side_effect_path: str | None = None) -> list[str]:
    """Issue #3596: the agent:/synthesize:/strategy: prefix infer must demand
    Mutate | MacroSelfEvo — dispatch MSE gate parity with effect_for_cap_name
    (#2489/#2583 residual; expand sites were closed by #3378)."""
    path = ROOT / side_effect_path if side_effect_path else ROOT / "src" / "compiler" / "security_side_effect.hh"
    if not path.is_file():
        return [f"{path}: missing — cannot verify #3596 infer MSE bits"]
    text = path.read_text(encoding="utf-8", errors="replace")
    idx = text.find('name.starts_with("agent:")')
    if idx < 0:
        return ["#3596: agent:/synthesize:/strategy: infer block not found"]
    window = text[idx : idx + 400]
    issues: list[str] = []
    if "kEffectMacroSelfEvo" not in window:
        issues.append(
            "#3596: agent:/synthesize:/strategy: infer must include kEffectMacroSelfEvo "
            "(Mutate|MSE — dispatch gate parity with effect_for_cap_name)"
        )
    if "kEffectMutate" not in window:
        issues.append("#3596: agent:/synthesize:/strategy: infer must keep kEffectMutate")
    return issues


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 on violations (default for ./build.py gate)",
    )
    # Issue #2494: --path overrides the production PRIM_GLOB so the
    # AC1 self-test can point the gate at a fixture prim without
    # contaminating src/compiler/. CI / build.py gate never pass --path.
    ap.add_argument(
        "--path",
        action="store",
        default=None,
        help="override prim scan path (test fixture only — CI never passes this)",
    )
    # Issue #3524: fixture TU for dispatch_batch without require_effect.
    ap.add_argument(
        "--dispatch-path",
        action="store",
        default=None,
        help="extra TU to scan for dispatch_* calls (test fixture only)",
    )
    # Issue #3593: fixture override for the JIT dispatch body scan.
    ap.add_argument(
        "--jit-dispatch-path",
        action="store",
        default=None,
        help="override service.ixx path for the #3593 dispatch scan (test fixture only)",
    )
    args = ap.parse_args()

    violations, reason_errors = scan(args.path)
    dispatch_violations = scan_dispatch_callers(args.dispatch_path)
    jit_violations = scan_jit_dispatch_body(args.jit_dispatch_path)
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

    if dispatch_violations:
        print("FAIL: dispatch_* without require_effect predecessor (Issue #3524):")
        for path, name, line in dispatch_violations:
            print(f"  + {name}  [{path}:{line}]")
        print(
            "\nJIT/FFI/ir_executor callers of dispatch_batch / dispatch_cellgrid\n"
            "must precede the call with require_effect(Render) and pass\n"
            "mint_render_effect_token(...) — no default token (#3524)."
        )
        failed = True

    infer_violations = scan_infer_mse_bits()

    if infer_violations:
        print("FAIL: infer MSE-bit violations (Issue #3596):")
        for v in infer_violations:
            print(f"  + {v}")
        print(
            "\nagent:/synthesize:/strategy: are the #2489 self-evo family —\n"
            "dispatch infer must demand kEffectMutate | kEffectMacroSelfEvo\n"
            "(#3596), not Mutate-only."
        )
        failed = True

    if jit_violations:
        print("FAIL: JIT prim dispatch routing violations (Issue #3593):")
        for v in jit_violations:
            print(f"  + {v}")
        print(
            "\naura_jit_prim_dispatch must route through invoke_prim_with_telemetry\n"
            "with fail-closed owner_evaluator resolution — exactly one (*pfn)(,\n"
            "inside the telemetry lambda (#3593)."
        )
        failed = True

    if not failed:
        print("OK: side-effect security coverage (Issue #2057/#2152/#3524/#3593/#3596) — no uncovered effectful prims")
        return 0

    return 1 if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
