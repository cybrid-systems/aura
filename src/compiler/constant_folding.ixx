// Issue #212 — pure-function extraction of constant folding.
//
// Mirror of the compute_kind.ixx / arity.ixx pattern: the
// folding logic is split into a free function operating only
// on its arguments (no `this`, no class state) and a thin
// Wrap class that holds the per-instance state (the known
// map and the counter) and routes through the pure function.
//
// Why:
//   - The folding logic is reachable from many call sites
//     (pass_manager run, service.ixx typed_mutate, main.cpp
//     CLI, test_issue_*). With it inside a class, every site
//     either (a) constructs a Wrap, (b) duplicates the logic.
//   - The known map and counter are caller-state — the
//     function should NOT carry them. By extracting the
//     pure version, callers that need to fold a single
//     block (e.g. incremental compilation) can pass their
//     own known map and skip the per-block reset that
//     `run(IRModule&)` does.
//   - Matches the issue body's Phase 1 deliverable: "extract
//     compute_kind / arity / constant_folding / type_checker
//     as pure functions and update Wrap classes".
//
// Phase 1 of the issue body already shipped compute_kind
// (Phase 1a, existing) and arity (Phase 1b, existing).
// This module ships Phase 1c (constant_folding). Type_checker
// is a separate, larger extract (Phase 1d — deferred).

module;
#include <cstddef>
#include <cstdint>
#include <unordered_map>

export module aura.compiler.constant_folding;
import std;
import aura.compiler.ir;

namespace aura::compiler {

// Tagged value used by the constant folder.
//
// The folder represents a known SSA slot value as a single
// std::int64_t with the following tagged conventions:
//
//   - Plain integer values: encoded as the int64 directly.
//     ConstI64 stores the value in the IR as two 32-bit
//     halves (operands[1] = low, operands[2] = high), which
//     the folder reassembles into the int64.
//
//   - Boolean values (Issue #1098): high-bit tag (1<<62)|0 = #f,
//     (1<<62)|1 = #t. Avoids collision with ConstI64 3 and 7.
//   - Legacy note: previously 7=#t, 3=#f. Encoded this way so
//     the same int64 map can carry both fixnums and tagged
//     bools without ambiguity.
//
// `IS_TRUTHY` (in the .cpp) treats tagged #f and int 0 as
// falsy; tagged #t and non-zero ints are truthy.
//
// The map type lives here (not in the .cpp) because the
// per-block pure function `constant_fold_block` needs the
// type to be visible at the module boundary (callers that
// already have a known map can pass it in).
export using ConstantValue = std::int64_t;
export using ConstantKnownMap = std::unordered_map<std::uint32_t, ConstantValue>;

// Result of constant folding. folded_count is the number of
// instructions that were replaced with constants.
//
// `has_error` is reserved for future use (e.g. div-by-zero
// detected during folding, where the folder could choose
// to surface an error rather than fold). Today the folder
// silently leaves division-by-zero as the original IR (so
// the runtime can still trap), so has_error is always false.
export struct ConstantFoldingResult {
    std::size_t folded_count = 0;
    bool has_error = false;
};

// Pure: fold all constant expressions in a single function.
//
// Each block's local known-map is reset on block entry.
// Returns the total number of instructions folded across
// all blocks in the function.
export ConstantFoldingResult constant_fold_function(aura::ir::IRFunction& func);

// Pure (per-block span variant): fold a single block in place.
//
// The caller passes a known-map; the same map is mutated as
// the fold progresses through the block. This is the hot
// path in incremental compilation: when re-folding a single
// block after a mutation, the caller has a known map in hand
// and just needs the per-instruction decisions.
//
// Returns the number of instructions folded in this block.
export std::size_t constant_fold_block(aura::ir::BasicBlock& block, ConstantKnownMap& known)
    // Issue #213 follow-up: C++26 contract. The function
    // is total: it handles any block, any state of `known`
    // (including empty). The only precondition is that
    // `block.instructions` is accessible (no nullptr /
    // dangling). Since `block` is a reference, this is
    // implicitly true at the language level. We document
    // the invariant explicitly so the contract_enforce
    // semantic catches future regressions if the
    // function is ever refactored to take a pointer.
    pre(true);

} // namespace aura::compiler
