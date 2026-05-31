# 统一 Cell Heap 方案

## 问题

IR 解释器和 tree-walker 各自有独立的 cell 存储：

```
Tree-walker:  Evaluator::cells_     (vector<EvalValue>)
IR:           IRInterpreter::cell_heap_ (unordered_map<uint64_t, EvalValue>)
```

`(define n 3)` 在 `cells_` 里分配 cell 0；`(set! n 5)` 走 IR 时却在 `cell_heap_` 里新建 cell 0。两个 cell ID 空间相互不可见。

## 方案：共享 vector + 索引访问

统一后的架构：

```
shared_cells_ (vector<EvalValue>) — 归 Evaluator 所有，IRInterpreter 引用
```

- `alloc_cell(v)` → `shared_cells_.push_back(v)`；返回 `size()-1` 作为 cell ID
- `cell_set(id, val)` → `(*shared_cells_)[id] = val`
- `cell_get(id)` → `(*shared_cells_)[id]`

### 修改文件

#### 1. `evaluator.ixx` — 所有权

```
- vector<EvalValue> cells_;                  // 原有，归Evalutor
+ vector<types::EvalValue>* shared_cells_;   // 指针，允许IR解释器引用
+ vector<types::EvalValue> cells_;           // 实际存储（Evaluator拥有）
```

添加方法：
```cpp
void set_shared_cells(vector<types::EvalValue>* c) { shared_cells_ = c; cells_ = c ? *c : ...; }
vector<types::EvalValue>& shared_cells() { return shared_cells_ ? *shared_cells_ : cells_; }
```

#### 2. `ir_executor.ixx` — 引用

```
- unordered_map<uint64_t, EvalValue> cell_heap_;  // 删除
+ vector<types::EvalValue>* shared_cells_;         // 指向Evaluator::cells_
```

构造函数增加参数：
```cpp
explicit IRInterpreter(IRModule& mod, const Primitives& pr, void* registry,
                        vector<types::EvalValue>* cells = nullptr);
```

`CellAlloc` 改为：
```cpp
case IROpcode::CellAlloc: {
    auto cell_id = shared_cells_->size();
    shared_cells_->push_back(make_void());
    locals[ops[0]] = make_cell(cell_id);
    break;
}
```

`CellSet` 改为：
```cpp
case IROpcode::CellSet: {
    auto& cell_id_val = locals[ops[0]];
    if (is_cell(cell_id_val)) {
        auto cell_id = as_cell_id(cell_id_val);
        if (cell_id < shared_cells_->size())
            (*shared_cells_)[cell_id] = locals[ops[1]];
    }
    break;
}
```

`CellGet` 改为：
```cpp
case IROpcode::CellGet: {
    auto& cell_id_val = locals[ops[1]];
    if (is_cell(cell_id_val)) {
        auto cell_id = as_cell_id(cell_id_val);
        locals[ops[0]] = cell_id < shared_cells_->size()
            ? (*shared_cells_)[cell_id] : make_void();
    } else {
        locals[ops[0]] = make_void();
    }
    break;
}
```

#### 3. `service.ixx` — 桥接

构造 IR 解释器时传入共享 cell 指针：

```cpp
auto result = ir_interp.execute();
```

改为：

```cpp
aura::compiler::IRInterpreter ir_interp(*last_ir_mod_, evaluator_.primitives(),
                                         &type_registry_, &evaluator_.cells());
auto result = ir_interp.execute();
```

同时删除 `last_cells_ = ir_interp.list_cells();`（不再需要单独捕获）。

### 指针稳定性

vector `push_back` 触发 reallocation 时会令所有已有指针失效。
现有 `lookup_cell_ptr` 返回 `&cells_[ci]` 的指针模式不再安全。

**解决：全部改用索引访问。**

新增 `Env::lookup_cell_index(name)`：

```cpp
std::optional<uint64_t> Env::lookup_cell_index(const std::string& n) const {
    for (auto& b : bindings_) {
        if (b.first == n) {
            if (is_cell(b.second))
                return as_cell_id(b.second);
            return std::nullopt;
        }
    }
    for (auto* p = parent_; p; p = p->parent_) {
        for (auto& b : p->bindings_) {
            if (b.first == n) {
                if (is_cell(b.second))
                    return as_cell_id(b.second);
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}
```

`evaluator_impl.cpp` 的 `Set` handler 改为：

```cpp
case aura::ast::NodeTag::Set: {
    auto name = p->resolve(v.sym_id);
    auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
    auto val = eval_flat(*f, *p, val_id, eval_env);
    if (!val) return val;
    
    auto cell_idx = eval_env.lookup_cell_index(std::string(name));
    if (cell_idx) {
        if (*cell_idx < cells_.size())
            cells_[*cell_idx] = *val;
        return *val;
    }
    // 回退：直接绑定
    for (auto& b : const_cast<Env&>(eval_env).bindings()) {
        if (b.first == name) {
            b.second = *val;
            return *val;
        }
    }
    // 父环境回退...
}
```

### 删除不必要的代码

- 删除 `IRInterpreter::list_cells()` — 不再需要
- 删除 `Service::last_cells_` — 不再需要
- 从 `tree_walker_only` 集合中移除 `"set!"` — 不再需要（IR 和 tree-walker 用同一个 heap）
- 从 `needs_tree_walker_fallback` 中移除 `Set` 节点检测 — 不再需要

### 收益

1. **`set!` 在任何上下文都正常工作** — top-level、函数内、while 内、letrec 内
2. **消除整类跨 eval 器 bug** — 不再有 "tree-walker 创建 cell，IR 写不到" 的问题
3. **移除 `tree_walker_only` + `Set` fallback 的 hack** — 性能回归
4. **简化 `last_cells_` 逻辑** — IR 执行后 cell 状态自动映射到 tree-walker

### 风险

1. **vector reallocation** — 索引访问已解决，但推入大块数据时仍需避免频繁 reallocation（当前 `cells_` 没有 `reserve`，可加）
2. **缩容** — 不删除 cell（Aura 的 cell 永不回收），所以 vector 只增不减
3. **线程安全** — 当前单线程，不构成问题
4. **`lookup_cell_index` 性能** — O(n) 线性搜索（n ≈ 绑定数量），与 `lookup_cell_ptr` 相同

### 实施步骤

1. 在 `Env` 中添加 `lookup_cell_index`
2. 在 `tree-walker Set handler` 中改用索引
3. 修改 `IRInterpreter` 接受共享 `vector*`，替换 `cell_heap_`
4. 修改 `service.ixx` 传入共享 cells
5. 删除 `list_cells()`, `last_cells_`, `tree_walker_only` 中多余的条目
6. 测试验证所有边界条件
