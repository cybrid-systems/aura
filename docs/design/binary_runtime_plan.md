# --emit-binary 运行时改进计划

**日期**: 2026-05-24
**状态**: 草案
**基于**: `lib/runtime.c` (2026-05-23) + `--emit-binary` 管线

---

## 现状评估

`--emit-binary` 于 2026-05-23 首次接入，`lib/runtime.c` 是早期占位实现。
当前 gap 分析：

| 维度 | 当前状态 | 问题 |
|------|----------|------|
| 内存释放 / Drop | 完全无实现 | runtime.c 无任何 `aura_drop_*` 函数，所有分配 append-only |
| 闭包捕获 | 空函数 | `aura_closure_capture` 为 `(void)params;` stub |
| 字符串支持 | 零 | `strings[]` 声明了但无分配函数 |
| 堆管理 | 固定大小静态数组 | 溢出直接 `exit(1)`，无复用、无扩容 |
| 所有权模型落地 | 仅编译器/IR 层面 | IR 有 `OpDrop` 指令，但 LLVM lowering 未翻译为运行时调用 |

### 所有权模型在各层面的覆盖

| 层面 | 状态 |
|------|:----:|
| 类型系统 (`Linear Int`) | ✅ 编译期检查 |
| Borrow checker (`borrow`/`move`) | ✅ 回归测试 |
| IR 层 (`OpDrop`) | ✅ 指令存在 |
| Tree-walk evaluator | ✅ drop 语义生效 |
| JIT 路径 | ⚠️ 部分 |
| **--emit-binary 路径** | **❌ 基本缺失** |

---

## Phase 0: 紧急修复（1-2 周）

目标: 让 `--emit-binary` 能跑通非 trivial 程序。

### P0-1: 基础 Drop/Release 机制（3-5 天）

在 `lib/runtime.c` 中增加释放函数族，当前 IR/LLVM 端先手动调用。

```c
// ── Pair drop ────────────────────────────────────────────
void aura_drop_pair(int64_t id) {
    uint64_t idx = (uint64_t)id;
    if (idx < pair_count && pair_heap[idx].live) {
        pair_heap[idx].live = false;
        // Recursively drop car / cdr if they are pair refs or cells
        // (type info needed — see design decision below)
    }
}

// ── Cell drop ────────────────────────────────────────────
void aura_drop_cell(int64_t id) {
    uint64_t idx = (uint64_t)id;
    if (idx < cell_count) {
        cell_heap[idx].live = false;
    }
}

// ── Closure drop ─────────────────────────────────────────
void aura_drop_closure(int64_t id) {
    uint64_t idx = (uint64_t)id & 0x7FFFFFFFFFFFFFFF;
    if (idx < closure_count) {
        closure_heap[idx].live = false;
        // Drop captured environment if any
    }
}
```

**设计决策**: 需要决定用引用计数还是 tracing GC。推荐:
- **短期**: 简单的 alive 标记位 + 惰性复用（free list）
- **长期**: 引用计数（与线性所有权语义自然匹配）

### P0-2: 可复用堆管理（4-6 天）

从 append-only 改为 free list + 复用:

```c
#define MAX_PAIRS 1048576

typedef struct {
    int64_t car, cdr;
    bool live;
} AuraPair;

static AuraPair pair_heap[MAX_PAIRS];
static uint64_t pair_count = 0;
static int64_t pair_free_head = -1; // free list head

int64_t aura_alloc_pair(int64_t car, int64_t cdr) {
    uint64_t id;
    if (pair_free_head >= 0) {
        id = (uint64_t)pair_free_head;
        pair_free_head = (int64_t)pair_heap[id].car; // reuse car as next pointer
    } else {
        if (pair_count >= MAX_PAIRS) {
            fprintf(stderr, "pair overflow\n");
            exit(1);
        }
        id = pair_count++;
    }
    pair_heap[id].car = car;
    pair_heap[id].cdr = cdr;
    pair_heap[id].live = true;
    return (int64_t)id;
}

void aura_drop_pair(int64_t id) {
    uint64_t idx = (uint64_t)id;
    if (idx < pair_count && pair_heap[idx].live) {
        pair_heap[idx].live = false;
        pair_heap[idx].car = (int64_t)pair_free_head; // link into free list
        pair_free_head = (int64_t)idx;
    }
}
```

