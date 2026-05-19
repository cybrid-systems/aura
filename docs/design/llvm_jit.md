# D1: LLVM ORC JIT 后端 — 详细设计

**目标**：在 Aura 现有 IR 解释器旁增加 LLVM ORC JIT 编译路径，实现 10-100x 加速。

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

**推荐方案 A**，性能更好。

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

## 实现计划（5 个增量阶段）

### Phase 1: 基础架构（8h）

| 步骤 | 描述 |
|------|------|
| 1.1 | 添加 LLVM 依赖到 CMake (`find_package(LLVM)`) |
| 1.2 | 创建 `aura_jit.ixx` + `aura_jit_impl.cpp` |
| 1.3 | 实现 `AuraJIT` 类 (`LLJIT` 包装) |
| 1.4 | 编译空函数并调用 (Hello World JIT) |

### Phase 2: 简单运算（8h）

| 步骤 | 描述 |
|------|------|
| 2.1 | 实现 `lower_instruction()` — ConstI64, Add, Sub 等 |
| 2.2 | 实现 Branch/Jump 控制流 |
| 2.3 | 实现函数参数传递 + Return |
| 2.4 | 测试: `(+ 1 2)` → JIT → 3 |

### Phase 3: 闭包 + Cell（8h）

| 步骤 | 描述 |
|------|------|
| 3.1 | MakeClosure/Capture — 闭包 struct |
| 3.2 | Call — 函数指针间接调用 |
| 3.3 | NewCell/CellGet/CellSet |
| 3.4 | 测试: `(letrec ((fact ...)) (fact 10))` |

### Phase 4: 运行时集成（8h）

| 步骤 | 描述 |
|------|------|
| 4.1 | 注册运行时 bridge 函数到 JIT |
| 4.2 | PrimCall — 调用 Aura 原语 |
| 4.3 | `--jit` CLI flag |
| 4.4 | 集成到 `CompilerService::eval()` |
| 4.5 | 测试: 全表达式通过 `--jit` |

### Phase 5: 优化（8h）

| 步骤 | 描述 |
|------|------|
| 5.1 | LLVM opt passes (-O2) |
| 5.2 | 尾调用优化 (TCO) |
| 5.3 | 内联小函数 |
| 5.4 | 增量 JIT 缓存 + 重编译 |
| 5.5 | Benchmark: JIT vs IR vs tree-walker |

## 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| LLVM API 版本兼容 | 构建失败 | 环境已验证: **LLVM 22.1.5** ✅ |
| `EvalValue` 类型不兼容 LLVM IR | 值传递复杂 | 用 tagged union 替代 |
| JIT 编译开销掩盖加速 | 小函数变慢 | 函数热度阈值，只 JIT 热点 |
| Aura 原语频繁调用 C++ bridge | 频繁 FFI 边界 | inline 简单原语 (+ - * /) |

## 里程碑

| 里程碑 | 交付 | 预计 |
|--------|------|------|
| M1 | `--jit` + `(+ 1 2)` → 3 | 16h |
| M2 | `--jit` + factorial(20) → 2432902008176640000 | 24h |
| M3 | `--jit` + 全 integ 测试 87/87 | 32h |
| M4 | JIT vs IR benchmark 基线 | 40h |

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
