# tests/fuzz/ — unified fuzzing (Issue #1935)

See **[docs/fuzzing.md](../../docs/fuzzing.md)** for full documentation.

```bash
./build.py fuzz --list
./build.py fuzz --all --quick
python3 tests/fuzz/corpus_tools.py status
```

| Path | Role |
|------|------|
| `run_all.py` | Orchestrator |
| `common.py` | Shared paths / FuzzResult |
| `drivers/` | Individual fuzzers |
| `corpus/` | Seed inputs (`.sexpr`) |
| `reproducers/` | Crash artifacts |
