# Aura

**AI-native Lisp — 代码自己进化。**

**让代码第一次拥有思考的能力。**

大多数语言为人类设计。Aura 问：如果使用者是 AI，语言应该长什么样？

不是“让 AI 写代码”，而是让语言成为 AI 可理解、操作、进化的活系统。

---

## 30 秒 Quickstart

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2)' | ./build/aura   # → 3
```

```scheme
(set-code "...")
(query:calls "fact")
(mutate:rebind "fact" "..." "迭代化")
(eval-current)
```

完整示例见 [`docs/tutorial.md`](docs/tutorial.md)。

---

## 哲学

Aura 遵循三个原则：

1. **着力即差**  
   不为 AI 设限。prompt 只说“你是 Aura”，语言自己处理。

2. **代码即记忆**  
   AST 是 Agent 可直接读写验证的自我意识。  
   `set-code` → `query:*` → `mutate:*` → `eval-current`。

3. **控制论闭环**  
   AI 给方向，Aura 做本地精确变异。信息素排序 + PID 代替蛮力，形成自演化闭环。

---

## 能力

- 精确自修改 EDSL（query / mutate / ast）
- 原生 Agent 编排与并发
- 版本化工作区与事务安全
- 增量编译执行管线

文档索引：[`docs/README.md`](docs/README.md)（**代码 + 测试为真相**；运行 `(api-reference)` 查看全部原语）。

```bash
./build.py build    # 构建
./build.py gate     # 静态门栅（与 CI job `gate` 相同）
./build.py check    # gate + 构建 + 核心测试矩阵
./build.py bench --strict  # 编译器 benchmark SLO 硬门栅（#1569）
```

**CI 与测试路径（#1570）** — 勿在文档/issue 中引用不存在的路径：

| 话题 | 真实位置 |
|------|----------|
| GitHub Actions CI | [`.github/workflows/ci.yml`](.github/workflows/ci.yml)（**不是** `.github/ci_pipeline.yml`） |
| Benchmark / SLO gate | [`tests/benchmark.py`](tests/benchmark.py) · `./build.py bench --strict`（**不是** `src/test/benchmark_gate.ixx`） |
| Issue-test 模式 | [`docs/test_harness_pattern.md`](docs/test_harness_pattern.md) · [`tests/test_harness.hpp`](tests/test_harness.hpp) · 模板 [`tests/templates/test_issue_pattern.cpp`](tests/templates/test_issue_pattern.cpp) |

贡献运行时：[`docs/contributing.md`](docs/contributing.md)。测试上手：[`docs/test_harness_pattern.md`](docs/test_harness_pattern.md)。

## Platform Notes

| 平台 | 核心功能 | 限制 | CI (#1573) |
|------|---------|------|------------|
| **Linux** | 全功能可用 | — | 生产门栅：`gate` + `build-test` + sanitizers + bench SLO |
| **macOS (Apple Silicon / Homebrew GCC)** | REPL、evaluator、IR、type-checker、JIT fallback 均可用 | `--serve-async`、`--concurrent-metrics`、`--serve-async-bench` 不可用（缺少 eventfd / epoll）；fiber scheduler 编译但不运行 | `platform-gates` / macos-core（preset + REPL smoke；main 加 `test_ir`） |
| **Windows** | 脚本/DX 工具链 | 原生 MSVC C++ **尚未** production-supported（C++26 modules + GCC contracts） | `platform-gates` / windows-scripts（`build.py gate --scripts-only`） |

详情：[docs/ci-platforms.md](docs/ci-platforms.md)。

macOS 构建需 Homebrew GCC ≥ 14（推荐 gcc@16，C++20 module 支持最完善）：

```bash
brew install gcc@16 cmake ninja
cmake --preset macos && cmake --build build-mac -j
echo '(+ 1 2)' | ./build-mac/aura   # → 3
```

## Demos

The flagship EDA demo is **SEVA** — a self-evolving
verification agent that drives a hardware verification
closed-loop using Aura's mutation + query primitives.

- Architecture + how-it-works: [docs/demos/seva.md](docs/demos/seva.md)
- Step-by-step tutorial: [demos/seva/TUTORIAL.md](demos/seva/TUTORIAL.md)
- Runnable demo: `cat demos/seva/seva_demo.aura | ./build/aura`
- OpenClaw skill: `python3 demos/seva/openclaw-skill/seva_skill.py "Achieve 95% coverage on FIFO"`

The loop: load DUT → verify:parse-coverage-feedback
→ query:verify-dirty-stats → strategy evolution controller
→ mutate:replace-pattern → query:seva-audit-log → repeat.

## License

Apache 2.0
