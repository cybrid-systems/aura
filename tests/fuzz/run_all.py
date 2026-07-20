#!/usr/bin/env python3
"""tests/fuzz/run_all.py — unified fuzz orchestrator (Issue #1935).

Usage:
  python3 tests/fuzz/run_all.py --list
  python3 tests/fuzz/run_all.py --all --iters 100
  python3 tests/fuzz/run_all.py --only corpus,edsl --quick
  python3 tests/fuzz/run_all.py --all --json
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time

from common import AURA_BIN, DRIVERS, REPO, FuzzResult

# Registered fuzzers: name → (script, default full iters for --iters override)
REGISTRY: dict[str, dict] = {
    "core": {
        "script": "fuzz.py",
        "desc": "random/structure/edge compiler fuzz",
        "supports_iters": False,  # uses --quick only
    },
    "corpus": {
        "script": "fuzz_corpus.py",
        "desc": "seed corpus parser/eval",
        "supports_iters": False,
    },
    "edsl": {
        "script": "fuzz_edsl.py",
        "desc": "EDSL mutation via --serve",
        "supports_iters": False,
    },
    "equiv": {
        "script": "fuzz_equiv_mutate.py",
        "desc": "semantic-preserving transform equivalence",
        "supports_iters": False,
    },
    "type": {
        "script": "fuzz_type_soundness.py",
        "desc": "typecheck after mutate sequences",
        "supports_iters": False,
    },
    "workspace": {
        "script": "fuzz_workspace.py",
        "desc": "workspace create/switch stress",
        "supports_iters": False,
    },
    "structured": {
        "script": "fuzz_structured.py",
        "desc": "structured s-expr generator",
        "supports_iters": False,
    },
    "messaging": {
        "script": "fuzz_messaging.py",
        "desc": "messaging / mailbox fuzz",
        "supports_iters": False,
    },
    "rule": {
        "script": "fuzz_rule.py",
        "desc": "rule engine fuzz",
        "supports_iters": False,
    },
    "pipeline": {
        "script": "fuzz_pipeline.py",
        "desc": "compile pipeline fuzz",
        "supports_iters": False,
    },
    "snapshot": {
        "script": "fuzz_snapshot.py",
        "desc": "ast snapshot/restore fuzz",
        "supports_iters": False,
    },
    "defuse": {
        "script": "fuzz_defuse.py",
        "desc": "def-use index fuzz",
        "supports_iters": False,
    },
    "diff": {
        "script": "fuzz_diff.py",
        "desc": "ast:diff fuzz",
        "supports_iters": False,
    },
    "hygiene_prop": {
        "script": "prop_hygiene_mutate.py",
        "desc": "property-based hygiene + mutate (#1935)",
        "supports_iters": True,
    },
}


def list_fuzzers() -> None:
    print("Registered fuzzers (Issue #1935):\n")
    width = max(len(k) for k in REGISTRY)
    for name, meta in sorted(REGISTRY.items()):
        script = DRIVERS / meta["script"]
        status = "ok" if script.is_file() else "MISSING"
        print(f"  {name:<{width}}  {meta['desc']}  [{status}]  ({meta['script']})")
    print(f"\nDrivers dir: {DRIVERS}")
    print(f"Aura bin:    {AURA_BIN} ({'exists' if AURA_BIN.is_file() else 'MISSING'})")


def run_one(
    name: str,
    *,
    quick: bool,
    iters: int | None,
    seed: int | None,
    timeout: int | None,
    extra: list[str],
) -> FuzzResult:
    meta = REGISTRY[name]
    script = DRIVERS / meta["script"]
    if not script.is_file():
        return FuzzResult(name=name, exit_code=2, notes=[f"missing {script}"])

    cmd = [sys.executable, str(script)]
    if quick:
        cmd.append("--quick")
    if iters is not None and meta.get("supports_iters"):
        cmd.extend(["--iters", str(iters)])
    if seed is not None:
        cmd.extend(["--seed", str(seed)])
    if timeout is not None:
        # drivers read FUZZ_TIMEOUT env more often than --timeout
        pass
    cmd.extend(extra)

    env = {
        **os.environ,
        "AURA_BIN": str(AURA_BIN),
        "AURA": str(AURA_BIN),
    }
    if timeout is not None:
        env["FUZZ_TIMEOUT"] = str(timeout)

    t0 = time.time()
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(REPO),
            env=env,
            capture_output=True,
            text=True,
            timeout=None,  # overall driver timeout left unbounded; per-case uses FUZZ_TIMEOUT
        )
        elapsed = time.time() - t0
        out = (proc.stdout or "") + "\n" + (proc.stderr or "")
        # Heuristic stats from common driver output
        crashes = out.lower().count("crash")
        timeouts = out.lower().count("timeout")
        # Prefer non-zero exit as fail signal
        return FuzzResult(
            name=name,
            passed=1 if proc.returncode == 0 else 0,
            failed=0 if proc.returncode == 0 else 1,
            crashes=crashes if proc.returncode != 0 else 0,
            timeouts=timeouts if proc.returncode != 0 else 0,
            iters=iters or 0,
            elapsed_s=round(elapsed, 3),
            exit_code=int(proc.returncode),
            notes=[ln.strip() for ln in out.splitlines() if ln.strip()][-5:],
            extra={"cmd": cmd},
        )
    except Exception as e:  # noqa: BLE001
        return FuzzResult(
            name=name,
            failed=1,
            exit_code=1,
            elapsed_s=round(time.time() - t0, 3),
            notes=[f"{type(e).__name__}: {e}"],
        )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list", action="store_true", help="list registered fuzzers")
    ap.add_argument("--all", action="store_true", help="run all registered drivers")
    ap.add_argument(
        "--only",
        default="",
        help="comma-separated fuzzer names (default: core,corpus,hygiene_prop)",
    )
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--iters", type=int, default=None)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--timeout", type=int, default=None, help="per-case FUZZ_TIMEOUT")
    ap.add_argument("--json", action="store_true")
    ap.add_argument(
        "--continue-on-error",
        action="store_true",
        help="run remaining fuzzers after a failure",
    )
    args = ap.parse_args(argv)

    if args.list:
        list_fuzzers()
        return 0

    if args.all:
        names = list(REGISTRY.keys())
    elif args.only:
        names = [n.strip() for n in args.only.split(",") if n.strip()]
    else:
        # Safe default subset for CI / quick local
        names = ["core", "corpus", "hygiene_prop"]

    unknown = [n for n in names if n not in REGISTRY]
    if unknown:
        print(f"unknown fuzzer(s): {unknown}", file=sys.stderr)
        print("use --list", file=sys.stderr)
        return 2

    if not AURA_BIN.is_file():
        print(f"ERROR: aura binary missing: {AURA_BIN}", file=sys.stderr)
        print("Build first: ./build.py build", file=sys.stderr)
        return 2

    print("═══ Fuzz orchestrator (#1935) ═══")
    print(f"  aura: {AURA_BIN}")
    print(f"  run:  {', '.join(names)}")
    results: list[FuzzResult] = []
    failed = 0
    for name in names:
        print(f"\n── {name}: {REGISTRY[name]['desc']} ──")
        r = run_one(
            name,
            quick=args.quick,
            iters=args.iters,
            seed=args.seed,
            timeout=args.timeout,
            extra=[],
        )
        results.append(r)
        status = "✓" if r.ok else "✗"
        print(f"  {status} exit={r.exit_code} time={r.elapsed_s:.1f}s")
        if not r.ok:
            failed += 1
            for n in r.notes[-3:]:
                print(f"    {n[:200]}")
            if not args.continue_on_error:
                break

    summary = {
        "schema": 1935,
        "issue": 1935,
        "ran": len(results),
        "failed": failed,
        "results": [r.to_json() for r in results],
    }
    if args.json:
        print(json.dumps(summary, indent=2))
    else:
        print(f"\nFuzz summary: {len(results) - failed}/{len(results)} drivers clean")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
