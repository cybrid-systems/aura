// offload/offload_job.h — private-compiler job bodies (issues #4887/#4888)
//
// The pool worker never receives a session Evaluator: each job builds a
// thread-private CompilerService, loads the immutable snapshot source, and
// returns a plain Result. The snapshot carries no session state.
#ifndef AURA_OFFLOAD_OFFLOAD_JOB_H
#define AURA_OFFLOAD_OFFLOAD_JOB_H

#include "offload_pool.h"

#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace aura::offload {

// Issue #4888: run the requested stages in order; stop at the first stage
// that cannot execute (stage execution failure, not a type/test failure).
// Supported stages: "typecheck", "test" (compile is accepted as a no-op
// because the private tree-walker is already loaded).
[[nodiscard]] Result run_stages(const SnapshotInput& input, const std::vector<std::string>& stages,
                                std::stop_token st);

// Issue #4887: evaluate a code string in a private CompilerService seeded
// with input.source. Returns the formatted value or an error string.
[[nodiscard]] std::string eval_code(const SnapshotInput& input, std::string_view code);

} // namespace aura::offload

#endif // AURA_OFFLOAD_OFFLOAD_JOB_H
