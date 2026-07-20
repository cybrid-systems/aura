#!/usr/bin/env python3
"""run_e2e.py — Issue #1934 commercial_readiness / e2e suite runner.

Usage:
  python3 tests/python/run_e2e.py
  python3 tests/python/run_e2e.py --update-golden
  python3 tests/run.py e2e
  ./build.py test e2e
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# bootstrap path
_py = str(Path(__file__).resolve().parent)
if _py not in sys.path:
    sys.path.insert(0, _py)

from _aura_harness import AURA_BIN, B, N, fail, ok, warn  # noqa: E402
from e2e_harness import (  # noqa: E402
    E2EAssertionError,
    check_e2e_pass,
    check_golden,
    discover_commercial_readiness,
    golden_for,
    run_aura_file,
    write_golden,
)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--update-golden",
        action="store_true",
        help="rewrite fixtures/e2e_golden/*.json from current PASS labels",
    )
    ap.add_argument(
        "--filter",
        default="",
        help="substring filter on script stem",
    )
    ap.add_argument(
        "--timeout",
        type=float,
        default=60.0,
        help="per-file timeout seconds",
    )
    ap.add_argument(
        "--require-golden",
        action="store_true",
        default=True,
        help="require golden PASS labels (default on)",
    )
    ap.add_argument(
        "--no-require-golden",
        action="store_true",
        help="skip golden label equality (still require E2E-PASS)",
    )
    args = ap.parse_args(argv)

    print(f"{B}═══ E2E commercial_readiness (#1934) ═══{N}")
    if not AURA_BIN.is_file():
        fail(f"aura binary missing: {AURA_BIN}")
        return 1

    scripts = discover_commercial_readiness()
    if args.filter:
        scripts = [p for p in scripts if args.filter in p.stem]
    if not scripts:
        fail("no commercial_readiness_*.aura under tests/e2e/commercial_readiness/")
        return 1

    require_golden = args.require_golden and not args.no_require_golden
    passed = 0
    failed = 0
    for script in scripts:
        name = script.name
        try:
            result = run_aura_file(script, timeout=args.timeout)
            check_e2e_pass(result, min_passes=1)
            gpath = golden_for(script)
            if args.update_golden:
                write_golden(result, gpath)
                ok(f"{name}  PASS×{len(result.pass_labels)}  golden updated")
            elif require_golden:
                if not gpath.is_file():
                    raise E2EAssertionError(f"{name}: missing golden {gpath} (run --update-golden)")
                check_golden(result, gpath)
                ok(f"{name}  PASS×{len(result.pass_labels)}  golden OK")
            else:
                ok(f"{name}  PASS×{len(result.pass_labels)}")
            passed += 1
        except (E2EAssertionError, FileNotFoundError, OSError) as e:
            fail(f"{name}: {e}")
            failed += 1
        except Exception as e:  # noqa: BLE001 — surface unexpected errors
            fail(f"{name}: unexpected {type(e).__name__}: {e}")
            failed += 1

    total = passed + failed
    print(f"  E2E: {passed}/{total} passed")
    if failed:
        warn(f"{failed} e2e file(s) failed — see FAIL lines / golden diffs above")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
