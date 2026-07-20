# tests/memory/

Long-run memory / leak / ASan-oriented drivers (Issue #1932).

## Layout

```
tests/memory/
├── README.md
└── (long_run_*, *leak* drivers)
```

Keep ASan/TSan matrix shell helpers under `tests/python/` (invoked by
CI). Put multi-hour memory soak scripts here so they do not clutter
the top-level `tests/` tree or the default gate.
