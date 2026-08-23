#!/usr/bin/env python3
"""Issue #3286 linter — production QueryResult must force schema-2 full
provenance (layout-only matches rejected / auto-upgraded).

Multi-round Agent memory hole (P0, I6): under production defaults, a path
that returns a durable QueryResult (or promotes matches via :as-query-result
/ :query-result #t) must stamp schema-2 full provenance (tenant + fiber +
cow + wrap + mutation_id + reserved marker). Layout-only (schema-1) matches
must be rejected or auto-upgraded. Soft/Off may keep the cheap layout-only
path.

Fix: the shared end_query_epoch_maybe_result finish helper now auto-upgrades
a bare match list to the schema-2 stamped hash under
production_defaults_active() — make_query_result_hash already calls
stamp_query_result_full_provenance (sets reserved = kQueryResultMatchSchema2)
and fails closed (restamp-lag / query-epoch-stale) when the stamp cannot
complete. Soft/Off keeps the layout-only bare path (zero-cost).

Gate rows:
  G1  evaluator_primitives_query_workspace.cpp cites Issue #3286.
  G2  end_query_epoch_maybe_result forces the stamp path under production:
      `if (!as_query_result && production_defaults_active()) as_query_result =
      true;` (auto-upgrade).
  G3  Soft path preserved: the bare-list early return still exists for
      non-production (`if (!as_query_result) return finished;`).
  G4  stamp helper present (stamp_query_result_full_provenance + reserved =
      kQueryResultMatchSchema2) and fail-closed errors unchanged.
  G5  test ACs in tests/compiler/test_query_result_full_provenance.cpp
      (#3103/#3137/#3231 suite home, #81967) citing #3286: production bare
      list is a hash (schema-2 auto-upgrade) + Soft bare list unchanged.
  G6  build.py wires this linter.
  G7  no docs/design/3286-* (per #1655), no tests/issue*/test_issue_3286.cpp
      (per #81967).

Exit 0 = all rows satisfied.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

failures: list[str] = []


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def read(rel: str) -> str:
    p = ROOT / rel
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def main() -> int:
    print("=== #3286 production QueryResult schema-2 linter ===")
    src = read("src/compiler/evaluator_primitives_query_workspace.cpp")
    test = read("tests/compiler/test_query_result_full_provenance.cpp")
    build = read("build.py")

    must("Issue #3286" in src, "G1: evaluator_primitives_query_workspace.cpp cites Issue #3286")
    must(
        "!as_query_result &&\n            aura::compiler::typed_audit::production_defaults_active()" in src
        or ("as_query_result = true; // auto-upgrade" in src and "production_defaults_active()" in src),
        "G2: end_query_epoch_maybe_result auto-upgrades under production",
    )
    must(
        "if (!as_query_result)\n            return finished;" in src,
        "G3: Soft bare-list early return preserved (zero-cost)",
    )
    must(
        "stamp_query_result_full_provenance" in src and "kQueryResultMatchSchema2" in src and "restamp-lag" in src,
        "G4: stamp helper + fail-closed errors present",
    )
    must(
        "test_ac3286_production_bare_list_auto_upgraded" in test
        and "test_ac3286_soft_bare_list_unchanged" in test
        and "Issue #3286" in src,
        "G5: test ACs in #3103/#3137/#3231 suite home cite #3286",
    )
    must("check_production_query_result_schema2_3286.py" in build, "G6: build.py wires linter")

    docs_ok = True
    if (ROOT / "docs/design").exists():
        docs_ok = not any(p.name.startswith("3286-") for p in (ROOT / "docs/design").glob("3286-*"))
    must(docs_ok, "G7a: no docs/design/3286-* per #1655")
    must(
        not (ROOT / "tests" / "issues" / "test_issue_3286.cpp").exists(),
        "G7b: no tests/issues/test_issue_3286.cpp per #81967",
    )

    print()
    if failures:
        print(f"#3286 linter FAILED: {len(failures)} gate(s) — {failures}")
        return 1
    print("#3286 linter: all gates OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
