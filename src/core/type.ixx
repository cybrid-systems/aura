// ═══════════════════════════════════════════════════════════════════
// MODULE BOUNDARY (Issue #58)
// ═══════════════════════════════════════════════════════════════════
//
// aura.core.type is a C++26 module. It exports the canonical TypeTag
// and TypeId used by every other compiler subsystem. Because
// constexpr constants in a module interface are hard to share with
// traditional translation units (lib/runtime.c, JIT C++ glue), the
// rule for THIS file is:
//
//   * TypeTag + TypeId are EXPORTED and used by the rest of the
//     compiler (.ixx modules and .cpp TUs that import this module).
//   * No constants leak to lib/runtime.c. The runtime C code uses its
//     own enum and a hand-written header
//     (src/compiler/value_tags.h) to keep in sync.
//
// Why not put TypeTag in a plain .h header like value_tags.h?
// Because TypeTag is referenced by every .ixx module via `import` and
// the module-exported enum is the single source of truth. Moving it to
// a header would force every consumer to do #include <type.h>
// in addition to `import`, and would lose the module's encapsulation
// benefits (private helpers, type_id interning, etc.).
//
// If you need a TypeTag-equivalent in a non-module TU (a .cpp that
// doesn't import aura.core.type), you must hand-define the enum and
// keep the numeric values in sync with this file. See
// src/compiler/value_tags.h for an example of how that's done.
//
// Related: Issue #58 (archived: git tag docs-archive-pre-2026-06)
//
module;
#include <cstdint>
#include <string_view>
#include <contracts>

export module aura.core.type;

import std;
import aura.core.type_arena;

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
    ~TypeRegistry();

    // ── 注册 ──
    TypeId register_type(TypeTag tag, std::string name);
    void register_adt_constructors(aura::core::TypeId type_id,
                                    std::vector<std::string> constructors);
    const std::vector<std::string>* get_adt_constructors(aura::core::TypeId type_id) const;
    TypeId register_func(std::vector<TypeId> args, TypeId ret)
        post (r: r.valid());
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
    TypeTag tag_of(TypeId id) const
        pre (id.valid());
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
    // Issue #99: was `const`, now non-const because polymorphic
    // subtyping needs to allocate a fresh type variable (alpha-rename)
    // during the check. The mutation is local to the type-variables
    // pool and doesn't affect the existing types the caller holds.
    bool is_subtype(TypeId sub, TypeId sup) const
        pre (sub.valid())
        pre (sup.valid());

    // ── 预定义常量 ──
    TypeId dynamic_type() const { return TypeId{0, 1}; }
    TypeId int_type() const { return TypeId{1, 1}; }
    TypeId bool_type() const { return TypeId{2, 1}; }
    TypeId string_type() const { return TypeId{3, 1}; }
    TypeId void_type() const { return TypeId{4, 1}; }
    TypeId type_type() const { return TypeId{5, 1}; }

    // ── Instantiate a forall type with fresh type variables
    TypeId instantiate(TypeId forall_id, std::function<TypeId()> fresh_var);

    // ── Batch instantiate a forall chain with concrete type arguments
    // Walks the ∀ chain, replacing each bound variable with the corresponding
    // arg from args. Remaining ∀ layers (when args < forall depth) are preserved.
    // Returns the instantiated (inner) type.
    TypeId instantiate_forall(TypeId forall_id, const std::vector<TypeId>& args);

    // ── 工具 ──
    std::string format_type(TypeId id) const;
    size_t size() const { return entries_.size(); }
    uint32_t generation() const { return next_generation_; }
    // Number of entries at construction (the predefined types that
    // survive every compact() call). Used to distinguish "user types"
    // from "always-present" types in tests / diagnostics.
    static constexpr size_t kPredefinedCount = 9;

    // Lookup type by name (returns invalid TypeId if not found)
    TypeId lookup_type(const std::string& name) const;

    // Bump the generation counter and clear all non-predefined entries.
    // After compact() returns, any TypeId with generation less than
    // generation() (i.e. type_id.generation < next_generation_) is
    // stale and must be considered invalid by callers. The 9
    // predefined types are re-registered so int_type(), string_type(),
    // etc. continue to work (but their TypeIds have the new generation).
    //
    // Returns the number of entries reclaimed.
    std::uint32_t compact();

    // Issue #70: is_subtype — replace the stub with real structural
    // subtyping rules. The private impl is a depth-limited helper that
    // is_subtype delegates to (avoids recursing the public method for
    // every level, keeping the depth counter consistent).
  private:
    bool is_subtype_impl(TypeId sub, TypeId sup, int depth);

    // Issue #67 follow-up: walk entries_ and explicitly destroy each
    // Entry's owned resources (FuncType::args, ModuleType body, etc.)
    // before letting the arena's raw bytes be freed. TypeEntryArena
    // uses placement-new on raw bytes, so its destructor only frees
    // the storage — it does not call ~Entry(), which means every
    // Entry's std::optional<...> members (and their std::vector
    // children) leak unless we destroy them here. compact() and
    // ~TypeRegistry() both invoke this.
    void destroy_all_entries() noexcept;

    // TypeId interning: type_equals compares two TypeIds by structural
    // content (not raw id). type_hash produces a stable hash for the
    // structural content (used for the dedup intern table — though the
    // initial implementation uses linear scan over entries_).
    bool type_equals(TypeId a, TypeId b) const;
    std::uint64_t type_hash(TypeId a) const;

  public:

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
    std::vector<Entry*> entries_;   // index → stable pointer into arena_
    TypeEntryArena arena_;           // bump-allocates Entry objects
    std::unordered_map<std::string, TypeId> name_to_id_;
    uint32_t next_generation_ = 1;
};

} // namespace aura::core
