#!/usr/bin/env python3
"""Issue #4671: the JIT side of the per-op typed-entry readiness re-sample.

Custom (window-scoped) static gate. The #4671 contract is an ORDER
contract: the JIT CastOp / Apply op sites must consult the per-op
readiness sample BEFORE the paths they guard, and the JIT bridge must
delegate to the SAME #4667 helper (one readiness model, no new
counter). A substring manifest cannot express "the consult precedes the
narrow-evidence fast-forward", so this gate re-derives the order from
the source with regex + window slicing.

  AC1  the JIT per-op bridge symbol exists (bridge + header declaration)
       and the ORC registration points at it.
  AC2  the bridge delegates to typed_audit::linear_per_op_typed_entry_
       readiness_ok (the #4667 helper) and declares NO new counter.
  AC3  the weak stub keeps the #3343 polarity (production refuses /
       Soft-Off allows).
  AC4  the CastOp site consults the per-op sample BEFORE the
       narrow-evidence fast-forward and BEFORE the runtime type check.
  AC5  the Apply site consults the per-op sample on the NON-linear path
       (the linear path keeps the #3759 begin_linear_epoch_fence).

Usage:
  python3 scripts/coverage/checks/check_jit_per_op_typed_entry_readiness_4671.py
  python3 scripts/coverage/checks/check_jit_per_op_typed_entry_readiness_4671.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

BRIDGE_REL = "src/compiler/aura_jit_bridge.cpp"
HEADER_REL = "src/compiler/aura_jit_bridge.h"
STUB_REL = "src/compiler/aura_jit_bridge_stub.cpp"
JIT_REL = "src/compiler/aura_jit.cpp"

SYM = "aura_jit_linear_per_op_typed_entry_readiness_ok"
DELEGATE = "linear_per_op_typed_entry_readiness_ok()"
GATE = "emit_per_op_readiness_gate"

# The op sites and the window each check is scoped to.
CAST_SITE_RE = re.compile(r"case OpCastOp: \{")
APPLY_SITE_RE = re.compile(r"case OpApply: \{")
BRIDGE_DEF_RE = re.compile(r'extern "C" int ' + re.escape(SYM) + r"\(void\) \{")
STUB_DEF_RE = re.compile(r'extern "C" __attribute__\(\(weak\)\) int ' + re.escape(SYM) + r"\(void\) \{")
REG_RE = re.compile(r'reg\("' + re.escape(SYM) + r'"')
HEADER_DECL_RE = re.compile(r"int " + re.escape(SYM) + r"\(void\);")

# The per-op helper the bridge must delegate to (the #4667 shared sample).
DELEGATE_RE = re.compile(re.escape(DELEGATE))


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _body(text: str, open_idx: int, limit: int = 1200) -> str:
    """Window of text starting at an opening brace (bounded)."""
    return text[open_idx : open_idx + limit]


def _case_body(text: str, pos: int, limit: int = 4000) -> str:
    """The body of one switch case (bounded by the next case label).

    The JIT op lowering is one big switch; a window that spills into the
    NEXT case would find a consult that belongs to a different opcode, so
    the case body is cut at the next case label at the SAME indentation.
    """
    line_start = text.rfind("\n", 0, pos) + 1
    indent = text[line_start:pos]
    win = text[pos : pos + limit]
    m = re.search(r"\n" + re.escape(indent) + r"case ", win)
    if m:
        win = win[: m.start()]
    return win


def check_bridge(bridge: str, header: str) -> list[str]:
    fails: list[str] = []
    m = BRIDGE_DEF_RE.search(bridge)
    if not m:
        fails.append(f"AC1: the strong JIT per-op bridge symbol {SYM} is missing from {BRIDGE_REL} (#4671)")
    else:
        win = _body(bridge, m.start(), 900)
        # AC2: the bridge delegates to the SAME #4667 helper.
        if not DELEGATE_RE.search(win):
            fails.append(
                f"AC2: the JIT per-op bridge does not delegate to {DELEGATE} - the JIT and the "
                f"interpreter must share ONE readiness face (#4671)"
            )
        # AC2: no second readiness model / no new counter.
        if "std::atomic" in win:
            fails.append(
                "AC2: the JIT per-op bridge declares a new counter - reuse "
                "g_linear_fast_path_elide_blocked_production_total (#4671)"
            )
    if not HEADER_DECL_RE.search(header):
        fails.append(f"AC1: {HEADER_REL} does not declare {SYM} - the JIT cannot resolve it (#4671)")
    return fails


def check_stub(stub: str) -> list[str]:
    fails: list[str] = []
    m = STUB_DEF_RE.search(stub)
    if not m:
        fails.append(f"AC3: the weak light-link stub for {SYM} is missing from {STUB_REL} (#4671)")
        return fails
    win = _body(stub, m.start(), 700)
    if "stub_production_defaults_active()" not in win:
        fails.append("AC3: the weak stub does not probe the production-defaults face (#4671)")
    # #3343 polarity: production refuses (0), Soft / Off allows (1).
    if not re.search(r"return 0;", win):
        fails.append("AC3: the weak stub never refuses - production must fail closed (#4671)")
    if not re.search(r"return 1;", win):
        fails.append("AC3: the weak stub never allows - Soft / Off must stay zero-cost (#4671)")
    return fails


def check_jit(jit: str) -> list[str]:
    fails: list[str] = []
    if not REG_RE.search(jit):
        fails.append(f"AC1: the JIT never registers {SYM} with the ORC session - the op site cannot resolve it (#4671)")

    # AC4: the CastOp site consults the sample BEFORE both cast paths.
    m = CAST_SITE_RE.search(jit)
    if not m:
        fails.append("AC4: the JIT CastOp op site was not found (#4671)")
    else:
        win = _case_body(jit, m.start())
        gate = win.find(GATE)
        narrow = win.find("inst.narrow_evidence")
        cast_call = win.find("fn_cast_op")
        if gate < 0:
            fails.append("AC4: the CastOp site does not consult the per-op sample (#4671)")
        else:
            if narrow >= 0 and gate > narrow:
                fails.append(
                    "AC4: the CastOp consult comes AFTER the narrow-evidence fast-forward - "
                    "the fast path would ride the entry-era grant (#4671)"
                )
            if cast_call >= 0 and gate > cast_call:
                fails.append(
                    "AC4: the CastOp consult comes AFTER the runtime type check - the cast "
                    "would run on a revoked type cone (#4671)"
                )

    # AC5: the Apply site consults the sample on the NON-linear path.
    m = APPLY_SITE_RE.search(jit)
    if not m:
        fails.append("AC5: the JIT Apply op site was not found (#4671)")
    else:
        win = _case_body(jit, m.start())
        gate = win.find(GATE)
        nonlin = win.find("if (!lin)")
        if gate < 0:
            fails.append("AC5: the Apply site does not consult the per-op sample (#4671)")
        else:
            if nonlin < 0:
                fails.append(
                    "AC5: the Apply consult is not scoped to the non-linear path - the "
                    "linear path already consults the face via begin_linear_epoch_fence (#4671)"
                )
            elif nonlin > gate:
                fails.append("AC5: the Apply consult is not gated on the non-linear path (#4671)")
        if "begin_linear_epoch_fence()" not in win:
            fails.append("AC5: the linear Apply path lost its #3759 begin_linear_epoch_fence (#4671)")
    return fails


def _synthetic(
    *,
    cast_gate: bool = True,
    apply_gate: bool = True,
    nonlin_guard: bool = True,
    delegate: bool = True,
) -> tuple[str, str, str, str]:
    """Build synthetic sources for the self-test."""
    bridge = (
        'extern "C" int aura_jit_ir_typed_entry_commit_readiness_ok(void) {\n'
        "    return 1;\n"
        "}\n"
        "// Issue #4671: JIT-side per-op typed-entry readiness re-sample.\n"
        'extern "C" int aura_jit_linear_per_op_typed_entry_readiness_ok(void) {\n'
        + (
            "    return aura::compiler::typed_audit::linear_per_op_typed_entry_readiness_ok() ? 1 : 0;\n"
            if delegate
            else "    return 1;\n"
        )
        + "}\n"
    )
    header = "int aura_jit_linear_per_op_typed_entry_readiness_ok(void);\n"
    stub = (
        'extern "C" __attribute__((weak)) int '
        "aura_jit_linear_per_op_typed_entry_readiness_ok(void) {\n"
        "    if (stub_production_defaults_active())\n"
        "        return 0;\n"
        "    return 1;\n"
        "}\n"
    )
    jit = (
        'reg("aura_jit_linear_per_op_typed_entry_readiness_ok", (void*)x);\n'
        "case OpApply: {\n"
        "    const bool lin = inst.linear_ownership_state != 0;\n"
        "    LinearEpochFence fb{};\n"
        "    if (lin)\n"
        "        fb = begin_linear_epoch_fence();\n"
        + ("    if (!lin)\n" if nonlin_guard else "    if (lin)\n")
        + ("        emit_per_op_readiness_gate(cur_block, cur_instr);\n" if apply_gate else "")
        + "    return true;\n"
        "}\n"
        "case OpCastOp: {\n"
        + ("    emit_per_op_readiness_gate(cur_block, cur_instr);\n" if cast_gate else "")
        + "    if (inst.narrow_evidence != 0) { store(0, load(1)); return true; }\n"
        "    auto call = irb->CreateCall(llvm::FunctionCallee(fn_cast_op), {val});\n"
        "    store(0, call);\n"
        "    return true;\n"
        "}\n"
    )
    return bridge, header, stub, jit


def _self_test() -> int:
    good = _synthetic()
    fails = check_bridge(good[0], good[1]) + check_stub(good[2]) + check_jit(good[3])
    if fails:
        print("FAIL #4671 self-test: the well-formed source should pass:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1

    cases = (
        ("the CastOp consult is missing", _synthetic(cast_gate=False), "AC4"),
        ("the Apply consult is missing", _synthetic(apply_gate=False), "AC5"),
        (
            "the Apply consult is not on the non-linear path",
            _synthetic(nonlin_guard=False),
            "AC5",
        ),
        (
            "the bridge does not delegate to the #4667 helper",
            _synthetic(delegate=False),
            "AC2",
        ),
    )
    for label, synth, tag in cases:
        bridge, header, stub, jit = synth
        fails = check_bridge(bridge, header) + check_stub(stub) + check_jit(jit)
        if not any(tag in f for f in fails):
            print(f"FAIL #4671 self-test: {label} was not caught", file=sys.stderr)
            for f in fails:
                print(f"  - {f}", file=sys.stderr)
            return 1

    print("OK #4671 self-test: the JIT per-op readiness gate catches the regressions")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Issue #4671 JIT per-op typed-entry readiness gate")
    ap.add_argument("--self-test", action="store_true", help="run the built-in probes")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    bridge = _read(BRIDGE_REL)
    header = _read(HEADER_REL)
    stub = _read(STUB_REL)
    jit = _read(JIT_REL)
    if not bridge or not header or not stub or not jit:
        print("FAIL #4671 jit_per_op_typed_entry_readiness: missing source files", file=sys.stderr)
        return 1

    fails = check_bridge(bridge, header) + check_stub(stub) + check_jit(jit)
    if fails:
        print("FAIL #4671 jit_per_op_typed_entry_readiness:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print(
        "OK #4671 jit_per_op_typed_entry_readiness: the JIT CastOp / Apply sites consult the "
        "same per-op readiness face as the interpreter"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
