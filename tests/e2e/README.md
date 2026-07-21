# tests/e2e/ — strengthened .aura E2E (Issue #1934)

## Layout

```
tests/e2e/
├── README.md
└── commercial_readiness/
    └── commercial_readiness_*.aura   # machine-checkable PASS/FAIL + E2E-PASS
tests/fixtures/e2e_golden/
    └── all.json                      # consolidated golden:
                                      #   {"suites": {<stem>: {source, pass_labels, min_passes}}}
tests/python/
    ├── e2e_harness.py                # check_* helpers (suite_name from result.path.stem)
    └── run_e2e.py                    # suite runner
```

## Contract

Each `commercial_readiness_*.aura` must:

1. Print `PASS: <label>` for every assertion that succeeds
2. Print `FAIL: <label> expected=… actual=…` on failure
3. Print `E2E-PASS` iff there were zero failures (else `E2E-FAIL`)

Python harness (`e2e_harness.check_e2e_pass`) fails the process if any
`FAIL:` line or crash marker appears, even when `aura` exits 0.

## Run

```bash
./build.py test e2e
python3 tests/run.py e2e
python3 tests/python/run_e2e.py --update-golden   # refresh goldens
```

## Adding a case

1. Add `tests/e2e/commercial_readiness/commercial_readiness_<name>.aura`
2. Run `python3 tests/python/run_e2e.py --update-golden` (updates the consolidated `all.json`)
3. Commit both the `.aura` and `fixtures/e2e_golden/all.json`
