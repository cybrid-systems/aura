# 增量编译 + Compiler as a Service 设计

**版本**: v1.0
**状态**: 设计阶段
**对应**: M4 基础设施

---

## 1. 问题定义

### 现状

```bash
# 每次请求都是全量管线
echo '(define add (lambda (x y) (+ x y)))' | ./aura --ir   # 编译 add
echo '(add 1 2)' | ./aura --ir                              # 重新编译 add!

# 无状态: arena 重置, IR 丢弃, 函数重新降低
```

### 根因

`CompilerService` 没有"定义"的概念。每次 `eval_ir()` 都是：

```
输入字符串 → parse → lower → passes → execute → 丢弃全部
```

即使两次输入差一个字符，整个流程重来。

### 目标

```
Session:                       处理方式
─────────────────────────────────────────────
(define add (lambda (x y) ...))  → 编译 + 缓存 IR
(define mul (lambda (x y) ...))  → 编译 + 缓存 IR  
(add 1 (mul 2 3))                → 顶层解析 + 链接缓存 → 执行
(redefine add ...)                → 只重编 add → hot-swap → 乘余缓存不动
(add 1 (mul 2 3))                → 同上次 → 快速返回
```

---

## 2. 架构设计

```
┌──────────────────────────────────────────────────────────────┐
│                      CompilerService                          │
│                                                               │
│  ┌──────────────┐  ┌────────────────┐  ┌──────────────────┐  │
│  │  Parser       │  │  IR Cache       │  │  Pass Manager     │  │
│  │               │  │  (函数粒度)      │  │  (per-function)   │  │
│  │ parse_to_flat │  │  func[0] = add  │  │  ck(0) ar(0) cf(0)│  │
│  │ parse_expr    │  │  func[1] = mul  │  │  ck(1) ar(1) cf(1)│  │
│  │ parse_define  │  │  func[2] = top  │  │  ck(2) ar(2) cf(2)│  │
│  └──────┬───────┘  └────────┬───────┘  └──────────┬─────────┘  │
│         │                   │                      │            │
│         ▼                   ▼                      ▼            │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │              Dependency Graph                            │   │
│  │  add → []          (add 不依赖自定义函数)                 │   │
│  │  mul → []          (mul 不依赖自定义函数)                 │   │
│  │  top → [add, mul]  (顶层表达式引用 add 和 mul)            │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  Hot Swap Engine  (已有的 hot_swap_function)             │   │
│  │  替换 func body → closures 自动重连                      │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  Session 状态                                             │   │
│  │  arena: 复用                                              │   │
│  │  env:   define 绑定持久                                   │   │
│  │  cache: IR + dep graph                                   │   │
│  └─────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. 核心组件

### 3.1 定义检测器

```cpp
// 在 lower 之前检测输入是否为 define 绑定
// 如果是, 分离"定义"和"主体"
struct DefinitionResult {
    std::string name;           // 函数名
    aura::ast::NodeId body;     // 函数体节点 (lambda)
};

std::optional<DefinitionResult> try_extract_define(
    FlatAST& flat, StringPool& pool, NodeId root);
```

检测逻辑:
1. 解析后检查 `root` 的 tag
2. 如果是 `DefineNode` → 提取 name + body
3. 如果是 `LetNode` / `LetRecNode` → 允许绑定和引用的混合
4. 否则作为普通求值表达式

### 3.2 IR 缓存

```cpp
// 函数粒度的 IR 缓存, 替换当前的 last_ir_mod_
class IRCache {
    struct CacheEntry {
        IRFunction func;
        ComputeKindResult ck;
        bool arity_checked = false;
        bool folded = false;
    };
    std::unordered_map<std::string, CacheEntry> cache_;  // name → func
    std::vector<IRFunction> ordered_;                     // 执行顺序
    uint32_t entry_func_id_ = 0;

public:
    // 注册一个新函数
    void register_function(std::string name, IRFunction func);
    
