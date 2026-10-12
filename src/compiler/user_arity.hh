// Issue #4467: user procedures, lambdas, and named let are arity-checked
// at the call. A fixed-arity procedure must be called with exactly its
// parameters. A dotted rest still requires each named parameter; extra
// arguments belong to the rest list. (lambda rest ...) accepts any count.
//
// apply_closure returns optional<EvalValue>. nullopt is InvalidClosure for
// its callers, so an arity miss is parked here and the caller that has the
// call's source location turns it into ArityMismatch. One inline
// thread_local so the tree-walker TU and the IR interpreter TU share it.

#ifndef AURA_COMPILER_USER_ARITY_HH
#define AURA_COMPILER_USER_ARITY_HH

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aura::compiler {

inline thread_local std::string pending_user_arity_message;

inline void clear_pending_user_arity() noexcept {
    pending_user_arity_message.clear();
}

inline void set_pending_user_arity(std::string message) {
    pending_user_arity_message = std::move(message);
}

inline std::optional<std::string> take_pending_user_arity() {
    if (pending_user_arity_message.empty())
        return std::nullopt;
    std::optional<std::string> out = std::move(pending_user_arity_message);
    pending_user_arity_message.clear();
    return out;
}

// param_count includes the rest symbol when dotted.
[[nodiscard]] inline bool user_arity_mismatch(std::size_t param_count, bool dotted,
                                              std::size_t got) noexcept {
    if (dotted) {
        const std::size_t named = param_count == 0 ? 0 : param_count - 1;
        return got < named;
    }
    return got != param_count;
}

[[nodiscard]] inline std::string user_arity_message(std::string_view name, std::size_t param_count,
                                                    bool dotted, std::size_t got) {
    std::string who(name.empty() ? "lambda" : name);
    if (dotted) {
        const std::size_t named = param_count == 0 ? 0 : param_count - 1;
        const char* word = named == 1 ? "argument" : "arguments";
        return who + " expects at least " + std::to_string(named) + " " + word + ", got " +
               std::to_string(got);
    }
    const char* word = param_count == 1 ? "argument" : "arguments";
    return who + " expects " + std::to_string(param_count) + " " + word + ", got " +
           std::to_string(got);
}

// IR define closures are named __lambda__. The call's variable is the
// name the diagnostic should use when the bridge drops the IR result.
inline void prefer_user_arity_call_name(std::string& message, std::string_view call_name) {
    constexpr std::string_view kLambda = "lambda";
    if (call_name.empty() || call_name == kLambda)
        return;
    if (message.starts_with(kLambda) &&
        (message.size() == kLambda.size() || message[kLambda.size()] == ' '))
        message.replace(0, kLambda.size(), call_name);
}

} // namespace aura::compiler

#endif // AURA_COMPILER_USER_ARITY_HH
