# Aura — Trees that Grow: 新增语法端到端流程

**版本**：v1.0
**对应**：Phase 3a (Ghuloum Step 9-13) 完整管线验证
**设计理念**：每个语言构造都是 Trees that Grow 的一个 Phase，必须穿透全部 8 层。

---

## 1. 为什么需要这个文档

Aura 的核心设计模式是 Trees that Grow（Peyton Jones 等, Haskell Symposium 2014）。
每个新语法形式不是"加一个分支"——它是一个贯穿整个编译管线的 **Phase**。

Phase 3a 验证了这个模式：`begin`/`set!`/`quote`/`cond` 四个构造总共修改了 **17 个文件**。

```
一个 Phase 影响多少代码？
─────────────────────────
  ast.ixx          ─ 新节点数据结构 (2-5 行)
  ast_flat.ixx     ─ SoA 元数据表 + 构造器 (10-15 行)
  parser.ixx       ─ 新方法声明 (1 行)
  parser_impl.cpp  ─ 解析逻辑 (10-30 行)
  flat_parser_impl.cpp ─ 扁平解析器 (10-30 行)
  frontend.ixx     ─ 求值器方法声明 (1 行)
  frontend_impl.cpp ─ 求值逻辑 (10-30 行)
  abf.rkt          ─ Racket 端序列化 (10-20 行)
  abf_deserializer.ixx  ─ C++ 端声明 (1 行)
  abf_deserializer_impl.cpp ─ C++ 端反序列化 (10-20 行)
  lowering.ixx     ─ Lowering 声明 (1 行)
  lowering_impl.cpp ─ Lowering 逻辑 + free vars (15-30 行)
  lowering_flat_impl.cpp ─ reconstruct_node (10-20 行)
  query.ixx        ─ 查询引擎 tag 注册 (3 行)
  query_impl.cpp   ─ 查询引擎 dispatch (3 行)
  ast_impl.cpp     ─ flatten_expr (5-15 行)
─────────────────────────
  16-17 个文件, 约 100-250 行新增
```

---

## 2. 管线全景

```
                   新增语法构造 (Phase)
                          │
                          ▼
              ┌─────────────────────┐
              │  1. AST 数据结构     │  src/core/ast.ixx
              │  NodeTag + struct   │  新枚举值 + variant 条目 + 构造函数
              └─────────┬───────────┘
                        │
              ┌─────────▼───────────┐
              │  2. SoA 元数据      │  src/core/ast_flat.ixx
              │  NodeMeta 表 + 构建器│  kNodeMeta 数组 + add_* 方法
              └─────────┬───────────┘
                        │
              ┌─────────▼───────────┐     ┌──────────────────┐
              │  3a. 指针解析器      │     │  3b. 扁平解析器   │
              │  parser_impl.cpp    │     │  flat_parser.cpp │
              │  parse_xxx()        │     │  同样实现        │
              └─────────┬───────────┘     └────────┬─────────┘
                        │                         │
                        └──────────┬──────────────┘
                                   │
                    ┌──────────────▼──────────────┐
                    │  4a. Tree-walker 求值器      │
                    │  frontend_impl.cpp          │
                    │  eval_in 新分支              │
                    └──────────────┬──────────────┘
                                   │
              ┌────────────────────┼────────────────────┐
              │                    │                    │
              ▼                    ▼                    ▼
    ┌──────────────────┐  ┌────────────────┐  ┌──────────────────┐
    │  5a. ABF 序列化   │  │  5b. ABF 反序列 │  │  5c. ABF tag     │
    │  abf.rkt (Racket) │  │  abf_deser.cpp │  │  一致 (0x0X)     │
    └──────────────────┘  └────────────────┘  └──────────────────┘
              │                    │
              └────────────────────┘
                                   │
              ┌────────────────────┼────────────────────┐
              │                    │                    │
              ▼                    ▼                    ▼
    ┌──────────────────┐  ┌────────────────┐  ┌──────────────────┐
    │  6. Lowering      │  │  7. Reconstruct │  │  8. Query Engine │
    │  lowering_impl   │  │  low_flat_impl │  │  query.ixx       │
    │  IR 指令生成     │  │  FlatAST→Expr*  │  │  Tag 匹配/变换   │
    └──────────────────┘  └────────────────┘  └──────────────────┘
```

---

## 3. 每层详解

### Layer 1: AST 数据结构 (`src/core/ast.ixx`)

新语法形式的"身份证明"。每加一种新构造，做三件事：

