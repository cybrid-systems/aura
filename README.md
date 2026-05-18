# Aura

**AI-native Lisp** — C++26 实现，IR 管线默认启用，全量类型系统增强已完成。

```bash
cmake -B build && cmake --build build --target aura -j

echo '(+ 1 2)' | ./build/aura                     # → 3
echo '(apply + (list 1 2 3))' | ./build/aura       # → 6
echo '(- 5 (* 2 3))' | ./build/aura --typecheck    # type: Int, result: -1
```

## 执行管线

```
输入 → 解析(lex+parse) → 宏展开 → fallback检测
  ├── 需求值器状态(EDSL/模块/特殊形式/副作用) → 树遍历求值器 (eval_flat)
  └── 其他(算术/原语/lambda/quote/闭包/hash/pair) → IR lowering
       → TypeSpecializationPass → ComputeKind → ArityCheck → ConstFold → IRInterpreter
```

## 已实现

| 类别 | 内容 |
|------|------|
| **语言核心** | `apply`, variadic lambda, TCO, `let`/`let*`/`letrec`, `cond`, `when`/`unless` |
| **宏系统** | quasiquote, gensym, 递归展开, dotted rest param |
| **模块** | `require`/`import` 前缀注入, `export` 控制, 循环检测, 自动 lib 发现 |
| **类型系统** | 渐进类型 L6, `--strict` 模式, `forall` 多态, 增量类型缓存, Float, occurrence typing |
| **类型 IR 集成** | `IRInstruction.type_id`, 运行时类型断言(strict), Let-Poly, TypeSpecializationPass |
| **错误处理** | `try`/`catch`/`raise`/`assert`, 原语返回 error 不崩溃 |
| **数据结构** | pair/list, vector, hash table, variadic functions |
| **I/O** | `display`, `write`, `read-file`, `write-file`, `file-copy`, `file-delete` |
| **标准库** | `hash`, `combinators`, `maybe`, `csv`, `set`, `io`, `list`, `math`, `string`, `test` (10 lib) |
| **CaaS 服务** | `--serve` with `compile`/`eval`/`module`/`define`/`config` 命令 |
| **增量编译** | ArenaGroup 多模块, `reload_module` dirty-only, mmap 磁盘缓存, 函数热替换 |
| **IR 管线** | 37 opcode, const folding, compute-kind, arity check, 闭包桥接, 类型特化 pass |
| **EDSL** | `set-code`, `query:*`, `mutate:*`, `typecheck-current`, `eval-current`, `apply` |

## 项目结构

```
src/core/         FlatAST(SoA), StringPool, arena, type system     ~2.7k
src/parser/       lexer + s-expr → FlatAST                         ~1.4k
src/compiler/     IR(lowering+passes+interpreter), 树遍历求值器,
                  类型检查(typeck + let-poly + specialization),
                  CaaS 服务, EDSL                                    ~10k
lib/std/          hash, combinator, maybe, csv, set, io, list, map,
                  for-each, math, string, test                       ~650 lines
tests/            bash(106), C++ unit(74), integ(87), smoke(5),
                  benchmark(44), mutation, AI agent demo              ~6k
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

# 严格类型检查
echo '(- 5 (* 2 3))' | ./build/aura --strict                     # type error on mixed type
./build/aura --serve <<< 'config strict true\neval (+ "1" 2)'   # type error

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
- 类型安全的表达式求值（`--strict` 模式）

不适合：
- 大型面向对象系统（无 class、无 GC）
- 需要高吞吐 JSON/网络 IO（无 socket 原语）

## 下一步重点

```
立即 ─── 标准库扩充（string-split, format 扩展, hash 迭代）
  ├── 类型系统打磨（fulfilling 模式严格化、Let-Poly 常规启用）
  ├── try/catch IR 指令（消除最后一个主要 fallback）
  └── AI agent 管线实测（需 API key）
```

## 测试

```bash
python3 build.py test smoke    # 冒烟 5/5
python3 build.py test unit     # C++ 单元 74/74
python3 build.py test integ    # 端到端 87/87
python3 build.py test bash     # 回归 106/106
python3 build.py test bench    # 基准 44/44
python3 build.py test          # 全部
```

## 文档

- **[docs/tutorial.md](docs/tutorial.md)** — 10 分钟快速入门
- [docs/roadmap.md](docs/roadmap.md) — 路线图
- [docs/known_issues.md](docs/known_issues.md) — 已知问题

## License

Apache 2.0
