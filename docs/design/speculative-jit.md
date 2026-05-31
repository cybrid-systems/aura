# Shape-based Speculative JIT — 详细设计

**目标**: 实现通用的形状推测 JIT，让任意 Aura 程序的热路径通过形状特化获得 ≥40% 性能提升。
**关联 Issue**: [#53](https://github.com/cybrid-systems/aura/issues/53)
**状态**: 设计阶段
**作者**: Anqi Yu + Ani

---

## 目录

1. [背景与动机](#1-背景与动机)
2. [架构总览](#2-架构总览)
3. [形状系统 (Shape System)](#3-形状系统-shape-system)
4. [推测编译 (Speculative Compilation)](#4-推测编译-speculative-compilation)
5. [去优化守卫 (Deoptimization Guards)](#5-去优化守卫-deoptimization-guards)
6. [与现有 JIT 的集成](#6-与现有-jit-的集成)
7. [与自修改系统的交互](#7-与自修改系统的交互)
8. [实现计划](#8-实现计划)
9. [验收标准](#9-验收标准)
10. [风险与缓解](#10-风险与缓解)

---

## 1. 背景与动机

### 1.1 问题

动态语言的性能瓶颈主要来自「不知道数据长什么样」。在 Aura 中：

```scheme
;; evo-kv 热路径: 90%+ 的 get 操作访问同一形状的 key/value
(define (kv-get store key)
  ;; key 大多是 string, value 大多是 int
  (hash-ref store key))

;; mutate 后形状可能改变
(mutate:replace-pattern
  '(hash-ref store key)
  '(custom-get store key))  ;; ← 形状变了！
```

当前 JIT（LLVM ORC）对所有值使用统一的 `EvalValue` Tagged Union，无法针对稳定形状做特化。

### 1.2 目标

- 对稳定 shape 的代码路径生成 speculative native code
- shape 变化时通过 deoptimization guard 安全回退
- 与 mutate:* 自修改系统兼容，不引入新的崩溃或 UB
- evo-kv 热路径（get/set/zrange）吞吐量 **≥40% 提升**

### 1.3 设计原则

1. **安全优先**：deoptimization 路径必须无崩溃、无 UB
2. **渐进式**：先从最简单的 shape 特化开始（string key → int value），再推广到复杂 shape
3. **通用性**：不只优化 evo-kv，任何 Aura 程序都能受益
4. **可观测**：暴露 shape profile 指标给 auto-evolution 引擎

---

## 2. 架构总览

```
                    ┌─────────────────────────────────────┐
                    │         Aura Source / EDSL           │
                    └──────────────┬──────────────────────┘
                                   │
                                   ▼
                    ┌─────────────────────────────────────┐
                    │         FlatAST + Lowering           │
                    └──────────────┬──────────────────────┘
                                   │
                                   ▼
                    ┌─────────────────────────────────────┐
                    │              Aura IR                 │
                    └──────┬──────────────┬───────────────┘
                           │              │
                           ▼              ▼
              ┌──────────────────┐  ┌──────────────────┐
              │   IRInterpreter   │  │  Shape Profiler  │  ← 新增
              │   (通用路径)      │  │  (记录形状)      │
              └──────────────────┘  └────────┬─────────┘
                                             │
                                             ▼
                                ┌────────────────────────┐
                                │  Shape Cache + Guard   │  ← 新增
                                │  (命中→特化, 未命中→通用)│
                                └────────┬───────────────┘
                                         │
                                         ▼
                          ┌──────────────────────────────┐
                          │    Speculative Compiler      │  ← 新增
                          │  (根据 shape 生成特化 LLVM IR)│
                          └────────┬─────────────────────┘
                                   │
                                   ▼
                          ┌──────────────────────────────┐
                          │   LLVM ORC JIT (现有)        │
                          └──────────────────────────────┘
```

### 执行路径选择

```
eval():
  if (!jit_mode_) → IRInterpreter (现有, 通用)
  if (!shape_profiler_.has_stable_shape(fn_id))
      → IRInterpreter + recording   (profile 阶段)
  else
      → guard_check(shape_id) ? SpecJIT(path) :
        (IRInterpreter + deopt_fn)  (推测执行)
```

---

## 3. 形状系统 (Shape System)

### 3.1 形状定义

形状是对值布局和操作模式的抽象描述：

```cpp
enum class ShapeTag : uint8_t {
    Any,            // 未知 / 通用（未特化）
    Int,            // 固定整数
    Float,          // 固定浮点
    Bool,           // 固定布尔
    String,         // 固定字符串类型
    Pair,           // 固定 pair (car/cdr 形状已知)
    Vector,         // 固定向量 (元素形状已知)
    Hash,           // 固定 hash (key/value 形状已知)
    Closure,        // 闭包 (fixed arity + env shape)
    Struct,         // 固定结构体布局 (字段名->形状)
    Union,          // 形状联合 (几种可能形状之一)
};

struct Shape {
    ShapeTag tag;
    int32_t type_id;            // TypeRegistry 中的类型 ID（若有）
    union {
        struct {                // Pair
            Shape* car_shape;
            Shape* cdr_shape;
        } pair;
        struct {                // Hash
            Shape* key_shape;
            Shape* value_shape;
        } hash;
        struct {                // Vector
            Shape* elem_shape;
            uint32_t min_len;
            uint32_t max_len;
        } vector;
        struct {                // Struct
            uint32_t field_count;
            Shape** field_shapes;
        } struct_;
        struct {                // Closure
            uint32_t arity;
            Shape* ret_shape;
        } closure;
        struct {                // Union
            uint32_t variant_count;
            Shape** variants;
        } union_;
        int32_t int_range[2];   // Int: [min, max] 范围
    };
    
    // 运行时缓存
    uint64_t shape_id;          // 全局唯一形状 ID（hash）
    uint32_t hit_count;         // 命中次数（用于稳定判定）
};
```

### 3.2 形状 ID

形状 ID 是形状的确定性 hash（SHA-256 截断到 64-bit）：

```
shape_id = hash(ShapeTag + type_id + 递归字段)
```

- 相同形状 → 相同 ID（可跨进程缓存）
- 形状变化 → ID 变化 → guard 触发

### 3.3 形状推断来源

| 来源 | 示例 | 信任度 |
|------|------|--------|
| 类型标注 | `(:: key String)` | 静态已知，高 |
| 类型推断 | 类型检查器的 `occurs` 分析 | 静态已知，中 |
| 运行时 profile | 95% 的调用 key 是 String | 动态观察，中 |
| `mutate:*` 后重建 | mutate 后重新 shape 分析 | 动态，取决于 mutate 范围 |

优先级：类型标注 > 类型推断 > 运行时 profile > 通用

### 3.4 Shape Profiler

`ShapeProfiler` 在 IRInterpreter 执行时记录：

```cpp
class ShapeProfiler {
    struct ShapeSample {
        ShapeID shape_id;
        uint64_t timestamp;     // 采样时间戳
        uint64_t call_count;    // 该形状的调用次数
    };
    
    // per-function shape history
    std::unordered_map<FnKey, std::vector<ShapeSample>> fn_shapes_;
    
    // 形状稳定性判定
    static constexpr uint32_t kStableThreshold = 100;   // 100 次调用
    static constexpr double  kStableRatio    = 0.90;    // 90% 以上同一形状
    
    bool is_stable(FnKey fn) const;
    ShapeID dominant_shape(FnKey fn) const;
    
    // 形状快照（用于 guard 比较）
    struct ShapeSnapshot {
        ShapeID id;
        uint64_t version;       // 单调递增，mutate 后增加
    };
    ShapeSnapshot current_snapshot(FnKey fn);
};
```

### 3.5 稳定性判定算法

```
每执行一条 eval 调用：
  1. 记录当前参数/返回值的 ShapeID
  2. 更新 fn_shapes_[fn] 的滑动窗口（最近 N=1000 次）
  3. 如果 dominant_shape 占比 ≥ 90% → 标记为 stable
  4. 触发 SpeculativeCompiler::compile(fn, shape_id)
```

---

## 4. 推测编译 (Speculative Compilation)

### 4.1 特化策略层级

| 层级 | 名称 | 适用范围 | 收益预期 |
|------|------|----------|----------|
| L0 | 无特化 | 通用 IRInterpreter | 1x |
| L1 | 类型特化 | 已知 type_id 的 PrimCall | 1.5-2x |
| L2 | 布局特化 | Pair/Struct 字段内联 | 2-5x |
| L3 | 调用约定特化 | 闭包调用去虚拟化 | 3-10x |
| L4 | 路径特化 | 内联热路径 + 循环展开 | 5-20x |

**初始实现**：L1 + L2，后续扩展 L3/L4。

### 4.2 L1: 类型特化

将通用的 Tagged Union dispatch 替换为针对特定类型的直接操作：

```llvm
; 通用路径 (当前 JIT):
define i64 @kv_get({i64,i8} %store, {i64,i8} %key) {
  %tag1 = extractvalue {i64,i8} %key, 1
  %is_str = icmp eq i8 %tag1, 3
  br i1 %is_str, label %str_path, label %generic
  
str_path:
  %str_ptr = extractvalue {i64,i8} %key, 0
  ; ... hash lookup on string key ...
  ret i64 %result

generic:
  ; ... full dispatch ...
}

; 特化路径 (假设 key 100% String):
define i64 @kv_get_spec({i64,i8} %store, i64 %key_ptr) {
  ; key 已知是 string → 直接传 string ptr 参数
  ; 无需 tag check
  %val = call i64 @hash_ref_str(%store, %key_ptr)
  ret i64 %val
}
```

**关键优化**：特化后消除 `tag` 检查和 `extractvalue` 开销。

### 4.3 L2: 布局特化

对于已知字段布局的结构体，直接生成字段访问：

```scheme
;; Aura 代码
(define (get-x point) (vector-ref point 0))
```

```llvm
; 通用: vector-ref 需要类型检查 + bounds check + 动态 dispatch
define i64 @get_x({i64,i8} %point) {
  %tag = extractvalue {i64,i8} %point, 1
  %is_vec = icmp eq i8 %tag, 4
  br i1 %is_vec, label %vec_path, label %type_error
vec_path:
  %ptr = extractvalue {i64,i8} %point, 0
  %elem = load i64, i64* %ptr
  ret i64 %elem
}

; 特化: point 已知是 {double, double} 向量
; → 直接双字段 struct 指针，内联元素偏移
define double @get_x_spec({double,double}* %point) {
  %x = getelementptr inbounds {double,double}, {double,double}* %point, i32 0, i32 0
  %val = load double, double* %x
  ret double %val
}
```

### 4.4 特化编译器实现

```cpp
class SpeculativeCompiler {
public:
    // 入口：为指定函数生成 shape-specialized LLVM IR
    JITTargetAddress compile(
        FnKey fn,
        ShapeID shape_id,
        const IRModule& ir_mod,
        AuraJIT& jit
    );
    
    // 检查是否已有特化版本
    bool has_specialization(FnKey fn, ShapeID shape_id) const;
    
    // 失效某个函数的特化版本（mutate 后）
    void invalidate(FnKey fn);
    
private:
    struct SpecEntry {
        ShapeID shape_id;
        JITTargetAddress code_addr;
        uint32_t version;       // 用于失效后 key 比较
        uint32_t hit_count;
    };
    
    std::unordered_map<FnKey, std::vector<SpecEntry>> specializations_;
    
    // Lowering helper: 根据 shape 生成特化 LLVM IR
    LLVMIRFunction lower_with_shape(
        const IRFunction& ir_fn,
        const Shape& shape
    );
};
```

### 4.5 热路径编译决策

```
决策流程:
  函数被 eval → ShapeProfiler 记录形状
  ↓
  is_stable? → 否 → 继续 profile（IRInterpreter）
  ↓ 是
  has_specialization? → 是 → 使用现有特化代码
  ↓ 否
  在后台线程编译特化版本
  ↓ 编译完成
  原子写入 SpecEntry
  ↓
  下次调用 → Guard + 特化路径
```

---

## 5. 去优化守卫 (Deoptimization Guards)

### 5.1 守卫类型

| 守卫 | 检查内容 | 开销 | 触发条件 |
|------|----------|------|----------|
| ShapeID Guard | 比较值 ShapeID 与预期 | ~1ns | 形状不匹配 |
| Version Guard | 检查函数版本号 | ~0.5ns | mutate: 后版本号变更 |
| Type Guard | 检查 TypeRegistry type_id | ~0.5ns | 类型变更 |
| Bounds Guard | 检查数组/向量长度 | ~0.5ns | bounds 越界 |

### 5.2 ShapeID Guard（主要守卫）

推测代码生成的入口处插入 ShapeID 检查：

```llvm
; 特化函数入口
define i64 @kv_get_spec_v3({i64,i8} %store, {i64,i8} %key) 
    gc "statepoint-example" {
entry:
  ; === ShapeID Guard ===
  %key_shape_id = call i64 @shape_of({i64,i8} %key)
  %expected = load i64, i64* @expected_key_shape
  %match = icmp eq i64 %key_shape_id, %expected
  br i1 %match, label %spec_path, label %deopt_path
  
spec_path:
  ; ... 特化代码（无 tag checks）...
  ret i64 %result

deopt_path:
  ; === 去优化回退 ===
  ; 调用通用的 IRInterpreter 路径
  %result = call i64 @generic_kv_get({i64,i8} %store, {i64,i8} %key)
  ; 重新进入 profile 阶段
  call void @shape_profiler_record_mismatch(i64 @kv_get_fn_id)
  ret i64 %result
}
```

### 5.3 Version Guard（变异感知）

每次 `mutate:*` 调用会增加被修改函数的版本号：

```cpp
// service.ixx 中 mutate 路径:
bool apply_mutation(patch) {
    // ... 现有 mutate 逻辑 ...
    for (auto& fn : affected_fns) {
        fn_version_[fn]++;           // 版本号递增
        speculative_jit_.invalidate(fn);  // 失效特化代码
    }
    // 注意: 不立即清除已编译代码——让正在执行的调用完成
    // 新调用将通过 Version Guard 检测到版本不匹配 → 走通用路径
}
```

Version Guard 检查：

```llvm
define i64 @kv_get_spec_v3(...) {
entry:
  %curr_ver = load i32, i32* @kv_get_version
  %exp_ver = load i32, i32* @kv_get_spec_v3_version
  %ver_match = icmp eq i32 %curr_ver, %exp_ver
  br i1 %ver_match, label %shape_check, label %deopt_path
  ; ...
}
```

### 5.4 去优化回调 (Deoptimization Bounce)

当 guard 触发时：

```
DeoptGuard 触发
    ↓
记录 deopt 原因（shape 不匹配 / 版本变化 / bounds 越界）
    ↓
将当前值提升 (Value Materialization):
  如果特化后值被拆解（如 string ptr 代替 tagged union），
  重新包装为 EvalValue Tagged Union
    ↓
调用通用 IRInterpreter 的对应函数
    ↓
ShapeProfiler 记录新形状
    ↓
如果新形状也稳定 → 编译新特化版本
```

**值提升 (Materialization) 的挑战**：
```cpp
// 特化函数可能将 EvalValue 拆解为原生类型:
llvm::Value* str_ptr = extract_string_ptr(tagged_value);
// 但在 deopt 时需要重新打包:
EvalValue materialized = EvalValue::make_string(str_ptr);
```

为简化初始实现，L1 特化**不拆解参数**——只在函数**内部**省略 tag checks，参数格式保持 EvalValue Tagged Union。这样 deopt 时无需 materialization。

### 5.5 Deopt 目标选择

```
deopt_path:
  call @generic_fn       // 调用对应的通用 IRInterpreter 版本
  // 或: 调用前一个稳定版本的特化代码
  // 或: 直接抛出错误（不应走到这里）
```

优先调用通用 IRInterpreter 版本（最安全，无需维护多个 deopt 目标）。

### 5.6 去优化安全性保证

| 场景 | 行为 | 正确性 |
|------|------|--------|
| Shape 不匹配 | deopt → IRInterpreter | ✅ 100% 语义一致 |
| mutate 后版本变化 | deopt → IRInterpreter | ✅ mutate 后语义正确 |
| 递归 mutate 期间 | Version Guard 失效 → deopt | ✅ 避免执行中修改 |
| GC 触发 | safe points 类似现有 JIT | ✅ 复用现有 GC root |

---

## 6. 与现有 JIT 的集成

### 6.1 分层架构

```
现有: AuraJIT (LLVM ORC) ── 通用 JIT 编译
新增: SpecJIT ───────────── 形状推测 JIT（基于 AuraJIT 扩展）
```

```
SpecJIT 依赖:
  ┌─────────────────────┐
  │    SpecJIT           │  ← Shape 分析 → LLVM IR → ORC JIT
  ├─────────────────────┤
  │    AuraJIT           │  ← LLVM ORC + 运行时 bridge（现有）
  ├─────────────────────┤
  │    LLVM Backend      │
  └─────────────────────┘
```

### 6.2 代码复用

| 组件 | 复用方式 |
|------|----------|
| LLVM ORC `LLJIT` | 直接复用现有实例 |
| 运行时 bridge (`aura_jit_runtime.cpp`) | 复用，新增 speculative shape helper |
| 值编码 (EvalValue ↔ Tagged Union) | 复用 L1，L2 可能需要扩展 |
| `--jit` flag | 扩展为 `--jit=spec` 模式 |
| 增量编译 cache | 特化版本独立 cache |

### 6.3 新增/修改文件

| 文件 | 类型 | 内容 |
|------|------|------|
| `src/compiler/shape.ixx` | 新增 | Shape 系统核心定义 |
| `src/compiler/shape_impl.cpp` | 新增 | Shape 分析、hash、推断 |
| `src/compiler/shape_profiler.ixx` | 新增 | Shape Profiler |
| `src/compiler/shape_profiler_impl.cpp` | 新增 | Profile 记录 + 稳定性判定 |
| `src/compiler/spec_jit.ixx` | 新增 | Speculative JIT 编译 |
| `src/compiler/spec_jit_impl.cpp` | 新增 | 特化代码生成 |
| `src/compiler/aura_jit.ixx` | 修改 | 暴露内部接口给 SpecJIT |
| `src/compiler/service.ixx` | 修改 | eval 路径加入 shape profiler + guard |
| `src/serve/gc_coordinator.*` | 修改 | 新增 shape metadata GC root |
| `src/serve/metrics.h` | 修改 | 新增 shape profile 指标 |
| `tests/spec_jit_test.aura` | 新增 | 特化 JIT 测试 |
| `tests/deopt_test.aura` | 新增 | 去优化测试 |
| `tests/bench_results/` | 新增 | 形状特化 benchmark |

### 6.4 Benchmark 框架扩展

```python
# tests/benchmark.py 扩展
def run_spec_jit_benchmark(model, suite="evo-kv"):
    """运行形状特化 benchmark"""
    results = {
        "generic_jit": {},      # 当前 JIT 基准
        "spec_jit_l1": {},      # L1 类型特化
        "spec_jit_l2": {},      # L2 布局特化
        "deopt_overhead": {},   # 去优化守卫开销
    }
    # ...
```

---

## 7. 与自修改系统的交互

### 7.1 mutate 时的形状失效

```
mutate:* 调用
    ↓
AST 变异 + 脏传播（现有）
    ↓
shape_profiler.invalidate(fn_id)  ← 新增
    ↓
spec_jit.invalidate(fn_id)        ← 新增
    ↓
fn_version[fn_id]++               ← 新增
    ↓
下次 eval → Version Guard 触发 → deopt → IRInterpreter
```

### 7.2 形状稳定性保证

| mutate 类型 | 对形状的影响 | 是否需要重新 profile |
|-------------|-------------|---------------------|
| `mutate:replace-pattern` | 可能改变形状（新代码） | 是 |
| `mutate:inline` | 不变（只是内联展开） | 否 |
| `mutate:rename` | 不变 | 否 |
| `mutate:extract` | 可能改变（新函数签名） | 是 |
| `mutate:optimize` | 可能改变 | 是 |
| `ast:rollback` | 恢复到旧形状 | 是（复用旧 cache） |

### 7.3 线程安全

当前 Aura 的 self-modification 是**单线程**的（serve 层的 fiber 调度已经保证了 yield point 隔离）。

SpecJIT 遵循相同约束：
- shape profile 只在 eval 时记录（单线程，安全）
- 特化编译在后台线程进行（编译结果用 atomic 写入）
- guard 检查在 eval 线程执行（版本号用 relaxed atomic load）
- deopt 路径在 eval 线程执行（安全）

### 7.4 递归 mutate 保护

```
; 在推测 JIT 代码中调用 mutate:* 的可能
(define (hot-path x)
  (mutate:replace-pattern ...)  ;; ← 如果特化代码内部调用 mutate
  (compute x))

; 安全措施:
; 1. Version Guard 在每次 spec_fn 入口检查
; 2. 如果 mutate 发生在递归调用中，返回时版本已变
; 3. 但上一层 spec_fn 无法感知 → 需要在关键 yield point 重新检查
```

**解决方案**：在每个 `PrimCall` 桥接点插入轻量级 Version Check（0.5ns 开销），如果版本变化则直接 deopt 返回。

---

## 8. 实现计划

### Phase 1: Shape 基础设施（~2 天）

| 步骤 | 内容 | 验收 |
|------|------|------|
| 1.1 | 定义 `Shape` / `ShapeID` / `ShapeTag` | Shape 可唯一标识 |
| 1.2 | 实现从 `EvalValue` 提取 ShapeID | 任意值 → ShapeID |
| 1.3 | 实现 `ShapeProfiler`（记录 + 稳定性判定） | 稳定形状可检测 |
| 1.4 | 在 IRInterpreter 中注入 profile hooks | eval 时自动记录 |
| 1.5 | 单元测试: shape 基础用例 | CI 通过 |

**关键 API**：
```cpp
ShapeID shape_of(const EvalValue& val);
bool ShapeProfiler::is_stable(FnKey fn);
ShapeSnapshot ShapeProfiler::snapshot(FnKey fn);
```

### Phase 2: L1 类型特化（~3 天）

| 步骤 | 内容 | 验收 |
|------|------|------|
| 2.1 | `SpeculativeCompiler` 实现 L1 lowering | 根据 shape 生成无 tag-check 的 LLVM IR |
| 2.2 | 特化代码通过 ORC JIT 编译 | 可执行特化二进制 |
| 2.3 | 入口 Guard（ShapeID check）生成 | Guard 正确触发/通过 |
| 2.4 | Deopt 路径（回退 IRInterpreter） | deopt 后语义一致 |
| 2.5 | 集成到 `service.ixx::eval()` | `--jit=spec` 可用 |
| 2.6 | 测试: 简单类型特化（always-int addition） | 结果正确 |

### Phase 3: L2 布局特化（~3 天）

| 步骤 | 内容 | 验收 |
|------|------|------|
| 3.1 | Struct/Pair 布局分析 | 字段布局已知 |
| 3.2 | 字段内联 + gep 直接访问 | 消除 pair/vector dispatch 开销 |
| 3.3 | Hash key/value 形状特化 | hash-ref 类型已知时直接访问 |
| 3.4 | Benchmark: 布局特化加速比 | ≥2x over generic JIT |

### Phase 4: mutate 集成 + Version Guard（~2 天）

| 步骤 | 内容 | 验收 |
|------|------|------|
| 4.1 | `fn_version_` 追踪 | mutate 后版本号递增 |
| 4.2 | Version Guard 生成 | 版本不匹配 → deopt |
| 4.3 | POC: mutate 后形状变更 → 新特化 | 自修改 + spec JIT 兼容 |
| 4.4 | 测试: mutate 后正确 deopt、不崩溃 | 完整性测试 |

### Phase 5: evo-kv 端到端优化 + Benchmark（~2 天）

| 步骤 | 内容 | 验收 |
|------|------|------|
| 5.1 | evo-kv 热路径 profile + 特化 | 确定 get/set/zrange shape |
| 5.2 | 端到端 benchmark | **吞吐量 ≥40% 提升** |
| 5.3 | 通用性验证（非 evo-kv 测试套件） | 其他 benchmark 不受损 |
| 5.4 | Shape profile 指标暴露 | `(query:metrics)` 可查看 |

### 总工期：~12 天

---

## 9. 验收标准

| 标准 | 测量方法 | 目标值 |
|------|----------|--------|
| evo-kv get 吞吐量 | `benchmark.py --suite=evo-kv` | ≥40% 提升 |
| evo-kv zrange 吞吐量 | `benchmark.py --suite=evo-kv` | ≥40% 提升 |
| mutate 后 deopt 正确性 | `deopt_test.aura` | 100% 无崩溃 |
| 非 evo-kv 性能 | 全 benchmark 套件 | 无倒退（≤3% 合理开销） |
| guard 检查开销 | microbenchmark | ≤2ns/guard |
| 内存开销 | heap profile | ≤5MB shape metadata |
| Aura 自修改安全 | 现有 EDSL 测试 + fuzz | 全部通过 |

---

## 10. 风险与缓解

| 风险 | 影响 | 概率 | 缓解 |
|------|------|------|------|
| Shape 稳定性误判 | 频繁 deopt → 性能下降 | 中 | 保守的 `kStableRatio=0.95` |
| Guard 开销抵消收益 | 小函数特化后反而更慢 | 低 | 函数大小阈值（≥10 opcode） |
| mutate 后 deopt 不及时 | 执行旧形状的推测代码 | 低 | Version Guard + PrimCall 检查 |
| Shape profile 内存膨胀 | 过多独特形状 | 低 | LRU 淘汰 + 合并相似形状 |
| LLVM IR 复杂度 | 编译时间增加 | 中 | 后台编译，不阻塞 eval |
| deopt 路径引入 bug | 罕见的 guard 触发路径 | 中 | 全面的 deopt fuzz 测试 |

### 10.1 关键决策记录

| 决策 | 选项 | 选择 | 理由 |
|------|------|------|------|
| Shape 表示方式 | Tagged Union vs 类型类 | Shape 结构体 | 灵活，可递归 |
| Guard 检查位置 | 入口 vs 每个 PrimCall | 入口 + PrimCall 检查点 | 平衡开销与安全 |
| Deopt 目标 | 通用 IRInterpreter vs 前一个特化版本 | 通用 IRInterpreter | 最安全，实现简单 |
| 编译时机 | 首次发现稳定形状时 | 后台线程 | 不阻塞 eval 线程 |
| L1 参数格式 | 保持 EvalValue | 不拆解参数 | 避免 deopt materialization |

---

## 附录 A: 与现有优化对比

| 优化 | 已实现 | 与 SpecJIT 的关系 |
|------|--------|-------------------|
| LLVM ORC JIT | ✅ (#33, #43) | SpecJIT 的上层调用端 |
| 编译期常量求值 | ✅ (#49? → `opt-const-eval.md`) | 互补（常量优化在前，形状推测在后） |
| 短字符串池 | ✅ (`opt-short-str.md`) | 互补（减少 string alloc → 更快 shape profile） |
| 内联 hash IR opcode | ✅ (`opt-primitive-inline.md`) | 互补（提升通用路径性能，让特化路径对比更清晰） |
| PGO | #52 (规划中) | **前置依赖**：PGO 提供准确的冷热路径数据 |
| 逃逸分析+Arena | #54 (规划中) | 互补（Arena 减少 GC，SpecJIT 减少 dispatch 开销） |

### 优先顺序建议

```
PGO (#52) → 提供冷热路径数据
    ↓
Escape Analysis (#54) → 减少内存分配开销
    ↓
Shape Spec JIT (#53) → 热路径形状特化  ← 我们在这里
    ↓
Performance Regions (#55) → 语言级别 hint
    ↓
LLVM Pass Manager (#56) → 编译器自进化
```

---

## 附录 B: 代码示例

### B.1 特化前（通用 JIT）

```scheme
;; evo-kv get 操作
(define (kv-get store key)
  ;; key 和 value 的类型在运行时未知
  (hash-ref store key))

;; 编译为通用 LLVM IR:
;; - 每次 hash-ref 需要检查 key 类型
;; - 需要检查 store 是否 hash
;; - 需要间接调用 hash_ops 的函数指针
```

### B.2 特化后（推测 JIT）

```scheme
; shape profile 显示:
;   store → Hash(String → Int)
;   key   → String (100%)
;   value → Int (95%)

; 编译为:
;; - key 直接传 char* (消除 tag check)
;; - store 已知是 HashLayout v3
;; - hash_ops 直接内联调用 str_hash()
;; - value 返回后直接作为 int 使用
```

### B.3 Mutate 触发 Deopt

```scheme
;; 1. 稳定阶段: kv-get 运行在特化路径
(kv-get store "key1")  ;; → spec JIT, ~100ns
(kv-get store "key2")  ;; → spec JIT, ~100ns

;; 2. 用户 mutate 形状:
(mutate:replace-pattern
  '(hash-ref store key)
  '(custom-get store key))  ;; ← 版本号 + 1

;; 3. 下次调用: Version Guard 触发
(kv-get store "key1")  
;; → Version Guard: curr_ver(5) != expected_ver(4)  
;; → deopt → IRInterpreter
;; → 新 profile → 编译新特化版本
;; → 后续调用走新特化路径
```

---

## 附录 C: 性能模型

### C.1 加速估算

```python
# evo-kv get 热路径性能模型
# 基于现有 IRInterpreter/JIT benchmark (fib-20: JIT 7.55x IRInterpreter)

generic_jit_time = 100ns  # 当前 JIT (含 tag dispatch + type checks)
spec_jit_time = {
    'L1_type': 55ns,       # 消 tag check
    'L2_layout': 35ns,     # +消字段 dispatch + 内联 hash ops
    'L3_devirt': 25ns,     # +函数去虚拟化
}

guard_overhead = 2ns       # ShapeID + Version check

effective_time = guard_overhead + (
    0.95 * spec_jit_time +       # 95% 命中特化路径
    0.05 * generic_jit_time      # 5% deopt 到通用路径
)

# L1: 55 + 2 + 0.05*100 = 62ns → ~1.6x
# L2: 35 + 2 + 0.05*100 = 42ns → ~2.4x
# L3: 25 + 2 + 0.05*100 = 32ns → ~3.1x
```

### C.2 evo-kv 端到端预期

| 模式 | get 延迟 | get 吞吐量 | 对比 baseline |
|------|---------|-----------|-------------|
| IRInterpreter | ~750ns | ~1.3M QPS | 1x |
| 当前 JIT | ~100ns | ~10M QPS | ~7.5x |
| Spec JIT L1 | ~62ns | ~16M QPS | ~12x |
| Spec JIT L2 | ~42ns | ~24M QPS | ~18x |
| Spec JIT L3 | ~32ns | ~31M QPS | ~24x |

**注意**: 上述数字为单线程模型。evo-kv 实际吞吐量还受 IO、网络、GC 影响。

---

## 附录 D: 测试策略

### D.1 单元测试

```
tests/spec_jit_test.aura:
  - shape_of: int/float/string/pair/hash/void → correct ShapeID
  - shape_equality: same values → same ShapeID
  - shape_stability: 100 identical values → is_stable == true
  - shape_instability: mixed types → is_stable == false

tests/deopt_test.aura:
  - shape_mismatch_deopt: wrong type → guard fires → correct result
  - version_change_deopt: mutate:* → version bump → guard fires
  - recursive_mutate_deopt: mutate in recursion → deopt at PrimCall
  - nested_deopt: deopt in nested spec fn → propagate correctly
```

### D.2 Fuzz 测试

扩展现有 `fuzz_edsl.py` 和 `fuzz_snapshot.py`：

```
# 随机生成 EDSL 操作序列，验证:
# 1. spec JIT 结果 == IRInterpreter 结果
# 2. mutate 后 spec JIT 结果 == mutate 后 IRInterpreter 结果
# 3. deopt 不崩溃
```

### D.3 性能回归测试

```
tests/bench_results/spec_jit.json:
  - 记录每次 Phase 交付后的 benchmark 数据
  - 比较 spec_jit vs generic_jit vs IRInterpreter
  - 高亮性能倒退
```
