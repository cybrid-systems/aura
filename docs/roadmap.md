# Aura — 实现进度跟踪

---

## 里程碑状态

```
M1 求值器          ✅  纯 FlatAST 管线 (SoA)，无 Expr* 指针树
M2 查询引擎        ✅  ASTIndex + QueryEngine + TransformEngine
M3a 语言补全       ✅  布尔/序对/begin/set!/quote/cond/letrec/string/vector/hash
M3b 宏系统         ✅  defmacro + 卫生宏 Phase 1-5
M3c 反射           ✅  P2996 auto_to_json + kNodeMeta + 结构布局验证
M3d 类型系统       ✅  L6.1-L6.8: 渐进类型 + Occurrence + forall 多态 + Float
M3e 工具链         ✅  Benchmark(44) + 增量编译 + --serve + CI
M3f AI 闭环         ✅  mutation_loop + LLM 驱动 + AI Agent 演示(6 场景)
M4a 缓存           ✅  ABF v4 列式 (O(1) resolve + SyntaxMarker)
M4b AI 协议         ✅  docs/ai_agent_protocol.md (7 工具定义)
M4c 模块系统       ✅  import + AURA_PATH + ABF v2 全链路
M4d 自进化         ✅  Typed Mutation 三周 (设计/MutationLog/原语/AI协议)
M4e 语言完善       ✅  变参算术 + 数值基元 + 类型谓词 + 字符操作 + Quote 修复
M4f 实战测试         ✅  BST/Mergesort/Quicksort/Compose 实战验证
M5a 尾递归优化      ✅  迭代式 eval_loop (TCO 支持)
M5b 相等比较        ✅  equal? 深度递归比较
M5c 列表访问简写    ✅  cadr/caddr/cddr/cadar 等
M5d 破坏性操作      ✅  set-car!/set-cdr!
M5e 生产后端        ⬜  LLVM JIT / AOT / 自举
```

---

## 代码库统计

| 指标 | 数值 |
|------|------|
| 源文件 | 33 (.ixx + .cpp) |
| 代码行数 | ~9200 |
| CTest | 52/52 |
| Benchmark | 44/44 |
| 集成测试 | 62/62 |
| 测试套件 | 8 (build.py test all) |
| IR opcodes | 23 (含 ConstF64) |
| 运行时类型 | 9 (Void/Int/Bool/String/Float/Pair/Closure/Cell/Vector/Hash) |
| 类型系统特性 | Gradual + Occurrence + forall + Float + lub promotion |
| 语言原语 | ~70 |
| CI | GitHub Actions |

---

## 已完成功能

### 2026.05.15 Session — Float + 类型加固 + 错误体验 + 闭包修复 + forall

| 特性 | 类型 | 文件 | 行数 |
|------|------|------|------|
| **Float 支持** | P0 | 14 | +403 |
| 词法/语法/值系统 | | lexer/parser/AST/EvalValue | 3.14 字面量、double variant |
| Float 运算 | | 求值器 + IR 执行器 | int/float 自动提升 |
| IR ConstF64 | | IR opcode + lowering + executor | 双精度常量加载 |
| Float 类型检查 | | TypeChecker | Float 类型注册 + lub 提升 |
| **类型推断加固** | P1 | TypeChecker | +80 |
| let-bound lambda 推断 | | synthesize_flat_call | 算术运算自定义返回类型 |
| 约束归一化 | | infer_flat | `cs_.normalize(result)` |
| **错误体验** | P1 | 3 文件 | +96 |
| 编辑距离建议 | | evaluator_impl.cpp | `(did you mean y?)` |
| 函数名+参数个数 | | type_checker_impl.cpp | `call 'map': expected 2, got 3` |
| 导入路径报告 | | evaluator_impl.cpp | `searched in: CWD + AURA_PATH` |
| **闭包生命周期修复** | P1 | main.cpp | -2 |
| 跨行 define+调用 | | 移除 cs.reset() | `(define add ...) (add 5 7)` → 12 |
| **forall 多态** | P2 | 4 文件 | +75 |
| register_forall 完善 | | type.ixx/type_impl.cpp | 存储 var+body, 实例化 |
| map/filter 类型推断 | | type_checker_impl.cpp | `∀a b. ((a→b), Any)→b` |
| **Typed Mutation 设计** | P3 | docs/typed_mutation_design.md | 三阶段实现路径 |

### 之前已完成

| 特性 | 类型 | 说明 |
|------|------|------|
| 哈希表 (Swiss table) | P0 | 8 原语 + string key 支持 |
| Float/Pair forall 启用 | P0 | TypeTag 注册 |
| IR 缓存序列化 | P1 | ABF v4 Phase3 |
| --cache-open 执行 | | 跳过 parse+lowering |
| import 原语 + 模块系统 | | ABF v2 全链路 |
| AURA_PATH | | 搜索路径 + .aura 自动扩展 |
| 卫生宏 Phase 1-5 | | SyntaxMarker → 展开 → 克隆 → 类型检查 |
| 向量类型 | | vector/vector-ref/vector-set!/vector-length/IR |
| IR string 原语 | | string-append/length/ref/substring/compare |
| 宏警告 | | 未使用参数 + 常量体 |

---

## 现有能力概览

| 能力 | 状态 | 说明 |
|------|------|------|
| 变参算术 | ✅ | `(+ 1 2 3)` → 6 |
| 数值基元 | ✅ | modulo/quotient/remainder/abs/gcd/lcm/min/max |
| 类型谓词 | ✅ | integer?/float?/boolean?/number?/symbol?/procedure? |
| 字符操作 | ✅ | char?/char->integer/integer->char/string->list/list->string |
| I/O | ✅ | read/read-line/write/display/newline/eof-object? |
| let* | ✅ | 变量依赖绑定 |
| named let | ✅ | `(let loop ((i 0) ...) ...)` — 循环模式 |
| `;` 注释 | ✅ | 行注释 |
| `()` sentinel | ✅ | null?/length/list? 同时识别 void 和 int 0 |
| take/drop/foldl | ✅ | 基础列表原语 |
| define 链 | ✅ | 顺序 define 跨 eval 调用正确 |
| 尾递归 | ✅ | 迭代式 eval_loop，大递归不爆栈 |
| equal? | ✅ | 深度递归比较序对/列表/向量 |
| cadr/caddr | ✅ | 标准 cXr 缩写 |
| set-car!/set-cdr! | ✅ | 破坏性列表操作 |
| 管道多行输入 | ✅ | S-表达式分割器 |
| display 递归打印 | ✅ | `(display '(1 2 3))` → `(1 2 3)` |

## 下一步计划

### 🟡 P6 — 模式匹配 + 记录
- `(match expr [pattern body]...)` 降级为 if+car/cdr
- `define-record-type` 或 `define-struct` 命名结构体

### 🟡 P6 — 标准库骨架
- `std/math.aura` (pi, sq, exp, sin/cos)
- `std/list.aura` (foldr, zip, take-while, partition, sort)
- `std/string.aura` (split, join, trim, replace, reverse)
- `std/json.aura` (parse, stringify)

### 🟢 P7 — LLVM JIT 后端
- 从 IR 管线到 LLVM IR 的降级
- `--jit` 模式
- 与 --serve 集成
