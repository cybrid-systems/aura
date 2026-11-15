"""Gate check commands (docs, fixtures, registries, audits) (#5246).

These are the commands the gate runs as individual steps.  They are thin
wrappers over scripts/ and tests/python/, so they only need the shared
paths and runner from tools/build/core.py.  Keeping them here leaves
build.py as the CLI table plus the generated wiring index.
"""

from __future__ import annotations

import subprocess
import sys

from tools.build.core import (
    AUDIT,
    COVERAGE_CHECKS,
    ROOT,
    TOOLS,
    B,
    N,
    fail,
    ok,
    run,
)
from tools.build.core import (
    coverage_run as _coverage_run,
)

GEN_DOCS = TOOLS / "gen_docs.py"
GEN_CLI_TEMPLATES = TOOLS / "gen_cli_templates_4913.py"
CLI_TEMPLATES_DIR = ROOT / "templates" / "new"
CLI_TEMPLATES_INC = ROOT / "src" / "cli_templates_4913.inc"


def cmd_docs(*, check: bool | None = None):
    """Generate or verify docs/generated/*.md from source."""
    if check is None:
        check = "--check" in sys.argv[2:]
    print(f"{B}═══ Docs {'(check)' if check else '(generate)'} ═══{N}")
    if not GEN_DOCS.exists():
        fail(f"missing {GEN_DOCS}")
        return 1
    args = [sys.executable, str(GEN_DOCS)]
    if check:
        args.append("--check")
    r = run(args, cwd=ROOT)
    if r == 0:
        ok("docs OK" if check else "docs generated")
    else:
        fail("docs stale — run ./build.py docs" if check else "docs generation failed")
    return r


def cmd_cli_templates(*, check: bool | None = None):
    """Generate or verify src/cli_templates_4913.inc from templates/new.

    The .inc is what the binary actually serves for 'aura new'. A stale copy
    means 'aura new' writes the old template, so this belongs in the fast
    gate next to the docs freshness check (#5161).
    """
    if check is None:
        check = "--check" in sys.argv[2:]
    print(f"{B}═══ CLI templates {'(check)' if check else '(generate)'} ═══{N}")
    if not GEN_CLI_TEMPLATES.exists():
        fail(f"missing {GEN_CLI_TEMPLATES}")
        return 1
    args = [
        sys.executable,
        str(GEN_CLI_TEMPLATES),
        str(CLI_TEMPLATES_DIR),
        str(CLI_TEMPLATES_INC),
    ]
    if check:
        args.append("--check")
    r = run(args, cwd=ROOT)
    if r == 0:
        ok("cli templates OK" if check else "cli templates generated")
    else:
        fail("cli templates stale — run ./build.py cli-templates" if check else "cli templates generation failed")
    return r


def cmd_fixtures():
    """Validate tests/fixtures/*.json schema and baseline sync (#1961 → run.py)."""
    print(f"{B}═══ Fixtures (check) ═══{N}")
    script = ROOT / "tests" / "python" / "run.py"
    legacy = ROOT / "tests" / "python" / "fixture_check.py"
    if script.exists():
        r = run([sys.executable, str(script), "fixtures"], cwd=ROOT)
    elif legacy.exists():
        r = run([sys.executable, str(legacy)], cwd=ROOT)
    else:
        fail(f"missing {script}")
        return 1
    if r == 0:
        ok("fixtures OK")
    else:
        fail("fixture validation failed")
    return r


def cmd_primitive_surface():
    """P0b/#1432 freeze + #1448 SlimSurface --strict (budget + facade report)."""
    print(f"{B}═══ Primitive surface freeze + SlimSurface ═══{N}")
    script = COVERAGE_CHECKS / "check_primitive_surface.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    # Issue #1448: --strict includes freeze + public budget + facade report.
    r = subprocess.run([sys.executable, str(script), "--strict"], cwd=ROOT)
    if r.returncode != 0:
        fail(
            "primitive surface freeze/strict failed "
            "(no new *-stats / string|json|math|vector|path|time-* / ast:ref-*; "
            "public count ≤ interim ceiling)"
        )
        return 1
    # Issue #1432 / #1448: synthetic unit tests (blocks deliberately-bad names + strict).
    # Issue #1932: gate unit tests live under tests/python/
    ut = ROOT / "tests" / "python" / "test_primitive_surface_gate.py"
    if ut.exists():
        r2 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r2.returncode != 0:
            fail("primitive surface gate unit tests failed")
            return 1
    ok("primitive surface freeze + SlimSurface --strict OK")
    return 0


