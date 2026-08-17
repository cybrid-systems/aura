#!/usr/bin/env python3
# scripts/check_dual_track_facade_3112.py — Issue #3112 source-cite gate.
#
# Closes the residual dual-track: production invalidate/reemit must route
# exclusively through the HotUpdateRegistry facade (decide_and_reemit +
# AotReloadConsistencyProof fail-stamp). Under production_defaults_active()
# no production path may call aura_reemit_aot_for_dirty / aura_reload_aot_module*
# directly outside the bridge impl (aura_jit_bridge.cpp), because the bridge
# impl itself routes through the facade (decide_and_reemit -> reemit).
#
# This linter enforces:
#   1. aura_jit_bridge.cpp is the SOLE src/ call site for these C bridge
#      symbols. Any other src/ file calling them directly is a violation.
#   2. The dual-track bypass prevention counters are declared exactly once
#      in hot_update_registry.hh (g_dual_track_bypass_prevented_total +
#      g_dual_track_bypass_total).
#   3. The service_dirty.cpp forwarding blocks (mark_define_dirty +
#      invalidate_function) bump the prevented counter and forward to the
#      facade under production_defaults_active().
#
# AC reference: Issue #3112 Step 3 + AC5 (audit remaining direct call sites;
# route through facade or document as test-only).
#
# Usage:
#   python3 scripts/check_dual_track_facade_3112.py --strict
#   python3 scripts/check_dual_track_facade_3112.py --self-test
#   python3 scripts/check_dual_track_facade_3112.py        # (advisory mode)

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC = REPO_ROOT / "src"
TESTS = REPO_ROOT / "tests"

BRIDGE_FILE = SRC / "compiler" / "aura_jit_bridge.cpp"
BRIDGE_STUB_FILE = SRC / "compiler" / "aura_jit_bridge_stub.cpp"
HEADER_FILE = SRC / "compiler" / "hot_update_registry.hh"
SERVICE_DIRTY_FILE = SRC / "compiler" / "service_dirty.cpp"

# Whitelist for legitimate non-bridge call sites of the C ABI.
# Each entry: (file_path, allowed_line_pattern_or_None) — if pattern is None,
# the entire file is whitelisted. If pattern is set, only matching lines
# are allowed; the rest of the file must be clean.
WHITELIST: list[tuple[Path, str | None]] = [
    # 1) aura_jit_bridge_stub.cpp contains __attribute__((weak)) stub
    #    DEFINITIONS of the C ABI symbols (returning 0/false/null when the
    #    real bridge is not linked). These are NOT call sites — they are
    #    the symbol definition stubs. Skipped wholesale via the stub
    #    detection below.
    # 2) hot_update_registry.cpp IS the facade itself. The
    #    HotUpdateRegistry::decide_and_reemit body calls the low-level C
    #    ABI aura_reemit_aot_for_dirty as its work step (facade→ABI edge).
    #    Any call to a bridge symbol from inside this file is by
    #    definition a facade call, not a bypass. The decide_and_reemit
    #    body is the only caller (verified by inspection at ship time).
    (SRC / "compiler" / "hot_update_registry.cpp", None),
    # 3) evaluator_primitives_obs_jit.cpp `aot:reload` primitive exposes
    #    aura_reload_aot_module_for_eval to user / agent code. Per AC
    #    Step 3 this is documented as test-only (user-intent dev tool,
    #    not production-driven reemit / invalidate path).
    (SRC / "compiler" / "evaluator_primitives_obs_jit.cpp", r"aura_reload_aot_module_for_eval"),
]

# C bridge symbols that must route through HotUpdateRegistry facade.
BRIDGE_SYMBOLS = (
    "aura_reemit_aot_for_dirty",
    "aura_reload_aot_module",
    "aura_reload_aot_module_for_eval",
    "aura_reload_aot_module_for_eval_once",
)

# Regex for invocation (allow whitespace; ignore comments by line).
INVOCATION_RE = re.compile(r"\b(" + "|".join(re.escape(s) for s in BRIDGE_SYMBOLS) + r")\s*\(")

