export module aura.compiler.value;
import std;

namespace aura::compiler::types {

// ═══════════════════════════════════════════════════════════
// Unified tagged value representation (matches AOT runtime tagging)
// ═══════════════════════════════════════════════════════════
//
// Bit encoding:
//   bits 0-1: TAG
//     00 = Fixnum (signed integer, value >> 1)
//     01 = Ref    (pool-indexed heap type) 
//     11 = Special (#f=3, #t=7, void=11)
//
//   Ref sub-type (bits 2-5, 4 bits = 16 types):
//     0=Pair, 1=Closure, 2=Cell, 3=Vector,
//     4=Hash, 5=Primitive, 6=String, 7=Module,
//     8=Error, 9=Opaque, 10=Linear, 11=Keyword
//
//   Ref bit layout: (pool_index << 6) | (type << 2) | 1
//
//   Float: stored in runtime pool with FLOAT_BIAS encoding
//          (is_float checks v <= FLOAT_BIAS && v > STRING_BIAS_AREA)
//
//   String: stored in runtime pool with STRING_BIAS encoding 
//           (is_string checks v <= STRING_BIAS_AREA)
//
// ═══════════════════════════════════════════════════════════

static constexpr std::int64_t FLOAT_BIAS_VAL = -10000000000000000LL;
static constexpr std::int64_t STRING_BIAS_VAL = -9000000000000000000LL;

// Ref type ids (bits 2-5)
using RefType = std::uint64_t;
constexpr std::uint64_t RefPair = 0;
constexpr std::uint64_t RefClosure = 1;
constexpr std::uint64_t RefCell = 2;
constexpr std::uint64_t RefVector = 3;
constexpr std::uint64_t RefHash = 4;
constexpr std::uint64_t RefPrimitive = 5;
constexpr std::uint64_t RefString = 6;
constexpr std::uint64_t RefModule = 7;
constexpr std::uint64_t RefError = 8;
constexpr std::uint64_t RefOpaque = 9;
constexpr std::uint64_t RefLinear = 10;
constexpr std::uint64_t RefKeyword = 11;

// The universal runtime value — a single int64_t with pointer tagging
export struct EvalValue {
    std::int64_t val = 0;
    
    constexpr EvalValue() noexcept : val(0) {}
    constexpr explicit EvalValue(std::int64_t v) noexcept : val(v) {}
    
    constexpr bool operator==(const EvalValue& o) const noexcept = default;
    constexpr auto operator<=>(const EvalValue& o) const noexcept = default;
    
    explicit operator bool() const = delete; // prevent implicit boolean conversion
    explicit operator std::int64_t() const noexcept { return val; }
};

// ── Tag helpers ────────────────────────────────────────
constexpr inline bool is_fixnum(std::int64_t v) noexcept { return (v & 1) == 0; }
constexpr inline bool is_ref(std::int64_t v) noexcept    { return (v & 3) == 1; }
constexpr inline bool is_special(std::int64_t v) noexcept { return (v & 3) == 3; }

constexpr inline std::uint64_t ref_type(std::int64_t v) noexcept {
    return (static_cast<std::uint64_t>(v) >> 2) & 0xF;
}
constexpr inline std::uint64_t ref_index(std::int64_t v) noexcept {
    return static_cast<std::uint64_t>(v) >> 6;
}
constexpr inline std::int64_t make_ref(std::uint64_t type, std::uint64_t index) noexcept {
    return static_cast<std::int64_t>((index << 6) | (type << 2) | 1ULL);
}

// ── Float helpers (extern "C" runtime functions) ──────
// These are provided by lib/runtime.c 
extern "C" {
    std::int64_t aura_alloc_float(double d);
    double aura_float_ref(std::int64_t val);
}

// ── String bias encoding (must match lib/runtime.c) ───
constexpr inline std::int64_t make_string_raw(std::uint64_t idx) noexcept {
    return STRING_BIAS_VAL - static_cast<std::int64_t>(idx);
}
constexpr inline std::uint64_t string_idx_raw(std::int64_t v) noexcept {
    return static_cast<std::uint64_t>(-static_cast<std::int64_t>(v) - 9000000000000000000LL);
}

// ── make_* / is_* / as_* helpers ──────────────────────
// All keep the same signature as before — this is a transparent refactor.

export inline EvalValue make_int(std::int64_t v) noexcept {
    return EvalValue(v << 1);  // fixnum encoding
}
export inline bool is_int(const EvalValue& v) noexcept {
    // Fixnum: bit0=0, but STRING_BIAS and FLOAT_BIAS values also have bit0=0.
    // Exclude both ranges to avoid type confusion.
    return is_fixnum(v.val) && v.val > FLOAT_BIAS_VAL;
}
export inline std::int64_t as_int(const EvalValue& v) noexcept {
    return v.val >> 1;
}

export inline EvalValue make_bool(bool v) noexcept {
    return EvalValue(v ? 7 : 3);  // #t=7, #f=3
}
export inline bool is_bool(const EvalValue& v) noexcept {
    return v.val == 3 || v.val == 7;
}
export inline bool as_bool(const EvalValue& v) noexcept {
    return v.val == 7;
}

export inline EvalValue make_void() noexcept {
    return EvalValue(11);  // void sentinel = 11
}
export inline bool is_void(const EvalValue& v) noexcept {
    return v.val == 11;  // void sentinel = 11
}

export inline EvalValue make_float(double d) {
    return EvalValue(aura_alloc_float(d));  // FLOAT_BIAS encoding
}
export inline bool is_float(const EvalValue& v) noexcept {
    return v.val <= FLOAT_BIAS_VAL && v.val > STRING_BIAS_VAL;
}
export inline double as_float(const EvalValue& v) {
    return aura_float_ref(v.val);
}

export inline EvalValue make_string(std::uint64_t idx) noexcept {
    return EvalValue(make_string_raw(idx));
}
export inline bool is_string(const EvalValue& v) noexcept {
    return v.val <= STRING_BIAS_VAL;
}
export inline std::uint64_t as_string_idx(const EvalValue& v) noexcept {
    return string_idx_raw(v.val);
}

export inline EvalValue make_pair(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefPair, idx));
}
export inline bool is_pair(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefPair;
}
export inline std::uint64_t as_pair_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_closure(std::uint64_t id) noexcept {
    return EvalValue(make_ref(RefClosure, id));
}
export inline bool is_closure(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefClosure;
}
export inline std::uint64_t as_closure_id(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_cell(std::uint64_t id) noexcept {
    return EvalValue(make_ref(RefCell, id));
}
export inline bool is_cell(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefCell;
}
export inline std::uint64_t as_cell_id(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_vector(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefVector, idx));
}
export inline bool is_vector(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefVector;
}
export inline std::uint64_t as_vector_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_hash(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefHash, idx));
}
export inline bool is_hash(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefHash;
}
export inline std::uint64_t as_hash_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_primitive(std::size_t slot) noexcept {
    return EvalValue(make_ref(RefPrimitive, static_cast<std::uint64_t>(slot)));
}
export inline bool is_primitive(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefPrimitive;
}
export inline std::size_t as_primitive_slot(const EvalValue& v) noexcept {
    return static_cast<std::size_t>(ref_index(v.val));
}