def cmd_test_registry():
    """Issue #1572: test-registry.json freshness (scripts/tools/gen_test_registry.py).

    Default: --check (fail if docs/generated/test-registry.json is stale).
    With --fix: rewrite the registry from tests/test_*.cpp headers.
    Also wired into pre-commit when tests/*.cpp is staged.
    """
    fix = "--fix" in sys.argv[2:]
    print(f"{B}═══ Test registry {'(fix)' if fix else '(check)'} (#1572) ═══{N}")
    script = TOOLS / "gen_test_registry.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    args = [sys.executable, str(script)]
    if not fix:
        args.append("--check")
    r = run(args, cwd=ROOT)
    if r != 0:
        if fix:
            fail("test-registry generation failed")
        else:
            fail("test-registry stale — run ./build.py test-registry --fix")
        return r
    ok("test-registry regenerated" if fix else "test-registry OK")
    return 0


def cmd_test_binding():
    """Issue #1453: prim source ↔ tests/ binding + test-registry freshness."""
    print(f"{B}═══ Test binding + coverage (#1453) ═══{N}")
    # Unit tests for the gate itself
    # Issue #1932: gate unit tests live under tests/python/
    ut = ROOT / "tests" / "python" / "test_test_binding_gate.py"
    if ut.exists():
        r0 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r0.returncode != 0:
            fail("test_test_binding_gate unit tests failed")
            return 1
    # Issue #901/#908: FNV-1a and the slot-fingerprint rule have exactly one
    # owner (src/compiler/hash_meta.h). Run the source invariant here so the
    # gate catches new copies even when the changed file is allowlisted.
    mc = ROOT / "tests" / "python" / "test_magic_constants.py"
    if mc.exists():
        r_mc = subprocess.run([sys.executable, str(mc)], cwd=ROOT)
        if r_mc.returncode != 0:
            fail("magic-constant invariant failed — use hash_meta.h constants")
            return 1
    # check_test_coverage.py umbrella removed per Anqi 2026-07-19 directive
    # (scripts/ audit wave 9). check_test_binding.py covers the same surface
    # (production primitive sources must have tests/).
    script = COVERAGE_CHECKS / "check_test_binding.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("test binding failed — production primitive sources changed without tests/")
        return 1
    ok("test binding + coverage OK")
    return 0


def cmd_new_test_files():
    """Reject newly added issue-numbered test filenames.

    Same classifier as ``.githooks/pre-commit`` (``--cached``). Gate mode also
    sees commits versus origin/main and untracked files, so a missing
    ``core.hooksPath`` does not skip the ban.
    """
    print(f"{B}═══ New test filenames ═══{N}")
    ut = ROOT / "tests" / "python" / "test_new_test_files.py"
    if ut.exists():
        r0 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r0.returncode != 0:
            fail("test_new_test_files unit tests failed")
            return 1
    script = TOOLS / "check_new_test_files.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("new issue-numbered test file blocked — append to the home in tests/HOMES.md")
        return 1
    ok("new test filenames OK")
    return 0


def cmd_dead_heap_push():
    """Issue #1488 / #1668: dead string_heap_ push pollution audit (strict)."""
    print(f"{B}═══ Dead string_heap push audit (#1668) ═══{N}")
    script = AUDIT / "audit_dead_heap_push.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    # Issue #1932: gate unit tests live under tests/python/
    ut = ROOT / "tests" / "python" / "test_audit_dead_heap_push.py"
    if ut.exists():
        r0 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r0.returncode != 0:
            fail("test_audit_dead_heap_push unit tests failed")
            return 1
    r = subprocess.run([sys.executable, str(script), "--strict"], cwd=ROOT)
    if r.returncode != 0:
        fail(
            "dead string_heap_ push candidates found — "
            "run python3 scripts/audit/audit_dead_heap_push.py and remove unused pushes"
        )
        return 1
    ok("dead heap push audit clean")
    return 0