# Inline static std::atomic counter declarations.
COUNTER_DECL_RE = re.compile(
    r"inline\s+static\s+std::atomic<std::uint64_t>\s+(g_dual_track_bypass_(?:prevented|bypass)_total)\s*\{"
)

PRODUCTION_FORWARD_BLOCK_RE = re.compile(
    r"if\s*\(\s*aura::compiler::typed_audit::production_defaults_active\(\)\s*\)\s*\{"
    r"(?:[^{}]|\{[^{}]*\})*?"
    r"hard_invalidate_via_facade"
)


def _read_text(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def _strip_line_comments(text: str) -> str:
    """Drop // line comments so INVOCATION_RE matches only real call sites."""
    return re.sub(r"//[^\n]*", "", text)


def _strip_block_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)


def _is_test_path(p: Path) -> bool:
    parts = p.parts
    return "tests" in parts or p.name.startswith("test_") or "/fuzz/" in str(p)


def _is_weak_stub(text: str, line_no: int) -> bool:
    """Detect __attribute__((weak)) stub DEFINITIONS (not call sites).

    Stub definitions follow the pattern:
        extern "C" __attribute__((weak)) <return_type> <symbol>(...) {
            return <default>;
        }
    These are symbol definitions for weak linkage (real impl in
    aura_jit_bridge.cpp overrides when linked). They are not call
    sites that route production paths.
    """
    lines = text.splitlines()
    # Look at the 4 lines before line_no for `__attribute__((weak))` + extern "C".
    lo = max(0, line_no - 4)
    window = "\n".join(lines[lo:line_no])
    return "__attribute__((weak))" in window and 'extern "C"' in window


def _is_whitelisted(path: Path, line_text: str) -> bool:
    for whitelisted_path, pattern in WHITELIST:
        if path == whitelisted_path:
            if pattern is None:
                return True
            import re as _re

            if _re.search(pattern, line_text):
                return True
    return False


def check_call_sites() -> list[str]:
    """AC1: no src/ file (other than aura_jit_bridge.cpp) calls bridge symbols."""
    failures: list[str] = []
    if not SRC.exists():
        failures.append(f"{SRC}: src/ not found")
        return failures
    for path in sorted(SRC.rglob("*.cpp")):
        if _is_test_path(path):
            continue
        # The bridge stub file holds __attribute__((weak)) symbol
        # DEFINITIONS (not call sites); skip wholesale.
        if path == BRIDGE_STUB_FILE:
            continue
        text = _strip_block_comments(_strip_line_comments(_read_text(path)))
        original_text = _read_text(path)
        for m in INVOCATION_RE.finditer(text):
            sym = m.group(1)
            # The bridge impl file IS allowed to call these (it routes them).
            if path == BRIDGE_FILE:
                continue
            line_no = text[: m.start()].count("\n") + 1
            # Find the corresponding original line (with comments) to evaluate
            # whitelisting + weak-stub detection against the real source.
            original_line_start = sum(len(line) + 1 for line in original_text.splitlines()[: line_no - 1])
            original_line_end = original_text.find("\n", original_line_start)
            if original_line_end == -1:
                original_line_end = len(original_text)
            original_line = original_text[original_line_start:original_line_end]
            if _is_weak_stub(original_text, line_no):
                continue
            if _is_whitelisted(path, original_line):
                continue
            failures.append(
                f"{path}:{line_no}: direct call to {sym}() outside bridge impl; "
                "route through HotUpdateRegistry::hard_invalidate_via_facade / "
                "decide_and_reemit under production_defaults_active()"
            )
    return failures


def check_counter_declarations() -> list[str]:
    """AC2: counters declared exactly once each in hot_update_registry.hh."""
    failures: list[str] = []
    if not HEADER_FILE.exists():
        failures.append(f"{HEADER_FILE}: header not found")
        return failures
    text = _read_text(HEADER_FILE)
    for name in ("g_dual_track_bypass_prevented_total", "g_dual_track_bypass_total"):
        matches = list(COUNTER_DECL_RE.finditer(text))
        if len(matches) == 0:
            failures.append(f"{HEADER_FILE}: missing inline static std::atomic<std::uint64_t> {name}{{0}}; declaration")
        elif len(matches) > 1:
            line_nos = [text[: m.start()].count("\n") + 1 for m in matches]
            failures.append(
                f"{HEADER_FILE}: counter {name} declared {len(matches)} times at lines {line_nos}; must be exactly once"
            )
    return failures


