# tests/fuzz/

Fuzz campaigns, seed corpora, and crash reproducers (Issue #1932).

## Layout

```
tests/fuzz/
├── README.md          # this file
├── corpus/            # seed inputs (optional)
└── reproducers/       # minimized crashing inputs
```

Python fuzz drivers that used to live at `tests/fuzz_*.py` belong here.
C++ stress tests that are not pure fuzzers stay as normal issue/domain
binaries under `tests/` / `tests/domain/`.

## Running

Prefer dedicated CI jobs or:

```bash
# when a driver exists:
python3 tests/fuzz/<driver>.py
```

Do not put long-running fuzz loops in the default `./build.py check` path.
