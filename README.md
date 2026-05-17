# Aura

**AI-native Lisp** — 从最小核心自然生长。C++26 实现，~12,500 行核心代码。

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2)' | ./build/aura          # → 6
echo '(require std/math) (math:sqrt 16)' | ./build/aura  # → 4.0
```

## 亮点

**可写 500+ 行业务代码** — 纯函数式 core + 模块系统 + 错误处理 + 测试框架 + AI Agent 闭环。

| 特性 | 状态 |
|------|------|
| 语言核心 | ✅ ~80 原语 + TCO + lambda + let/let* + letrec |
| 宏系统 | ✅ quasiquote + gensym + 递归展开 |
| 模块 | ✅ `import`/`require` 前缀注入 + `export` 控制 + 循环检测 |
| 类型 | ✅ 渐进类型 L6 + 增量 typecheck（跳过 clean 子树） |
| 错误处理 | ✅ `try`/`catch`/`raise` + 原语返回 error |
| 测试 | ✅ `check`/`check=` + `test-suite` + bash runner（77 tests） |
| 标准库 | ✅ list / math / string / json / struct / validate / test |
| EDSL | ✅ `query:*`/`mutate:*`/`set-code` 15+ 原语 + workspace 模型 |
| 格式化 | ✅ `--fmt` / `--fmt -i` / `--fmt --check` |
| AI Agent | ✅ DeepSeek/MiniMax/GPT 通用接口 + 优化 prompt |

## 快速体验

```bash
# 基本运算
echo '(+ 1 2 3 4 5)' | ./build/aura

# 函数 + 标准库
echo '(require std/list) (foldl + 0 (range 1 101))' | ./build/aura

# 词频统计（纯函数式方案，5 个测试全通过）
./build/aura < /tmp/word-freq.aura

# 格式化库
./build/aura --fmt lib/std/list.aura

# 跑测试
python3 build.py check
```

## 项目结构

```
src/
  core/        FlatAST (SoA 结构), StringPool, arena 分配器
  parser/      lexer + s-exp → FlatAST（支持 dotted pair、quote、quasiquote）
  compiler/    树遍历求值器 | IR 管线 + lowering | 渐进类型检查 | EDSL query/mutate

lib/std/       list math string json struct validate test
tests/         run-tests.sh（77 回归）+ 集成测试 + C++ test_ir + AI Agent
docs/          设计文档 + 路线图
```

## 完整例子

```lisp
;; 两模块项目：sets.aura + analysis.aura
(require std/list)
(import "sets.aura" "sets:")

(define (word-freq words)
  (define (add-to-alist alist word)
    (if (null? alist) (cons (cons word 1) '())
        (if (string=? word (car (car alist)))
            (cons (cons word (+ (cdr (car alist)) 1)) (cdr alist))
            (cons (car alist) (add-to-alist (cdr alist) word)))))
  (foldl add-to-alist '() words))

(analyze-words '("the" "quick" "brown" "fox")
               '("the" "lazy" "cat" "fox"))
;; → top1 / top2 / intersection / difference
```

## 开发者

```bash
# 构建
python3 build.py build

# 测试（全部 5 套件）
python3 build.py test

# 特定套件
python3 build.py test bash      # bash 回归（77 tests）
python3 build.py test unit      # C++ 单元（61 cases）
python3 build.py test integ     # 端到端管线

# 格式化
./build/aura --fmt -i file.aura

# AI Agent（需 LLM API key）
LLM_API_KEY="sk-..." LLM_BASE_URL="https://api.deepseek.com" \
  LLM_MODEL="deepseek-v4-flash" \
  python3 tests/ai_agent_edsl.py "your task"
```

## 设计

- [`docs/roadmap.md`](docs/roadmap.md) — 完整路线图
- [`docs/module_namespace_design.md`](docs/module_namespace_design.md) — 模块系统
- [`docs/query_edsl_design.md`](docs/query_edsl_design.md) — EDSL query/mutate
- [`docs/error_handling_v2.md`](docs/error_handling_v2.md) — try/catch
- [`docs/formatter_design.md`](docs/formatter_design.md) — --fmt
- [`docs/testing_framework_design.md`](docs/testing_framework_design.md) — 测试框架

## License

Apache 2.0
