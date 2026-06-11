# D1: LLVM ORC JIT 后端 — 详细设计

**目标**：在 Aura 现有 IR 解释器旁增加 LLVM ORC JIT 编译路径，实现 10-100x 加速。

---

## 0. Implementation Status (2026-06-11, Issue #156)

**重要**：本文档的 **Phase 1-5 全部实装**（✅ 全部完成 ✅）；M1-M4 里程碑全部命中（fib-20 7.55x 加速）。准确分两层：

### C++ Core Layer (`src/compiler/aura_jit.ixx` / `aura_jit_impl.cpp` / `ir.ixx` / `service.ixx`)

| 组件 | 实装 | 备注 |
|------|------|------|
| `AuraJIT` (LLJIT + ThreadSafeContext + ExecutionSession) | ✓ | Phase 1 |
| `compile_function` 入口 (LLVM FunctionType + alloca locals) | ✓ | Phase 2 |
| 算术 / 比较 / 控制流 (38 opcode) | ✓ | Phase 2 — `Builder.CreateAdd/Sub/Mul/SDiv` + ICmp |
| 闭包 + Cell (捕获修复 / 递归闭包 / MakePair/Car/Cdr) | ✓ | Phase 3 |
| `PrimCall` 运行时 bridge | ✓ | Phase 4 — 调用 C++ primitive bridge |
| 运行时符号注册 (`add_runtime_symbols`) | ✓ | Phase 4 — `aura_jit_primitive_call` / `_alloc_pair` / `_alloc_string` / `_make_closure` / `_cell_new` / `_error` |
| `display` / `eval` 集成 | ✓ | Phase 4 |
| `--jit` flag (`service.ixx` `jit_mode_`) | ✓ | Phase 4 |
| LLVM `-O2` PassBuilder | ✓ | Phase 5 |
| 增量 cache (per-function `ir_cache_` + `invalidate_function`) | ✓ | Phase 5 |
| `CastOp` JIT 化 | ✓ | Phase 5 — IR CastOp 编译到 LLVM IR |
| `hot_swap_function` (incremental) | ✓ | Phase 5 — 依赖变化时重编译 |
| AOT 路径 (JITFunction → .o → 链接) | 🟡 (设计) | §"AOT 路径" 描述，但未实装 |
| 跨 host 共享 JIT cache | 🔴 | 未实装 |

### Aura Layer (无 EDSL 包装)

JIT 是 **C++ 内部优化**，通过 `--jit` 编译 flag 启用。Aura 代码本身不感知 JIT / IR Interpreter 的差异。

### 已实现 vs 计划

- ✅ **Phase 1-5 全部完成**（AuraJIT / 算术 / 闭包 / 运行时 / 优化）
- ✅ **M1-M4 全部命中**：`(+ 1 2)` → 3、factorial(20) → 2432902008176640000、integ 测试全过（加速比会随 workload 变化，当前以 §0 表格为准）
- 🟡 **设计稿未实装**：AOT 路径
- 🔴 **未做**：跨 host 共享 JIT cache

**AI Agent 读者请注意**：本文档作为设计意图 + 实装说明保留。`--jit` 编译 flag 在 `python3 build.py` 调用时启用；Aura 代码本身不感知 JIT / IR Interpreter 差异，不要假设某条 IR 指令在 Aura 层有对应原语。

---


## 架构概览

```
          Aura IR Module
               │
      ┌────────┴────────┐
      │                 │
      ▼                 ▼
  IRInterpreter    LLVM JIT (新增)
  (现有, 1.87x)    (编译到原生码)
      │                 │
      │           ┌─────┴──────┐
      │           │            │
      │           ▼            ▼
      │       ORC JIT     MachineCode
      │        (JIT)       (AOT存盘)
      │           │
      └───┬───────┘
          │
  统一 EvalResult 返回值
```

## 架构分层

### Layer 1: IR → LLVM IR 降维（Instruction Lowering）

每个 Aura IR opcode → LLVM IR 函数/指令。

| Aura Opcode | LLVM IR 策略 |
|-------------|-------------|
| ConstI64 | `ConstantInt::get(Int64Ty, val)` |
| ConstF64 | `ConstantFP::get(DoubleTy, val)` |
| ConstBool | `ConstantInt::get(Int1Ty, val)` |
| Add/Sub/Mul/Div | `Builder.CreateAdd/Sub/Mul/SDiv` (with type check) |
| Eq/Lt/Gt/Le/Ge | `Builder.CreateICmpEQ/SLT/SGT/...` |
| And/Or/Not | `Builder.CreateAnd/Or/Not` |
| Branch/Jump | `Builder.CreateCondBr/Br` |
| Call/Apply | 闭包调用 → 通过 ORC JIT 符号查找 |
| MakeClosure | 创建闭包 struct (fn_ptr + env*) |
| Capture | 填充闭包 env 字段 |
| NewCell/CellGet/CellSet | `alloca` + load/store |
| MakePair/Car/Cdr | `malloc` + 结构体字段访问 |
| Return | `Builder.CreateRet` |
| PrimCall | 调用 C++ bridge 函数 |

### Layer 2: 函数编译（Function Compilation）

```
AuraIRFunction → JITFunction
  1. 创建 LLVM FunctionType (根据参数数)
  2. 创建函数入口 alloca 所有 locals
  3. 逐 block/instruction 降维
  4. 函数尾插入 Return
  5. 调用 ORC JIT addModule
  6. 返回函数地址 (JITTargetAddress)
```

