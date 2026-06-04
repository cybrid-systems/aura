# 双 Arena 内存管理设计

## 问题

Aura 使用单一 `ASTArena`（pmr monotonic bump allocator）管理所有内存：

- 模块 FlatAST（永久）
- Workspace FlatAST（set-code 复用）
- 闭包体 cl_flat / cl_pool（每次 lambda 创建分配）
- 捕获环境 Env（每次 copy_env 分配）
- 模式匹配 AST（每次 match 分配）

`ASTArena::reset()` 是 O(1) 释放全部内存的唯一方式，但它会同时销毁**全部** arena 内对象——包括模块和 while 循环闭包，所以不能在 benchmark 的 task 间隙安全调用。

当前 `gc-heap` 只能清理堆 vector（string_heap_、pairs_、hash_heap_ 等），但 `closures_` 的 cl_flat/cl_pool 指针指向 arena 内存，无法释放。

**后果**：每个 benchmark task 产生 ~3 个临时闭包 → `closures_` +3、`cells_` +3、arena 新增 ~300B。405 轮后累计 ~500KB，线性增长。

## 设计

### 核心思路

将持久数据（模块、while 循环闭包）和临时数据（task 闭包、临时 env）分配在不同的 arena 中。Task 结束后只重置临时 arena。

```
┌─────────────────────────────────┐
│        CompilerService          │
│                                 │
│  persistent_arena_  (原 arena_) │──→ 模块 FlatAST
│                                     workspace FlatAST (复用)
│                                     while 循环闭包体
│                                     模块 Env
│                                 │
│  temp_arena_        (新增)      │──→ task 闭包体 (cl_flat/cl_pool)
│                                     task copy_env 结果
│                                     eval-expr 临时 AST
│                                     模式匹配 AST
│                                 │
│  ir_cache_          (共享)      │──→ IR 缓存（与 arena 无关）
└─────────────────────────────────┘
```

### 文件改动

#### 1. `evaluator.ixx`

**Closure 结构体加 owner_arena 字段：**

```cpp
export struct Closure {
    std::string name = "";
    std::vector<std::string> params;
    ast::FlatAST* flat = nullptr;
    ast::StringPool* pool = nullptr;
    ast::NodeId body_id = ast::NULL_NODE;
    const Env* env = nullptr;
    bool dotted = false;
    ast::ASTArena* owner_arena = nullptr;  // 新增
};
```

**Evaluator 加 temp arena + 上下文标志：**

```cpp
// — 新增成员
ast::ASTArena* temp_arena_ = nullptr;
// Issue #68: depth counter (was bool) for nested intend support.
// 0 = not in any intend; >0 = inside N nested intends. Each intend
// saves the current depth, sets it to depth+1, and restores on exit.
// The allocation rule (line below) still works because 0 is falsy
// and non-zero is truthy.
int in_task_context_ = 0;
```

#### 2. `evaluator_impl.cpp`

**闭包创建时分配 arena 选择（~line 9505）：**

```cpp
// 当前：
auto cl_alloc = arena_->allocator();
auto* cl_flat = arena_->create<aura::ast::FlatAST>(cl_alloc);
auto* cl_pool = arena_->create<aura::ast::StringPool>(cl_alloc);

// 改为：
auto* target = (temp_arena_ && in_task_context_) ? temp_arena_ : arena_;
auto cl_alloc = target->allocator();
auto* cl_flat = target->create<aura::ast::FlatAST>(cl_alloc);
auto* cl_pool = target->create<aura::ast::StringPool>(cl_alloc);
// ...
closures_[cid] = Closure{
    /*...*/, /*env*/ copy_env(env, target), /*dotted*/false, target
};
```

**copy_env 分派 arena：**

```cpp
// 当前：
Env* Evaluator::copy_env(const Env& e) {
    return arena_ ? arena_->create<Env>(e) : nullptr;
}

// 改为：
Env* Evaluator::copy_env(const Env& e, ast::ASTArena* target) {
    auto* ar = target ? target : arena_;
    return ar ? ar->create<Env>(e) : nullptr;
}
```

**gc-temp 原语：**

```cpp
primitives_.add("gc-temp", [this](const auto&) -> EvalValue {
    if (!temp_arena_) return types::make_bool(false);

    // 1. 擦除所有 body 在 temp_arena_ 里的闭包 —— 安全
    //    因为 temp arena 里的闭包不可能是 while 循环的 predicate/body
    //    （那些由 in_task_context_=false 时创建，在 persistent arena）
    for (auto it = closures_.begin(); it != closures_.end(); ) {
        if (it->second.owner_arena == temp_arena_)
            it = closures_.erase(it);
        else
            ++it;
    }

    // 2. 重置临时 arena（O(1)）
    temp_arena_->reset();

    // 3. 清理堆 vector（同 gc-heap）
    string_heap_.clear();
    string_heap_.shrink_to_fit();
    pairs_.clear();
    pairs_.shrink_to_fit();
    error_values_.clear();
    error_values_.shrink_to_fit();
    hash_heap_.clear();
    hash_heap_.shrink_to_fit();
    vector_heap_.clear();
    vector_heap_.shrink_to_fit();
    opaque_heap_.clear();
    opaque_heap_.shrink_to_fit();

    return types::make_bool(true);
});
```

