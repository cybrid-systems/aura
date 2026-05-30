module;
#include <cstdint>
#include <string_view>

export module aura.core.type;

import std;

namespace aura::core {

// ── TypeTag ──────────────────────────────────────────────────
export enum class TypeTag : uint8_t {
    DYNAMIC = 0, // Any / 无标注
    INT,         // int64_t
    BOOL,        // bool
    STRING,      // std::string
    FLOAT,       // double
    PAIR,        // (cons ...)
    VECTOR,      // vector
    CLOSURE,     // 闭包函数
    FUNC,        // 多参数函数
    RECORD,      // 记录
    VARIANT,     // 和类型
    TYPE_VAR,    // 类型变量
    FORALL,      // 多态 ∀
    LINEAR,      // 线性所有权 M4
    VOID,        // 无返回值
    TYPE,        // 类型自身的类型
    HASH,        // hash table
    MODULE,      // 模块签名 {sym1: T1, sym2: T2, ...}
    EFFECT,      // 效果类型 !EffectName
    CAPABILITY,  // 能力集合 {FileRead, FileWrite}
};

// ── TypeId ────────────────────────────────────────────────────
export struct TypeId {
    uint32_t index = 0;      // TypeRegistry 中的索引
    uint32_t generation = 0; // 防重用/校验

    bool valid() const noexcept { return index != 0 || generation != 0; }
    auto operator<=>(const TypeId&) const = default;
};

// 预定义特殊 TypeId（注册在 TypeRegistry 构造函数中）
// DYNAMIC = 0, INT = 1, BOOL = 2, STRING = 3, ...

// ── TypeInfo ──────────────────────────────────────────────────
export struct TypeInfo {
    TypeId resolved{}; // 经过推断的类型
    TypeId expected{}; // 用户标注的类型（可选）
    bool has_annotation = false;

    bool matches() const { return resolved == expected || !expected.valid(); }
};

// ── FunctionType ──────────────────────────────────────────────
export struct FuncType {
    std::vector<TypeId> args;
    TypeId ret;
};

export struct ForallType {
    TypeId var;  // bound type variable
    TypeId body; // body type (usually a FuncType)
};

export struct LinearType {
    TypeId inner; // the wrapped type (e.g. Int in (Linear Int))
};

// Module type: {sym1: T1, sym2: T2, ...}
export struct ModuleType {
    std::vector<std::pair<std::string, TypeId>> members; // (name, type)
    std::vector<std::string> type_params;                // functor type param names, e.g. ["T"]
    std::vector<TypeId> type_param_vars;                 // fresh type vars for each type param
};

export struct VariantType {
    std::vector<std::pair<std::string, std::vector<TypeId>>> variants;
    // e.g. (Maybe a) = {Just: [a], Nothing: []}
    // variant name → field types
};

export struct RecordType {
    std::vector<std::pair<std::string, TypeId>> fields;
    // e.g. Person: {name: String, age: Int}
    // field name → field type
};

// Effect type: !EffectName (e.g., !IO, !FileRead)
export struct EffectType {
    std::string name;
    TypeId arg{};
};

// Capability type: {FileRead, FileWrite} — set of required effects
export struct CapabilityType {
    std::vector<std::string> effects;  // 包含的 effect 名称列表
    bool is_unrestricted = false;      // true = 允许所有 effect
};

// ── TypeRegistry ──────────────────────────────────────────────
export class TypeRegistry {
public:
    TypeRegistry();

    // ── 注册 ──
    TypeId register_type(TypeTag tag, std::string name);
    void register_adt_constructors(aura::core::TypeId type_id,
                                    std::vector<std::string> constructors);
    const std::vector<std::string>* get_adt_constructors(aura::core::TypeId type_id) const;
    TypeId register_func(std::vector<TypeId> args, TypeId ret);
    TypeId register_func_named(std::vector<TypeId> args, TypeId ret, std::string name);
    TypeId register_forall(TypeId var, TypeId body);
    TypeId register_linear(TypeId inner);
    TypeId register_variant(VariantType vt);
    TypeId register_record(RecordType rt);
    TypeId register_module(ModuleType mt);
    TypeId register_effect(std::string name, TypeId arg = {});
    TypeId register_capability(std::vector<std::string> effects, bool unrestricted = false);
    TypeId make_var(std::string name = "");

    // ── 查询 ──
    TypeTag tag_of(TypeId id) const;
    std::string_view name_of(TypeId id) const;
    const FuncType* func_of(TypeId id) const;
    const ForallType* forall_of(TypeId id) const;
    const LinearType* linear_of(TypeId id) const;
    const ModuleType* module_of(TypeId id) const;
    const VariantType* variant_of(TypeId id) const;
    const RecordType* record_of(TypeId id) const;
    const EffectType* effect_of(TypeId id) const;
    const CapabilityType* capability_of(TypeId id) const;
    bool is_var(TypeId id) const;
    bool is_subtype(TypeId sub, TypeId sup) const;

    // ── 预定义常量 ──
    TypeId dynamic_type() const { return TypeId{0, 1}; }
    TypeId int_type() const { return TypeId{1, 1}; }
    TypeId bool_type() const { return TypeId{2, 1}; }
    TypeId string_type() const { return TypeId{3, 1}; }
    TypeId void_type() const { return TypeId{4, 1}; }
    TypeId type_type() const { return TypeId{5, 1}; }

    // ── Instantiate a forall type with fresh type variables
    TypeId instantiate(TypeId forall_id, std::function<TypeId()> fresh_var);

    // ── 工具 ──
    std::string format_type(TypeId id) const;
    size_t size() const { return entries_.size(); }

    // Lookup type by name (returns invalid TypeId if not found)
    TypeId lookup_type(const std::string& name) const;

    // Substitute type variables in a type using the given substitution map
    // Returns a new TypeId (may register new types), does not modify original
    TypeId substitute(TypeId ty, const std::unordered_map<std::uint32_t, TypeId>& subst);

    // Collect all type variables in a type (for let-polymorphism)
    std::vector<TypeId> free_vars(TypeId id) const;

private:
    struct Entry {
        TypeTag tag;
        std::string name;
        std::optional<FuncType> func;
        std::optional<ForallType> forall;
        std::optional<LinearType> linear;
        std::optional<ModuleType> module_type;
        std::optional<std::vector<std::string>> adt_constructors;
        std::optional<EffectType> effect;
        std::optional<CapabilityType> capability;
        std::optional<VariantType> variant;
        std::optional<RecordType> record;
    };
    std::vector<Entry> entries_;
    std::unordered_map<std::string, TypeId> name_to_id_;
    uint32_t next_generation_ = 1;
};

} // namespace aura::core
