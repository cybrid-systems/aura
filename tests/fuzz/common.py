"""tests/fuzz/common.py — shared paths and CLI for Aura fuzz drivers (#1935).

Every driver under tests/fuzz/drivers/ should prefer these helpers so
paths and flags stay consistent:

  --quick / --iters N / --seed N / --timeout N / --json
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

FUZZ_ROOT = Path(__file__).resolve().parent  # tests/fuzz
TESTS = FUZZ_ROOT.parent
REPO = TESTS.parent
DRIVERS = FUZZ_ROOT / "drivers"
CORPUS = FUZZ_ROOT / "corpus"
REPRODUCERS = FUZZ_ROOT / "reproducers"

AURA_BIN = Path(os.environ.get("AURA_BIN", str(REPO / "build" / "aura")))


@dataclass
class FuzzResult:
    """Structured result for orchestrator aggregation."""

    name: str
    passed: int = 0
    failed: int = 0
    crashes: int = 0
    timeouts: int = 0
    skipped: int = 0
    iters: int = 0
    elapsed_s: float = 0.0
    exit_code: int = 0
    notes: list[str] = field(default_factory=list)
    extra: dict[str, Any] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return self.exit_code == 0 and self.crashes == 0 and self.failed == 0

    def to_json(self) -> dict[str, Any]:
        d = asdict(self)
        d["ok"] = self.ok
        return d


def parse_common_args(
    argv: list[str] | None = None,
    *,
    default_iters: int = 200,
    description: str = "Aura fuzzer",
) -> argparse.Namespace:
    """Parse standardized fuzz CLI flags."""
    ap = argparse.ArgumentParser(description=description)
    ap.add_argument("--quick", action="store_true", help="reduced iterations")
    ap.add_argument("--iters", type=int, default=None, help="override iteration count")
    ap.add_argument("--seed", type=int, default=None, help="RNG seed")
    ap.add_argument(
        "--timeout",
        type=int,
        default=int(os.environ.get("FUZZ_TIMEOUT", "5")),
        help="per-case timeout seconds",
    )
    ap.add_argument("--json", action="store_true", help="emit FuzzResult JSON trailer")
    ap.add_argument(
        "--corpus-dir",
        type=Path,
        default=CORPUS,
        help="seed corpus directory",
    )
    ap.add_argument(
        "--repro-dir",
        type=Path,
        default=REPRODUCERS,
        help="directory for crash reproducers",
    )
    ns, rest = ap.parse_known_args(argv)
    # Compat: some drivers still scan sys.argv for --quick/--seed
    if ns.quick and ns.iters is None:
        ns.iters = max(20, default_iters // 10)
    elif ns.iters is None:
        ns.iters = default_iters
    ns.rest = rest
    ns.aura = AURA_BIN
    ns.repo = REPO
    ns.fuzz_root = FUZZ_ROOT
    return ns


def resolve_iters(args: argparse.Namespace, full: int, quick: int) -> int:
    if getattr(args, "iters", None) is not None and "--iters" in sys.argv:
        return int(args.iters)
    if getattr(args, "quick", False):
        return quick
    return full


def print_result(result: FuzzResult, *, as_json: bool = False) -> int:
    if as_json:
        print(json.dumps(result.to_json(), indent=2, sort_keys=True))
    else:
        status = "PASS" if result.ok else "FAIL"
        print(
            f"[{status}] {result.name}: pass={result.passed} fail={result.failed} "
            f"crash={result.crashes} timeout={result.timeouts} "
            f"iters={result.iters} time={result.elapsed_s:.1f}s"
        )
        for n in result.notes[:10]:
            print(f"  note: {n}")
    return 0 if result.ok else 1