    // 替换一个已有函数 (hot-swap)
    bool hot_swap(std::string name, IRFunction new_func);
    
    // 构建完整 IRModule (从缓存组装)
    IRModule build_module();
    
    // 检查缓存
    bool has(std::string name) const;
};
```

### 3.3 依赖图

```cpp
// 追踪函数间的调用关系
class DependencyGraph {
    // name → [依赖的函数名列表]
    std::unordered_map<std::string, std::vector<std::string>> deps_;
    // name → [被谁依赖]
    std::unordered_map<std::string, std::vector<std::string>> rdeps_;

public:
    void record_call(std::string caller, std::string callee);
    
    // 找到所有需要重编的: 节点 + 依赖它的传递闭包
    std::vector<std::string> find_affected(std::string changed);
    
    // 清空
    void reset();
};
```

依赖分析在 IR 降低时完成 —— `lower_call` 中如果 callee 是已注册的 define 函数名，记录依赖边。

### 3.4 增量 Pass 调度

```cpp
class IncrementalPassManager {
    // 只对 changed functions 运行 passes
    // 未修改的函数跳过 (保留上次的 ck/arity/cf 结果)
    
    void run_on_changed(IRCache& cache, 
                        const std::vector<std::string>& changed);
};
```

每个 pass 记录 per-function 结果：
- `ComputeKindResult` — per-function
- `ArityCheckResult` — per-function  
- `ConstantFoldingResult` — per-function

当一个函数变了，只有它的 passes 重新执行。依赖它的函数只重新执行 passes 中受影响的部分。

---

## 4. 增量场景工作流

### 场景 1: 首次定义

```python
# 请求: (define add (lambda (x y) (+ x y)))
# 处理:
1. parse_to_flat → DefineNode
2. try_extract_define → name="add", body=lambda
3. lower_to_ir(body) → IRFunction
4. passes → ck, arity, cf
5. cache.register_function("add", func)
6. 不执行 (define 没有求值结果)
```

### 场景 2: 引用已定义的函数

```python
# 请求: (add 1 2)
# 处理:
1. parse_to_flat → CallNode(function="add", args=[1,2])
2. 检测到 "add" 在缓存中
3. cache.build_module() → [func_add, wrapper]
4. wrapper 调用 func_add
5. passes → 只跑 wrapper
6. execute → 3
```

### 场景 3: 重定义

```python
# 请求: (define add (lambda (x y) (* 2 x y)))
# 处理:
1. parse_to_flat → DefineNode(name="add")
2. 检测到 "add" 已在缓存中
3. dep_graph.find_affected("add") → ["add", "top"]
4. lower_to_ir(body) → 新 IRFunction
5. hot_swap("add", new_func) → 替换缓存
6. 对 "top" (依赖 add 的函数) 重新降低 + passes
7. 其他缓存不动
```

### 场景 4: 热替换 (M2.6)

```python
# 请求: (hot-swap (+ 1 3))
# 处理 (已有, 保持不变):
1. lower_to_ir → IRModule
2. hot_swap_function(entry_func_id, new_func)
3. closures 自动重连
4. re-execute
```

---

## 5. 数据流

```
请求字符串
    │
    ▼
parse_to_flat ──→ FlatAST
    │
    ├── DefineNode ──→ try_extract_define ──→ name + body
    │                       │
    │                       ▼
    │                   lower_to_ir ──→ IRFunction
    │                       │
    │                       ▼
    │                   passes (per-func)
    │                       │
    │                       ▼
    │                   cache[name] = func
    │                       │
    │                       ▼
    │                   dep_graph.record_call(name, callees)
    │
    └── ExprNode ──→ 查找缓存
                         │
                    ├── 全部命中 → build_module → passes(新)
                    │                               │
                    │                               ▼
                    │                           execute → result
                    │
                    └── 部分命中 → 降低未缓存部分 → build_module
                                    │
                                    execute → result