**set-code workspace 不动**：workspace FlatAST 分配在 `persistent_arena_`，通过 clear() 复用，不新增分配。

**in_task_context_ 设置：**

```cpp
// intend 原语入口处（~line 10872）：
// Issue #68: depth-counter form, supports nested intend.
int saved_depth = in_task_context_;
in_task_context_ = saved_depth + 1;
auto restore = [&] { in_task_context_ = saved_depth; };
// ... existing preamble ...

// ... intend body (generator/verifier/fixer 执行) ...

// 退出 intent（RAII 触发）：
restore();  // in_task_context_ = saved_depth
```

// eval-expr 处（~line 3945）：
// 如果处于 task 上下文，临时 AST 可分配在 temp arena
// 但最简单做法：eval-expr 始终走 persistent（量小，不频繁）
```

#### 3. `service.ixx`

**CompilerService 加第二 arena：**

```cpp
// 当前：ASTArena arena_;
// 改为：
ASTArena persistent_arena_;
ASTArena temp_arena_;
```

**reset 方法拆分：**

```cpp
// 全量重置（现有 gc 调用）
void reset() {
    persistent_arena_.reset();
    ir_cache_.clear();
    ir_cache_strings_.clear();
}

// 仅重置临时数据（gc-temp 调用）
void reset_temp() {
    temp_arena_.reset();
}
```

**设置 evaluator 的 arena 指针：**

```cpp
// 当前：evaluator_.set_arena(&arena_);
// 改为：
evaluator_.set_arena(&persistent_arena_);
evaluator_.set_temp_arena(&temp_arena_);
```

#### 4. `bench.aura`

```scheme
; 之前：每轮调用 (gc-heap)
; 改为：每轮调用 (gc-temp)

(define (run-rounds ...)
  (gc-temp)  ; 新：同时清 temp arena + heap vectors
  ; ... rest of loop ...
  (gc-temp)  ; 每轮结束后
)
```

### 预期效果

| 指标 | 之前 | 之后 |
|------|------|------|
| `string_heap_` | 每轮 0 (gc-heap) | 每轮 0 (gc-temp) |
| `hash_heap_` | 每轮 0 (gc-heap) | 每轮 0 (gc-temp) |
| `pairs_` | 每轮 0 (gc-heap) | 每轮 0 (gc-temp) |
| `cells_` | 每轮 +3~5 | **每轮 0** |
| `closures_` | 每轮 +3~5 | **每轮 0** |
| Arena 内存 | 单调增长 | **temp 部分重置，persistent 稳定** |

### 安全分析

**为什么 closure.erase() 是安全的：**

- temp arena 的闭包体（cl_flat/cl_pool）里的 `NodeId` 只引用同 arena 的 AstNode
- temp arena 的 `copy_env` 产生的 `Env` 对象也在 temp arena
- `Env::cells_` 指针指向 `Evaluator::cells_`（heap vector），arena reset 不影响
- `Env::bindings_` 是 `std::unordered_map`（heap），不被 arena 管理

所以 erasing temp closures + resetting temp arena 不会产生悬空指针。

**为什么 while 循环闭包不进入 temp arena：**

while 循环的 predicate/body 在 `run-rounds` 开始时创建，此时 `in_task_context_` = false，分配在 `persistent_arena_`。只有意图内的 generator/verifier/fixer 闭包在 `in_task_context_` = true 时创建。

### 边界情况

1. **嵌套 task 上下文** — `intend` 内再调 `intend`（罕见）。当前设计 `in_task_context_` 是 bool，嵌套会导致外层的闭包也进 temp arena。安全但不精准。如需嵌套支持，改为计数器 `task_context_depth_`。

2. **`eval-expr` 在 intend 内** — 默认走 persistent。因为 eval-expr 创建临时 FlatAST 只用于一次 eval，量小。

3. **gc-temp 重复调用** — 幂等。第二次调用时 temp arena 已空，closures_ 无 owner==temp 的条目，vector 已是空。

4. **Full gc 后调 gc-temp** — safe。full gc 重置全部 arena + IR cache，temp arena 也是刚重置的状态。

### 不在此次改动范围

- `ArenaGroup`（`core/arena.ixx`）已有多 arena 管理能力，但当前未使用，暂不引入
- module cache 分离 — 未来可以让每个模块独立 arena，按需卸载
- closure `cl_flat`/`cl_pool` 的世代标记 — closure erase 已经足够

---

### 实施步骤

1. `evaluator.ixx` — Closure 加 owner_arena + Evaluator 加 temp_arena_ / in_task_context_
2. `evaluator_impl.cpp` — closure 创建分派 + copy_env 分派 + gc-temp 原语 + intend 上下文标志
3. `service.ixx` — 双 arena + reset_temp
4. `bench.aura` — (gc-heap) → (gc-temp)
5. 测试：更新 gc.aura 测试