```cpp
// 1. 新增 NodeTag 枚举值
enum class NodeTag : std::uint32_t {
    ...,
    Begin = 0x09, Set = 0x0A, Quote = 0x0B,
};

// 2. 新增节点结构体
struct BeginNode { NodeTag tag; std::vector<Expr*> exprs; };

// 3. 加入 variant 并添加构造函数
std::variant<..., BeginNode, SetNode, QuoteNode> payload;
Expr(BeginNode n) : tag(n.tag), payload(n) {}
```

### Layer 2: SoA 元数据 (`src/core/ast_flat.ixx`)

扁平 AST (FlatAST / SoA) 需要知道新节点的布局：

```cpp
// 1. kNodeMeta 表新增条目
constexpr std::array<NodeMeta, 11> kNodeMeta = {{
    ...
    {NodeTag::Begin, "Begin", 0, true,  false, false, false},
    //                       ↑fixed  ↑var    ↑name  ↑int  ↑params
    //                        孩子数  孩子   有符    有整数 有参数
    //                               向量   号名           列表
}};

// 2. FlatAST 添加构造器
NodeId add_begin(NodeId* exprs, uint32_t count) {
    auto id = add_node(NodeTag::Begin);
    ...
    child_count_[id] = count;
    return id;
}
```

**注意**：每加一种新构造，`kNodeMeta` 数组的模板参数必须同步更新：
```
std::array<NodeMeta, 8> kNodeMeta  →  std::array<NodeMeta, 11> kNodeMeta
                          ↑                          ↑
                      原来 8 种节点                 现在 11 种
```

### Layer 3a/3b: 解析器

两个解析器（指针树 + 扁平 SoA）必须同步实现新构造的解析。

**指针解析器** (`parser_impl.cpp`)：

```cpp
// parse_list() 中增加关键字检查
if (kw == "begin") return parse_begin();
if (kw == "set!")  return parse_set();

// 新增解析函数
ast::Expr* Parser::parse_begin() {
    lexer_->consume();  // 消耗 'begin' 关键字
    ast::BeginNode begin{{}};
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto* e = parse_expr();
        if (e) begin.exprs.push_back(e);
    }
    lexer_->consume();  // 消耗右括号
    return arena_.create<ast::Expr>(std::move(begin));
}
```

**扁平解析器** (`flat_parser_impl.cpp`) — 结构和指针解析器一致，但返回 `NodeId` 并调用 `flat_.add_xxx()`。

### Layer 4: 求值器 (`frontend_impl.cpp`)

```cpp
if constexpr (std::is_same_v<T, ast::BeginNode>) {
    ast::Expr* last = nullptr;
    for (auto* e : n.exprs) {
        auto v = eval_in(e, env);
        if (!v) return v;
        last = e;
    }
    return last ? eval_in(last, env) : EvalResult(0);
}
```

**模式**：`if constexpr (std::is_same_v<T, ast::XXXNode>)` 分支。

### Layer 5a/5b: ABF 序列化/反序列化

**Racket 端** (`lang/private/abf.rkt`)：

```racket
(define TAG-BEGIN #x09)
(define TAG-SET   #x0A)

(define (tag-for-expr expr)
  (case (car expr)
    ...
    [(begin) TAG-BEGIN]
    [(set!)  TAG-SET]))

(define (write-begin buf expr phase-id)
  (define exprs (cdr expr))
  (put-varint! buf (length exprs))
  (for ([e exprs]) (write-node buf e phase-id)))
```

**C++ 端** (`abf_deserializer_impl.cpp`)：

```cpp
case 0x09: return read_begin(r);

ast::Expr* ABFDeserializer::read_begin(Reader& r) {
    auto count = r.read_varint();
    ast::BeginNode begin{{}, {}};
    for (uint64_t i = 0; i < count; ++i)
        begin.exprs.push_back(read_node(r));
    return arena_.create<ast::Expr>(std::move(begin));
}
```

**ABF Tag 分配规则：**
```
0x01-0x08: Phase 1-2 原有节点 (LiteralInt / Variable / Call / If /
            Lambda / Let / LetRec / Define)
0x09-0x0B: Phase 3a 新增 (Begin / Set / Quote)
0x0C-0x1F: 预留给 Phase 3b (宏系统) 和未来 Phase
```

### Layer 6: Lowering (`lowering_impl.cpp`)

将新 AST 节点转换为 IR 指令序列：

