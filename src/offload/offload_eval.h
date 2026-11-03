// offload/offload_eval.h — Issue #4887
//
// Abstract bridge from the evaluator module to the isolated offload pool.
// The evaluator cannot name CompilerService (module cycle), so the runtime
// publishes an implementation pointer (OffloadService) at startup. The
// interface is header-only: evaluator-only test targets link no pool code.
#ifndef AURA_OFFLOAD_OFFLOAD_EVAL_H
#define AURA_OFFLOAD_OFFLOAD_EVAL_H

#include <string>

namespace aura::offload {

struct OffloadEval {
    virtual ~OffloadEval() = default;
    // Evaluate a code string in the private snapshot on the pool. Fiber
    // context parks until the ticket is terminal; non-fiber context runs
    // inline. Returns the formatted result or the error message.
    [[nodiscard]] virtual std::string eval_async(const std::string& code) = 0;
};

// Issue #4887: runtime-owned adapter backed by OffloadService. Defined in
// offload_eval_bridge.cpp (aura binary only), so evaluator-only test targets
// link neither the pool nor CompilerService.
[[nodiscard]] OffloadEval& offload_eval_instance();
void set_offload_eval_source(std::string source);

} // namespace aura::offload

#endif // AURA_OFFLOAD_OFFLOAD_EVAL_H
