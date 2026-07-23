#!/usr/bin/env python3
"""check_jit_batch_deopt_clear_coverage.py — Issue #1996 source gate.

  AC1: aura_jit_bridge.cpp defines `extern "C" void
       aura_clear_jit_batch_deopt_target(void* aura_jit_ptr)` (the
       symmetric clear-side API for g_batch_deopt_jit).
  AC2: aura_clear_jit_batch_deopt_target body compares
       `g_batch_deopt_jit == static_cast<aura::jit::AuraJIT*>(aura_jit_ptr)`
       before nulling (prevents clobbering a sibling CompilerService's
       live pointer in the multi-service scenario).
  AC3: aura_clear_jit_batch_deopt_target body handles null
       `aura_jit_ptr` as a force-clear (resets g_batch_deopt_jit
       unconditionally).
  AC4: service.ixx ~CompilerService destructor calls
       `aura_clear_jit_batch_deopt_target(&jit_);` before the
       destructor body completes — so the file-scope pointer is
       nulled before jit_ (member AuraJIT) is destroyed.
  AC5: tests/test_issue_1996.cpp exists.
  AC6: linter self-test (--self-test passes).

Rationale (Issue #1996 body):
  g_batch_deopt_jit is file-scope in aura_jit_bridge.cpp:948, wired
  once from service.ixx:668 via `aura_set_jit_batch_deopt_target(&jit_)`.
  No symmetric clear API — when the CompilerService owning jit_ is
  destroyed, g_batch_deopt_jit continues pointing into the freed
  object. A late batch_deopt_for / deopt_pending_count /
  is_deopt_pending call then dereferences freed memory (UAF).

  Fix: add aura_clear_jit_batch_deopt_target C-linkage API + wire
  ~CompilerService destructor to call it with &jit_. The clear
  matches the pointer before nulling (no clobber of a sibling
  CompilerService's live pointer in the multi-service scenario);
  null ptr is treated as a force-clear for host-bridge shutdown.
"""

from __future__ import annotations

import re
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRIDGE = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
SERVICE = ROOT / "src" / "compiler" / "service.ixx"
TEST = ROOT / "tests" / "test_issue_1996.cpp"


def _extract_body(text: str, open_idx: int) -> str:
    """Extract the body of a brace-delimited block (handles nested {})."""
    assert text[open_idx] == "{", f"Expected '{{' at {open_idx}"
    depth = 0
    i = open_idx
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1 : i]
        i += 1
    return ""


def _find_function_body(text: str, signature: str) -> str:
    """Find a function by its signature regex and return its body."""
    m = re.search(signature, text)
    if not m:
        return ""
    open_idx = text.find("{", m.end() - 1)
    if open_idx < 0:
        return ""
    return _extract_body(text, open_idx)


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    failures: list[str] = []

    if not BRIDGE.exists():
        failures.append("AC1-AC3: src/compiler/aura_jit_bridge.cpp not found")
    else:
        bridge = BRIDGE.read_text(encoding="utf-8", errors="replace")
        body = _find_function_body(
            bridge,
            r'extern\s+"C"\s+void\s+aura_clear_jit_batch_deopt_target\(\s*void\s*\*\s*aura_jit_ptr\s*\)',
        )
        if not body:
            failures.append(
                "AC1: aura_clear_jit_batch_deopt_target C-linkage definition "
                "not found in aura_jit_bridge.cpp (#1996 B-003 UAF fix)"
            )
        else:
            # AC2: match-before-null pattern (don't clobber sibling)
            if not re.search(
                r"g_batch_deopt_jit\s*==\s*static_cast<aura::jit::AuraJIT\*>\(\s*aura_jit_ptr\s*\)",
                body,
            ):
                failures.append(
                    "AC2: aura_clear_jit_batch_deopt_target body does not compare "
                    "g_batch_deopt_jit == static_cast<aura::jit::AuraJIT*>(aura_jit_ptr) "
                    "before nulling (would clobber a sibling CompilerService's live "
                    "pointer in the multi-service scenario)"
                )
            # AC3: null ptr = force-clear
            if not re.search(r"aura_jit_ptr\s*==\s*nullptr", body):
                failures.append(
                    "AC3: aura_clear_jit_batch_deopt_target body does not handle "
                    "null aura_jit_ptr as a force-clear (host-bridge shutdown path)"
                )
            if not re.search(r"g_batch_deopt_jit\s*=\s*nullptr", body):
                failures.append(
                    "AC3: aura_clear_jit_batch_deopt_target body does not reset g_batch_deopt_jit to nullptr"
                )

    if not SERVICE.exists():
        failures.append("AC4: src/compiler/service.ixx not found")
    else:
        service = SERVICE.read_text(encoding="utf-8", errors="replace")
        # Find ~CompilerService destructor body and check it calls the clear API
        m_dtor = re.search(r"~\s*CompilerService\s*\(\s*\)\s*(?:noexcept\s*)?\{", service)
        if not m_dtor:
            failures.append("AC4: ~CompilerService destructor signature not found")
        else:
            dtor_body = _extract_body(service, m_dtor.end() - 1)
            if "aura_clear_jit_batch_deopt_target" not in dtor_body:
                failures.append(
                    "AC4: ~CompilerService destructor does not call "
                    "aura_clear_jit_batch_deopt_target — file-scope "
                    "g_batch_deopt_jit will dangle after destruction (#1996 B-003)"
                )
            if "&jit_" not in dtor_body:
                failures.append(
                    "AC4: ~CompilerService destructor does not pass "
                    "&jit_ to aura_clear_jit_batch_deopt_target — pointer "
                    "mismatch means the clear won't match (sibling-service "
                    "clobber regression)"
                )

    if not TEST.exists():
        failures.append("AC5: tests/test_issue_1996.cpp not found")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: all #1996 ACs satisfied (aura_clear_jit_batch_deopt_target + ~CompilerService wiring + test present)")
    return 0


