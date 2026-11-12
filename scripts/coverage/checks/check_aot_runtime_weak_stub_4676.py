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

# The loader-plumbing bridge family (Issue #5173). These are the C ABI
# entries the JIT emits INTO the AOT IR for the loader's own writes: a
# define / let / letrec init (aura_cell_set_jit_bind) and a closure env
# capture (aura_closure_capture_jit). They are named *_jit_* because
# they skip the user Mutate choke, and they are defined ONLY in
# src/compiler/aura_jit_runtime.cpp (host JIT) - which the standalone
# --emit-binary link does NOT compile.
#
# So a rename inside that family is a silent standalone-link break: the
# emitted .o references a symbol no one defines, the link fails with
# "undefined reference", --emit-binary produces no binary, and every
# emit:* case in tests/python/run-tests.sh fails with "(no binary)".
# #5173 renamed aura_cell_set -> aura_cell_set_jit_bind and
# aura_closure_capture -> aura_closure_capture_jit and broke exactly
# that way; the dist workflow caught it, the fast pre-push gate did not.
#
# Every member of this family needs a weak stub in lib/runtime.c.
PLUMBING_FAMILY_RE = re.compile(r"aura_[A-Za-z0-9_]*_(?:jit_bind|capture_jit)")

# The bridge namespaces whose host-side definitions live in the JIT
# runtime TU. A symbol in these namespaces that the JIT emits into the
# AOT IR but that lib/runtime.c does not define can only be resolved by
# the HOST build - the standalone --emit-binary link compiles runtime.c
# and the emitted .o only, so such a symbol is an undefined reference.
BRIDGE_NS_RE = re.compile(r"aura_(?:cell|closure)_[A-Za-z0-9_]+")

# The host JIT TU. Its strong definitions win over runtime.c's weak
# stubs in the host build; the standalone AOT link never sees it.
HOST_JIT_REL = "src/compiler/aura_jit_runtime.cpp"

# The evaluator fall-through family (Issue #5175). When the JIT's inline
# hash-ref scan misses, it falls through to an EVALUATOR primitive
# (aura_jit_eval_hash_ref) instead of the runtime.c miss-error helper. That
# callee is defined only in the host evaluator TU (src/compiler/service.ixx
# strong, src/compiler/aura_jit_prim_dispatch_stub.cpp weak); the standalone
# --emit-binary link compiles NEITHER, so the emitted .o referenced a symbol
# no one defines, the link failed, and --emit-binary produced no binary for
# every emit:* case that reaches a hash-ref.
#
# The family is keyed on the LLVM call-site variable (fn_eval_*), not on the
# spelling of the C symbol, so renaming the symbol cannot slip past.
EVAL_FAMILY_VAR_RE = re.compile(r"^fn_eval_[A-Za-z0-9_]*$")

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


def _external_linkage_symbols(jit: str) -> dict[str, str]:
    """Map the LLVM fn_ variable -> the C symbol it is declared as.

    Only ExternalLinkage declarations count: those are the ones that end
    up as undefined references in the emitted .o.
    """
    out: dict[str, str] = {}
    for m in re.finditer(
        r"(fn_[A-Za-z0-9_]+)\s*=\s*\n?\s*llvm::Function::Create\("
        r"(?:[^;]*?)llvm::Function::ExternalLinkage,\s*\n?\s*\"([A-Za-z_][A-Za-z0-9_]*)\"",
        jit,
    ):
        out[m.group(1)] = m.group(2)
    return out


def _create_call_args(jit: str) -> list[str]:
    """Return the argument text of every CreateCall(...) site.

    The fn_ variable is not always the whole callee expression - it can
    sit in a ternary (fn_cell_set_jit_bind vs fn_cell_set) - so the
    guard scans the whole call's argument list rather than requiring an
    exact "CreateCall(llvm::FunctionCallee(fn_x))" shape.
    """
    args: list[str] = []
    for m in re.finditer(r"CreateCall\(", jit):
        i = m.end()
        depth = 1
        start = i
        while i < len(jit) and depth:
            if jit[i] == "(":
                depth += 1
            elif jit[i] == ")":
                depth -= 1
            i += 1
        args.append(jit[start : i - 1])
    return args


def _called_external_symbols(jit: str) -> dict[str, str]:
    """The ExternalLinkage symbols the JIT actually calls into the IR.

    Only symbols that are actually CALLED count: a declaration nobody
    calls never becomes an undefined reference, so it needs no stub.
    """
    declared = _external_linkage_symbols(jit)
    call_args = _create_call_args(jit)
    out: dict[str, str] = {}
    for fn_var, sym in declared.items():
        if not any(re.search(r"\b" + re.escape(fn_var) + r"\b", arg) for arg in call_args):
            continue
        out[fn_var] = sym
    return out