```cpp
std::uint32_t LoweringPass::lower_begin(const ast::BeginNode& node) {
    std::uint32_t last_slot = 0;
    for (auto* e : node.exprs)
        last_slot = lower_expr(e);
    return last_slot;
}
```

同时需要在 `collect_free_vars` 中递归遍历新节点的子表达式。

### Layer 7: Reconstruct (`lowering_flat_impl.cpp`)

扁平 AST → Expr* 的反向转换：

```cpp
case NodeTag::Begin: {
    ast::BeginNode begin{v.tag, {}};
    for (size_t i = 0; i < v.children.size(); ++i)
        begin.exprs.push_back(reconstruct_node(v.child(i), ...));
    return arena.create<Expr>(std::move(begin));
}
```

### Layer 8: 查询引擎 (`query.ixx` + `query_impl.cpp`)

让新节点可通过 AuraQuery 查询和变换：

```cpp
// query.ixx — 模板匹配
if (op == "Begin") r.tag = NodeTag::Begin;

// query_impl.cpp — 节点类型查询
else if (s == "Begin") q.node_tag = NodeTag::Begin;
```

---

## 4. Phase 3a 验证数据

| 指标 | 数值 |
|------|------|
| 新增文件 | 0 (纯扩展现有文件) |
| 修改文件数 | 17 |
| 总新增行数 | ~360 |
| 每构造平均行数 | ~90 |
| 新增测试 | 5 (begin/set!/quote/cond/复合) |
| 回归测试 | 35/36 通过 (1 pre-existing) |

### 管线贯通耗时

```
层              begin    set!    quote   cond
──────────────  ─────   ─────   ─────   ─────
AST 定义        1 min   1 min   1 min   0 (→ nested if)
SoA 元数据      5 min   5 min   5 min   0
指针解析器       3 min   3 min   2 min   5 min
扁平解析器       5 min   3 min   2 min   5 min
求值器          5 min   8 min   1 min   0
ABF Racket     2 min   2 min   2 min   0
ABF C++        2 min   2 min   2 min   0
Lowering       3 min   5 min   1 min   0
Reconstruct    2 min   2 min   2 min   0
Query Engine   1 min   1 min   1 min   0
───────────────────────────────────────────
累计            29 min  32 min  19 min  10 min
```

**cond 最便宜**（纯语法糖，→ nested if），**set! 最贵**（需要环境变异逻辑）。

---

## 5. 新 Phase 加入检查清单

每加一个新语法构造，逐项核对：

- [ ] `src/core/ast.ixx`: NodeTag + struct + variant + constructor
- [ ] `src/core/ast_flat.ixx`: kNodeMeta 条目 + 数组大小更新 + add_* 方法
- [ ] `src/parser/parser.ixx`: parse_xxx 声明
- [ ] `src/parser/parser_impl.cpp`: parse_list() 关键字 + parse_xxx() 实现
- [ ] `src/parser/flat_parser_impl.cpp`: 同样内容
- [ ] `src/compiler/frontend.ixx`: eval_in 方法声明（如需要）
- [ ] `src/compiler/frontend_impl.cpp`: eval_in 分支
- [ ] `lang/private/abf.rkt`: TAG-XXX + write-xxx + write-node dispatch
- [ ] `src/binary/abf_deserializer.ixx`: read_xxx 声明
- [ ] `src/binary/abf_deserializer_impl.cpp`: read_xxx + switch case
- [ ] `src/compiler/lowering.ixx`: lower_xxx 声明
- [ ] `src/compiler/lowering_impl.cpp`: lower_xxx + collect_free_vars
- [ ] `src/compiler/lowering_flat_impl.cpp`: reconstruct_node case
- [ ] `src/compiler/query.ixx`: tag match 分支
- [ ] `src/compiler/query_impl.cpp`: by_tag 分支
- [ ] `src/core/ast_impl.cpp`: flatten_expr case
- [ ] `tests/`: 新增红线测试

---

## 6. 设计原则总结

1. **不要走捷径** — 每个新构造必须穿透所有 8 层。跳过一层就是技术债。
2. **两个解析器同步** — 指针解析器和扁平解析器必须同时更新。
3. **cond 用语法糖** — 可以 desugar 成已有构造的，优先 desugar。
4. **Phase 编号对应 NodeTag** — 0x01-0x08 Phase 1-2, 0x09-0x0B Phase 3a, 0x0C+ 预留给未来。
5. **红线先行** — 在实现之前先写好红线测试。

---

> "每加一个构造，整个编译器都要认识它。这不是代码量问题，是设计纪律问题。"
