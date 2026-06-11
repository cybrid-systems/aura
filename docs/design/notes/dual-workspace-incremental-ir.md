# Dual Workspace + Incremental IR for EDSL

> **注意（历史文档）**：本文档记录了 workspace 分层与 IR 增量编译的早期联合设计。当前 Workspace P0（COW、lock、per-workspace budget）已在 `design/core/workspace_layering.md` 完整实装；IR 增量已集成到 `eval()`（见 `design/compilation/ir_pipeline.md`）。许多早期方案已被简化或演进。

> **Status:** 设计中
> **基于:** Aura commit `99c5455` (C++26 contracts 已合并)
> **关联:** Issue #98 Action 1 (workspaces), Issue #32b (incremental eval)
> **作者:** Ani
> **目标阅读:** 维护者 + 后续实现者

## TL;DR

把 `workspace_flat_` 拆成 `current_flat_` 和 `workspace_flat_` 两块, 解决 set-code 跟 IR 路径互相踩的问题. 顺手把 EDSL (`set-code` + `mutate:*` + `eval-current`) 接入 IR cache, 让 EDSL 写出的代码享受 type-specialize / const-fold / JIT. 全程**增量**: 改 1 字节只重 lower 1 个 define, 其余命中.

## 1. 背景

### 1.1 当前两个体系互相踩

Aura 运行时有两个 eval 路径, 共享一个 `workspace_flat_` 字段:

| 路径 | 入口 | 用途 | workspace_flat_ 来源 |
|---|---|---|---|
| IR/JIT (新) | `eval_ir()` / `exec_jit()` | stdin 脚本, `(+ 1 2)` 走这条 | **从不设**, 始终 null (新分配的 arena flat 没人接进 workspace_flat_) |
| EDSL (老) | `set-code` 原语 | `(set-code "...")` 后 mutate/query/eval-current | 由 `set-code` 分配并设入 |

后果:

```aura
;; Bug: IR 路径下, current-source 看不到脚本自己的源
$ echo '(+ 1 2) (display (current-source))' | aura
;; 实际: 返回 "" (workspace_flat_ 是 null)
;; 期望: 返回 "(+ 1 2) (display (current-source))"

;; Bug: EDSL 路径下, workspace:conflicts-with 看不到脚本自己的 defines
;; (除非显式 (workspace:create ...) 创建一个 child 才能用)
$ cat > /tmp/x.aura <<'EOF'
(define foo 42)
(define cid (workspace:create "c"))
(workspace:conflicts-with cid)
EOF
$ aura < /tmp/x.aura
;; 实际: ()
;; 期望: (foo)
```

### 1.2 EDSL 跑得慢

`eval-current` (evaluator_impl.cpp:4655) 直接调 `eval_flat` 走 tree-walker, **绕过** `ir_cache_`. 一个 EDSL 用户写的函数跟直接 stdin 写的同一个函数, 性能差一个数量级 (没 type-specialize, 没 const-fold, 没 JIT).

```aura
;; 这个 (define f ...) 走 IR, 快
echo '(define (f x) (* x x)) (f 5)' | aura  ; ~5ms

;; 同样代码, 通过 EDSL set-code 走, 慢
echo '(set-code "(define (f x) (* x x))") (eval-current) (f 5)' | aura  ; ~30ms
```

### 1.3 已有的 IR cache 基础设施

`CompilerService` 已经有 `cache_define` (service.ixx:2340) 走 IR 路径, 但只对 **直接 stdin 的 define** 生效:

- `ir_cache_`: `name -> vector<IRFunction>`  (key: 函数名, value: 该函数 lower 出的 IR)
- `ir_cache_bridge_`: 同步的 closure bridge 数据
- `ir_cache_strings_`: 同步的 string pool
- `function_sources_`: `name -> source`  (用于 arena reset 后从 source 重建)
- `dep_graph_`: 依赖图, 用于 mutuate 后级联失效

`set-code` 触发的 define 走的是 `cache_define` 也写进 `ir_cache_` (service.ixx:2592). 但 `eval-current` **没用 `ir_cache_`**, 它直接 `eval_flat`. 所以 IR 编了也没用.

## 2. 设计目标

