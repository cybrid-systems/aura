# Aura 内核迭代标准

**版本**：v1.0
**原则**：每步新增一个语言特性，必须穿透完整的 Trees that Grow 管线。
**方法**：测试先行（TDD），基础设施同步，ABF/查询引擎/IR/SoA 全线覆盖。

---

## 1. 新增语言特性的标准管线

每加一个新特性（如字符串、列表库、向量等），必须穿透 **12 层**：

```
Layer  组件              文件                责任
────  ────────────────  ─────────────────  ─────────────────────────
  1   AST 数据结构       src/core/ast.ixx   定义新节点类型 + variant
  2   SoA 元数据         src/core/ast_flat   kNodeMeta 表 + FlatAST 构造器
  3   指针解析器          src/parser/*       解析新语法产生 AST 节点
  4   扁平解析器          src/parser/*       扁平 AST 版本（与 3 同步）
  5   求值器             src/compiler/*      eval_in 分支 + primitives
  6   ABF 序列化 (Racket) lang/private/*     TAG + write-xxx
  7   ABF 反序列化 (C++)  src/binary/*        read_xxx + dispatch 表
  8   Lowering           src/compiler/*      lower_xxx + free vars
  9   Reconstruct        src/compiler/*      FlatAST → Expr* 重建
 10   查询引擎            src/compiler/*      tag 解析 + node-type 匹配
 11   Flatten            src/core/*          Expr* → FlatAST
 12   测试               tests/*             TDD 红线 + ABF E2E + ctest
```

**强制规则**：第 12 层（测试）必须在第 1 层之前写。

---

## 2. 开发流程

### Step 0: 写红线测试（TDD）

在 `tests/` 下创建测试文件，定义新特性的红线：

```cpp
// tests/test_string.cpp — Step 16 红线
// 编译: g++ -std=c++26 -freflection ...
// 运行: ctest -R test_string

// 红线清单（每个特性 5-15 个测试用例）:
//   1. 基础字面量: "hello" → 字符串
//   2. 类型谓词: (string? "hello") → #t
//   3. 操作: (string-length "hello") → 5
//   4. 操作: (string-ref "hello" 0) → 104
//   5. 组装: (string-append "a" "b") → "ab"
//   6. 比较: (string=? "a" "a") → #t
//   7. ABF 管线: #lang aura → racket --abf → ./aura --abf → 相同结果
//   8. 查询: (query (node-type LiteralString)) → 匹配
//   9. IR 模式: --ir 不崩溃（可返回 0）
//  10. serve 模式: --serve 返回 JSON
```

红线必须覆盖所有层：
- 2-3 个解析/求值测试
- 1 个 ABF E2E 测试
- 1 个查询测试
- 1 个 IR 稳定性测试

### Step 1-11: 逐层实现

**顺序**：AST → SoA → 解析器 → ABF → 求值器 → 查询 → 测试

每实现一层后运行已有的所有测试，确保没有 regression。

### 红线验证

红线测试全部通过后，运行 `ctest` 确保 39+ 测试全绿。

---

## 3. 测试覆盖标准

### 每个特性必须有的测试类型

| 测试类型 | 数量 | 说明 |
|----------|------|------|
| 基础功能 | 3-5 | 正常使用场景 |
| 边界条件 | 2-3 | 空值、越界、异常参数 |
| 类型检查 | 1-2 | 类型谓词的正/反面 |
| ABF E2E | 1-2 | Racket → ABF → C++ 闭环 |
| 查询 | 1 | `(node-type Xxx)` 能匹配 |
| Regression | 1 | 已有测试全绿 |

### 测试文件组织

```
tests/
├── step-09-10.t        # 布尔/序对 (Racket)
├── step-11-14.t        # begin/set!/quote/cond
├── step-15.t           # defmacro（开发中）
├── step-16-18.t        # 字符串 ← 新的
├── step-19-22.t        # 列表库（规划）
├── validate_abf_nodes.cpp  # P2996 结构验证
├── test_ir.cpp              # IR 管线测试
├── agent_demo.rkt           # AI Agent 演示
└── reflect_json_demo.cpp    # P2996 反射演示
```

---

## 4. 模块同步清单

每新增一个语言特性，用以下清单逐项核对：

