// ffi_primitives.ixx — C FFI primitives extracted from
// the monolithic evaluator_impl.cpp (Issue #131).
//
// This module owns the FFI state (loaded libraries and
// registered C functions) and provides a single
// registration function that wires the FFI primitives
// into a Primitives table. Callers (currently only
// Evaluator's constructor) call register_ffi_primitives
// once during init.
//
// The FFI state was previously file-scope statics in
// evaluator_impl.cpp. Moving it here is the first step
// of the Issue #131 refactor: reduce the size of
// evaluator_impl.cpp by extracting focused modules.

export module aura.compiler.ffi_primitives;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler {

// Mirror of aura::compiler::PrimFn (defined in
// evaluator.ixx). To avoid the cyclic import, we
// re-declare the same alias here. The compiler enforces
// that the types are compatible at the call site.
using PrimFn = std::function<types::EvalValue(std::span<const types::EvalValue>)>;

// FFI function record. Each registered C function gets
// one of these. The closures_ lookup uses the index.
struct FFIFunc {
    void* fn_ptr = nullptr;
    std::string name;
    int ret_type = 0;           // 0=void, 1=Int, 2=Float, 3=String, 4=Opaque
    std::vector<int> arg_types; // per-arg type tags
};

// Issue #131: state is now per-FFIRuntime instance (not
// file-scope statics). The previous file-scope statics
// (g_ffi_libs, g_ffi_funcs) are gone. Multiple evaluators
// can now have independent FFI state.
//
// To break the cyclic import between this module and
// evaluator.ixx, we accept a callback (RegisterFn)
// rather than a `Primitives&` directly. The callback
// signature matches `Primitives::add(string, PrimFn)`.
export class FFIRuntime {
public:
    using RegisterFn = std::function<void(std::string, PrimFn)>;

    // Returns true if signature was parsed successfully.
    // sig format: "(ArgType) -> RetType"
    // e.g. "(String) -> Int", "(Float Float) -> Float"
    //
    // Defined inline in the .ixx so test_issue_131 can use
    // it without linking ffi_primitives_impl.cpp (which
    // transitively depends on libaura-reflect symbols).
    static bool parse_ffi_sig(const std::string& sig, int& ret_type, std::vector<int>& arg_types,
                              std::string* err_type = nullptr) {
        auto arrow = sig.find("->");
        if (arrow == std::string::npos) {
            if (err_type)
                *err_type = "missing '->' in signature";
            return false;
        }
        if (sig.empty() || sig[0] != '(') {
            if (err_type)
                *err_type = "signature must start with '('";
            return false;
        }
        auto arg_part = sig.substr(1, arrow - 1);
        auto ret_part = sig.substr(arrow + 2);
        auto type_to_int = [](const std::string& tn, std::string* err = nullptr) -> int {
            auto t = tn;
            while (!t.empty() && t.front() == ' ')
                t = t.substr(1);
            while (!t.empty() && t.back() == ' ')
                t.pop_back();
            if (t == "Int")
                return 1;
            if (t == "Float")
                return 2;
            if (t == "String")
                return 3;
            if (t == "Opaque")
                return 4;
            if (t == "Void")
                return 0;
            if (err)
                *err = t.empty() ? "empty type" : "unknown type: " + t;
            return -1;
        };
        std::string cur;
        for (auto c : arg_part) {
            if (c == ' ' || c == '(' || c == ')') {
                if (!cur.empty()) {
                    int at = type_to_int(cur, err_type);
                    if (at < 0)
                        return false;
                    arg_types.push_back(at);
                    cur.clear();
                }
                continue;
            }
            cur += c;
        }
        if (!cur.empty()) {
            int at = type_to_int(cur, err_type);
            if (at < 0)
                return false;
            arg_types.push_back(at);
        }
        ret_type = type_to_int(ret_part, err_type);
        if (ret_type < 0)
            return false;
        return true;
    }

    // Register the FFI primitives (c-load, c-func,
    // c-opaque, c-alloc, c-free, c-struct-*) via the
    // given RegisterFn callback. The string_heap and
    // opaque_heap are accessed via the pointers (the
    // Evaluator owns them; this module just borrows).
    // The coverage_counters pointer is the Evaluator's
    // coverage array; nullable for tests.
    void register_primitives(RegisterFn add_primitive, std::pmr::vector<std::string>* string_heap,
                             std::vector<void*>* opaque_heap,
                             std::array<std::uint64_t, 16>* coverage_counters = nullptr);

    // Accessors for the FFI state (for tests / debug).
    std::size_t lib_count() const { return libs_.size(); }
    std::size_t func_count() const { return funcs_.size(); }
    const FFIFunc& func_at(std::size_t i) const { return funcs_[i]; }
    void* lib_at(std::size_t i) const { return libs_[i]; }

private:
    std::vector<void*> libs_;
    std::vector<FFIFunc> funcs_;
};

} // namespace aura::compiler