按优先级:

1. **拆分 `current_flat_` / `workspace_flat_`**, 结束两边互相踩
2. **`current-source` 默认读 `current_flat_`** (per-eval ephemeral), `set-code` 出来的源走 `workspace_flat_` (persistent)
3. **`eval-current` 走 IR cache**, 不再 tree-walker
4. **增量失效**: mutate 一个 define, 只重 lower 那一个 + 下游依赖
5. **JIT 接入** (后续): `(eval-current :jit)` 触发 LLVM ORC 编译

## 3. 架构

### 3.1 Dual-Workspace 拆分

`Evaluator` 加 4 个字段, 加 3 个 setter:

```cpp
// evaluator.ixx
class Evaluator {
    // ── existing ──
    ast::FlatAST* current_flat_ = nullptr;     // for mutate:* (eval 期间, mutation 目标)
    ast::StringPool* current_pool_ = nullptr;
    ast::FlatAST* workspace_flat_ = nullptr;   // ← 重命名语义: "用户 set-code 出来的"
    ast::StringPool* workspace_pool_ = nullptr;

    // ── new ──
    ast::FlatAST* current_flat_ = nullptr;     // per-eval, eval 路径设
    ast::StringPool* current_pool_ = nullptr;  //  (独立于 current_ast_ 的 current_flat_ 名冲突,
                                                //   见 3.2 关于命名的说明)
    ast::FlatAST* current_ir_flat_ = nullptr;  // snapshot of flat for which current_ir_ was lowered
    ast::ir::IRModule current_ir_;             // 增量化: current_flat_ 改了 → 全失效
    std::size_t current_ir_hash_ = 0;          // (avoid resnap on no-change)
};
```

`CompilerService::eval()` / `eval_ir()` / `exec_jit()` 在 parse 之后立即:

```cpp
evaluator_.set_workspace_flat(flat_ptr);   // legacy, mutate:* 继续用
evaluator_.set_workspace_pool(pool_ptr);
evaluator_.set_current_flat(flat_ptr);     // NEW, source-reading 默认走这个
evaluator_.set_current_pool(pool_ptr);
```

#### 3.2 字段命名说明

`current_flat_` 在 `Evaluator` 已有同名字段 (mutation target). 为避免混淆, 命名分两种方案:

| 方案 | 字段名 | 备注 |
|---|---|---|
| **A. 改旧名** (推荐) | `current_flat_` → `mutate_target_flat_` | 改 4-5 处引用, 长期清晰 |
| B. 新字段用不同名 | 新增 `eval_flat_` / `eval_pool_` | 不动旧字段, 但 reader 看 evaluator.ixx 会困惑 |

推荐 A. 改动很小 (grep `current_flat_` 在 evaluator_impl.cpp 共 ~10 处引用, 改 5 分钟).

### 3.3 原语读哪个 workspace

| 原语 | 读 | 备注 |
|---|---|---|
| `current-source` | **`current_flat_`** (新默认) | 看到的是当前正在跑的代码 |
| `current-source :workspace` | `workspace_flat_` | 可选参数, 显式看 EDSL workspace |
| `current-source :all` | both, 拼成 list | 调试用 |
| `query:find` / `query:node-type` | `workspace_flat_` (不变) | EDSL 查询, 语义就是查 EDSL workspace |
| `workspace:conflicts-with` | `workspace_flat_->tree->nodes_[0].flat` (不变) | 多 workspace 树, 跟单 workspace 拆开 |
| `mutate:*` | `current_flat_` (不变, 用旧 current_flat_ = mutate target) | mutate 永远改当前 eval 的 AST |
| `eval-current` | `workspace_flat_` (不变) | EDSL 视角, "eval 一下我 EDSL 写的东西" |

### 3.4 IR cache 升级

`CompilerService` 已有 `ir_cache_`. 新增 **per-source 哈希 + 失效标记**:

```cpp
// service.ixx
struct IRCacheEntry {
    std::string source;              // canonical form (unparsed for hash stability)
    std::size_t source_hash = 0;     // FNV-1a of source
    std::vector<aura::ir::IRFunction> irs;
    std::vector<aura::ir::ClosureBridgeData> bridges;
    std::vector<std::string> strings;
    std::size_t mutation_count = 0;  // snapshot at lower time
    bool dirty = true;
    std::unordered_set<std::string> depends_on;  // free vars captured by this define
};

std::unordered_map<std::string, IRCacheEntry> ir_cache_v2_;  // replaces ir_cache_ etc
```

`cache_define` 改造:

```cpp
EvalResult cache_define_v2(std::string_view source, ast::FlatAST& flat,
                            ast::StringPool& pool, ast::NodeId expanded_root,
                            const std::string& name_str) {
    auto canonical = canonicalize_define(name_str, flat, pool, expanded_root);
    auto hash = fnv1a(canonical);

    auto& entry = ir_cache_v2_[name_str];
    bool is_redefine = !entry.source.empty();

    // Incremental: hit if same source, not dirty, deps still valid
    if (is_redefine && entry.source_hash == hash && !entry.dirty) {
        if (all_deps_valid(entry.depends_on, ir_cache_v2_)) {
            return cached_result;  // skip lowering entirely
        }
    }

    // (slow path: lower and update entry)
    // ... existing lowering code ...
    entry.source = canonical;
    entry.source_hash = hash;
    entry.dirty = false;
    entry.mutation_count = flat.mutation_count();
    entry.depends_on = scan_free_vars(flat, pool, expanded_root, name_str);
    return tree_walker_result;
}
```

### 3.5 失效策略 (两层)

**第一层: 粗粒度** (简单, 够用)

- `(mutate:rebind NAME "...")` → 把 `ir_cache_v2_[NAME].dirty = true`
- `(set-code "...")` 改变了整个 workspace → 全部 dirty
- 失效只 lazy: 下次 `eval-current` 重新 lower dirty entries

**第二层: 细粒度** (后续优化, 不在 Phase 1)

- 维护 `dep_graph_` (已有), mutate `f` 时把所有 `g in deps_of_f` 也标 dirty
- 跨 define 失效: `g` 引用 `f`, mutate `f` 后 `g` 也要 re-lower

**Phase 1 用粗粒度**, 性能已经够好. 细粒度做 P2 优化.

### 3.6 eval-current 改造

```cpp
// evaluator_impl.cpp:4655 — primitives_.add("eval-current", ...)
primitives_.add("eval-current", [this, mev](const auto& a) {
    if (!workspace_flat_ || !workspace_pool_)
        return make_void();

    // Iterate all (define ...) in workspace root, ensure each is in IR cache
    std::vector<std::string> defines = scan_top_level_defines(*workspace_flat_, *workspace_pool_,
                                                              workspace_flat_->root);

    for (auto& name : defines) {
        // cache_define_v2 takes care of incremental lowering
        cache_define_v2(workspace_source_, *workspace_flat_, *workspace_pool_,
                        find_define_node(*workspace_flat_, name), name);
    }

    // Now build IRModule from cache and run
    aura::ir::IRModule mod;
    for (auto& [name, entry] : ir_cache_v2_) {
        for (auto& func : entry.irs)
            mod.functions.push_back(func);
    }
    // ... link, set entry_function, run IRInterpreter ...

    if (a.size() == 1 && is_keyword(a[0]) && keyword_eq(a[0], "jit")) {
        // (eval-current :jit) — compile to machine code
        return run_jit(mod);
    }
    return ir_interpreter_.execute();
});
```

## 4. 实施阶段

### Phase 1: Dual-workspace 拆分 (基础)

**目标**: 解决 set-code / IR 路径互相踩. 不动 IR cache.

**改动**:
1. `evaluator.ixx`: 加 `current_flat_` / `current_pool_` 字段, 3 个 setter
2. `service.ixx`: `eval()` / `eval_ir()` / `exec_jit()` 三个路径在 parse 之后设 `current_flat_/pool_` (不再 set `workspace_flat_/pool_`)
3. `evaluator_impl.cpp`: `current-source` 改读 `current_flat_` (新), 加可选 `:workspace` keyword 走 `workspace_flat_`
4. 测试: 新增 `tests/dual_workspace_test.aura`, 验证两个原语在 stdin 路径下能看到脚本自己的源

