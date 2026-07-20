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

Testing / layout policy: [`tests/README.md`](tests/README.md) · [`docs/contributing.md`](docs/contributing.md)

Module layering (Core ← Parser ← Compiler ← …): [`docs/architecture.md`](docs/architecture.md) · [`src/core/module_boundary.ixx`](src/core/module_boundary.ixx)

Naming & comments: [`docs/naming_convention.md`](docs/naming_convention.md)

Test strategy (hot path / self-mod): [`tests/STRATEGY.md`](tests/STRATEGY.md)

License: Apache 2.0