def self_test() -> int:
    """Self-test: feed good + bad fixtures through the linter."""
    tmp = Path(tempfile.mkdtemp(prefix="check_1996_selftest_"))
    try:
        good_bridge = tmp / "bridge.cpp"
        good_bridge.write_text(
            'extern "C" void aura_clear_jit_batch_deopt_target(void* aura_jit_ptr) {\n'
            "    if (aura_jit_ptr == nullptr) {\n"
            "        g_batch_deopt_jit = nullptr;\n"
            "        return;\n"
            "    }\n"
            "    if (g_batch_deopt_jit == static_cast<aura::jit::AuraJIT*>(aura_jit_ptr)) {\n"
            "        g_batch_deopt_jit = nullptr;\n"
            "    }\n"
            "}\n",
            encoding="utf-8",
        )
        good_service = tmp / "service.ixx"
        good_service.write_text(
            "struct CompilerService {\n"
            "    ~CompilerService() {\n"
            "        aura_clear_jit_batch_deopt_target(&jit_);\n"
            "    }\n"
            "    aura::jit::AuraJIT jit_;\n"
            "};\n",
            encoding="utf-8",
        )
        good_test = tmp / "test.cpp"
        good_test.write_text("// test_issue_1996.cpp\n", encoding="utf-8")

        import check_jit_batch_deopt_clear_coverage as self_mod

        original = {
            "BRIDGE": self_mod.BRIDGE,
            "SERVICE": self_mod.SERVICE,
            "TEST": self_mod.TEST,
        }
        try:
            self_mod.BRIDGE = good_bridge
            self_mod.SERVICE = good_service
            self_mod.TEST = good_test
            rc_good = self_mod.main()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_good != 0:
            print(f"SELF-TEST FAIL: known-good mock rejected (rc={rc_good})", file=sys.stderr)
            return 1

        # Bad fixture 1: clear API missing match-before-null
        bad_bridge = tmp / "bridge_bad.cpp"
        bad_bridge.write_text(
            'extern "C" void aura_clear_jit_batch_deopt_target(void* aura_jit_ptr) {\n'
            "    if (aura_jit_ptr == nullptr) {\n"
            "        g_batch_deopt_jit = nullptr;\n"
            "        return;\n"
            "    }\n"
            "    g_batch_deopt_jit = nullptr;\n"  # BAD: no match-before-null
            "}\n",
            encoding="utf-8",
        )
        try:
            self_mod.BRIDGE = bad_bridge
            self_mod.SERVICE = good_service
            self_mod.TEST = good_test
            rc_bad = self_mod.main()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad == 0:
            print("SELF-TEST FAIL: known-bad (no match-before-null) accepted", file=sys.stderr)
            return 1

        # Bad fixture 2: destructor missing the clear call
        bad_service = tmp / "service_bad.ixx"
        bad_service.write_text(
            "struct CompilerService {\n"
            "    ~CompilerService() {\n"
            "        // BAD: missing aura_clear_jit_batch_deopt_target\n"
            "    }\n"
            "    aura::jit::AuraJIT jit_;\n"
            "};\n",
            encoding="utf-8",
        )
        try:
            self_mod.BRIDGE = good_bridge
            self_mod.SERVICE = bad_service
            self_mod.TEST = good_test
            rc_bad2 = self_mod.main()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad2 == 0:
            print("SELF-TEST FAIL: known-bad (destructor no clear) accepted", file=sys.stderr)
            return 1

        print("SELF-TEST OK: linter accepts good fixture and rejects bad fixtures")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
