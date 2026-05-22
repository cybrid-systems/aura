export module aura.compiler.value;
import std;

namespace aura::compiler::types {

// Lightweight handle value for heap-indexed values
export struct StringRef {
    std::uint64_t index = 0;
    constexpr auto operator<=>(const StringRef&) const = default;
};
export struct PairRef {
    std::uint64_t index = 0;
    constexpr auto operator<=>(const PairRef&) const = default;
};
export struct ClosureRef {
    std::uint64_t id = 0;
    constexpr auto operator<=>(const ClosureRef&) const = default;
};
export struct CellRef {
    std::uint64_t id = 0;
    constexpr auto operator<=>(const CellRef&) const = default;
};
export struct VectorRef {
    std::uint64_t index = 0;
    constexpr auto operator<=>(const VectorRef&) const = default;
};
export struct HashRef {
    std::uint64_t index = 0;
    constexpr auto operator<=>(const HashRef&) const = default;
};

// Primitive slot reference (for passing builtins like `+` as values)
export struct PrimitiveRef {
    std::size_t slot = 0;
    constexpr auto operator<=>(const PrimitiveRef&) const = default;
};

// Module reference (returned by load-module / use)
export struct ModuleRef {
    std::uint64_t index = 0;
    constexpr auto operator<=>(const ModuleRef&) const = default;
};

// Error value (returned by error/raise, caught by try/catch)
export struct ErrorRef {
    std::uint64_t index = 0;  // index into error heap (stores cause value)
    constexpr auto operator<=>(const ErrorRef&) const = default;
};

// The universal runtime value
export using EvalValue = std::variant<
    std::monostate,    // Void (index 0)
    std::int64_t,      // Int  (index 1)
    bool,              // Bool (index 2)
    StringRef,         // String (index 3)
    PairRef,           // Pair (index 4)
    ClosureRef,        // Closure (index 5)
    CellRef,           // Cell (index 6)
    VectorRef,         // Vector (index 7)
    HashRef,           // Hash (index 8)
    double,            // Float (index 9)
    PrimitiveRef,      // Primitive (index 10)
    ModuleRef,         // Module (index 11)
    ErrorRef           // Error (index 12)
>;

export inline EvalValue make_int(std::int64_t v) { return EvalValue(std::in_place_index<1>, v); }
export inline EvalValue make_bool(bool v) { return EvalValue(std::in_place_index<2>, v); }
export inline EvalValue make_string(std::uint64_t idx) { return EvalValue(std::in_place_index<3>, StringRef{idx}); }
export inline EvalValue make_pair(std::uint64_t idx) { return EvalValue(std::in_place_index<4>, PairRef{idx}); }
export inline EvalValue make_closure(std::uint64_t id) { return EvalValue(std::in_place_index<5>, ClosureRef{id}); }
export inline EvalValue make_cell(std::uint64_t id) { return EvalValue(std::in_place_index<6>, CellRef{id}); }
export inline EvalValue make_vector(std::uint64_t idx) { return EvalValue(std::in_place_index<7>, VectorRef{idx}); }
export inline EvalValue make_hash(std::uint64_t idx) { return EvalValue(std::in_place_index<8>, HashRef{idx}); }
export inline EvalValue make_float(double v) { return EvalValue(std::in_place_index<9>, v); }
export inline EvalValue make_primitive(std::size_t slot) { return EvalValue(std::in_place_index<10>, PrimitiveRef{slot}); }
export inline EvalValue make_module(std::uint64_t idx) { return EvalValue(std::in_place_index<11>, ModuleRef{idx}); }
export inline EvalValue make_error(std::uint64_t idx) { return EvalValue(std::in_place_index<12>, ErrorRef{idx}); }
export inline EvalValue make_void() { return EvalValue(std::in_place_index<0>); }

export inline bool is_int(const EvalValue& v) noexcept { return std::holds_alternative<std::int64_t>(v); }
export inline bool is_bool(const EvalValue& v) noexcept { return std::holds_alternative<bool>(v); }
export inline bool is_void(const EvalValue& v) noexcept { return std::holds_alternative<std::monostate>(v); }
export inline bool is_string(const EvalValue& v) noexcept { return std::holds_alternative<StringRef>(v); }
export inline bool is_pair(const EvalValue& v) noexcept { return std::holds_alternative<PairRef>(v); }
export inline bool is_closure(const EvalValue& v) noexcept { return std::holds_alternative<ClosureRef>(v); }
export inline bool is_cell(const EvalValue& v) noexcept { return std::holds_alternative<CellRef>(v); }
export inline bool is_vector(const EvalValue& v) noexcept { return std::holds_alternative<VectorRef>(v); }
export inline bool is_hash(const EvalValue& v) noexcept { return std::holds_alternative<HashRef>(v); }
export inline bool is_float(const EvalValue& v) noexcept { return std::holds_alternative<double>(v); }
export inline bool is_primitive(const EvalValue& v) noexcept { return std::holds_alternative<PrimitiveRef>(v); }
export inline bool is_module(const EvalValue& v) noexcept { return std::holds_alternative<ModuleRef>(v); }
export inline bool is_error(const EvalValue& v) noexcept { return std::holds_alternative<ErrorRef>(v); }

export inline std::int64_t as_int(const EvalValue& v) { return std::get<std::int64_t>(v); }
export inline bool as_bool(const EvalValue& v) { return std::get<bool>(v); }
export inline std::uint64_t as_string_idx(const EvalValue& v) { return std::get<StringRef>(v).index; }
export inline std::uint64_t as_pair_idx(const EvalValue& v) { return std::get<PairRef>(v).index; }
export inline std::uint64_t as_closure_id(const EvalValue& v) { return std::get<ClosureRef>(v).id; }
export inline std::uint64_t as_cell_id(const EvalValue& v) { return std::get<CellRef>(v).id; }
export inline std::uint64_t as_vector_idx(const EvalValue& v) { return std::get<VectorRef>(v).index; }
export inline std::uint64_t as_hash_idx(const EvalValue& v) { return std::get<HashRef>(v).index; }
export inline double as_float(const EvalValue& v) { return std::get<double>(v); }
export inline std::size_t as_primitive_slot(const EvalValue& v) { return std::get<PrimitiveRef>(v).slot; }
export inline std::uint64_t as_module_idx(const EvalValue& v) { return std::get<ModuleRef>(v).index; }
export inline std::uint64_t as_error_idx(const EvalValue& v) { return std::get<ErrorRef>(v).index; }

export inline bool is_truthy(const EvalValue& v) {
    if (is_bool(v)) return as_bool(v);
    if (is_void(v)) return false;
    if (is_int(v)) return as_int(v) != 0;
    if (is_float(v)) return as_float(v) != 0.0;
    return true;
}

export inline std::string format_value(const EvalValue& v) {
    if (is_void(v)) return "()";
    if (is_bool(v)) return as_bool(v) ? "#t" : "#f";
    if (is_int(v)) return std::to_string(as_int(v));
    if (is_float(v)) return std::to_string(as_float(v));
    if (is_string(v)) return std::format("<string[{}]>", as_string_idx(v));
    if (is_vector(v)) return std::format("<vector[{}]>", as_vector_idx(v));
    if (is_hash(v)) return std::format("<hash[{}]>", as_hash_idx(v));
    if (is_pair(v)) return std::format("<pair[{}]>", as_pair_idx(v));
    if (is_closure(v)) return "#<procedure>";
    if (is_cell(v)) return std::format("<cell[{}]>", as_cell_id(v));
    if (is_primitive(v)) return "<primitive>";
    if (is_module(v)) return std::format("<module[{}]>", as_module_idx(v));
    if (is_error(v)) return std::format("<error[{}]>", as_error_idx(v));
    return "<unknown>";
}

export inline std::string format_value(const EvalValue& v, const std::vector<std::string>* heap) {
    if (is_void(v)) return "()";
    if (is_bool(v)) return as_bool(v) ? "#t" : "#f";
    if (is_int(v)) return std::to_string(as_int(v));
    if (is_string(v)) {
        if (heap) {
            auto idx = as_string_idx(v);
            if (idx < heap->size()) return std::format("\"{}\"", (*heap)[idx]);
        }
        return std::format("<string[{}]>", as_string_idx(v));
    }
    if (is_float(v)) return std::to_string(as_float(v));
    if (is_vector(v)) return std::format("<vector[{}]>", as_vector_idx(v));
    if (is_hash(v)) return std::format("<hash[{}]>", as_hash_idx(v));
    if (is_pair(v)) return std::format("<pair[{}]>", as_pair_idx(v));
    if (is_closure(v)) return "#<procedure>";
    if (is_cell(v)) return std::format("<cell[{}]>", as_cell_id(v));
    if (is_module(v)) return std::format("<module[{}]>", as_module_idx(v));
    if (is_error(v)) return std::format("<error[{}]>", as_error_idx(v));
    return "<unknown>";
}

} // namespace aura::compiler::types