**不变量**:
- 旧 `current_flat_` 字段保留 (mutate target), 新加的 `current_flat_` 是 per-eval source
- 等待 — **命名冲突**. 见 3.2, 推荐把旧 `current_flat_` 改成 `mutate_target_flat_`

**预计**: 30 行 + 5 处调用点 + 1-2 个测试, 半天.

### Phase 2: IR cache 升级 (核心)

**目标**: EDSL 走 IR, 增量.

**改动**:
1. `service.ixx`: 新增 `IRCacheEntry` struct, 替换 `ir_cache_` 为 `ir_cache_v2_`
2. `cache_define` → `cache_define_v2` (含 hash 比对 + dirty 标记)
3. `evaluator_impl.cpp:eval-current` 改造 (3.6 节)
4. 失效钩子: `mutate:rebind` 标 dirty
5. 测试:
   - 第一次 `eval-current` 全 lower, 计时
   - 第二次 `eval-current` 命中 cache, 应该 < 第一次的 20% 时间
   - `(mutate:rebind "f" "...")` 后再 `eval-current`, 只 `f` 重 lower
   - `current-source` 行为 (dual-workspace 测试继续过)

**预计**: 150 行, 1-2 天, 需要写几个 ctest 验证 lower 次数.

### Phase 3: 细粒度失效 (优化)

**目标**: mutate 不波及无关 define.

**改动**:
1. `cache_define_v2` 算 `depends_on` (lambda body 的 free vars)
2. mutate `f` 时遍历 `dep_graph_[f]` 反向, 把所有 `g in dependents_of_f` 也标 dirty
3. 测试: mutate 1 个 define, 验证其他 N-1 个仍命中 cache

**预计**: 50 行, 半天.

### Phase 4: JIT 集成 (可选, 后续)

**目标**: `(eval-current :jit)` 编机器码.

**改动**:
1. `aura_jit.cpp` 已有 JIT, 接入 IR cache
2. 新增 `:jit` keyword 解析
3. 测试: `eval-current` 跟 `eval-current :jit` 结果一致, 性能提升 (hot loop 测试)

**预计**: 半天, 等 Phase 2 稳定后做.

## 5. 测试策略

### 5.1 新增测试 (in `tests/`)

```
tests/dual_workspace_test.aura     # Phase 1
  - current-source 默认读 stdin 脚本源
  - current-source :workspace 读 set-code 的源
  - (set-code "...") 后 (current-source) 返回 "(set-code \"...\")" (set-code 之前的 SCRIPT 视角)
  - (set-code "...") 后 (current-source :workspace) 返回 "(define...)"

tests/edsl_ir_cache_test.aura      # Phase 2
  - (set-code "(define (f x) (* x x))") (eval-current)  第一次: 走 IR
  - (f 5) → 25
  - (define (g y) (f y)) (eval-current)  增量: f 命中, 只 lower g
  - (g 3) → 25 (g 用了 f, f 命中)
  - (mutate:rebind "f" "(lambda (x) (+ x 1))") (eval-current)  失效 f, 重 lower
  - (g 3) → 4 (g 重新走 IR, 用新 f)

tests/edsl_ir_perf_test.cpp        # ctest, 性能回归
  - 计时: 第一次 eval-current 走 IR 完整 lower
  - 计时: 第二次 eval-current 命中 cache, 时间 < 第一次 20%
  - 计时: mutate + 第二次 eval-current, 只重 lower 1 个 define
```

### 5.2 现有测试不能破

| 测试 | 期望 |
|---|---|
| `edsl:find` (5) | 必须保持. Phase 1 的 set-code 不动 (仍直接设 workspace_flat_) |
| `edsl:node-type` (5) | 同上 |
| 212 个 .aura 测试 | 全过 |
| 37 个 ctest | 全过 |

### 5.3 UB 风险

Phase 1 实施时, `set_workspace_flat` 跟之前"覆盖到底被谁覆盖"的 debug 经验表明, IR 解释器可能有 workspace_flat_ 相关的内存别名. 实施时必须:

- Debug + Release 两个 build 都过
- 加 ASan build 跑测试 (`cmake -DCMAKE_BUILD_TYPE=Debug -DAURA_SAN=1`)
- 任何指针 double-set 立刻爆

