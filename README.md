# Aura

AI-native Lisp — 代码自己进化。

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2)' | ./build/aura   # → 3
```

```bash
./build.py gate    # static gate (lint / format / fixtures / primitive surface / test-registry / dead heap / catch-swallow / mutation-guard / orch-MVP / legacy-test-inventory)
./build.py check   # gate + build + tests
```

License: Apache 2.0