def _defined_symbols(text: str) -> set[str]:
    """The C symbols a translation unit defines (weak or strong)."""
    found: set[str] = set()
    for m in re.finditer(
        r"^\s*(?:__attribute__\(\(weak\)\)\s*)?"
        r"(?:extern\s+\"C\"\s+)?"
        r"[A-Za-z_][A-Za-z0-9_ \t*]*?\b(aura_[A-Za-z0-9_]+)\s*\([^;{]*\)\s*\{",
        text,
        re.MULTILINE,
    ):
        found.add(m.group(1))
    return found


def _plumbing_family(jit: str) -> dict[str, str]:
    """The loader-plumbing bridge symbols the JIT emits into the AOT IR.

    Two independent triggers, so a rename cannot slip past by changing
    the name shape:

      1. the *_jit_bind / *_capture_jit naming convention #5173 used;
      2. any aura_cell_* / aura_closure_* bridge whose ONLY definition
         is the host JIT TU - i.e. the standalone AOT link cannot
         resolve it no matter what it is called.
    """
    declared = _external_linkage_symbols(jit)
    call_args = _create_call_args(jit)
    out: dict[str, str] = {}
    for fn_var, sym in declared.items():
        if not any(re.search(r"\b" + re.escape(fn_var) + r"\b", arg) for arg in call_args):
            continue
        out[fn_var] = sym
    return out


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


def check_plumbing_family(runtime: str, jit: str, host_jit: str) -> list[str]:
    """AC5: every loader-plumbing bridge the JIT emits has a runtime.c stub.

    A rename inside this family moves a symbol that the standalone
    --emit-binary link cannot resolve, so the emitted .o fails to link
    and no binary is produced (#5173).

    A bridge needs a runtime.c stub when the standalone link cannot
    resolve it, i.e. when the symbol is called into the AOT IR and its
    ONLY definition is the host JIT TU. Two triggers catch that:

      1. the *_jit_bind / *_capture_jit naming convention #5173 used;
      2. any aura_cell_* / aura_closure_* bridge that is host-only.

    A bridge that runtime.c DOES define needs no extra stub - renaming
    it and moving the stub with it is fine.
    """
    fails: list[str] = []
    stubs = _weak_stub_bodies(runtime)
    runtime_defs = _defined_symbols(runtime)
    host_defs = _defined_symbols(host_jit)
    family = _plumbing_family(jit)

    # Validity: the parser must actually see the emitted IR surface. If it
    # ever stops matching, the guard would go silently inert and the next
    # rename would slip through again.
    if not family:
        fails.append(
            f"AC5: no called ExternalLinkage symbol was found in {JIT_REL} - the "
            "guard cannot see the emitted IR surface, so it is inert and a rename "
            "would slip through again (#5173)"
        )
        return fails

    required: dict[str, str] = {}
    for fn_var, sym in sorted(family.items(), key=lambda kv: kv[1]):
        if PLUMBING_FAMILY_RE.fullmatch(sym):
            required[sym] = fn_var
            continue
        if BRIDGE_NS_RE.fullmatch(sym) and sym in host_defs and sym not in runtime_defs:
            required[sym] = fn_var

    for sym, fn_var in sorted(required.items()):
        if sym not in stubs:
            fails.append(
                f"AC5: {RUNTIME_REL} has NO weak stub for {sym} (emitted via {fn_var}) - "
                "the standalone --emit-binary link has no definition of it and fails "
                "with 'undefined reference', so --emit-binary emits no binary and every "
                "emit:* case fails with '(no binary)' (#5173)"
            )
            continue
        body = stubs[sym]
        delegates = [m.group(1) for m in re.finditer(r"\b(aura_[A-Za-z0-9_]+)\s*\(", body) if m.group(1) != sym]
        if not delegates:
            fails.append(
                f"AC5: the weak stub for {sym} does not delegate to a runtime.c "
                "implementation - the standalone AOT binary needs the write to land in "
                "the same cell / closure heap the emitted IR reads back (#5173)"
            )
    return fails


