// C++26 contract violation handler for GCC 16.1
// Required by the compiler runtime for contract violations.
// Note: <contracts> is included via -include compiler flag (CMakeLists.txt)
// to avoid header-unit resolution issues with -fmodules-ts.
//
// Issue #144: this handler used to be a one-line abort stub. It now:
//   1. Logs the violation to stderr with full context (kind, comment,
//      source location, evaluation semantic)
//   2. Calls a user-registered hook so DiagnosticCollector (or
//      observability metrics) can capture the violation for the
//      self-evolution loop to inspect
//   3. Aborts (matches the previous hard-fail behavior)
//
// The hook is set via set_contract_violation_hook(). It's a free
// function pointer (not a std::function) because the handler runs
// at global scope and must remain trivially constructible.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <contracts>

// ── Public C API for the hook ─────────────────────────────────
// We pass a C struct (not std::contracts::contract_violation) so
// the hook signature doesn't depend on <contracts> at every call
// site. The C struct is a faithful subset of the std one.
extern "C" {
struct AuraContractViolation {
    std::uint16_t kind;             // 1=pre, 2=post, 3=assert
    std::uint16_t semantic;         // 1=ignore, 2=observe, 3=enforce, 4=quick_enforce
    std::uint16_t mode;             // 1=predicate_false, 2=evaluation_exception
    const char* comment;            // human-readable condition text
    const char* file;               // source file
    std::uint32_t line;             // source line (0 if unknown)
    const char* function;           // enclosing function ("" if unknown)
};

using AuraContractViolationHook = void (*)(const AuraContractViolation*);

// Module-local hook pointer. NULL = no hook installed.
static AuraContractViolationHook g_hook = nullptr;
} // extern "C"

extern "C" void aura_set_contract_violation_hook(AuraContractViolationHook fn) {
    g_hook = fn;
}

extern "C" void aura_clear_contract_violation_hook() {
    g_hook = nullptr;
}

// ── Convert std::contracts::contract_violation → AuraContractViolation
static AuraContractViolation to_c_violation(
    const std::contracts::contract_violation& v) {
    AuraContractViolation out;
    out.kind = static_cast<std::uint16_t>(v.kind());
    out.semantic = static_cast<std::uint16_t>(v.semantic());
    out.mode = static_cast<std::uint16_t>(v.mode());
    out.comment = v.comment();
    auto loc = v.location();
    out.file = loc.file_name();
    out.line = loc.line();
    out.function = loc.function_name();
    return out;
}

static const char* kind_str(std::uint16_t k) {
    switch (k) {
        case 1: return "pre";
        case 2: return "post";
        case 3: return "assert";
        default: return "?";
    }
}

static const char* semantic_str(std::uint16_t s) {
    switch (s) {
        case 1: return "ignore";
        case 2: return "observe";
        case 3: return "enforce";
        case 4: return "quick_enforce";
        default: return "?";
    }
}

// GCC 16.1 requires this exact signature at global scope.
// The contract_violation type is in std::contracts.
void handle_contract_violation(const std::contracts::contract_violation& v) {
    auto cv = to_c_violation(v);

    // 1. Always log to stderr (matches the previous behavior + adds
    //    context that was previously missing).
    std::cerr << "contract violation: "
              << kind_str(cv.kind) << " (" << semantic_str(cv.semantic) << ")"
              << " at " << (cv.file ? cv.file : "?")
              << ":" << cv.line
              << (cv.function ? cv.function[0] ? std::string(" in ") + cv.function : "" : "")
              << " — " << (cv.comment ? cv.comment : "(no comment)")
              << "\n";

    // 2. Call the user-registered hook, if any. The hook can record
    //    the violation into DiagnosticCollector, observability
    //    metrics, etc. We catch nothing — a throwing hook would
    //    skip the abort, which is the right thing for "observe"
    //    semantic but unsafe for "enforce". Document this.
    if (g_hook) {
        g_hook(&cv);
    }

    // 3. Hard-fail. Even for "observe" semantic, the C++26 stdlib
    //    only invokes the handler on actual violations, so aborting
    //    here is sound. If we ever want "observe → continue", we
    //    can branch on cv.semantic here.
    std::abort();
}
