#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4058: capability_stack_ (with-capability pushes) satisfied
# Evaluator::has_capability — a zero-grant Agent could read host files
# (read-file deny_io), clear process-level exception stacks
# (jit:exception-fibers-clear), and open the kPrimSecSandboxed dispatch gate
# (invoke_prim_with_telemetry) by pushing strings, with no EffectDeny.
#
# AC1 — has_capability scans zero capability_stack_ layers: the oracle reads
#       effects_effective_for(tenant) + the granted_capabilities_ string
#       mirror only (with-capability stays a non-grant, no registry write).
# AC2 — both with-capability write sites (prim + eval_flat special form)
#       keep the lexical push for the check-capability / capability-stack
#       readouts and carry the #4058 contract comment.
# AC3 — with-capability sites write no registry (no g_capability_registry in
#       the policy / eval_flat TUs; the push is the only stack mutation).
# AC4 — lexical readouts intact: check-capability reverse scan +
#       capability-stack collector still read capability_stack_.
# AC5 — the dispatch kPrimSecSandboxed gate keeps its shape: sandboxed,
#       non-heap-mutate prims require has_capability(kCapSandbox).
# AC6 — body gates keep consulting the oracle: file prims (kCapIoRead) and
#       jit:exception-fibers-clear (kCapExceptionControl).
# AC7 — runtime ACs extend tests/compiler/test_dispatch_required_effects.cpp
#       (per #81934); no tests/core/test_issue_4058.cpp; no
#       docs/design/4058-* (per #1655).
#
# Self-test:
#   python3 scripts/check_capability_stack_authorization_4058.py

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _strip_cpp_comments(src: str) -> str:
    """Remove // line comments and /* block comments */ so substring search
    does not false-positive on prose. Cheap state machine; good enough for
    source-cite checks (does not need to handle raw strings / trigraphs).
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


failures: list[str] = []
passes = 0


def must(cond: bool, label: str) -> None:
    global passes
    if cond:
        passes += 1
    else:
        failures.append(label)


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> int:
    # ── AC1/AC2: has_capability oracle ──
    sec = read("src/compiler/evaluator_security.cpp")
    start = sec.find("bool Evaluator::has_capability")
    end = sec.find("void Evaluator::grant_capability")
    must(start != -1 and end != -1 and start < end, "AC1: has_capability slice located")
    if start != -1 and end != -1 and start < end:
        body = _strip_cpp_comments(sec[start:end])
        must("capability_stack_" not in body, "AC1: has_capability never scans capability_stack_ (#4058)")
        must(
            body.count("granted_capabilities_") >= 2,
            "AC1: granted_capabilities_ mirror scans remain (wildcard + string path)",
        )
        must("effects_effective_for" in body, "AC1: effect matrix route remains")
        must("#4058" in sec[max(0, start - 2000) : end], "AC2: rationale cites #4058 near has_capability")

    # ── AC2/AC3: with-capability sites keep the lexical push, write no registry ──
    pol = read("src/compiler/evaluator_primitives_policy.cpp")
    must("ev.capability_stack_.push_back(caps)" in pol, "AC2: with-capability prim keeps the lexical push")
    must("#4058" in pol, "AC2: with-capability prim cites #4058")
    must("g_capability_registry" not in _strip_cpp_comments(pol), "AC3: policy TU writes no capability registry")
    efl = read("src/compiler/evaluator_eval_flat.cpp")
    must("capability_stack_.push_back(caps)" in efl, "AC2: with-capability special form keeps the lexical push")
    must("#4058" in efl, "AC2: special form cites #4058")
    must("g_capability_registry" not in _strip_cpp_comments(efl), "AC3: eval_flat TU writes no capability registry")

    # ── AC4: lexical readouts intact ──
    must("capability_stack_.rbegin()" in pol, "AC4: check-capability reverse scan intact")
    must("for (auto& layer : ev.capability_stack_)" in pol, "AC4: capability-stack collector intact")

    # ── AC5: dispatch sandbox gate unchanged ──
    ixx = read("src/compiler/evaluator.ixx")
    must("kPrimSecSandboxed && !heap_mutate" in ixx, "AC5: kPrimSecSandboxed non-heap-mutate gate shape intact")
    must("has_capability(security::kCapSandbox)" in ixx, "AC5: gate consults has_capability(kCapSandbox)")

    # ── AC6: body gates keep the oracle ──
    fp = read("src/compiler/evaluator_primitives_file.cpp")
    must("const auto deny_io" in fp and "ev.has_capability(cap)" in fp, "AC6: deny_io lambda consults has_capability")
    must("deny_io(aura::compiler::security::kCapIoRead" in fp, "AC6: read-file routes kCapIoRead through deny_io")
    oj = read("src/compiler/evaluator_primitives_obs_jit.cpp")
    must("kCapExceptionControl" in oj, "AC6: jit:exception-fibers-clear exception-control gate intact")

    # ── AC7: runtime ACs in the host file; no standalone artifacts ──
    tf = read("tests/compiler/test_dispatch_required_effects.cpp")
    must("#4058" in tf and "with-capability" in tf, "AC7: runtime ACs extend the #2152/#4036/#4057 host test file")
    must(not (ROOT / "tests/core/test_issue_4058.cpp").exists(), "AC7: no standalone test_issue_4058.cpp (#81934)")
    design = ROOT / "docs" / "design"
    must(not design.exists() or not any(design.glob("4058-*")), "AC7: no docs/design/4058-* (#1655)")

    print(f"check_capability_stack_authorization_4058: {passes} checks passed")
    if failures:
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
