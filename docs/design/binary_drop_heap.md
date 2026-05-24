# --emit-binary Drop 机制 + 可复用堆管理（P0-1 + P0-2）

**版本**: v1.0（2026-05-24）
**目标**: 让生成的独立二进制真正支持 M4 linear ownership 的 drop 语义，解决当前"只分配不释放"的致命问题。

---

## 1. 设计目标

| 目标 | 说明 |
|------|------|
| 核心目标 | 实现完整的 drop 语义，让所有权模型在二进制中真正生效 |
| 内存复用 | 解决当前 append-only 导致的快速 OOM |
| 递归释放 | Pair / Closure 等复合类型能正确释放子对象 |
| 向后兼容 | 最小改动现有接口，保持 ID-based 设计 |
| 实现难度 | 适合 1-2 周内完成（MVP 版本） |

---

## 2. 当前问题回顾

```c
// 当前致命问题
uint64_t pair_count = 0;   // 只增不减
uint64_t cell_count = 0;
uint64_t closure_count = 0;

int64_t aura_alloc_pair(...) { pair_count++; ... } // 永久占用
// 完全没有 drop 函数！
```

**后果**: 即使 Aura 代码写了 `drop`，生成的二进制也**无法释放**任何内存。

---

## 3. 整体架构设计

采用 **Free List + 递归 Drop** 模型，符合线性所有权思想：

```
分配流程:
  1. 先尝试从 Free List 取空闲槽位
  2. 没有则使用 Bump Allocator（当前逻辑）
  3. 返回 ID

释放流程（Drop）:
  1. 根据类型调用对应 drop 函数
  2. 递归释放子对象（Pair 的 car/cdr、Closure 的环境等）
  3. 将槽位加入 Free List（可复用）
```

---

## 4. 运行时详细设计（`lib/runtime.c` 修改）

### 4.1 新增数据结构

```c
// === 新增: Free List（每种类型一个） ===
static uint64_t pair_free_list[MAX_PAIRS];
static uint64_t pair_free_count = 0;

static uint64_t cell_free_list[MAX_CELLS];
static uint64_t cell_free_count = 0;

static uint64_t closure_free_list[MAX_CLOSURES];
static uint64_t closure_free_count = 0;
```

### 4.2 新增核心 Drop 函数

```c
void aura_drop_pair(int64_t id) {
    if ((uint64_t)id >= pair_count) return;
    // 加入 free list
    if (pair_free_count < MAX_PAIRS) {
        pair_free_list[pair_free_count++] = (uint64_t)id;
    }
}

void aura_drop_cell(int64_t id) {
    if ((uint64_t)id >= cell_count) return;
    if (cell_free_count < MAX_CELLS) {
        cell_free_list[cell_free_count++] = (uint64_t)id;
    }
}

void aura_drop_closure(int64_t id) {
    if ((uint64_t)id >= closure_count) return;
    if (closure_free_count < MAX_CLOSURES) {
        closure_free_list[closure_free_count++] = (uint64_t)id;
    }
}
```

### 4.3 修改分配函数（支持复用）

```c
int64_t aura_alloc_pair(int64_t car, int64_t cdr) {
    uint64_t id;
    if (pair_free_count > 0) {
        id = pair_free_list[--pair_free_count]; // 优先复用
    } else {
        if (pair_count >= MAX_PAIRS) {
            fprintf(stderr, "runtime: pair overflow\n");
            exit(1);
        }
        id = pair_count++;
    }
    pairs[id].car = car;
    pairs[id].cdr = cdr;
    return (int64_t)id;
}
```

Cell 和 Closure 同理修改。

### 4.4 设计决策

- **幂等性**: 所有 drop 函数应支持重复调用（对已释放 ID 不做任何操作）
- **递归释放**: 短期不加，先用简单 free list，后续再支持 pair car/cdr 递归 drop
- **线程安全**: 当前不要求，--serve 路径单线程

---

## 5. 编译器层设计（LLVM IR 生成端）

### 5.1 变量离开作用域时插入 Drop

在 `src/compiler/aura_jit.cpp` 的 LLVM lowering 中：

```cpp
// 当变量离开作用域时
auto* drop_fn = module.getOrInsertFunction("aura_drop_pair",
    Type::getVoidTy(ctx), Type::getInt64Ty(ctx));
builder.CreateCall(drop_fn, {local_slot});
```

### 5.2 Move/Borrow 语义

- **move**: 原绑定标记已转移，不生成 drop
- **borrow**: 不生成 drop（借用不拥有所有权）

### 5.3 函数返回 / 异常路径

在所有退出路径统一生成 drop 调用。

---

## 6. 实现路线图

| 步骤 | 内容 | 预计时间 |
|------|------|----------|
| Step 1 | runtime.c: Free List + 基础 Drop 函数 | 2 天 |
| Step 2 | runtime.c: 修改 alloc 支持复用 | 1 天 |
| Step 3 | LLVM IR: 插入简单 Drop 调用（局部变量） | 3-4 天 |
| Step 4 | LLVM IR: 处理 Move/Borrow 语义 | 2 天 |
| Step 5 | 递归 Drop（Pair car/cdr） | 2 天 |
| Step 6 | 测试 + 修复边界 | 2-3 天 |

**合计**: ~12-14 天


---

## 7. 性能评估与业界对比

### 7.1 当前方案评估

| 维度 | 评估 | 说明 |
|------|------|------|
| 分配速度 | 优秀 | O(1)，Free List pop 或 Bump fallback |
| 释放速度 | 良好 | O(1) + 递归子对象释放 |
| 缓存友好性 | 中等 | Free List 复用可能导致碎片 |
| 长期运行稳定性 | 良好 | 可复用，不会快速 OOM |
| 递归 Drop 风险 | 中等 | 深度嵌套结构可能栈溢出 |

### 7.2 业界对比

| 方案 | 分配速度 | 释放速度 | 缓存友好 | 实现复杂度 |
|------|----------|----------|----------|-----------|
| 纯 malloc/free | 中 | 中 | 差 | 低 |
| Free List (当前方案) | 快 | 快 | 中 | 低 |
| **Bump + Arena Reset** | **极快** | **极快** | **优** | **中** |
| Rust 风格 Drop | 快 | 快 | 优 | 高 |

### 7.3 推荐演进路线

**短期 (MVP, 本周)**: 按当前 Free List + Explicit Drop 方案实现，功能优先。

**中期 (2-4周后)**: 升级为 Bump Allocator + Arena Reset 为主，关键对象用 Free List。
- 分配速度提升 20-50%
- 更符合 Aura "高性能独立二进制"定位
- 参考 Zig 的 ArenaAllocator 模式

### 7.4 递归 Drop 安全化

使用迭代 + 显式栈替代递归，避免深度嵌套结构（如长链表）导致的栈溢出。
