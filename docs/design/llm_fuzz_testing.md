# LLM-Driven Fuzz Testing — 设计文档

> 让 LLM 当 QA：生成"按语义应该能跑"的代码 → 编译器验证 → 发现隐藏缺陷。

---

## 现状

EDSL benchmark 的 `--intend` 模式已经隐含了这个循环：

```
LLM 写代码 → set-code + typecheck + eval → 通过 ❓ 失败
                                              ↓
                                        编译器 bug？还是 LLM 写错了？
```

当前问题：**失败时我们不知道是谁的锅**。`memoize` 挂了一周，所有人以为是 LLM 写不对，结果是编译器闭包捕获有 bug。

## 设计目标

1. **自动区分** 编译器 bug vs LLM 写错
2. **自动提取最小复现用例** — 抽取出独立于 LLM 的 .aura 文件
3. **回归守卫** — 修复后自动加入测试套件，确保不再复发
4. **覆盖率追踪** — 跟踪 LLM 触及了哪些编译器代码路径

---

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│  LLM 生成代码                                               │
│  (benchmark --intend)                                       │
│       ↓                                                    │
│  ┌──────────────────── 验证器 ───────────────────────┐     │
│  │                                                    │     │
│  │  Phase 1: set-code(code) → 语法验证                 │     │
│  │   成功 → Phase 2                                    │     │
│  │   失败 → "syntax-error" (可能是 LLM 写错)            │     │
│  │                                                    │     │
│  │  Phase 2: typecheck-current → 类型验证               │     │
│  │   成功 → Phase 3                                    │     │
│  │   失败 → "type-error" (可能是 LLM 写错)              │     │
│  │                                                    │     │
│  │  Phase 3: eval-current → 运行时验证                   │     │
│  │   成功 → 输出匹配期望值 → PASS                       │     │
│  │   失败 → 检查是否是 Aura 运行时崩溃                    │     │
│  │       ┌ 内部错误 (invalid node id / SIGSEGV / hang) │     │
│  │       │ → 自动生成最小复现 → 编译器 BUG             │     │
│  │       └ 正常 Aura 错误 (div by zero / unbound var)  │     │
│  │         → 可能是 LLM 写错                           │     │
│  └──────────────────────────────────────────────────────┘
│       ↓
│  ┌────────── 分类器 ──────────┐
│  │                            │
│  │  SIGSEGV / SIGFPE / SIGABRT│ → 编译器崩溃 🚨
│  │  "internal error"          │ → 编译器内部错误 🚨
│  │  "unbound variable"        │ → LLM 写错（95%）/ 作用域 bug（5%）
│  │  "type mismatch"           │ → LLM 写错（99%）
│  │  "bad-code" (set-code 失败)│ → LLM 写错 / 语法不兼容
│  │  timeout / hang            │ → 编译器死循环 🚨 或 LLM 生成了无限递归
│  └────────────────────────────┘
└─────────────────────────────────────────────────────────────┘
```

---

## Phase 1: 编译器崩溃检测（~1h）

### 信号分类

在 benchmark runner 中检测子进程的非正常退出：

| 退出信号 | 含义 | 响应 |
|----------|------|------|
| exit code -6 (SIGABRT) | assert 失败 | 🚨 编译器 bug |
| exit code -8 (SIGFPE) | 除零 / 整数溢出 | 🚨 编译器 bug（已有 err_div_zero 用例） |
| exit code -11 (SIGSEGV) | 空指针 / 释放后使用 | 🚨 编译器 bug |
| exit code -15 (SIGTERM) | 被外部 kill | 忽略 |
| 超时 (timeout) | 死循环 / 无限递归 | 🚨 危险 |
| "internal error" | 内部错误消息 | 🚨 编译器 bug |
| 正常退出但输出为空 | 不明故障 | ⚠️ 需人工检查 |

### 最小复现提取

当检测到编译器 bug 时，自动保存复现文件：

```bash
# 自动生成
tests/reproducers/2026-05-20_memoize_closure_dangling.aura

;; file content — 最小复现用例
;; compiler-bug: closure capture with let-created env
;; fixes: 953d6c4
;; discovered-by: memoize eds_benchmark task
;; llm-prompt: "Write (memoize fn) that wraps..."

(define (memoize fn)
  (let ((cache (hash)))
    (lambda (x) (fn x))))

