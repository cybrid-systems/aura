# 卫生宏系统设计

**版本**: v1.0
**状态**: 设计阶段
**对应**: M3b — defmacro + 卫生宏 + 编译期验证

---

## 1. 问题

当前宏系统工作流：

```
(defmacro unless (cond body)
  '(if (not ,cond) ,body))

(unless #f 42)  →  (if (not #f) 42)  →  42
```

**问题**: 宏展开是纯文本模板替换，不处理变量捕获：

```
(defmacro swap (x y)
  '(let ((tmp ,x))
     (set! ,x ,y)
     (set! ,y tmp)))

(let ((tmp 1) (y 2))
  (swap tmp y))    →  (let ((tmp ...)) ...)  ← tmp 被宏内部的 let 捕获！
```

`gensym` 可以手动解决，但需要程序员记住调用：

```
(defmacro swap (x y)
  (let ((g (gensym)))
    `(let ((,g ,x))
       (set! ,x ,y)
       (set! ,y ,g))))
```

**目标**: 自动卫生 —— 宏展开时自动重命名引入的绑定，消除捕获风险。

---

## 2. 设计

### 2.1 核心概念

```
宏展开时:
  1. 宏定义中引入的绑定 → 自动重命名 (gensym)
  2. 宏用户传入的表达式 → 保持原名
  3. 宏定义中引用的外部变量 → 保持原名
```

```
(defmacro swap (x y)
  '(let ((tmp ,x))       ← tmp 是宏引入的绑定
     (set! ,x ,y)        ← x, y 是用户传入
     (set! ,y ,tmp)))

展开后:
  (let ((tmp_1234 x))    ← tmp → tmp_1234 (自动重命名)
    (set! x y)
    (set! y tmp_1234))
```

### 2.2 标记系统

在 AST 节点上标记"宏引入"和"用户引入"：

```cpp
// 在 FlatAST 的 NodeView 中扩展
// 或在每个节点加一个 bool flag

enum class SyntaxMarker : uint8_t {
    User,           // 用户写的代码
    MacroIntroduced // 宏展开引入的绑定
};
```

实现方式：在 FlatAST 中新增 `marker_` 列（每个节点 1 字节）：

```cpp
// FlatAST SoA 新增列
std::pmr::vector<SyntaxMarker> marker_;

// 解析时所有节点为 User
// 宏展开时新创建的绑定节点为 MacroIntroduced
```

### 2.3 重命名机制

宏展开时，对标记为 `MacroIntroduced` 的绑定名进行重命名：

```cpp
struct MacroRename {
    std::string original;     // 原名 (tmp)
    std::string renamed;      // 新名 (tmp_1234)
    SymId renamed_sym;        // 字符串池中的 ID
};

class MacroExpander {
    std::vector<MacroRename> renames_;
    uint64_t gensym_counter_ = 0;
    FlatAST& flat_;
    StringPool& pool_;
    
    // 在宏展开的模板中，为每个引入的绑定生成新名
    SymId fresh_name(std::string_view hint) {
        auto name = std::string(hint) + "_" + std::to_string(gensym_counter_++);
        return pool_.intern(name);
    }
    
    // 展开时: 复制节点, 如果节点是 MacroIntroduced 的绑定, 重命名
    NodeId copy_and_rename(NodeId id, NodeView v) {
        switch (v.tag) {
        case NodeTag::Variable: {
            auto name = pool_.resolve(v.sym_id);
            // 检查这个变量是否是宏引入的
            auto it = macro_introduced_.find(std::string(name));
            if (it != macro_introduced_.end()) {
                // 替换为生成的名字
                return flat_.add_variable(it->second);
            }
            // 用户传入的变量: 保持原名
            return flat_.add_variable(v.sym_id);
        }
        case NodeTag::Let:
        case NodeTag::LetRec: {
            auto new_sym = pool_.intern(...);
            // 记录重命名
            macro_introduced_[name] = renamed_sym;
            // 递归复制 body
        }
        // ... 其他节点类型
        }
    }
};
```

### 2.4 和 FlatAST 的集成

```
展开前 (User 节点):
  [Variable: tmp] [Variable: x] [Variable: y]

展开后:
  [Variable: tmp_1234] ← MacroIntroduced
  [Variable: x]         ← User (保持原名)
  [Variable: y]         ← User (保持原名)
```

FlatAST 的 `add_node` 扩展：

```cpp
NodeId add_node(NodeTag tag, SyntaxMarker m = SyntaxMarker::User) {
    auto id = static_cast<NodeId>(tag_.size());
    tag_.push_back(tag);
    marker_.push_back(m);    // 新增
    // ... 其他字段
    return id;
}
```

### 2.5 和 eval_in 的集成

当前宏展开发生在 `eval_in` 的调用点 —— `expand_macro()` 返回展开后的 `Expr*`。

卫生宏的展开应该发生在**解析后、求值前**：

```
解析 → 宏展开 (全部展开) → 类型检查 → 降低 → 执行
                        ↑ 展开后的 AST 不含宏