export inline EvalValue make_module(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefModule, idx));
}
export inline bool is_module(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefModule;
}
export inline std::uint64_t as_module_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_error(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefError, idx));
}
export inline bool is_error(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefError;
}
export inline std::uint64_t as_error_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_opaque(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefOpaque, idx));
}
export inline bool is_opaque(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefOpaque;
}
export inline std::uint64_t as_opaque_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_linear(std::uint64_t id) noexcept {
    return EvalValue(make_ref(RefLinear, id));
}
export inline bool is_linear(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefLinear;
}
export inline std::uint64_t as_linear_id(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_keyword(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefKeyword, idx));
}
export inline bool is_keyword(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefKeyword;
}
export inline std::uint64_t as_keyword_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

// ── Truthiness ─────────────────────────────────────────
export bool is_truthy(const EvalValue& v) noexcept;

// ── Formatting (debug/error output) ────────────────────
export inline std::string format_value(const EvalValue& v) {
    if (is_void(v))
        return "()";
    if (is_bool(v))
        return as_bool(v) ? "#t" : "#f";
    if (is_int(v))
        return std::to_string(as_int(v));
    if (is_float(v))
        return std::to_string(as_float(v));
    if (is_string(v))
        return "\"" + std::to_string(as_string_idx(v)) + "\"";
    if (is_pair(v))
        return "#<pair " + std::to_string(as_pair_idx(v)) + ">";
    if (is_closure(v))
        return "#<closure " + std::to_string(as_closure_id(v)) + ">";
    if (is_cell(v))
        return "#<cell " + std::to_string(as_cell_id(v)) + ">";
    if (is_vector(v))
        return "#<vector " + std::to_string(as_vector_idx(v)) + ">";
    if (is_hash(v))
        return "#<hash " + std::to_string(as_hash_idx(v)) + ">";
    if (is_primitive(v))
        return "#<primitive " + std::to_string(as_primitive_slot(v)) + ">";
    if (is_module(v))
        return "#<module " + std::to_string(as_module_idx(v)) + ">";
    if (is_keyword(v))
        return "#<keyword " + std::to_string(as_keyword_idx(v)) + ">";
    if (is_error(v))
        return "#<error " + std::to_string(as_error_idx(v)) + ">";
    if (is_linear(v))
        return "#<linear " + std::to_string(as_linear_id(v)) + ">";
    if (is_opaque(v))
        return "#<opaque " + std::to_string(as_opaque_idx(v)) + ">";
    return "#<unknown " + std::to_string(v.val) + ">";
}


export inline std::string format_value(const EvalValue& v, const std::vector<std::string>* heap) {
    if (is_void(v))
        return "()";
    if (is_bool(v))
        return as_bool(v) ? "#t" : "#f";
    if (is_string(v)) {
        if (heap) {
            auto idx = as_string_idx(v);
            if (idx < heap->size())
                return std::format("\"{}\"", (*heap)[idx]);
        }
        return std::format("<string[{}]>", as_string_idx(v));
    }
    if (is_int(v))
        return std::to_string(as_int(v));
    if (is_float(v))
        return std::to_string(as_float(v));
    if (is_pair(v))
        return std::format("<pair[{}]>", as_pair_idx(v));
    if (is_closure(v))
        return "#<procedure>";
    if (is_cell(v))
        return std::format("<cell[{}]>", as_cell_id(v));
    if (is_vector(v))
        return std::format("<vector[{}]>", as_vector_idx(v));
    if (is_hash(v))
        return std::format("<hash[{}]>", as_hash_idx(v));
    if (is_primitive(v))
        return "<primitive>";
    if (is_linear(v))
        return std::format("<linear[{}]>", as_linear_id(v));
    if (is_module(v))
        return std::format("<module[{}]>", as_module_idx(v));
    if (is_error(v))
        return std::format("<error[{}]>", as_error_idx(v));
    return "<unknown>";
}

} // namespace aura::compiler::types
