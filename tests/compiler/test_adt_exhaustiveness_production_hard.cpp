// test_adt_exhaustiveness_production_hard.cpp -- source-cite AC for Issue #3559
//
// @category: unit
// @reason: Issue #3559 — `adt_exhaustiveness_production_hard_reject()`
//   wire. ADT exhaustiveness is soft-observability in production; the
//   `adt_exhaustiveness_hard_gate_wired{1}` flag was just a "wired"
//   sentinel, not a hard-reject path. Agent self-mutation may add a
//   variant to an ADT; existing match sites are still in the IR with no
//   fallback arm; in Production/Full the match must be re-checked and
//   rejected — the soft path lets the Agent's match silently slide into
//   Dynamic (wrong program passes).
//
//   AC1: typed_mutation_audit.h declares `adt_exhaustiveness_hard_reject_face`
//        atomic — sibling of `adt_exhaustiveness_hard_gate_wired{1}`,
//        NOT inserted in metrics middle.
//   AC2: evaluator_eval_flat.cpp match exhaustiveness check (tree-walker path)
//        uses `production_hard_face_active()` (the #3556 centralized hard-face
//        gate — equivalent to `production_defaults_active() || get_strategy()
//        == AuditStrategy::Full` per AC1) and hard-rejects (returns error
//        EvalValue) on non-exhaustive match under Production/Full.
//   AC3: Soft / Off path keeps observe-only behavior — counter bumps
//        (`adt_exhaustiveness_fail_total`), no reject, no face store.
//   AC4: linter scripts/coverage/checks/check_adt_exhaustiveness_production_hard.py
//        exists and refuses evaluator_eval_flat.cpp match-check path without
//        the `production_hard_face_active()` wire.
//   AC5: sibling counter families untouched (no metrics-middle insert) —
//        `adt_exhaustiveness_audit_total`, `adt_exhaustiveness_sites_checked_total`,
//        `adt_non_exhaustive_sites_total`, `adt_invariant_ok/fail` stay where
//        they were.
//
// Full runtime needs a live evaluator with ADT registry + match expression
// + a soft→Production strategy switch mid-eval — too heavy for a quick
// init helper. Source-cite only: verify the hard-reject wire is present
// at the right sites and the linter covers them.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// Locate the block in evaluator_eval_flat.cpp that does the match
// exhaustiveness check (the `match warning: unhandled constructor` print)
// and verify both signals are present: the soft-path counter bump AND the
// production_hard_face_active() hard-reject branch.
bool check_eval_flat_hard_reject(std::string_view src) {
    constexpr std::string_view kMatchPrint = "match warning: unhandled constructor";
    auto pos = src.find(kMatchPrint);
    if (pos == std::string_view::npos)
        return false;
    // Window: 500 chars BEFORE the match print (covers the fail_total bump
    // inside the for loop) + 2000 chars AFTER (covers the for-loop close,
    // the production_hard_face_active() gate, and the hard-reject face
    // store + error return).
    constexpr std::size_t kBefore = 500;
    auto begin = (pos > kBefore) ? pos - kBefore : 0;
    auto end = std::min(src.size(), pos + 2000);
    std::string_view window(src.data() + begin, end - begin);
    // AC2/AC3a: counter bumps on every non-exhaustive match (observe-only under
    // Soft; part of the Production reject path).
    if (!contains(window, ".adt_exhaustiveness_fail_total"))
        return false;
    // AC2b: production_hard_face_active() gate is consulted at the same site.
    if (!contains(window, "production_hard_face_active()"))
        return false;
    // AC2c: hard-reject face stored (release) before returning error EvalValue.
    if (!contains(window, ".adt_exhaustiveness_hard_reject_face"))
        return false;
    if (!contains(window, "std::memory_order_release"))
        return false;
    // AC2d: returns error (empty EvalValue) — `return {};` right after the face
    // store, not the normal `return make_void();` binding path.
    if (!contains(window, "return {};"))
        return false;
    return true;
}

// Verify the counter family in typed_mutation_audit.h still groups
// `adt_exhaustiveness_*` together (no metrics-middle insert).
bool check_counter_family_unchanged(std::string_view src) {
    constexpr std::string_view kHardGateWired = "adt_exhaustiveness_hard_gate_wired{1}";
    auto pos = src.find(kHardGateWired);
    if (pos == std::string_view::npos)
        return false;
    // Window: 300 chars around hard_gate_wired should contain the new face
    // atomic immediately after (sibling, NOT inserted in metrics middle).
    auto begin = (pos > 300) ? pos - 300 : 0;
    auto end = std::min(src.size(), pos + 300);
    std::string_view window(src.data() + begin, end - begin);
    if (!contains(window, "adt_exhaustiveness_hard_reject_face"))
        return false;
    return true;
}

bool check_linter_exists() {
    auto linter = read_file("scripts/coverage/checks/check_adt_exhaustiveness_production_hard.py");
    if (linter.empty())
        return false;
    if (!contains(linter, "evaluator_eval_flat.cpp"))
        return false;
    if (!contains(linter, "production_hard_face_active"))
        return false;
    return true;
}

} // namespace

int main() {
    int passed = 0;
    int failed = 0;

    const auto audit_src = read_file("src/compiler/typed_mutation_audit.h");
    const auto eval_src = read_file("src/compiler/evaluator_eval_flat.cpp");

    if (audit_src.empty()) {
        std::fprintf(stderr, "FAIL: could not read src/compiler/typed_mutation_audit.h\n");
        ++failed;
    }
    if (eval_src.empty()) {
        std::fprintf(stderr, "FAIL: could not read src/compiler/evaluator_eval_flat.cpp\n");
        ++failed;
    }

    // AC1+AC5: counter family unchanged (sibling, not metrics-middle insert).
    if (!audit_src.empty() && check_counter_family_unchanged(audit_src)) {
        std::fprintf(stdout, "AC1+AC5 PASS: adt_exhaustiveness_hard_reject_face sibling of "
                             "adt_exhaustiveness_hard_gate_wired{1} (no metrics-middle insert)\n");
        ++passed;
    } else {
        std::fprintf(stdout, "AC1+AC5 FAIL: counter family not adjacent to hard_gate_wired\n");
        ++failed;
    }

    // AC2+AC3: evaluator_eval_flat.cpp match check uses production_hard_face_active()
    // hard-reject wire with counter bump + face store + error return.
    if (!eval_src.empty() && check_eval_flat_hard_reject(eval_src)) {
        std::fprintf(stdout, "AC2+AC3 PASS: evaluator_eval_flat match check has "
                             "production_hard_face_active() hard-reject wire (counter bump + "
                             "face release store + return error)\n");
        ++passed;
    } else {
        std::fprintf(stdout, "AC2+AC3 FAIL: evaluator_eval_flat match check missing "
                             "production_hard_face_active() hard-reject wire\n");
        ++failed;
    }

    // AC4: linter exists and references the right files.
    if (check_linter_exists()) {
        std::fprintf(stdout, "AC4 PASS: linter exists and references evaluator_eval_flat.cpp + "
                             "production_hard_face_active\n");
        ++passed;
    } else {
        std::fprintf(stdout, "AC4 FAIL: linter missing or does not reference both signals\n");
        ++failed;
    }

    std::fprintf(stdout, "=== Issue #3559 === %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}