### P0-3: 闭包捕获修复（2-3 天）

```c
typedef struct {
    void* fn_ptr;
    int64_t env[8];       // fixed-size capture slots (expandable)
    int env_count;
    bool live;
} AuraClosure;

void aura_closure_capture(int64_t closure_id, int idx, int64_t val) {
    uint64_t cid = (uint64_t)closure_id & 0x7FFFFFFFFFFFFFFF;
    if (cid < closure_count && idx < 8) {
        closure_heap[cid].env[idx] = val;
    }
}
```

### P0-4: 字符串支持（2 天）

```c
#define MAX_STRINGS 65536
static char* string_heap[MAX_STRINGS];
static uint64_t string_count = 0;

int64_t aura_alloc_string(const char* s) {
    if (string_count >= MAX_STRINGS) exit(1);
    string_heap[string_count] = strdup(s);
    return (int64_t)(string_count++);
}

const char* aura_string_ref(int64_t id) {
    uint64_t idx = (uint64_t)id;
    return (idx < string_count) ? string_heap[idx] : "";
}
```

---

## Phase 1: 所有权模型落地（3-5 周）

### P1-1: LLVM IR 端插入 Drop 调用（5-7 天）

当前 LLVM JIT lowering (`src/compiler/aura_jit.cpp`) 需要在以下位置生成 `aura_drop_*` 调用:

1. **变量离开作用域** — let/letrec 绑定的值在退出作用域时 drop
2. **move 语义** — `(move x)` 后原绑定标记为 moved，但 runtime 端需清理
3. **函数返回** — 返回值所有权转移，临时值在 return 前 drop
4. **if/cond 分支** — 未走的分支中的绑定需要 drop

主要修改文件: `src/compiler/aura_jit.cpp`

```cpp
// 在 LLVM IR 生成中，当变量离开作用域时:
auto* drop_fn = module.getOrInsertFunction("aura_drop_pair",
    Type::getVoidTy(ctx), Type::getInt64Ty(ctx));
builder.CreateCall(drop_fn, {local_slot});
```

### P1-2: Borrow Checker 与二进制路径联动（4-6 天）

确保 borrow checker 报错的代码不能在 binary 中产生悬垂指针。

### P1-3: Cell/Pair 所有权转移（3-4 天）

```c
// Move: source cell 置空，目标获得所有权
void aura_move_cell(int64_t src_id, int64_t dst_id) {
    cell_heap[(uint64_t)dst_id] = cell_heap[(uint64_t)src_id];
    cell_heap[(uint64_t)src_id].live = false;
}
```

### P1-4: 运行时 Ownership 跟踪（4 天，可选轻量版）

```c
typedef enum { OWN_OWNED, OWN_MOVED, OWN_BORROWED, OWN_MUT_BORROWED } OwnState;
static OwnState cell_ownership[MAX_CELLS];
```

---

## Phase 2: 高价值增强（6-10 周）

| 任务 | 工作量 | 说明 |
|------|--------|------|
| Closure 完整实现（环境捕获 + drop） | 1-2 周 | 闭包成为一等公民 |
| Stdlib 模块二进制可用 | 2-3 周 | list, hash, string, io |
| 堆性能优化（arena / bump allocator） | 1 周 | 减少碎片和分配开销 |
| `--emit-binary` 优化选项 | 3-5 天 | `-O2` 等 |

---

## 推荐的执行顺序

```
本周:    P0-1 (Drop fn) + P0-2 (Free list)
下两周:  P0-3 (Closure) + P1-1 (IR drop call)
3-5周:   P1-2 (Borrow+Binary) + P1-3 (Move semantics)
6周+:    Phase 2
```

## 相关文件

- `lib/runtime.c` — 运行时实现（主要修改目标）
- `src/compiler/aura_jit.cpp` — LLVM IR 生成（插入 drop 调用）
- `src/compiler/lowering_impl.cpp` — IR lowering（确保 drop 指令正确发出）
- `src/compiler/ir.ixx` — IR opcode 定义（OpDrop 已存在）