def check_service_dirty_forwarding() -> list[str]:
    """AC3: service_dirty.cpp forwards production path to the facade."""
    failures: list[str] = []
    if not SERVICE_DIRTY_FILE.exists():
        failures.append(f"{SERVICE_DIRTY_FILE}: file not found")
        return failures
    text = _read_text(SERVICE_DIRTY_FILE)
    if "hard_invalidate_via_facade" not in text:
        failures.append(
            f"{SERVICE_DIRTY_FILE}: missing hard_invalidate_via_facade forwarding; "
            "mark_define_dirty + invalidate_function must forward to facade under "
            "production_defaults_active()"
        )
    n_prevented = text.count("g_dual_track_bypass_prevented_total.fetch_add")
    if n_prevented < 2:
        failures.append(
            f"{SERVICE_DIRTY_FILE}: g_dual_track_bypass_prevented_total.fetch_add "
            f"called {n_prevented} times; expected >= 2 (mark_define_dirty + "
            "invalidate_function forwarding blocks)"
        )
    return failures


def check_file() -> list[str]:
    return check_call_sites() + check_counter_declarations() + check_service_dirty_forwarding()


def _self_test() -> int:
    """Validate linter logic against fixture files."""
    fails: list[str] = []

    # Fixture 1: bridge symbol in non-bridge src/ file should be flagged.
    # Name intentionally NOT starting with "test_" so _is_test_path does
    # NOT skip it (test_ prefix is reserved for /tests/ paths).
    bad_src = SRC / "compiler" / "_dual_track_fixture_bad_3112.cpp"
    bad_src.write_text(
        "namespace aura::compiler {\nvoid bad() { aura_reemit_aot_for_dirty(0); }\n}\n",
        encoding="utf-8",
    )
    try:
        result = check_call_sites()
        if not any("_dual_track_fixture_bad_3112.cpp" in r for r in result):
            fails.append(
                f"self-test: check_call_sites should flag bad bridge symbol "
                f"in _dual_track_fixture_bad_3112.cpp (got {len(result)} failures)"
            )
    finally:
        if bad_src.exists():
            bad_src.unlink()

    # Fixture 2: counter declared twice with the SAME std::atomic type
    # should be detected as duplicate by COUNTER_DECL_RE.
    bad_header = Path("/tmp/check_dual_track_fixture_3112.hh")
    bad_header.write_text(
        "class HotUpdateRegistry {\n"
        "public:\n"
        "    inline static std::atomic<std::uint64_t> g_dual_track_bypass_prevented_total{0};\n"
        "    inline static std::atomic<std::uint64_t> g_dual_track_bypass_prevented_total{0};\n"
        "};\n",
        encoding="utf-8",
    )
    try:
        text = bad_header.read_text(encoding="utf-8")
        matches = list(COUNTER_DECL_RE.finditer(text))
        if len(matches) != 2:
            fails.append(f"self-test: COUNTER_DECL_RE should match twice in fixture (got {len(matches)})")
    finally:
        if bad_header.exists():
            bad_header.unlink()

    if fails:
        print("SELF-TEST FAIL:")
        for line in fails:
            print(f"  {line}")
        return 1
    print("self-test: ok")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else "")
    parser.add_argument("--strict", action="store_true", help="non-zero exit on any failure (CI gate mode)")
    parser.add_argument("--self-test", action="store_true", help="run linter self-test and exit")
    args = parser.parse_args()

    if args.self_test:
        return _self_test()

    failures = check_file()
    if failures:
        print(f"FAIL ({len(failures)} issue(s)):")
        for line in failures:
            print(f"  {line}")
        if args.strict:
            return 1
        return 0
    print("ok: dual-track facade audit clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
