# Aura

**AI-native Lisp** — C++26 实现，100+ 原语，106 测试全通过，IR 管线默认启用。

```bash
cmake -B build && cmake --build build --target aura -j

echo '(+ 1 2)' | ./build/aura                # → 3
echo '(map (lambda (x) (* x 2)) (list 1 2 3))' | ./build/aura  # → (2 4 6)
./build/aura --fmt lib/std/list.aura          # 格式化
python3 build.py check                        # 构建 + 全部测试
```

## 执行管线

```
输入 → 解析(lex+parse) → 宏展开 → fallback检测
  ├── 需求值器状态(EDSL/模块/特殊形式) → 树遍历求值器 (eval_flat)
  └── 其他(算术/lambda/原语/quote/闭包) → IR lowering → passes → IRInterpreter
```

## 能力矩阵

| 类别 | 内容 |
|------|------|
| **语言核心** | 100+ 原语、TCO、lambda、let/let\*/letrec、cond、match、when/unless |
| **宏系统** | quasiquote、gensym、递归展开、dotted rest param |
| **模块** | `require`/`import` 前缀注入 + `export` 控制 + 循环检测 + 自动 lib 发现 |
| **类型** | 渐进类型 L6、forall、Float、增量 typecheck（dirty skip + type cache） |
| **错误处理** | `try`/`catch`/`raise`/`assert`，原语返回 error 不崩溃 |
| **测试框架** | `check`/`check=` + `test-suite` 宏 + `run-tests` 原语 |
| **正则** | `regex-match?` `regex-find` `regex-replace` `regex-split` |
| **数学** | `sin cos tan asin acos atan log log10 exp pow sqrt floor ceil round`| 
| **文件 IO** | `read-file` `write-file` `file-copy` `file-delete` `file-size` `file-exists?` `directory-list` |
| **标准库** | `list` `math` `string` `json` `struct` `validate` `test` (8 lib) |
| **IR 管线** | 默认启用、const folding、compute-kind、arity check、闭包桥接 |
| **EDSL** | `set-code` `query:*` `mutate:*` `typecheck-current` `eval-current` 15+ 原语 |
| **格式化** | `--fmt` / `--fmt -i` / `--fmt --check` |
| **AI Agent** | DeepSeek 默认 + Phase 2 EDSL 工作流 + 自动测试 + truncation 检测 |

## 快速体验

```bash
# 算术 + lambda + map
echo '(map (lambda (x) (* x 2)) (list 1 2 3))' | ./build/aura  # → (2 4 6)

# 链式比较
echo '(= 5 5 5)' | ./build/aura          # → #t

# 引用的列表
echo "'(1 2 3)" | ./build/aura            # → (1 2 3)

# 标准库
python3 -c "print('(import \"std/list\")(foldl + 0 (range 1 101))')" | ./build/aura

# 正则
echo '(regex-match? "[0-9]+" "abc123")' | ./build/aura    # → #t

# 文件 IO
echo '(directory-list ".")' | ./build/aura | head -3

# 测试
echo '(require std/test all:)(test-suite "sm" (check= 3 (+ 1 2)))(run-tests)' | ./build/aura

# EDSL 变换
printf '(set-code "(define (f x) (+ x 1))")(query:find "f")(mutate:replace-value 0 "*")(eval-current)(f 5)' | ./build/aura --serve

# AI Agent（需 LLM API key）
LLM_API_KEY="sk-..." LLM_BASE_URL="https://api.deepseek.com" \
  LLM_MODEL="deepseek-v4-flash" \
  python3 tests/ai_agent_edsl.py "Implement word-frequency from a CSV."
```

## 项目结构

```
src/core/         FlatAST（SoA）、StringPool、arena
src/parser/       lexer + s-expr → FlatAST
src/compiler/     IR 管线(lowering+passes+interpreter) | 树遍历求值器(fallback) | 类型检查 | EDSL

lib/std/          list math string json struct validate test (8 lib, ~300 行 Aura)
tests/            bash 回归(106) + C++ test_ir(61) + 集成 + AI Agent
docs/             roadmap · error handling · formatter · module 等
```

## 测试

```bash
python3 build.py test bash      # bash 回归（106 tests ✅）
python3 build.py test unit      # C++ 单元（61 cases ✅）
python3 build.py test integ     # 端到端管线
python3 build.py test           # 全部
```

## 设计文档

- [docs/roadmap.md](docs/roadmap.md) — 路线图
- [docs/known_issues.md](docs/known_issues.md) — 已知问题
- [docs/README.md](docs/README.md) — 完整文档索引

## License

Apache 2.0
