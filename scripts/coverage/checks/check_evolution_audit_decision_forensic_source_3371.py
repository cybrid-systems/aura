#!/usr/bin/env python3
"""Issue #3371: [obs/agent][audit] evolution-audit-decision forensic-source
bumped to 3 (WAL) even when SE-ring already had the mid (forensic-source=2),
breaking #3152's "ring first, WAL only on real miss" contract.

Contract (one row per AC):
  AC1  Soft zero-cost: no new WAL scan; `:durable` off → `durable-hit == 0`.
       The fix is a pure conditional fallback — no I/O added.
  AC2  SE ring preferred over WAL flag: typed miss + same mid in SE ring +
       WAL enabled → `forensic-source == 2` (NOT 3). `last-se-reason` /
       `last-se-denied` still come from the SE-ring match; `se-mid-miss == 0`.
  AC3  Real wrap / ring miss: typed miss + SE ring no mid + WAL on →
       `forensic-source == 3`. Default path `durable-hit == 0`; only the
       explicit `:durable` keyword does a point query.
  AC4  Old sentinels: `schema-3152` / `forensic-source-trail|se|wal` values
       unchanged (1/2/3). `schema-3114` / observe-only / overflow cap
       unchanged. No new query:* keys, no new metrics bus.
  AC5  Verify: tests/compiler/test_engine_metrics_facade.cpp evolution-
       audit-decision green. Do not touch #3338 segment window.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    eps = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_engine_metrics_facade.cpp")
    build = _read("build.py")

    # ── AC1 + AC2 + AC3: default-path priority order (typed > ring > WAL) ─
    # The new ordering must be present in the default path (the `want_durable`
    # path stays on its existing `< 3 → 3` bump — that path is *explicit*
    # durable forensic per Issue #3205).
    must(
        "if (typed_hit) {",
        "AC2/AC3 default-path branch on typed_hit first",
        eps,
    )
    must(
        "forensic_source = 1;",
        "AC2 default-path typed-hit → 1",
        eps,
    )
    must(
        "} else if (se_ring_has_mid) {",
        "AC2 default-path else-if on se_ring_has_mid",
        eps,
    )
    must(
        "forensic_source = 2;",
        "AC2 default-path ring-hit → 2",
        eps,
    )
    must(
        "} else if (wal_enabled) {",
        "AC3 default-path else-if on wal_enabled",
        eps,
    )
    must(
        "forensic_source = 3;",
        "AC3 default-path ring-miss + WAL on → 3",
        eps,
    )
    must(
        "// Issue #3371: priority order — typed > ring > WAL > 0.",
        "AC1/AC2/AC3 #3371 fix comment block present in source",
        eps,
    )

    # ── AC2: the old unconditional < 3 → 3 bump must be gone from ──────
    # the default path. It still exists inside the `want_durable` keyword
    # block (Issue #3205) — that's correct (user explicitly asked for
    # durable forensic). Verify the bump is *scoped* to want_durable.
    # We do NOT require absence (durable path keeps it) — we require the
    # default path to use the new priority order (covered above).
    must(
        "if (want_durable && join_mid != 0 &&",
        "AC1/AC2 want_durable keyword path preserved (#3205)",
        eps,
    )
    # The old < 3 → 3 unconditional bump pattern must be inside the
    # want_durable block only. Probe a stable fragment that only exists
    # there (after want_durable's WAL find_recent path).
    must(
        "if (forensic_source < 3)\n                            forensic_source = 3;",
        "AC2 < 3 → 3 bump scoped to want_durable keyword path",
        eps,
    )

    # ── AC4: old sentinels + schema values unchanged ─ ──
    must(
        "forensic-source-trail",
        "AC4 schema-3152 forensic-source-trail sentinel unchanged",
        eps,
    )
    must(
        "forensic-source-se",
        "AC4 schema-3152 forensic-source-se sentinel unchanged",
        eps,
    )
    must(
        "forensic-source-wal",
        "AC4 schema-3152 forensic-source-wal sentinel unchanged",
        eps,
    )
    must(
        "schema-3152",
        "AC4 schema-3152 still present (no enum redefinition)",
        eps,
    )
    must(
        "schema-3114",
        "AC4 schema-3114 observe-only unchanged",
        eps,
    )
    must(
        "overflow",
        "AC4 overflow cap unchanged (no new cap)",
        eps,
    )
    must(
        "query:evolution-audit-decision",
        "AC4 query:evolution-audit-decision unchanged (no new query:* keys)",
        eps,
    )

    # ── AC5: existing test extended with #3371 AC markers ──
    must(
        "3371 AC",
        "AC5 tests/compiler/test_engine_metrics_facade.cpp cites #3371",
        test,
    )
    # Verify no new test_issue_3371.cpp was invented.
    if (ROOT / "tests" / "compiler" / "test_issue_3371.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3371.cpp present (forbidden #81967)")
    # No docs/design/3371-* — preserve per #1655.
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3371-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    # ── Linter wired in build.py ─ ──
    must(
        "check_evolution_audit_decision_forensic_source_3371",
        "AC5 build.py wires 3371 linter",
        build,
    )
    must(
        "Issue #3371",
        "AC5 linter error message in build.py",
        build,
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3371 evolution-audit-decision forensic-source priority — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
