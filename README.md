# Aura

**AI-native Lisp** — C++26 实现，从最小核心自然生长。

## 快速开始

```bash
# 构建
cmake -B build && cmake --build build --target aura -j

# 跑测试
python3 build.py check

# Hello
echo '(+ 1 2 3)' | ./build/aura          # → 6
echo '(define (sq x) (* x x)) (sq 12)' | ./build/aura  # → 144

# 标准库（自动发现路径，无需 AURA_PATH）
./build/aura --fmt lib/std/list.aura      # 格式化
./build/aura < tests/run-all.aura         # 运行测试
```

## 核心能力

| 特性 | 状态 |
|------|------|
| **~80 原语 + TCO + match + define-struct** | ✅ |
| **宏系统 v2** — quasiquote + gensym + 递归展开 | ✅ |
| **渐进类型** — L6 推断 + forall + Float + 增量 typecheck | ✅ |
| **增量编译** — dirty 跟踪 + 跳过 clean 子树 | ✅ |
| **模块命名空间** — use/module-get/import prefix/export | ✅ |
| **错误处理** — try/catch/raise（不崩溃） | ✅ |
| **测试框架** — check/check=/test-suite + bash runner（80+ 测试） | ✅ |
| **EDSL** — query:*/mutate:*/set-code/typecheck-current 15+ 原语 | ✅ |
| **`--serve` 协议** — JSON Lines 与 AI Agent 交互 | ✅ |
| **`--fmt` 格式化** — 代码格式化（--fmt/-i/--check） | ✅ |
| **AI Agent** — MiniMax/GPT/DeepSeek 通用接口 | 🟡 |

## 两行代码

```bash
# 前缀导入（命名空间安全）
echo '(require std/math) (math:sqrt 16)' | ./build/aura  # → 4.0

# EDSL 增量修改
printf '(set-code "(define (f x) (+ x 1))")(mutate:replace-value 0 "*" "swap")(typecheck-current)(eval-current)(f 5)' | ./build/aura --serve
```

## 源码结构

```
src/core/       arena, FlatAST (SoA), type
src/parser/     lexer, parser (S-expr → FlatAST)
src/compiler/   evaluator | IR + lowering | type_checker | query | service
lib/std/        std/list, math, string, json, struct, validate, test
tests/          C++ test_ir (22 tests) + bash runner (80+ tests)
docs/           roadmap + 设计文档
```

## 设计文档

- [`docs/roadmap.md`](docs/roadmap.md) — 完整路线图
- [`docs/error_handling_v2.md`](docs/error_handling_v2.md) — try/catch
- [`docs/module_namespace_design.md`](docs/module_namespace_design.md) — 模块系统 v2
- [`docs/testing_framework_design.md`](docs/testing_framework_design.md) — 测试框架
- [`docs/query_edsl_design.md`](docs/query_edsl_design.md) — EDSL 设计
- [`docs/formatter_design.md`](docs/formatter_design.md) — --fmt 格式化
- [`docs/known_issues.md`](docs/known_issues.md) — 已知问题

## License

Apache 2.0
