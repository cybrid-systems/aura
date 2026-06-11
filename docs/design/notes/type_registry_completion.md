# TypeRegistry 补全 — Implementation Plan

> Issue #41 — 完成 Variant/Record/List/forall + let-polymorphism + occurs check

## 当前状态

### TypeRegistry 已有类型构造器

| 类型 | 工厂函数 | 查询函数 | 状态 |
|------|---------|---------|------|
| FuncType | `register_func()` | `func_of()` | ✅ 完整 |
| ForallType | `register_forall()` | `forall_of()` | ✅ 基本 |
| LinearType | `register_linear()` | `linear_of()` | ✅ 完整 |
| ModuleType | `register_module()` | `module_of()` | ✅ 基本 |
| EffectType | `register_effect()` | `effect_of()` | ✅ 完整 |
| CapabilityType | `register_capability()` | `capability_of()` | ✅ 完整 |
| **VariantType** | ❌ 不存在 | ❌ 不存在 | ❌ |
| **RecordType** | ❌ 不存在 | ❌ 不存在 | ❌ |
| **ListType** | ❌ 不存在 | ❌ 不存在 | ❌ (可用 Pair 模拟) |

### 其他关键能力

| 能力 | 状态 | 说明 |
|------|------|------|
| `occurs_check()` | ⚠️ 部分 | 只处理 FuncType，不处理 Forall/Linear/Module |
| `do_subst` | ⚠️ 部分 | 只处理 FuncType/ModuleType，不处理 Forall |
| `instantiate(forall, fresh_var)` | ✅ 存在 | 但仅单变量 forall，无 `instantiate_forall(forall, args)` 批量接口 |
| Let-polymorphism | ⚠️ 部分 | 值限制 + generalize 已实现，但 `free_vars` 不追踪复合类型中的变量 |
| `free_vars()` | ⚠️ 部分 | 不处理 Linear/Module 中的类型变量 |

## 设计方案

### Step 1: 新增类型构造器

#### VariantType (和类型 / tagged union)

```cpp
export struct VariantType {
    std::vector<std::pair<std::string, std::vector<TypeId>>> variants;
    // e.g. (Maybe a) = {Just: [a], Nothing: []}
};
```

TypeTag: 复用或新增 `VARIANT` (已有 `TypeTag::VARIANT`)

注册函数:
```cpp
TypeId register_variant(VariantType vt);
const VariantType* variant_of(TypeId id) const;
```

#### RecordType (记录类型 / product type)

```cpp
export struct RecordType {
    std::vector<std::pair<std::string, TypeId>> fields;
    // e.g. (Person: {name: String, age: Int})
};
```

TypeTag: 复用 `RECORD` (已有 `TypeTag::RECORD`)

注册函数:
```cpp
TypeId register_record(RecordType rt);
const RecordType* record_of(TypeId id) const;
```

#### ListType (列表类型)

List 可以用 Pair 模拟：`(Pair a (Pair a ...))`。不需要独立的类型构造器。
对于 List 类型标注 `(List Int)` → 展开为 `(Pair Int (Pair Int ...))`，在类型系统中用递归类型或 Dynamic 简化。

### Step 2: 扩展 occurs_check

当前 `ConstraintSystem::occurs_check()` 只处理 FuncType。需要扩展：

```cpp
bool ConstraintSystem::occurs_check(TypeId var, TypeId ty) {
    if (!reg_.is_var(var)) return false;
    ty = find(ty);
    if (var == ty) return true;
    
    // 现有: FuncType
    if (auto* f = reg_.func_of(ty)) {
        for (auto a : f->args)
            if (occurs_check(var, a)) return true;
        return occurs_check(var, f->ret);
    }
    
    // 新增: ForallType
    if (auto* ft = reg_.forall_of(ty)) {
        return occurs_check(var, ft->body);
    }
    
    // 新增: LinearType
    if (auto* lt = reg_.linear_of(ty)) {
        return occurs_check(var, lt->inner);
    }
    
    // 新增: VariantType
    if (auto* vt = reg_.variant_of(ty)) {
        for (auto& [name, args] : vt->variants)
            for (auto& a : args)
                if (occurs_check(var, a)) return true;
    }
    
    // 新增: RecordType
    if (auto* rt = reg_.record_of(ty)) {
        for (auto& [name, type] : rt->fields)
            if (occurs_check(var, type)) return true;
    }
    
    // 新增: ModuleType
    if (auto* mt = reg_.module_of(ty)) {
        for (auto& [name, type] : mt->members)
            if (occurs_check(var, type)) return true;
    }
    
    return false;
}
```

### Step 3: 扩展 do_subst

当前 `do_subst` 在 `synthesize_flat_call` 中定义，闭包捕获 `subst` map。需要提取成 TypeRegistry 方法。

新增 TypeRegistry 方法：
```cpp
TypeId substitute(TypeId ty, const std::unordered_map<uint32_t, TypeId>& subst);
```

递归处理所有类型构造器，包括新增的 Variant/Record：