```
特性: ____________  日期: ____________

Redlines (TDD):
  [ ] 红线测试已写（在实现之前）
  [ ] 红线覆盖所有 12 层

Layer 1: src/core/ast.ixx
  [ ] NodeTag 枚举值
  [ ] 节点结构体
  [ ] variant 条目
  [ ] Expr 构造函数

Layer 2: src/core/ast_flat.ixx
  [ ] kNodeMeta 表条目
  [ ] kNodeMeta 数组大小更新
  [ ] FlatAST::add_xxx 构造器
  [ ] reflect/type_validate.hh P2996 验证条目

Layer 3: src/parser/parser_impl.cpp
  [ ] 关键字检查（如适用）
  [ ] parse_xxx 函数

Layer 4: src/parser/flat_parser_impl.cpp
  [ ] 关键字检查
  [ ] parse_xxx 函数

Layer 5: src/compiler/frontend_impl.cpp
  [ ] eval_in 分支
  [ ] primitives（如适用）

Layer 6: lang/private/abf.rkt
  [ ] TAG-XXX 常量
  [ ] tag-for-expr 映射
  [ ] write-xxx 函数
  [ ] write-node dispatch

Layer 7: src/binary/abf_deserializer_impl.cpp
  [ ] read_xxx 函数
  [ ] 注册到 register_all_readers

Layer 8: src/compiler/lowering_impl.cpp
  [ ] lower_xxx dispatch
  [ ] collect_free_vars case

Layer 9: src/compiler/lowering_flat_impl.cpp
  [ ] reconstruct_node case

Layer 10: src/compiler/query.ixx + query_impl.cpp
  [ ] ReplaceTemplate op 解析
  [ ] QueryExpr node_type 分支
  [ ] QueryEngine by_tag 分支

Layer 11: src/core/ast_impl.cpp
  [ ] flatten_expr case

Layer 12: tests/
  [ ] 红线测试（TDD）
  [ ] ABF E2E 测试
  [ ] ctest 全绿

Layer 12b: src/compiler/type_checker.ixx (L6.3+)
  [ ] TypeChecker 模块接口
  [ ] infer_type 实现
  [ ] check_type 实现

Special:
  [ ] ABF ABF 端到端（racket --abf | ./aura --abf）
  [ ] --serve 不崩溃
  [ ] --query '(node-type Xxx)' 匹配
  [ ] type_redlines.txt 红线测试 (TDD)
```

---

## 5. 语言内核状态看板

```
特性           AST SoA解析扁平求值ABF_R ABF_C Lo Re Q Fl Te 完成?
──────────────────────────────────────────────────────────────
整数           ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12
变量           ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12
lambda         ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12
if             ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12
算术原语       ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12
布尔           ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12
序对           ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12
begin          ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12
set!           ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12
quote          ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12
cond           ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12
defmacro       ✅ ✅ ⚠  ⚠  ⚠  ⚠  ⚠  ⚠  ⚠  ✅ ⚠  ⚠   4/12
字符串         ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12 ✅
列表库          ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12 ✅
I/O (display/  ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12 ✅
  newline)
类型标注        ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅  12/12 ✅
(TypeAnnotation)

符号说明: ✅=完成 ⬜=未开始 ⚠=部分实现
```

---

## 6. 示例：Step 16-18 字符串的完整管线

以字符串为例展示完整 12 层覆盖：

```
printf '"hello"' | ./build/aura          → 134217728  ✅ 求值
printf '#lang aura\n"hello"' |            → 134217728  ✅ ABF
  racket -l aura -- --abf | ./aura --abf                
echo '"hello"' | ./aura --serve          → JSON       ✅ serve
echo '"hello"' | ./aura --query          → 0 matches  ✅ 查询
  '(node-type LiteralString)'                         （在只有整数的表达式里是对的）
ctest                                    → 39/39      ✅ 全绿
```

---

### 示例：L6.2 TypeAnnotationNode 的完整管线

以 `(: x Int)` 为例展示类型标注的 12 层覆盖 + 反射验证：

```
echo '(: x Int)' | racket -l aura -- --abf
                 → hex: 41 42 46 32 02 00 0F ... (TAG-TYPE-ANNOTATION 0x0F)
echo '(: x Int)' | racket -l aura -- --abf | ./aura --abf
                 → eval error: unbound variable: x  ✅ annotation stripped
echo '(begin (define x 42) (: x Int))' | racket -l aura -- --abf | ./aura --abf
                 → 42  ✅ value through annotation
./aura --serve '(node-type TypeAnnotation)'
                 → query result: 0  ✅ query supports tag
./build/validate-abf-nodes
                 → 13/13 passed  ✅ P2996 layout validation
ctest            → 39/39 ✅
```
---

## 7. 与设计文档的关系

| 文档 | 关系 |
|------|------|
| `aura_language_plan.md` | 定义"做什么"——Ghuloum 步骤 16-35 |
| `kernel_iteration_standard.md` | 定义"怎么做"——12 层管线标准 |
| `aura_tree_grow.md` | 定义"为什么这样设计"——Trees that Grow |
| `aura_reflection_plan.md` | 反射工具链的长期路线 |

---

> **每步可测试，每层不失联。测试先写，逐层过关。**