### Layer 3: 运行时集成（Runtime Integration）

```
CompilerService::eval() 中:
  if (jit_mode_ && lowered_module.can_jit()) {
    auto fn_ptr = jit_.compile_function(*ir_mod, entry_func_id);
    auto result = fn_ptr(args...);
    return wrap_eval_result(result);
  }
  // 否则走 IRInterpreter
  return ir_interp.execute();
```

### Layer 4: LLVM ORC JIT 基础设施

```
class AuraJIT {
  LLJITBuilder builder;
  ThreadSafeContext ctx;
  ExecutionSession es;
  JITDylib &mainJD;

  // 编译单个函数
  JITTargetAddress compile(const IRFunction& fn);

  // 添加运行时符号 (primitive bridge, alloc, etc.)
  void add_runtime_symbols();
};
```

## 值表示（Value Representation）

LLVM IR 中 Aura 值的类型选择：

**方案 A：Tagged Union（推荐）**

```llvm
; Aura value: { i64 payload, i8 tag }
%AuraValue = type { i64, i8 }

; Tags
; 0 = int,    1 = float,  2 = bool
; 3 = string, 4 = pair,   5 = closure
; 6 = cell,   7 = void
```

每条 IR 指令检查 tag → 读取 payload。

**优化**：整数的 tag=0 时可省略检查（最常见路径），使用 `llvm.assume` hint。

**方案 B：`EvalValue` 直传**（更简单）
- 直接传递 `EvalValue` (std::variant) 指针
- 调用 C++ helper 函数进行类型判断和操作
- 更简单但会频繁跨越 FFI 边界

**最终方案**：方案 A（Tagged Union），但简化了实现 — IRInterpreter 的 EvalValue 通过 `aura_jit_runtime.cpp` 的运行时 bridge 传递到 JIT 编译代码。

## 运行时 Bridge

JIT 编译的函数需要调用 Aura 运行时函数：

| 运行时函数 | 作用 |
|-----------|------|
| `aura_jit_primitive_call(slot, args)` | 调用 Aura 原语 |
| `aura_jit_alloc_pair(car, cdr)` | 分配 pair |
| `aura_jit_alloc_string(data)` | 分配字符串 |
| `aura_jit_make_closure(func_id, env)` | 创建闭包 |
| `aura_jit_cell_new()` | 创建 cell |
| `aura_jit_error(msg)` | 抛出错误 |

这些函数编译到 JIT 的符号表。

## AOT 路径（可选扩展）

```
JITFunction → 序列化 → .o 文件
  → 链接 → 共享对象 → dlopen
  → 或静态链接到 Aura 运行时
```

AOT 适合生产部署，不需要运行时 JIT 编译。

## 增量编译兼容

现有增量编译（`ir_cache_` + 依赖追踪）与 JIT 兼容：
- 每个 cached function 缓存 JIT 编译结果
- 依赖变化时 `invalidate_function()` 清除 JIT 缓存
- `hot_swap_function()` 重新编译 → 更新函数指针

## 实现状态（全部完成 ✅）

### Phase 1-5 全部完成

| Phase | 内容 | 状态 |
|-------|------|:----:|
| 1 | 基础架构: AuraJIT, ORC LLJIT | ✅ |
| 2 | 算术: 38 opcode, 控制流, 比较 | ✅ |
| 3 | 闭包+Cell: 捕获修复, 递归闭包, MakePair/Car/Cdr | ✅ |
| 4 | 运行时: PrimCall bridge, display, eval 集成, --jit flag | ✅ |
| 5 | 优化: LLVM -O2 PassBuilder, 增量 cache, CastOp JIT | ✅ |

### Benchmark

```
fib-20: TW 48.6ms → IR 23.0ms → JIT 6.4ms (7.55x)
```

## 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| LLVM API 版本兼容 | 构建失败 | 环境已验证: **LLVM 22.1.5** ✅ |
| `EvalValue` 类型不兼容 LLVM IR | 值传递复杂 | 用 tagged union 替代 |
| JIT 编译开销掩盖加速 | 小函数变慢 | 函数热度阈值，只 JIT 热点 |
| Aura 原语频繁调用 C++ bridge | 频繁 FFI 边界 | inline 简单原语 (+ - * /) |

## 里程碑（全部完成 ✅）

| 里程碑 | 交付 | 状态 |
|--------|------|:----:|
| M1 | `--jit` + `(+ 1 2)` → 3 | ✅ |
| M2 | `--jit` + factorial(20) → 2432902008176640000 | ✅ |
| M3 | `--jit` + 全 integ 测试通过 | ✅ |
| M4 | JIT vs IR benchmark: fib-20 7.55x | ✅ |

## 文件清单

```
src/compiler/
├── aura_jit.ixx          ← 新增: JIT 模块声明
├── aura_jit_impl.cpp     ← 新增: JIT 实现
├── ir.ixx                ← 修改: 增加 can_jit() 标志
├── service.ixx           ← 修改: jit_mode_, --jit flag
CMakeLists.txt            ← 修改: find_package(LLVM), 链接 LLVM
```

## 测试

```bash
echo '(+ 1 2)' | ./build/aura --jit      # → 3
echo '(fact 10)' | ./build/aura --jit     # → 3628800
python3 build.py test all --jit           # 全量测试
```