(display ((memoize (lambda (z) (* z 2))) 5))
;; expected: 10
;; actual (before fix): empty output
```

### 实现

在 `build.py` 增加 `test_fuzz` 套件，跑 `--intend` 模式并捕获崩溃：

```python
def test_fuzz():
    """运行 LLM fuzz 测试，检测编译器 bug"""
    results = {"crashes": [], "timeouts": [], "regressions": []}
    env = os.environ.copy()
    env["LLM_API_KEY"] = read_key()
    
    for task in TASKS:
        code = build_intend_aura(task)
        try:
            r = subprocess.run(
                [AURA], input=code, capture_output=True,
                timeout=30, env=env)
        except subprocess.TimeoutExpired:
            results["timeouts"].append(task.name)
            save_reproducer(task, "timeout")
            continue
        
        if r.returncode < 0:  # killed by signal
            sig = -r.returncode
            if sig in (6, 8, 11):  # SIGABRT, SIGFPE, SIGSEGV
                results["crashes"].append((task.name, sig))
                save_reproducer(task, f"signal-{sig}")
        
        if "internal error" in (r.stderr or ""):
            results["crashes"].append((task.name, "internal"))
            save_reproducer(task, "internal")
    
    return results
```

---

## Phase 2: 回归守卫（~1h）

每次 `test_fuzz` 发现的 bug 修复后，把最小复用例加入回归测试套件：

```python
REGRESSION_TESTS = [
    # (name, aura-code, expected-output, issue-ref)
    ("closure-let-dangling",
     "(define (f fn) (let ((x 1)) (lambda (y) (fn y))))"
     "(display ((f (lambda (z) (* z 2))) 5))",
     "10",
     "fix: 953d6c4"),
]
```

这些测试在 `build.py test` 的 `test_regression` 套件中独立运行，不依赖 LLM：

```bash
# 每次都跑
python3 build.py test_regression

# 验证回归测试全部通过
All 12 regression tests passed ✅
```

---

## Phase 3: 覆盖率引导（~2h，远期）

### 代码路径追踪

利用 Aura 编译器的诊断标记，追踪 LLM 生成代码触及的编译器路径：

```
LLM 代码 → set-code (parse + AST build)      ✅
         → typecheck (约束求解 + 类型推断)     ✅  
         → eval (tree-walker 路径)            ✅
         → JIT (LLVM IR 生成 + 优化 + 执行)    ❌ (未触及)
```

输出覆盖率热图：

```
Compiler coverage by LLM fuzz (47 tasks):
  parser:        ████████████████ 95%
  typechecker:   ████████████████ 92%
  tree-walker:   ████████████████ 88%
  JIT:           ████████         42%  ← LLM 很少触发 JIT 路径
  FFI:           ████             18%
  macro expander: ████            15%
```

可以指导添加针对低覆盖率区域的 benchmark 任务。

---

## 实施计划

### Phase 1（~1h）
- `build.py` 增加 `test_fuzz` 命令
- signal/timeout/internal-error 检测
- 自动保存 `tests/reproducers/` 复现文件
- （其实 `err_div_zero` 已经证明了这种模式的可行性）

### Phase 2（~1h）
- 建立 `tests/regression.aura` — 已知修复的编译器 bug 的最小复现
- `build.py test_regression` 套件
- 每次修 bug 后手动添加（未来可自动）

### Phase 3（远期）
- 编译器注入覆盖率标记
- 按 EDSL 路径分区统计
- 指导 benchmark 任务设计

---

## 对比：传统 fuzzing vs LLM fuzzing

| 维度 | 传统 fuzzer (AFL/libFuzzer) | LLM fuzzer (本方案) |
|------|---------------------------|-------------------|
| 输入生成 | 随机变异字节流 | LLM 按语义生成 Aura 代码 |
| 覆盖率 | 边覆盖 (edge coverage) | 隐式场景覆盖 |
| 发现类型 | 内存安全、崩溃 | 语义缺陷、作用域 bug、类型系统盲区 |
| 最小化 | 自动 (afl-tmin) | 半自动（从 benchmark 提取） |
| 误报率 | 低 | 中（需区分 LLM 写错 vs 编译器 bug） |
| 维护成本 | 低 | 低（复用 benchmark 任务） |

LLM fuzzer 的优势在于发现**语义级** bug——不是"除零崩溃"这种被 fuzzer 轻松找到的，而是"闭包捕获变量在特定嵌套模式下失效"这种需要语言语义理解才能构造的用例。

---

## 已有的成功案例

| bug | 发现方式 | 分类 |
|-----|---------|------|
| `OpDiv` SIGFPE x86_64 | benchmark `--fix` 模式 | 编译器崩溃 ✅ |
| Division UB (三元表达式) | benchmark `--intend` | 编译器未定义行为 ✅ |
| `let` 环境悬空指针 | memoize 任务 `--intend` | 编译器语义缺陷 ✅ |
| `json-get-string` 截断 | benchmark `--fix` 模式 | 原语实现 bug ✅ |