## 6. 风险与权衡

| 风险 | 缓解 |
|---|---|
| 字段重命名 `current_flat_` 破坏 ABI (模块接口) | 模块名 + 函数签名不动, 字段是私有; 改完检查所有 import 处 |
| IR cache 内存增长 (累积所有 set-code 过的 define) | `function_sources_` 已有, 配套 `ir_cache_` 在 unload 时清; `set-code` 重新分配触发 `update_workspace_define_cache` 清理旧 entries |
| Mutate invalidate 误伤 (P1 粗粒度) | "全清 + lazy re-lower" 也只 dirty entries, 不是真清空, 第二次 eval-current 仍然快 |
| Eval-current 走 IR 后, 旧 tree-walker 路径没人用, 退化 | 保留 tree-walker 作为 fallback (`(eval-current :tree)` 显式触发); IR 失败时回退 |
| user_bindings_ 跟 IR 闭包 binding 不一致 (旧有) | 这是已知问题 (不属本次), 单独 follow-up |

## 7. 不在本次 scope

- 跨进程 / 跨机器的 IR cache 共享 (那是 disk cache, 已有)
- LLVM ORC 之外的 JIT 方案 (Cranelift, MLIR, etc.)
- 反向: 把 IR 改回 AST 后给 query:* 用 (太重, 不值得)
- type-specialize pass 的 EDSL 适配 (应该自动适配, 但需要 Phase 2 后再验证)

## 8. 关联文档

- `docs/design/code_evolution_pipeline.md` — EDSL 总览
- `docs/design/compile_time_reflection.md` — query:* / mutate:* 起源
- `docs/design/history/closings/83-closing.md` — C++26 contracts (pre/post, 跟 EDSL 无关但同时间)
- `docs/design/history/closings/98-closing.md` — workspaces (EDSL 工作区管理)

## 9. 实施 checklist

```
[ ] Phase 1: Dual-workspace 拆分
    [ ] evaluator.ixx: 字段命名, 3 个 setter
    [ ] service.ixx: 3 个 eval 路径设 current_flat_/pool_
    [ ] evaluator_impl.cpp: current-source 改默认 + 加 :workspace keyword
    [ ] tests/dual_workspace_test.aura
    [ ] 212 + 37 测试全过
    [ ] Debug + Release 两 build 都过
    [ ] commit + push

[ ] Phase 2: IR cache 升级
    [ ] service.ixx: IRCacheEntry struct, ir_cache_v2_
    [ ] service.ixx: cache_define_v2 (hash + dirty)
    [ ] evaluator_impl.cpp: eval-current 走 IR cache
    [ ] evaluator_impl.cpp: mutate:rebind 标 dirty
    [ ] tests/edsl_ir_cache_test.aura
    [ ] tests/edsl_ir_perf_test.cpp (ctest)
    [ ] 性能 baseline 记录
    [ ] commit + push

[ ] Phase 3: 细粒度失效 (后续, P2 优化)
    [ ] depends_on 扫描
    [ ] dep_graph_ 反向遍历
    [ ] 测试: mutate 后其他 N-1 命中
    [ ] commit + push

[ ] Phase 4: JIT 集成 (后续)
    [ ] :jit keyword 解析
    [ ] aura_jit.cpp 接入 IR cache
    [ ] 测试: eval-current vs eval-current :jit 结果一致
    [ ] commit + push
```

## 10. 总结

| 改动 | 行数估计 | 时间 | 风险 |
|---|---|---|---|
| Phase 1 (dual-workspace) | ~30 + 5 处调用 | 0.5 天 | 中 (字段重命名) |
| Phase 2 (IR cache) | ~150 | 1-2 天 | 中-高 (IR 集成复杂) |
| Phase 3 (细粒度失效) | ~50 | 0.5 天 | 低 (build on Phase 2) |
| Phase 4 (JIT) | ~30 | 0.5 天 | 中 (LLVM ORC) |

**建议先做 Phase 1**, 解决 set-code / IR 互相踩的架构问题, 是后续的 foundation. Phase 2 单独开干, 跟 Phase 1 commit 分开 (避免一个 PR 太大).