```

这样 TypeChecker 和 Lowering 不需要知道宏的存在。

但当前设计是**按需展开**（在 `eval_in` 中遇到宏调用才展开）。这是树遍历求值器的特性。

对于卫生宏，两种方案：

**方案 A: 预展开** (推荐)
```
parse_to_flat → macro_expand_all → typecheck / lower / exec
```

- 解析后在 FlatAST 上做一次完整宏展开
- 展开后 AST 不含任何 MacroDefNode
- 后续管线完全不知道宏的存在
- TypeChecker / Lowering 不需要修改

**方案 B: 按需展开 + 卫生** (当前方案改)
- 保持当前的按需展开
- 展开时引入重命名逻辑
- TypeChecker 见到 MacroDefNode 时展开 Body 再检查

方案 A 更干净，但需要实现 FlatAST 上的宏展开器。

### 2.6 和 TypeChecker 的集成

预展开方案下，TypeChecker 不需要改——它见不到宏。

但需要支持对宏体的类型检查（宏定义验证）：

```
(defmacro swap (x y)
  '(let ((tmp ,x))
     (set! ,x ,y)
     (set! ,y ,tmp)))
```

在定义 `swap` 时，检查宏体是否合法：
- `tmp` 是宏引入的，类型 √
- `x`, `y` 是参数，类型 Any (传入时决定)
- `set!` 是否需要类型一致？

### 2.7 和增量编译的集成

```
宏展开 → 缓存展开后的 FlatAST
              │
        宏未变 → 使用缓存
        宏变了 → 重新展开
```

在 `define_function` 或宏定义时，缓存展开后的 AST。如果宏变了，失效依赖该宏的展开。

### 2.8 和 unparse 的集成

`unparse_node` 遇到 `MacroDefNode` 时：

```
(defmacro name (params ...) body)
```

展开后的宏调用在 AST 中已经是展开后的形式，unparse 输出展开后的代码。

也可以保留宏调用标记（`MacroCall` 节点），让 unparse 输出宏调用形式：

```
(swap x y)  →  (swap x y)
```

### 2.9 和 serve 协议的集成

```
{"cmd":"defmacro","name":"swap","params":["x","y"],"body":"(let ((tmp x)) ...)"}
→ {"status":"defined","name":"swap"}
```

---

## 3. 实现计划

### Phase 1: FlatAST 标记系统

```
- 新增 marker_ 列 (SyntaxMarker 枚举)
- add_node 接受标记参数
- 解析器: 所有节点标记为 User
```

### Phase 2: 宏展开器重写

```
- MacroExpander 类
- copy_and_rename: 复制子树, 自动重命名 MacroIntroduced 绑定
- expand_macro_flat: 在 FlatAST 上展开宏
```

### Phase 3: 预展开管线

```
- eval_ir/exec_with_cache 中: parse → macro_expand_all → lower → exec
- eval (树遍历): 按需展开 + 卫生
```

### Phase 4: TypeChecker 宏体检查

```
- 宏定义时检查模板合法性
- 参数类型推导
- 递归宏展开检查
```

### Phase 5: unparse + serve 集成

```
- unparse 输出 MacroDefNode
- serve 协议支持 defmacro 命令
```

---

## 4. 当前状态 vs 目标

```
能力                     当前              目标
──────────────────────────────────────────────
defmacro                 ✅ 非卫生         ✅ 卫生
gensym                  ✅ 原始            ✅ 保留 (备用)
自动重命名               ⬜               ✅ MacroIntroduced 标记
FlatAST 展开             ⬜               ✅ pre-expand
TypeChecker 宏体检查     ⬜               ✅ compile-time validation
unparse MacroDefNode    ⬜               ✅
serve defmacro 协议      ⬜               ✅
```

---

## 5. 设计决策

### 为什么不走 Racket 风格的 syntax-case?

Racket 的卫生宏系统 (`syntax-case`, `syntax->datum`, `with-syntax`) 功能强大但极其复杂。Aura 的宏系统应该：

1. **默认卫生** —— 大多数宏不需要手动 gensym
2. **支持逃生舱** —— 需要时可以用 `(syntax-aliases ...)` 显式捕获
3. **实现简单** —— 标记 + 重命名，不在 AST 中携带作用域信息

### 为什么预展开？

- TypeChecker 不需要改
- Lowering 不需要改
- IRInterpreter 不需要改
- 增量编译只需要缓存展开结果

### 如何处理多层嵌套

```
(defmacro a ...)  →  展开后包含 (b ...)
(defmacro b ...)  →  展开后包含 (c ...)
```

预展开会递归展开，直到没有 MacroDefNode 剩余。这确保 TypeChecker 见到纯净 AST。

### 性能和内存

标记列增加 1 字节/节点。展开会产生新节点（重命名后的绑定）。对于 gzipped 文件级别的 AST, 额外开销 < 5%。
