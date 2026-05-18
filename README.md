# Aura

**AI-native Lisp** — C++26 实现，IR 管线默认启用，跨表达式增量编译。

```bash
cmake -B build && cmake --build build --target aura -j

echo '(+ 1 2)' | ./build/aura                     # → 3
echo '(apply + (list 1 2 3))' | ./build/aura       # → 6
echo '(define h (hash "a" 1)) (hash-length h)'      # → 1
```

## 执行管线

```
输入 → 解析(lex+parse) → 宏展开 → fallback检测
  ├── 需求值器状态(EDSL/模块/特殊形式/副作用) → 树遍历求值器 (eval_flat)
  └── 其他(算术/原语/lambda/quote/闭包/hash/pair) → IR lowering → passes → IRInterpreter
```

## 已实现

| 类别 | 内容 |
|------|------|
| **语言核心** | `apply`, variadic lambda, TCO, `let`/`let*`/`letrec`, `cond`, `when`/`unless` |
| **宏系统** | quasiquote, gensym, 递归展开, dotted rest param |
| **模块** | `require`/`import` 前缀注入, `export` 控制, 循环检测, 自动 lib 发现 |
| **类型** | 渐进类型 L6, `forall`, Float, 增量 typecheck (dirty skip + type cache) |
| **错误处理** | `try`/`catch`/`raise`/`assert`, 原语返回 error 不崩溃 |
| **数据结构** | pair/list, vector, **hash table**, **variadic functions** |
| **I/O** | `display`, `write`, `read-file`, `write-file`, `file-copy`, `file-delete` |
| **标准库** | `hash`, `combinators`, `maybe`, `csv`, `set`, `io`, `list`, `math`, `string` (10 lib) |
| **CaaS 服务** | `--serve` with `compile`/`eval`/`module`/`define` 命令 |
| **增量编译** | ArenaGroup 多模块, `reload_module` dirty-only, mmap 磁盘缓存 |
| **IR 管线** | 37 opcode, const folding, compute-kind, arity check, 闭包桥接 |
| **EDSL** | `set-code`, `query:*`, `mutate:*`, `typecheck-current`, `eval-current`, `apply` |

## 项目结构

```
src/core/         FlatAST(SoA), StringPool, arena, type system   ~2.5k
src/parser/       lexer + s-expr → FlatAST                       ~1.2k
src/compiler/     IR(lowering+passes+interpreter), 树遍历求值器  ~10k
                  类型检查, CaaS 服务, EDSL
lib/std/          hash, combinator, maybe, csv, set, io, list, map, for-each
                  math, string, test                              ~400 lines
tests/            bash(106), C++ unit(61), integ(87), smoke(5)
```

## 快速体验

```bash
# hash 表
printf '(require std/hash)(define h (hash "a" 1))(hash-set h "b" 2)\n(hash->list h)\n' | ./build/aura

# variadic lambda
echo '((lambda xs (length xs)) 1 2 3)' | ./build/aura           # → 3

# apply
echo '(apply + (list 1 2 3 4 5))' | ./build/aura                # → 15

# 组合器
printf '(require std/combinators)((compose (lambda (x) (+ x 1))\n(lambda (x) (* x 2))) 5)\n' | ./build/aura

# CSV 解析
printf '(require std/csv)(csv-parse "a,b,c\\n1,2,3")\n' | ./build/aura

# CaaS serve
printf '(module compile "demo" "(define (f x) (+ x 1))")(module eval demo "(f 41)")\n' | ./build/aura --serve

# 管道多表达式
printf '(define h (hash "a" 1))\n(hash-set! h "b" 2)\n(hash-length h)\n' | ./build/aura
```

## 极限：当前能做到什么规模

Aura 当前适用于：
- 单文件 500 行以内的脚本
- 哈希表/CSV 数据处理
- 函数式组合（compose/curry/filter/map/foldl）
- EDSL 驱动的 AI agent 代码变换
- 通过 `--serve` 做 CaaS 增量服务

不适合：
- 大型面向对象系统（无 class、无 GC）
- 需要高吞吐 JSON/网络 IO（无 socket 原语）
- 需要异常控制的业务逻辑（try/catch 尚未 IR 化）

## 下一步重点

```
立即 ─── 标准库补齐（char predicates, string-copy, format）
  ├── 测试覆盖（hash/variadic/apply 集成测试）
  ├── AI agent 管线实测（需 API key）
  └── 语言文档
```

## 测试

```bash
python3 build.py test smoke    # 冒烟 5/5
python3 build.py test unit     # C++ 单元（含预存 4 个比较器返回值偏离）
python3 build.py test integ    # 端到端 67/67
python3 build.py test          # 全部
```

## 文档

- **[docs/tutorial.md](docs/tutorial.md)** — 10 分钟快速入门
- [docs/roadmap.md](docs/roadmap.md) — 路线图
- [docs/known_issues.md](docs/known_issues.md) — 已知问题

## License

Apache 2.0
