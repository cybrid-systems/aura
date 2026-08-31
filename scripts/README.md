# scripts/

Reorganized coverage / tools / audit layout (architecture Phase 1–2).

```
scripts/
  coverage/
    runner.py           # declarative issue coverage (JSON manifests)
    manifests/*.json    # issue AC contracts (prefer these for new issues)
    checks/check_*.py   # legacy + complex gates; pure ones thin-wrap the runner
  tools/                # gen_docs, gen_test_registry, inventory, release, …
  audit/                # catch silent-swallow, dead heap push, …
```

## Commands

```bash
# All declarative manifests
python3 scripts/coverage/runner.py --all

# Only issues whose paths intersect git diff (default for build.py)
python3 scripts/coverage/runner.py --changed
./build.py issue-coverage

# Full inventory / docs
python3 scripts/tools/gen_docs.py
python3 scripts/tools/inventory_legacy_tests.py --check
```

**Policy (SSOT):** [tests/COVERAGE.md](../tests/COVERAGE.md). New issues add
`scripts/coverage/manifests/<N>.json`. Reserve `checks/check_*.py` for custom
logic (window / regex / AST). Gate (`run_checks.py`) runs remaining Python
checks **and** all manifests. `fold_simple_checks.py` folds leftover
substring scripts. `check_coverage_policy.py` rejects new substring-only
scripts and thin wrappers.
