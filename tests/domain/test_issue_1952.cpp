// tests/domain/test_issue_1952.cpp — Wave 4 relocate from tests/test_issue_1952.cpp
// Prefer domain/; do not re-add under tests/ root. (#root_test_classification)
// test_issue_1952.cpp — Issue #1952: AOT incremental re-emit pipeline +
// stable DefineId/func_id persistence (MVP scope per Anqi comment).
//
// AC check (Issue #1952):
//   #1 aura_reemit_aot_for_dirty returns actual re-emit count — verified
//      at the C-bridge call site: the new aura_set_aot_emit_fn callback
//      returns bool (true = real emit succeeded). On true we bump
//      aot_incremental_reemit_success_total + stable_func_id_preserved_total
//      and the function's return value reflects the success count.
//   #2 <5% recompile cost on small body change — out of micro-benchmark
//      scope for this TU; tracked as cycle 2 follow-up.
//   #3 func_id stable after re-emit — MVP stub bumps 1:1 with success.
//      Full cross-epoch func_id mapping is deferred per Anqi comment.
//   #4 new metrics: aot_incremental_reemit_success_total +
//      stable_func_id_preserved_total — both declared in
//      observability_metrics.h + accessible via Evaluator getters.
//   #5 1000-round mutate + hot-update fuzz no stale IR / UAF — covered
//      by existing test_runtime_concurrent_full_cycle_chaos.
//   #6 alignment with #1930 — the success metric pairs with the existing
//      aot_incremental_reemit_count (#1480).
//
// Static reachability check (this TU): the helper callback API + getter
// methods + new primitive must be reachable via the module surface.

#include "test_harness.hpp"

import std;
import aura.compiler.evaluator;
import aura.compiler.value;

using aura::compiler::Evaluator;
using aura::test::g_failed;
using aura::test::g_passed;

int main() {
    std::println("\n--- Issue #1952: AOT incremental re-emit MVP ---");

    // AC #4: 2 new metrics reachable via Evaluator getters (the bridge
    // bumps them; the getters expose them to EDSL via
    // query:aot-incremental-reemit-stats).
    {
        std::println("\n--- AC4: metrics + getters reachable ---");
        Evaluator ev;
        // Default 0 on a fresh evaluator (no re-emit activity yet).
        CHECK(ev.get_aot_incremental_reemit_success_total() == 0,
              "get_aot_incremental_reemit_success_total default 0");
        CHECK(ev.get_stable_func_id_preserved_total() == 0,
              "get_stable_func_id_preserved_total default 0");
        CHECK(ev.get_aot_incremental_reemit_count() == 0,
              "get_aot_incremental_reemit_count default 0");
        CHECK(ev.get_aot_closure_dependency_reemit_total() == 0,
              "get_aot_closure_dependency_reemit_total default 0");
    }

    std::println("\n--- Issue #1952: {} passed, {} failed ---", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