```

---

## 6. 与协议集成

### --serve 扩展

```json
// 请求: define
{"cmd": "define", "code": "(lambda (x y) (+ x y))", "as": "add"}
→ {"status": "ok", "type": "function"}

// 请求: exec (引用缓存)
{"cmd": "exec", "code": "(add 1 2)"}
→ {"status": "ok", "value": "3"}

// 请求: redefine
{"cmd": "define", "code": "(lambda (x y) (* 2 x y))", "as": "add"}
→ {"status": "ok", "affected": ["top"], "hot_swapped": true}

// 请求: 纯表达式 (不引用缓存)
{"cmd": "eval", "code": "(+ 1 2)"}
→ {"status": "ok", "value": "3"}
```

### --hot-swap (已有)

```bash
echo '(+ 1 2)' | ./aura --hot-swap    # seed cache
echo '(+ 10 20)' | ./aura --hot-swap  # swap entry function
→ 30
```

---

## 7. --hot-swap 在增量编译中的定位

```
增量编译管线:
                         ┌── define 缓存 ──→ IR Cache
请求 ──→ 解析 ──→ 分类 ──┼── 纯表达式 ──→ 全量降低 ──→ 执行
                         └── hot-swap ──→ 替换缓存 → 增量 passes → 执行
                                            │
                                            └── hot_swap_function()
                                                闭包重连
```

`--hot-swap` 是 Hot Swap Engine 的 CLI 入口。在增量编译的语境下它是"不重解析的重编译"——函数体换了但函数签名不变，闭包引用不需要更新。

---

## 8. 实现路线

### Phase 1: 定义分离（1 天）

```python
- try_extract_define: 检测 DefineNode, 分离 name + body
- eval_ir 中: 如果是 define → lower body → 缓存 → 不执行
- 如果是纯表达式 → 正常流程
```

### Phase 2: IR 缓存（1 天）

```python
- IRCache 类: register_function / hot_swap / build_module
- 解析后查找函数引用 → 命中则跳过降低
- unbound function 回退到全量降低
```

### Phase 3: 依赖追踪（1 天）

```python
- DependencyGraph 类: record_call / find_affected
- lower_call 中记录调用关系
- redefine 时级联重编依赖函数
```

### Phase 4: 增量 Pass（1 天）

```python
- PassManager 支持 per-function 调度
- ck / arity / cf 结果缓存 per-function
- 只重跑 changed + affected 的 passes
```

### Phase 5: --serve 集成（2 天）

```python
- session 概念: 保持 cache 跨请求
- agent 协议: define / exec / redefine / eval
- 错误处理: redefine 冲突检测
```

### Phase 6: 持久化（远期）

```python
- IR cache 序列化到文件
- 进程间共享 cache
- 启动时预加载热函数
```

---

## 9. 收益估算

| 场景 | 当前 | 增量编译后 | 加速比 |
|------|------|-----------|--------|
| 首次 (add 1 2) | 3ms | 3ms | 1× |
| 第二次 (add 1 2) | 3ms | 0.5ms | 6× |
| redefine add | N/A | 1ms | — |
| 大函数 (fib 20) 第一次 | 45ms | 45ms | 1× |
| 大函数 (fib 20) 第二次 | 45ms | 0.5ms | 90× |
| 修改顶层表达式 | 45ms | 1ms | 45× |

---

## 10. 与现有系统的关系

```
现有组件                    增量编译中的作用
────────────────────────────────────────────────
IRCache (新增)              集中管理所有编译后的函数
DependencyGraph (新增)       追踪函数间调用关系 → 增量重编
try_extract_define (新增)    分离定义和表达式 → 决定缓存策略
hot_swap_function (已有)     运行时替换函数体 → 热更新
Per-function passes (新增)   pass 结果 per-function 缓存
IRModule::find_callers_of    分析函数调用关系 (已有, 被 dep_graph 使用)
CompilerService::last_ir_mod_  被 IRCache 替换 (升级)
```
