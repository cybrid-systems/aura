#!/usr/bin/env python3
"""Issue #2942: mandate require_effect_for_node_id on workspace NodeId side effects.

Closes residual late-isolation window after #2839 / #2881: every workspace-
mutating prim that takes a concrete NodeId must go through
require_effect_for_node_id or require_effect_on_ref. Bare 2-arg
require_effect is only for documented non-workspace ops (file/io/network/exec).

Issue #3040 successor: compile:/verify:/syntax: NodeId writers that never
called require_effect at all are gated by check_compile_node_id_entry_3040.py.

Contract (one row per AC):
  AC1  add_mutate / NodeId workspace prims use for_node_id or on_ref
  AC2  EXEMPT_2ARG_OPS documents non-workspace ops only (with rationale)
  AC3  Soft/Off lineage preserved; additive schema-2942 only
  AC4  Drift scan: bare require_effect near NodeId without mandated helpers
  AC5  Additive schema keys; #2839 / #2881 / #2689 lineage preserved
  AC6  Source-cite + tests in existing suites; no invent/design
  AC7  Issue #3526 reverse rule: flag 3-arg require_effect(req, op, node_id)
       (or 4-arg with literal 0 in 4th position) in a non-EXEMPT_2ARG_OPS
       function. Suggest require_effect_for_node_id. 2-arg io/exec exempt
       ops (write-file / git-commit / deny_sys / …) stay allowed. No new
       query key — reuse nodeid_only_entry_prevented_total (#3040).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Prim TUs that may host side-effect + NodeId paths (full residual mandate).
SCOPE_FILES = [
    "src/compiler/evaluator_security.cpp",
    "src/compiler/evaluator_primitives_mutate.cpp",
    "src/compiler/evaluator_primitives_mutation.cpp",
    "src/compiler/evaluator_primitives_compile.cpp",
    "src/compiler/evaluator_primitives_runtime.cpp",
    "src/compiler/evaluator_primitives_io.cpp",
    "src/compiler/evaluator_primitives_messaging.cpp",
    "src/compiler/evaluator_primitives_file.cpp",
    "src/compiler/evaluator_primitives_security.cpp",
    "src/compiler/evaluator_primitives_agent.cpp",
    "src/compiler/evaluator_fiber_mutation.cpp",
    "src/compiler/evaluator_primitives_query_workspace.cpp",
    "src/compiler/evaluator_primitives_diagnostic.cpp",
    "src/compiler/evaluator_primitives_memory.cpp",
    "src/compiler/evaluator_primitives_module.cpp",
    "src/compiler/evaluator_primitives_obs_jit.cpp",
    "src/compiler/evaluator_primitives_json.cpp",
    # #2942 residual: workspace layer ops also mention NodeId
    "src/compiler/evaluator_primitives_workspace.cpp",
]

# Documented exempt 2-arg require_effect ops (non-workspace NodeId).
# Same inventory as #2839 + #2881 EXEMPT_2ARG_OPS — keep in lockstep.
EXEMPT_2ARG_OPS: dict[str, str] = {
    "write-file": "filesystem, not workspace node (#2839)",
    "mutation-log-compact": "log maintenance, no NodeId target (#2839)",
    "security:check-effect": "capability probe, not mutate body (#2839)",
    "git-commit": "exec+network (Issue #2072) — no NodeId target (#2881)",
    "deny_sys": "syscall wrapper (Issue #1329) — cap is string, no NodeId (#2881)",
}

# Ops that may appear as 2-arg require_effect when target_node == 0 inside
# add_mutate (global mutate without concrete NodeId). Not EXEMPT_2ARG_OPS
# inventory entries; the linter accepts require_effect only when the same
# function also contains for_node_id / on_ref (mandated NodeId branch).
NODE_ID_MARKERS = re.compile(r"\b(?:NodeId|target_node|node_id|ast::NodeId)\b")
BARE_REQUIRE = re.compile(r"\brequire_effect\s*\(")
MANDATED = re.compile(r"\b(?:require_effect_for_node_id|require_effect_on_ref)\s*\(")
# Call shape: require_effect( bits, "op" )  — 2-arg string op
TWO_ARG_OP = re.compile(r'\brequire_effect\s*\(\s*[^,]+,\s*"([^"]+)"\s*\)')
# 3/4-arg with NodeId target: require_effect(bits, op, target_node[, tenant])
MULTI_ARG_NODE = re.compile(
    r"\brequire_effect\s*\([^;]{0,200}?\b(?:target_node|node_id|nid)\b",
    re.DOTALL,
)
# Issue #3526: bare require_effect( — not on_ref / for_node_id (those have
# a suffix before the opening paren, so this pattern does not match them).
REQUIRE_CALL = re.compile(r"\brequire_effect\s*\(")
LIT_ZERO = re.compile(r"^0(?:[uUlL]+)?$")
TYPEISH = re.compile(r"\b(?:std::|ast::|uint16_t|uint32_t|uint64_t|NodeId|string_view|noexcept)\b")


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _strip_comments(text: str) -> str:
    # Remove // line comments and /* */ blocks so comment cites don't
    # false-positive bare require_effect scans.
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return text


def _split_args(inner: str) -> list[str]:
    args: list[str] = []
    buf: list[str] = []
    depth = 0
    for ch in inner:
        if ch == "(":
            depth += 1
            buf.append(ch)
        elif ch == ")":
            depth -= 1
            buf.append(ch)
        elif ch == "," and depth == 0:
            args.append("".join(buf).strip())
            buf = []
        else:
            buf.append(ch)
    tail = "".join(buf).strip()
    if tail:
        args.append(tail)
    return args


def _iter_require_effect_calls(code: str) -> list[tuple[int, list[str]]]:
    out: list[tuple[int, list[str]]] = []
    for m in REQUIRE_CALL.finditer(code):
        i = m.end()
        depth = 1
        j = i
        n = len(code)
        while j < n and depth:
            if code[j] == "(":
                depth += 1
            elif code[j] == ")":
                depth -= 1
            j += 1
        args = _split_args(code[i : j - 1])
        line = code.count("\n", 0, m.start()) + 1
        out.append((line, args))
    return out


def _is_signature(args: list[str]) -> bool:
    return any(TYPEISH.search(a) for a in args)


def _is_lit_zero(s: str) -> bool:
    return bool(LIT_ZERO.match(s.strip()))


def _op_name(args: list[str]) -> str | None:
    if len(args) < 2:
        return None
    m = re.fullmatch(r'"([^"]+)"', args[1].strip())
    return m.group(1) if m else None


def scan_three_arg_default(code: str, rel: str) -> list[str]:
    """Issue #3526 reverse rule: 3-arg NodeId / 4-arg literal tenant 0."""
    fails: list[str] = []
    for line, args in _iter_require_effect_calls(code):
        if _is_signature(args):
            continue
        op = _op_name(args)
        if op in EXEMPT_2ARG_OPS:
            continue
        n = len(args)
        if n == 3 and not _is_lit_zero(args[2]):
            fails.append(
                f"AC7: {rel}:{line}: 3-arg require_effect(req, op, node_id) "
                f"defaults ref_tenant=0 — use require_effect_for_node_id "
                f"(op={op or args[1]!r})"
            )
        elif n >= 4 and _is_lit_zero(args[3]) and not _is_lit_zero(args[2]):
            fails.append(
                f"AC7: {rel}:{line}: 4-arg require_effect(..., node_id, 0) "
                f"— use require_effect_for_node_id or pass a real tenant "
                f"(op={op or args[1]!r})"
            )
    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    # Issue #3526: fixture TU for 3-arg require_effect (test only; CI never passes).
    ap.add_argument(
        "--probe",
        action="store",
        default=None,
        help="extra TU to scan for 3-arg require_effect (test fixture only)",
    )
    args = ap.parse_args()

    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    sec = _read("src/compiler/evaluator_security.cpp")
    mutate = _read("src/compiler/evaluator_primitives_mutate.cpp")
    evaluator_ixx = _read("src/compiler/evaluator.ixx")
    posture = _read("src/compiler/evaluator_primitives_security.cpp")
    compile_cpp = _read("src/compiler/evaluator_primitives_compile.cpp")
    io_cpp = _read("src/compiler/evaluator_primitives_io.cpp")
    test_re = _read("tests/compiler/test_require_effect_auto_isolation.cpp")
    test_ts = _read("tests/compiler/test_tenant_scope_fiber_mandate.cpp")
    build = _read("build.py")
    linter_self = _read("scripts/coverage/checks/check_side_effect_node_id_mandate_2942.py")

    # ── AC1: workspace NodeId paths use mandated helpers ──
    must("Issue #2942", "AC1", sec)
    must("require_effect_for_node_id", "AC1", sec)
    must("require_effect_on_ref", "AC1", sec)
    # add_mutate must route NodeId through for_node_id / on_ref (not bare
    # require_effect with target_node + default ref_tenant).
    must("require_effect_for_node_id", "AC1", mutate)
    must("require_effect_on_ref", "AC1", mutate)
    must("Issue #2942", "AC1", mutate)
    # Canonical #2839/#2881 NodeId site preserved.
    must("require_effect_for_node_id", "AC1", compile_cpp)
    # Must NOT have require_effect(..., target_node, ref_tenant) bare form
    # in add_mutate (converted under #2942).
    if re.search(
        r"require_effect\s*\([^;]*target_node\s*,\s*ref_tenant",
        _strip_comments(mutate),
    ):
        fails.append("AC1: mutate.cpp still has bare require_effect(…, target_node, ref_tenant)")

    # ── AC2: exempt ops documented (non-workspace only) ──
    for op, reason in EXEMPT_2ARG_OPS.items():
        if op not in linter_self:
            fails.append(f"AC2: EXEMPT_2ARG_OPS missing {op!r}")
        if not reason:
            fails.append(f"AC2: empty rationale for exempt op {op!r}")
    must("write-file", "AC2", io_cpp + _read("src/compiler/evaluator_primitives_file.cpp"))
    must("git-commit", "AC2", io_cpp)
    must("deny_sys", "AC2", io_cpp)
    if len(EXEMPT_2ARG_OPS) != 5:
        fails.append(
            f"AC2: EXEMPT_2ARG_OPS count={len(EXEMPT_2ARG_OPS)} expected 5 "
            "(lockstep with #2881 kResidualNodeIdExemptOpsCount)"
        )
    # Constants match.
    m_ex = re.search(r"kNodeIdMandateExemptOpsCount\s*=\s*(\d+)", evaluator_ixx)
    if not m_ex or int(m_ex.group(1)) != len(EXEMPT_2ARG_OPS):
        fails.append("AC2: kNodeIdMandateExemptOpsCount missing or != EXEMPT_2ARG_OPS count")

    # ── AC3: Soft/Off — additive only (schema present; no behavior hard-assert
    # in require_effect body for Off paths). Cite Soft zero-cost lineage. ──
    must("schema-2942", "AC3", posture)
    must("kNodeIdMandateWired", "AC3", evaluator_ixx)
    # Soft path comments / Off unchanged in security entry.
    must("require_effect", "AC3", sec)

    # ── AC4: drift scan — bare require_effect near NodeId without mandated ──
    for rel in SCOPE_FILES:
        text = _read(rel)
        if not text:
            # workspace may exist; missing SCOPE entry is a fail
            if not (ROOT / rel).is_file():
                fails.append(f"AC4: SCOPE_FILES lists {rel} but file missing")
            continue
        code = _strip_comments(text)
        # Skip the definition of require_effect itself.
        if rel.endswith("evaluator_security.cpp"):
            # Only scan call sites outside the method definitions — still
            # check definitions aren't the only for_node_id.
            continue
        if not NODE_ID_MARKERS.search(code):
            continue
        # File mentions NodeId + bare require_effect without mandated helpers
        # is a residual violation — unless every 2-arg op is exempt.
        if BARE_REQUIRE.search(code) and not MANDATED.search(code):
            # Collect 2-arg ops; all must be exempt.
            ops = TWO_ARG_OP.findall(code)
            bad = [o for o in ops if o not in EXEMPT_2ARG_OPS]
            # Also catch multi-arg with target_node without mandated helpers.
            if MULTI_ARG_NODE.search(code) or bad:
                fails.append(
                    f"AC4: {rel}: NodeId + bare require_effect without for_node_id/on_ref (ops={ops or ['?']})"
                )
        # File has both NodeId markers and bare multi-arg target_node form.
        if MULTI_ARG_NODE.search(code) and not MANDATED.search(code):
            fails.append(f"AC4: {rel}: require_effect(…, target_node/node_id) without for_node_id/on_ref")
        # When file has bare require_effect AND NodeId, mandated helpers
        # must also appear (mutate pattern: branch for_node_id + 2-arg
        # no-target fallback).
        if NODE_ID_MARKERS.search(code) and BARE_REQUIRE.search(code) and MANDATED.search(code):
            # 2-arg ops that are not exempt and not the generic mutate
            # no-target fallback (op is a variable, not a string) — only
            # fail on literal non-exempt string ops.
            for op in TWO_ARG_OP.findall(code):
                if op not in EXEMPT_2ARG_OPS:
                    # Variable-op 2-arg is OK when for_node_id also present
                    # (add_mutate no-target branch uses `op` variable).
                    # Literal string ops must be exempt.
                    fails.append(f"AC4: {rel}: non-exempt 2-arg require_effect op {op!r}")

    # ── AC5: additive schema + lineage ──
    must("schema-2942", "AC5", posture)
    must("issue-2942", "AC5", posture)
    must("node-id-side-effect-mandate-wired", "AC5", posture)
    must("node-id-mandate-exempt-ops-count", "AC5", posture)
    must("schema-2881", "AC5", posture)
    must("schema-2839", "AC5", posture)
    must("require-effect-for-node-id-wired", "AC5", posture)
    must("check_side_effect_node_id_mandate_2942", "AC5", build)
    must("kNodeIdMandateExemptOpsCount", "AC5", evaluator_ixx)
    must("kNodeIdMandateWired", "AC5", evaluator_ixx)

    # ── AC6: tests + no invent/design ──
    must("2942", "AC6", test_re)
    must("2942", "AC6", test_ts)
    if (ROOT / "tests" / "compiler" / "test_issue_2942.cpp").is_file():
        fails.append("AC6: test_issue_2942.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2942-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # ── AC7: Issue #3526 reverse rule — 3-arg NodeId / 4-arg literal 0 ──
    must("Issue #3526", "AC7", linter_self)
    must("require_effect_for_node_id", "AC7", linter_self)
    must("scan_three_arg_default", "AC7", linter_self)
    must("Issue #3526", "AC7", sec)
    must("Issue #3526", "AC7", evaluator_ixx)
    must("3526", "AC7", test_re)
    # io/exec 2-arg exempt ops stay documented (do not flag legitimate 2-arg).
    must("git-commit", "AC7", io_cpp)
    must("deny_sys", "AC7", io_cpp)
    must("write-file", "AC7", io_cpp + _read("src/compiler/evaluator_primitives_file.cpp"))
    if (ROOT / "tests" / "compiler" / "test_issue_3526.cpp").is_file():
        fails.append("AC7: test_issue_3526.cpp present (forbidden per #81967)")
    if docs.is_dir():
        for f in sorted(docs.glob("3526-*")):
            fails.append(f"AC7: docs/design/{f.name} present (forbidden per #1655)")
    reverse_files = list(SCOPE_FILES) + [
        "src/compiler/evaluator.ixx",
        "src/compiler/evaluator_module_loader.cpp",
    ]
    if args.probe:
        reverse_files.append(args.probe)
    for rel in reverse_files:
        p = Path(rel)
        text = (
            _read(rel)
            if not p.is_absolute()
            else (p.read_text(encoding="utf-8", errors="replace") if p.is_file() else "")
        )
        if not text:
            if args.probe and rel == args.probe:
                fails.append(f"AC7: --probe {rel} missing")
            continue
        fails.extend(scan_three_arg_default(_strip_comments(text), rel))

    # Cross-check #2881/#2839 residual linter still green.
    r = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_side_effect_fiber_principal_2839.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        fails.append(f"check_side_effect_fiber_principal_2839 regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(
        "OK: Issue #2942/#3526 side-effect NodeId mandate (for_node_id/on_ref; no 3-arg default ref_tenant=0 residual)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
