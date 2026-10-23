#!/usr/bin/env python3
"""Issue #4676: a JIT readiness gate emitted into the AOT IR must have a
weak stub in lib/runtime.c, or the standalone --emit-binary link fails.

  CI symptom
      x emit:merge (no binary)       aot_err: type warning (eval_ir): 91:25: ...
      x emit:uniq (no binary)
      x emit:list-merge (no binary)
      x emit:bin-search (no binary)
      x emit:factorial (no binary)
      x emit:range (no binary)
      x emit:sorted? (no binary)
      x emit:combine (no binary)

  Mechanism
      #4671 added the per-op typed-entry readiness sample. The gate is
      emitted into the IR at the CastOp / non-linear Apply op sites and
      calls aura_jit_linear_per_op_typed_entry_readiness_ok - a C++
      symbol defined in aura_jit_bridge.cpp (strong) and
      aura_jit_bridge_stub.cpp (weak, light-link).

      The STANDALONE AOT link never sees either of those. It compiles
      lib/runtime.c (pure C), the generated registration/prim .c and the
      per-function LLVM .o, then links with -lm - no C++ bridge objects
      at all. lib/runtime.c already carried the weak stubs for the
      SIBLING gates (aura_jit_linear_move_drop_elision_ok,
      aura_jit_ir_typed_entry_commit_readiness_ok); #4671 added the
      per-op symbol to the bridge and the light-link stub but NOT to
      lib/runtime.c, so the standalone link has no definition of it.

      The 8 failing cases are exactly the 8 run_emit_test cases with an
      (import "std/...") form: the std lowering reaches a CastOp /
      non-linear Apply site, so the per-op gate is emitted for those
      programs. Every pure-literal emit case never reaches that op site,
      never emits the gate, and links fine.

  What this gate does
      AC1  the per-op gate has a weak stub in lib/runtime.c, with the
           Soft/Off polarity (return 1 - a standalone AOT binary has no
           typed-audit state and no production profile, so the gate is
           open).
      AC2  GENERAL invariant: every readiness/elision gate declared in
           aura_jit_bridge.h (the aura_jit_*_ok family) has a weak stub
           in lib/runtime.c. This is the rule that would have caught the
           #4671 omission automatically, for the next gate as well.
      AC3  the JIT really does emit the per-op gate into the IR
           (ExternalLinkage declaration + a call site), so the stub is
           load-bearing rather than dead.
      AC4  lib/runtime.c is NOT linked into a host target, so its weak
           stub can never shadow the bridge's strong definition.

Usage:
  python3 scripts/coverage/checks/check_aot_runtime_weak_stub_4676.py
  python3 scripts/coverage/checks/check_aot_runtime_weak_stub_4676.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

RUNTIME_REL = "lib/runtime.c"
HEADER_REL = "src/compiler/aura_jit_bridge.h"
JIT_REL = "src/compiler/aura_jit.cpp"
CMAKE_REL = "CMakeLists.txt"

# The gate #4671 introduced (the one the standalone link was missing).
PER_OP_SYM = "aura_jit_linear_per_op_typed_entry_readiness_ok"

# The readiness/elision gate family declared in the bridge header. Every
# member of this family can be emitted into the AOT IR, so every member
# needs a weak stub in lib/runtime.c.
GATE_FAMILY_RE = re.compile(r"int\s+(aura_jit_[A-Za-z0-9_]*_ok)\s*\(void\);")

# The standalone-AOT runtime source, matched as a PATH so that
# "test_spec_runtime.cpp" and "aura_jit_runtime.cpp" do not match.
RUNTIME_PATH_RE = re.compile(r"(?<![A-Za-z0-9_./-])(?:lib/)?runtime\.c(?![A-Za-z0-9_])")


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _weak_stub_bodies(text: str) -> dict[str, str]:
    """Map symbol -> body of its __attribute__((weak)) definition."""
    out: dict[str, str] = {}
    for m in re.finditer(
        r"__attribute__\(\(weak\)\)[^;{]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^)]*\)\s*\{",
        text,
    ):
        sym = m.group(1)
        depth, i = 1, m.end()
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        out[sym] = text[m.end() : i - 1]
    return out


def _gate_family(header: str) -> list[str]:
    seen: list[str] = []
    for m in GATE_FAMILY_RE.finditer(header):
        if m.group(1) not in seen:
            seen.append(m.group(1))
    return seen


def check_per_op_stub(runtime: str) -> list[str]:
    """AC1: the per-op gate has a weak stub with the Soft/Off polarity."""
    fails: list[str] = []
    stubs = _weak_stub_bodies(runtime)
    body = stubs.get(PER_OP_SYM)
    if body is None:
        fails.append(
            f"AC1: {RUNTIME_REL} has NO weak stub for {PER_OP_SYM} - the standalone "
            "--emit-binary link has no definition of the #4671 per-op gate and fails "
            "with 'undefined reference' (#4676)"
        )
        return fails
    if not re.search(r"return\s+1\s*;", body):
        fails.append(
            f"AC1: the weak stub for {PER_OP_SYM} does not return 1 - a standalone AOT "
            "binary has no typed-audit state and no production profile, so the gate is "
            "open (the Soft/Off answer), like its sibling entry gate (#4676)"
        )
    if "4676" not in runtime and "4671" not in runtime:
        fails.append(
            f"AC1: the {RUNTIME_REL} stub for {PER_OP_SYM} cites neither #4676 nor "
            "#4671 - the reason the stub exists must be recorded (#4676)"
        )
    return fails


def check_gate_family(runtime: str, header: str) -> list[str]:
    """AC2: every bridge-header gate has a weak stub in lib/runtime.c."""
    fails: list[str] = []
    stubs = _weak_stub_bodies(runtime)
    for sym in _gate_family(header):
        if sym not in stubs:
            fails.append(
                f"AC2: {RUNTIME_REL} has no weak stub for the bridge gate {sym} - a gate "
                "emitted into the AOT IR would fail the standalone --emit-binary link "
                "with 'undefined reference' (#4676)"
            )
    return fails


def check_jit_emits(jit: str) -> list[str]:
    """AC3: the JIT really emits the per-op gate into the IR."""
    fails: list[str] = []
    if PER_OP_SYM not in jit:
        fails.append(
            f"AC3: {JIT_REL} never mentions {PER_OP_SYM} - the gate is not emitted into "
            "the IR, so the runtime.c stub this check requires is not load-bearing (#4676)"
        )
        return fails
    if not re.search(
        r'llvm::Function::Create\((?:[^;]*?)llvm::Function::ExternalLinkage,\s*\n?\s*"' + re.escape(PER_OP_SYM) + r'"',
        jit,
    ):
        fails.append(
            f"AC3: {JIT_REL} does not declare {PER_OP_SYM} with ExternalLinkage - the "
            "gate must be an external symbol for the standalone AOT link to need a "
            "runtime.c definition (#4676)"
        )
    if not re.search(r"CreateCall\(llvm::FunctionCallee\(fn_linear_per_op_typed_entry_readiness_ok\)", jit):
        fails.append(
            f"AC3: {JIT_REL} never calls {PER_OP_SYM} in the emitted IR - the stub would "
            "be dead code and the standalone link would not actually need it (#4676)"
        )
    return fails


def check_runtime_not_in_host(cmake: str) -> list[str]:
    """AC4: lib/runtime.c is not linked into a host target."""
    fails: list[str] = []
    # Any mention of the FILE lib/runtime.c in a target_sources() /
    # add_executable() / add_library() line would put the weak stub into a
    # host binary. Match the path, not a loose substring: "runtime.c" is a
    # prefix of "test_spec_runtime.cpp" and of "aura_jit_runtime.cpp".
    for i, line in enumerate(cmake.splitlines(), 1):
        if not RUNTIME_PATH_RE.search(line):
            continue
        if re.search(r"(target_sources|add_executable|add_library)\s*\(", line):
            fails.append(
                f"AC4: {CMAKE_REL}:{i} adds lib/runtime.c to a host target - its weak stub "
                "could shadow the bridge's strong definition in the host binary (#4676): "
                f"{line.strip()}"
            )
    return fails


def _self_test() -> int:
    good_rt = """