```cpp
TypeId TypeRegistry::substitute(TypeId ty, const std::unordered_map<uint32_t, TypeId>& subst) {
    auto it = subst.find(ty.index);
    if (it != subst.end()) return it->second;
    
    switch (tag_of(ty)) {
        case FUNC: {
            auto* f = func_of(ty);
            std::vector<TypeId> new_args;
            for (auto& a : f->args) new_args.push_back(substitute(a, subst));
            return register_func(std::move(new_args), substitute(f->ret, subst));
        }
        case FORALL: {
            auto* ft = forall_of(ty);
            return register_forall(ft->var, substitute(ft->body, subst));
        }
        case LINEAR: {
            auto* lt = linear_of(ty);
            return register_linear(substitute(lt->inner, subst));
        }
        case MODULE: {
            auto* mt = module_of(ty);
            std::vector<std::pair<std::string, TypeId>> new_members;
            for (auto& [n, t] : mt->members)
                new_members.push_back({n, substitute(t, subst)});
            return register_module(ModuleType{std::move(new_members)});
        }
        case VARIANT: {
            auto* vt = variant_of(ty);
            VariantType new_vt;
            for (auto& [name, args] : vt->variants) {
                std::vector<TypeId> new_args;
                for (auto& a : args) new_args.push_back(substitute(a, subst));
                new_vt.variants.push_back({name, std::move(new_args)});
            }
            return register_variant(std::move(new_vt));
        }
        case RECORD: {
            auto* rt = record_of(ty);
            RecordType new_rt;
            for (auto& [name, type] : rt->fields)
                new_rt.fields.push_back({name, substitute(type, subst)});
            return register_record(std::move(new_rt));
        }
        default:
            return ty;
    }
}
```

### Step 4: 补全 free_vars

当前 `TypeRegistry::free_vars()` 不处理 Linear/Module/Forall 中的类型变量。需要扩展所有类型构造器：

```cpp
std::vector<TypeId> TypeRegistry::free_vars(TypeId id) const {
    // 现有逻辑 + 扩展
    ...
    if (auto* lt = linear_of(cur))
        stack.push_back(lt->inner);
    if (auto* mt = module_of(cur))
        for (auto& [n, t] : mt->members)
            stack.push_back(t);
    if (auto* vt = variant_of(cur))
        for (auto& [name, args] : vt->variants)
            for (auto& a : args) stack.push_back(a);
    if (auto* rt = record_of(cur))
        for (auto& [name, type] : rt->fields)
            stack.push_back(type);
    ...
}
```

### Step 5: 优化 instantiate_forall

新增批量实例化接口：

```cpp
TypeId instantiate_forall(TypeId forall_id, const std::vector<TypeId>& args);
```

遍历 forall 链，将每个 bound var 替换为对应的 arg：

```cpp
TypeId TypeRegistry::instantiate_forall(TypeId forall_id,
                                         const std::vector<TypeId>& args) {
    TypeId result = forall_id;
    std::size_t arg_idx = 0;
    while (auto* ft = forall_of(result)) {
        if (arg_idx >= args.size())
            break; // 剩余 forall 保留
        std::unordered_map<uint32_t, TypeId> subst;
        subst[ft->var.index] = args[arg_idx++];
        result = ft->body; // 先在 body 中替换
        // 递归替换 body 中的 bound var
        result = substitute(result, subst);
    }
    return result;
}
```

### Step 6: 复合类型解析器支持

在 parser 中支持 `(Variant ...)` / `(Record ...)` / `(List ...)` 类型表达式：

- `(Variant (Just Int) Nothing)` → 解析为 VariantType
- `(Record (name String) (age Int))` → 解析为 RecordType
- `(List Int)` → 解析为 Pair<Int, List<Int>> 或 Dynamic

### Step 7: 测试矩阵

```aura
;; 1. Variant 类型
(define (maybe-default [x : (Maybe Int)])
  (match x
    [(Just v) v]
    [Nothing 0]))

;; 2. Record 类型
(define (get-name [p : Person])
  (: name p))

;; 3. Let-polymorphism
(define (id [x : (forall [a] a)]) x)
(define (use-id)
  (+ (id 42) (string-length (id "hello"))))

;; 4. Occurs check
(define (bad [x : (-> a a)]) x)  ; a is free → error or bind to forall

;; 5. Functor 复合类型
(define-module (PairFunctor :T)
  (define (map [f : (-> T U)] [p : (Pair T T)]) ...))
```

## 实现顺序

1. **Step 1**: TypeRegistry 新增 VariantType / RecordType 结构体 + 注册/查询函数
2. **Step 2**: 扩展 occurs_check 覆盖所有类型构造器
3. **Step 3**: 提取 substitute() 为 TypeRegistry 方法 + 扩展新类型
4. **Step 4**: 补全 free_vars
5. **Step 5**: 新增 instantiate_forall()
6. **Step 6**: 测试

## 风险

- Variant/Record 类型目前没有成熟的 lowering 支持（IR 中无对应表示）
- Let-polymorphism 与 gradual typing 的交互需要小心（值限制已实现，但 forall with Any 边界有风险）
- 复合类型工厂函数的缓存策略（避免大量重复注册同一类型）