def cmd_catch_silent_swallow():
    """Issue #1669 / #615: catch(...) must carry SILENCE-PRIM marker (strict)."""
    print(f"{B}═══ catch(...) SILENCE-PRIM audit (#1669) ═══{N}")
    script = AUDIT / "audit_catch_silent_swallow.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    # Issue #1932: gate unit tests live under tests/python/
    ut = ROOT / "tests" / "python" / "test_audit_catch_silent_swallow.py"
    if ut.exists():
        r0 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r0.returncode != 0:
            fail("test_audit_catch_silent_swallow unit tests failed")
            return 1
    r = subprocess.run([sys.executable, str(script), "--strict"], cwd=ROOT)
    if r.returncode != 0:
        fail("unmarked catch(...) found — add [SILENCE-PRIM-#615] (or fix silent swallow)")
        return 1
    ok("catch silent-swallow audit clean")
    return 0


def cmd_aot_env_linear_stamp():
    """Issue #2091 / #2168: forbid literal (0,0) env/linear on production mangle/emit.

    Runs scripts/coverage/checks/check_aot_env_linear_stamp_coverage.py as a hard gate:
    any production call to mangle_aot_name / aot_link_name that passes
    literal (0, 0) without `# 2091-allow-zero` (or `# 2091-legacy`) fails
    the build. Tests/stubs/header defaults remain allowed (script skip list).
    """
    print(f"{B}═══ AOT env/linear stamp coverage (#2091 / #2168) ═══{N}")
    script = COVERAGE_CHECKS / "check_aot_env_linear_stamp_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    # Unit tests for the linter itself (annotation contract + self-test).
    ut = ROOT / "tests" / "python" / "test_aot_env_linear_stamp_gate.py"
    if ut.exists():
        r0 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r0.returncode != 0:
            fail("test_aot_env_linear_stamp_gate unit tests failed")
            return 1
    # Built-in self-test (bridge TU has no bare (0,0)).
    r_st = subprocess.run(
        [sys.executable, str(script), "--self-test"],
        cwd=ROOT,
    )
    if r_st.returncode != 0:
        fail("aot env/linear stamp self-test failed (aura_jit_bridge.cpp regression)")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail(
            "production mangle_aot_name/aot_link_name passes literal (0,0) — "
            "thread live env/linear via aot_resolve_emit_* or add "
            "`# 2091-allow-zero` (see aot_mangle.h / #2168)"
        )
        return 1
    ok("aot env/linear stamp coverage clean (no bare (0,0) emit paths)")
    return 0


def cmd_legacy_test_inventory():
    """Issue #1957: living legacy test inventory freshness check.

    Without --fix: ``scripts/tools/inventory_legacy_tests.py --check`` (exit 1 if
    tests/legacy_test_inventory.md is stale). With --fix: regenerate the
    markdown. Re-run after domain migrations or bulk test adds.
    """
    print(f"{B}═══ Legacy test inventory (#1957) ═══{N}")
    script = TOOLS / "inventory_legacy_tests.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    fix = "--fix" in sys.argv[2:]
    args = [sys.executable, str(script)]
    if not fix:
        args.append("--check")
    r = subprocess.run(args, cwd=ROOT)
    if r.returncode != 0:
        if fix:
            fail("legacy inventory regenerate failed")
        else:
            fail(
                "legacy_test_inventory.md stale — run "
                "`python3 scripts/tools/inventory_legacy_tests.py` or `./build.py gate --fix`"
            )
        return 1
    ok("legacy test inventory up to date" if not fix else "legacy test inventory regenerated")
    return 0
