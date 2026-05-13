module;

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <string_view>

export module aura.core.type;

import std;

namespace aura::core {

// ── TypeTag ──────────────────────────────────────────────────
export enum class TypeTag : uint8_t {
    DYNAMIC = 0,    // Any / 无标注
    INT,            // int64_t
    BOOL,           // bool
    STRING,         // std::string
    FLOAT,          // double
    PAIR,           // (cons ...)
    VECTOR,         // vector
    CLOSURE,        // 闭包函数
    FUNC,           // 多参数函数
    RECORD,         // 记录
    VARIANT,        // 和类型
    TYPE_VAR,       // 类型变量
    FORALL,         // 多态 ∀
    VOID,           // 无返回值
    TYPE,           // 类型自身的类型
};

// ── TypeId ────────────────────────────────────────────────────
export struct TypeId {
    uint32_t index = 0;     // TypeRegistry 中的索引
    uint32_t generation = 0; // 防重用/校验

    bool valid() const noexcept { return index != 0 || generation != 0; }
    auto operator<=>(const TypeId&) const = default;
};

// 预定义特殊 TypeId（注册在 TypeRegistry 构造函数中）
// DYNAMIC = 0, INT = 1, BOOL = 2, STRING = 3, ...

// ── TypeInfo ──────────────────────────────────────────────────
export struct TypeInfo {
    TypeId resolved{};      // 经过推断的类型
    TypeId expected{};      // 用户标注的类型（可选）
    bool has_annotation = false;

    bool matches() const { return resolved == expected || !expected.valid(); }
};

// ── FunctionType ──────────────────────────────────────────────
export struct FuncType {
    std::vector<TypeId> args;
    TypeId ret;
};

// ── TypeRegistry ──────────────────────────────────────────────
export class TypeRegistry {
public:
    TypeRegistry();

    // ── 注册 ──
    TypeId register_type(TypeTag tag, std::string name);
    TypeId register_func(std::vector<TypeId> args, TypeId ret);
    TypeId register_forall(TypeId var, TypeId body);
    TypeId make_var(std::string name = "");

    // ── 查询 ──
    TypeTag tag_of(TypeId id) const;
    std::string_view name_of(TypeId id) const;
    const FuncType* func_of(TypeId id) const;
    bool is_var(TypeId id) const;
    bool is_subtype(TypeId sub, TypeId sup) const;

    // ── 预定义常量 ──
    TypeId dynamic_type() const { return TypeId{0, 1}; }
    TypeId int_type()     const { return TypeId{1, 1}; }
    TypeId bool_type()    const { return TypeId{2, 1}; }
    TypeId string_type()  const { return TypeId{3, 1}; }
    TypeId void_type()    const { return TypeId{4, 1}; }
    TypeId type_type()    const { return TypeId{5, 1}; }

    // ── 工具 ──
    std::string format_type(TypeId id) const;
    size_t size() const { return entries_.size(); }

    // Lookup type by name (returns invalid TypeId if not found)
    TypeId lookup_type(const std::string& name) const;

private:
    struct Entry {
        TypeTag tag;
        std::string name;
        std::optional<FuncType> func;
    };
    std::vector<Entry> entries_;
    std::unordered_map<std::string, TypeId> name_to_id_;
    uint32_t next_generation_ = 1;
};

} // namespace aura::core