def check_eval_family(runtime: str, jit: str) -> list[str]:
    """AC6: every evaluator fall-through callee the JIT emits has a
    lib/runtime.c definition.

    The standalone --emit-binary link compiles lib/runtime.c, the generated
    registration / prim .c and the per-function LLVM .o - no C++ bridge
    objects at all. So a callee the JIT emits into the IR that only the HOST
    defines is an undefined reference there, the link fails, and every
    emit:* case that reaches that op site fails with '(no binary)' (#5175).

    The family is the LLVM call-site variable (fn_eval_*), so a rename of
    the C symbol keeps it in the family and is still caught.
    """
    fails: list[str] = []
    runtime_defs = _defined_symbols(runtime)
    stubs = _weak_stub_bodies(runtime)
    declared = _external_linkage_symbols(jit)
    call_args = _create_call_args(jit)

    family: dict[str, str] = {}
    for fn_var, sym in declared.items():
        if not EVAL_FAMILY_VAR_RE.match(fn_var):
            continue
        if not any(re.search(r"\b" + re.escape(fn_var) + r"\b", arg) for arg in call_args):
            continue
        family[sym] = fn_var

    # Validity: the guard must still see the family. If the parser stops
    # matching, the guard would go silently inert and the next rename would
    # slip through again.
    if not family:
        fails.append(
            f"AC6: no evaluator fall-through callee was found in {JIT_REL} - the "
            "guard cannot see the fn_eval_* family, so it is inert and the next "
            "rename would slip through again (#5175)"
        )
        return fails

    for sym, fn_var in sorted(family.items(), key=lambda kv: kv[1]):
        if sym not in runtime_defs:
            fails.append(
                f"AC6: {RUNTIME_REL} has NO definition for {sym} (emitted via "
                f"{fn_var}) - the standalone --emit-binary link compiles only "
                "runtime.c and the emitted .o, so it fails with 'undefined "
                "reference' and --emit-binary emits no binary (#5175)"
            )
            continue
        body = stubs.get(sym)
        if body is None:
            continue
        delegates = [m.group(1) for m in re.finditer(r"\b(aura_[A-Za-z0-9_]+)\s*\(", body) if m.group(1) != sym]
        if not delegates:
            fails.append(
                f"AC6: the weak stub for {sym} does not delegate to a runtime.c "
                "implementation - the standalone AOT binary must take the same "
                "path the host takes with no owner wired (#5175)"
            )
            continue
        for target in delegates:
            if target not in runtime_defs:
                fails.append(
                    f"AC6: the weak stub for {sym} delegates to {target}, which "
                    f"{RUNTIME_REL} does not define - the standalone AOT link would "
                    "fail with 'undefined reference' (#5175)"
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

    # AC5 probes: a bridge the JIT calls into the IR.
    bridge_jit = """
fn_cell_set_jit_bind =
    llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64, i64}, false),
                           llvm::Function::ExternalLinkage, "aura_cell_set_jit_bind", mod);
irb->CreateCall(llvm::FunctionCallee(fn_cell_set_jit_bind), vals);
"""
    bridge_rt = """
__attribute__((weak)) void aura_cell_set_jit_bind(int64_t cell_id, int64_t val) {
    aura_cell_set(cell_id, val);
}
"""
    host_rt = """
extern "C" void aura_cell_set_jit_bind(int64_t cell_id, int64_t val) {
    aura_cell_set_checked(cell_id, val);
}
"""

    cases = [
        ("the stub is found", PER_OP_SYM in _weak_stub_bodies(good_rt)),
        ("a missing weak stub is not found", PER_OP_SYM not in _weak_stub_bodies(nostub)),
        ("the gate family is derived from the header", _gate_family(hdr) == [PER_OP_SYM]),
        ("a non-weak definition does not satisfy AC2", check_gate_family(nostub, hdr) != []),
        (
            "the plumbing family is derived from the JIT IR",
            _plumbing_family(bridge_jit).get("fn_cell_set_jit_bind") == "aura_cell_set_jit_bind",
        ),
        (
            "a missing plumbing stub is caught",
            check_plumbing_family("", bridge_jit, host_rt) != [],
        ),
        (
            "a delegating plumbing stub passes",
            check_plumbing_family(bridge_rt, bridge_jit, host_rt) == [],
        ),
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
    host_jit = _read(HOST_JIT_REL)
    cmake = _read(CMAKE_REL)
    if not runtime or not header or not jit or not host_jit:
        print("FAIL #4676 aot_runtime_weak_stub: missing source files", file=sys.stderr)
        return 1

    fails = (
        check_per_op_stub(runtime)
        + check_gate_family(runtime, header)
        + check_plumbing_family(runtime, jit, host_jit)
        + check_eval_family(runtime, jit)
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