__attribute__((weak)) int aura_jit_linear_per_op_typed_entry_readiness_ok(void) {
    // Issue #4671
    return 1;
}
"""
    bad_rt = """
__attribute__((weak)) int aura_jit_linear_per_op_typed_entry_readiness_ok(void) {
    // Issue #4671
    return 0;
}
"""
    nostub = """
int aura_jit_linear_per_op_typed_entry_readiness_ok(void) { return 1; }
"""
    hdr = "int aura_jit_linear_per_op_typed_entry_readiness_ok(void);\n"

    cases = [
        ("the stub is found", PER_OP_SYM in _weak_stub_bodies(good_rt)),
        ("a missing weak stub is not found", PER_OP_SYM not in _weak_stub_bodies(nostub)),
        ("the gate family is derived from the header", _gate_family(hdr) == [PER_OP_SYM]),
        ("a non-weak definition does not satisfy AC2", check_gate_family(nostub, hdr) != []),
    ]
    for label, ok in cases:
        if not ok:
            print(f"FAIL #4676 self-test: {label}", file=sys.stderr)
            return 1

    # The polarity check must reject a stub that returns 0.
    if not check_per_op_stub(bad_rt):
        print("FAIL #4676 self-test: a stub that returns 0 was accepted", file=sys.stderr)
        return 1
    if check_per_op_stub(good_rt):
        print("FAIL #4676 self-test: a correct stub was rejected", file=sys.stderr)
        return 1

    print("OK #4676 self-test: the runtime.c weak-stub gate catches the regression")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Issue #4676 AOT runtime weak-stub gate")
    ap.add_argument("--self-test", action="store_true", help="run the built-in probes")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    runtime = _read(RUNTIME_REL)
    header = _read(HEADER_REL)
    jit = _read(JIT_REL)
    cmake = _read(CMAKE_REL)
    if not runtime or not header or not jit:
        print("FAIL #4676 aot_runtime_weak_stub: missing source files", file=sys.stderr)
        return 1

    fails = (
        check_per_op_stub(runtime)
        + check_gate_family(runtime, header)
        + check_jit_emits(jit)
        + check_runtime_not_in_host(cmake)
    )
    if fails:
        print("FAIL #4676 aot_runtime_weak_stub:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print("OK #4676 aot_runtime_weak_stub: every JIT gate emitted into the AOT IR has a weak stub in lib/runtime.c")
    return 0


if __name__ == "__main__":
    sys.exit(main())
