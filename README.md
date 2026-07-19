# Aura

AI-native Lisp — 代码自己进化。

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2)' | ./build/aura   # → 3
```

```bash
./build.py gate    # static gate (docs / lint / format / fixtures / primitive surface / test-registry / dead heap / catch-swallow)
./build.py check   # gate + build + tests
./build.py bench --strict  # compiler benchmark SLO gate (#1569)
```

CI: [`.github/workflows/ci.yml`](.github/workflows/ci.yml).
Onboard: [`docs/architecture.md`](docs/architecture.md) · [`docs/contributing.md`](docs/contributing.md) · `docs/agent-*.md`.
Code + tests = source of truth (aura philosophy).

License: Apache 2.0