# Benchmarks Game 性能用例

三个 [Computer Language Benchmarks Game](https://benchmarksgame-team.pages.debian.net/benchmarksgame/) 算法的 Aura 移植，用于测量 Aura 运行时在**分配/GC 压力**、**bignum 运算**、**正则表达式**三类典型负载下的表现。输出与官方 reference 字节一致（已用独立 Python 实现交叉验证）。

| Benchmark | 测什么 | 文件 |
|-----------|--------|------|
| binary-trees | 分配 / 解分配压力（cons / vector 堆） | `tests/bench/binarytrees_{cons,array,while}.aura` |
| pidigits | 任意精度整数（GMP FFI） | `tests/bench/pidigits.aura` + `scripts/gen_pidigits.py` |
| regex-redux | 正则表达式匹配 / 替换（std::regex） | `tests/bench/regexredux.aura` + `scripts/gen_regexredux_input.py` |

## 前置

```bash
# 构建（macOS，需 Homebrew GCC ≥ 16）
cmake --preset macos && cmake --build build-mac -j
# 或直接用已存在的 build-gcc16
ls build-gcc16/aura
```

> 当前 `build-gcc16/aura` 是 2026-06-23 的预构建二进制，能运行下列所有 .aura 文件。重新构建需先解决 `import std;` 工具链问题（见 `docs/import_std_build_fix.md`）。

---

## binary-trees

构建 / 检查许多完美二叉树，压力测试分配器。参数 `B` = 最大深度（官方 correctness 用 N=10）。

**三个变体**：

| 文件 | 节点表示 | 默认栈上限 | 适用 |
|------|---------|-----------|------|
| `binarytrees_cons.aura` | 每节点一个 cons cell（忠实版） | N=7 | 小 N 演示 |
| `binarytrees_array.aura` | 每棵树一个 vector（per-tree arena pool） | N=8 | 内存更省 |
| `binarytrees_while.aura` | cons + `while` 循环 | **N=14** | **跑大 N 用这个** |

```bash
# correctness (N=6, 三个版本都通过)
B=6 ./build-gcc16/aura < tests/bench/binarytrees_cons.aura
B=6 ./build-gcc16/aura < tests/bench/binarytrees_array.aura
B=6 ./build-gcc16/aura < tests/bench/binarytrees_while.aura

# 大规模 (N=12, 只 while 版能跑通)
B=12 ./build-gcc16/aura < tests/bench/binarytrees_while.aura    # ~50s
```

**输出**（N=6，三版一致，tab 分隔，与官方 reference 一致）：
```
stretch tree of depth 7	 check: -1
128	 trees of depth 4	 check: -128
32	 trees of depth 6	 check: -32
long lived tree of depth 6	 check: -1
```

**为什么有三版**：Aura 的 named-let 循环**没有尾调用优化（TCO）**——每次迭代是真递归 C++ 调用，默认 8MB 栈在 ~450 次迭代就溢出，且 evaluator 有 `>2000` 递归深度硬限。`cons`/`array` 版用 named-let 所以撞限（N=7/8 退出）；`while` 版用 C++ 层的 `while` 原语（每轮只占 1 帧 eval 深度）绕过。真正的内存墙在 while 版 N≈16-18（cons cell 无运行期 GC 回收，OOM）。

**环境变量**：`B` = 最大深度（默认 10）。

---

## pidigits

Gibbons unbounded spigot 算法产出 π 的前 N 位，测任意精度整数。参数 `N` = 位数（官方 correctness 用 N=27）。

```bash
# 官方规模 N=27 (默认)
./build-gcc16/aura < tests/bench/pidigits.aura

# 任意位数: 用生成器重新生成
python3 scripts/gen_pidigits.py 100 > tests/bench/pidigits.aura
./build-gcc16/aura < tests/bench/pidigits.aura
```

**输出**（N=27，与官方 reference 一致）：
```
3141592653	:10
5897932384	:20
6264338   	:27
```

**依赖**：libgmp。Aura 的 `int` 是 int64 fixnum（无 bignum，会静默溢出），所以这个移植通过 `c-load`/`c-func` FFI 调 `/opt/homebrew/lib/libgmp.dylib`。N=100 验证过 bignum 真在工作（num/den/acc 此时已远超 int64）。

**为什么循环被预展开**：Aura 的 FFI closure（`c-func` 返回值）**不能从用户闭包内调用**（lambda/while/named-let body 里调 FFI 会 segfault——dispatch 在 `evaluator_eval_flat.cpp:2498` 的 `cid < func_count()` 检查不可达）。FFI 只能从顶层内联 `begin`/`if` 调用。所以 `scripts/gen_pidigits.py` 在 Python 里预演算法，把每步展开成顶层 GMP 调用序列。每个 GMP 调用仍是真调用，只是循环控制流被预展开。详见 `docs/ffi_closure_dispatch_fix.md`。

---

## regex-redux

对 FASTA DNA 序列跑 9 个正则计数 + 5 个正则替换，测 std::regex 性能。参数 `INPUT` = FASTA 文件路径（官方 correctness 规模 10000 字节）。

```bash
# 1. 生成输入
python3 scripts/gen_regexredux_input.py 10000 > /tmp/rr.fasta

# 2. 运行 (INPUT 指向文件)
INPUT=/tmp/rr.fasta ./build-gcc16/aura < tests/bench/regexredux.aura    # ~1s

# 更大规模 (线性扩展)
python3 scripts/gen_regexredux_input.py 100000 > /tmp/rr100k.fasta
INPUT=/tmp/rr100k.fasta ./build-gcc16/aura < tests/bench/regexredux.aura  # ~10s
```

**输出**（9 行 `pattern count` + 3 个裸长度，与官方格式一致）：
```
agggtaaa|tttaccct 0
[cgt]gggtaaa|tttaccc[acg] 0
a[act]ggtaaa|tttacc[agt]t 1
... (共 9 行)
10155      ← 初始输入长度
10000      ← 去掉 >header 行和换行后
4801       ← 5 次 magic replace 后
```

**为什么用文件而不是 stdin**：Aura 的 stdin **同时是程序代码源**——parser 把整个 stdin 当代码解析，`read-line` 在代码执行时已读到 EOF（返回 void）。所以 stdin 无法用作数据源，改用 `INPUT` 环境变量指定文件路径，代码里 `read-file` 读。

**count 的实现**：`regex-find` 不返回匹配位置，逐位置 substring 是 O(n²) 且撞 named-let 的 `>2000` 递归限。解法：用 `regex-replace` 把每个匹配替成单字符 `!`（DNA 是 [acgt]，不含 `!`），再数 `!`——1 次 regex 编译 + 1 次扫描，线性，用 `while` 计数。这把 10k 从 98s 降到 1s。

---

## 交叉验证参考实现

每个 benchmark 都有独立的 Python 参考实现用来验证 Aura 输出正确性：

```bash
# binarytrees: 输出格式固定，直接 diff 官方 N=6 的已知正确输出
#   stretch tree of depth 7	 check: -1
#   128	 trees of depth 4	 check: -128
#   ...

# pidigits: 与 mpmath/pi 已知位数对比
python3 -c "print('314159265358979323846264338')"  # N=27

# regex-redux: 用 Python re 实现同算法对比
python3 - <<'EOF'
import re, sys
raw = open(sys.argv[1]).read()
# ... (见 docs/benchmarks_game.md 同算法)
EOF
```

---

## 三个 benchmark 的限制总结

| Benchmark | Aura 运行时限制 | 表现 | 上限 |
|-----------|----------------|------|------|
| binarytrees | 无运行期 mark-sweep GC，cons cell 不回收 | N=12 内存 ~116MB，~2×/每+2 depth | N≈14-16 OOM |
| pidigits | FFI closure 不能从闭包内调 → 循环预展开 | N=100 输出正确，~0.1s | 无（N 受生成步数限制，约线性） |
| regex-redux | regex-find 无位置返回，regex 无预编译 | 10k ~1s，100k ~10s，线性 | 性能受 std::regex 编译开销限制 |

要突破这些限制需要运行时侧改进（见各文件头注释和 `docs/`）：
- binarytrees：运行期 GC 或 `gc:keep` 单根保护原语
- pidigits：修 FFI dispatch 的 `cid < func_count()` bug（见 `docs/ffi_closure_dispatch_fix.md`）
- regex-redux：regex 预编译 + 返回匹配位置的 `regex-find-pos` 原语

## 相关文件

- 算法说明 + Benchmarks Game 链接：各 `.aura` 文件头注释
- binarytrees 三版差异 + 限制分析：`memory/binarytrees-no-tco-recursion-limit.md`
- pidigits FFI 限制：`docs/ffi_closure_dispatch_fix.md`
- 构建问题（重新构建时）：`docs/import_std_build_fix.md`
