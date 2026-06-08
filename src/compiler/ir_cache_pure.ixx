// ir_cache_pure.ixx — Pure free functions extracted from
// CompilerService and Evaluator (Issue #126).
//
// These functions take all state as parameters and return
// values without mutating global / member state. They can be
// unit-tested in isolation, composed freely, and used from
// future parallel lowering pipelines without locks.
//
// The "pure" guarantee:
//   - No `this` access (no member mutation)
//   - No I/O (no logging, no file writes)
//   - No global state lookup
//   - No mutation of any input argument (FlatAST, StringPool
//     are taken as non-const because macro expansion in
//     `try_extract_define` is read-only, but the helper
//     itself never mutates them)
//
// What is NOT pure here:
//   - `try_extract_define` calls `pool.resolve(sym_id)`,
//     which is read-only on the pool, so it is pure
//   - `compute_dependencies` walks the AST read-only
//   - `should_relower` is a pure decision function

module;

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

export module aura.compiler.ir_cache_pure;

import aura.core.ast;

export namespace aura::compiler {

// ── should_relower ────────────────────────────────────────
// Issue #126: pure decision function extracted from
// CompilerService::lookup_define_v2 (which combined this
// decision with cache lookups).
//
// Returns true iff the cached IR entry needs to be
// re-lowered. The decision combines:
//   - dirty flag (set by mutate:* / set-code on the entry)
//   - source-hash mismatch (the define body was re-defined)
//   - mutation-count drift (the global mutation counter
//     advanced beyond the snapshot at lower time)
//
// All inputs are values (no reference to the cache entry or
// the global mutation counter), so the function is fully
// pure: same inputs → same output, always.
bool should_relower(std::size_t source_hash,
                           std::size_t cached_source_hash,
                           bool dirty,
                           std::uint64_t cached_mutation_count,
                           std::uint64_t current_mutation_count) noexcept {
    if (dirty) return true;
    if (source_hash != cached_source_hash) return true;
    if (cached_mutation_count < current_mutation_count) return true;
    return false;
}

// ── compute_dependencies ──────────────────────────────────
// Issue #126: pure walker extracted from the local
// DepWalker struct inside CompilerService::record_define
// (which depended on this->ir_cache_ and this->dep_graph_).
//
// Walks the FlatAST rooted at `root` and returns the list
// of names that are both (a) referenced via Variable nodes
// and (b) present in `available_defines` (typically the
// keys of the ir_cache_ map).
//
// The output is in first-encounter order, deduplicated. The
// function never mutates the input FlatAST or StringPool.
std::vector<std::string> compute_dependencies(
    const aura::ast::FlatAST& flat,
    const aura::ast::StringPool& pool,
    aura::ast::NodeId root,
    const std::unordered_set<std::string>& available_defines) {
    std::vector<std::string> deps;
    std::unordered_set<std::string> seen;

    // Recursive lambda walker. The walk is bounded by
    // the AST size; we never recurse into NULL_NODE or
    // out-of-range ids.
    auto walk = [&](auto self, aura::ast::NodeId id) -> void {
        if (id == aura::ast::NULL_NODE || id >= flat.size())
            return;
        auto nv = flat.get(id);
        if (nv.tag == aura::ast::NodeTag::Variable) {
            std::string_view name = pool.resolve(nv.sym_id);
            if (!name.empty()) {
                std::string name_str(name);
                if (available_defines.count(name_str) &&
                    !seen.count(name_str)) {
                    seen.insert(name_str);
                    deps.push_back(name_str);
                }
            }
        }
        for (auto c : nv.children) {
            self(self, c);
        }
    };

    walk(walk, root);
    return deps;
}

// ── try_extract_define ────────────────────────────────────
// Issue #126: pure AST pattern match extracted from the
// private static CompilerService::try_extract_define
// method. The function is now a free function and can be
// called from any context (e.g., from lower_to_ir's static
// helpers, from the REPL frontend, or from test harnesses).
//
// Returns {name, body_node_id} if the root is a Define
// node, otherwise nullopt. No side effects.
std::optional<std::pair<std::string, aura::ast::NodeId>>
try_extract_define(const aura::ast::FlatAST& flat,
                   const aura::ast::StringPool& pool,
                   aura::ast::NodeId root) {
    if (root == aura::ast::NULL_NODE)
        return std::nullopt;
    if (root >= flat.size())
        return std::nullopt;
    auto v = flat.get(root);
    if (v.tag == aura::ast::NodeTag::Define) {
        std::string_view name = pool.resolve(v.sym_id);
        if (name.empty()) return std::nullopt;
        aura::ast::NodeId body =
            v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
        return std::make_pair(std::string(name), body);
    }
    return std::nullopt;
}

// ── FNV-1a 64-bit hash ────────────────────────────────────
// Issue #126: extracted as a pure function so the source
// hash logic can be unit-tested in isolation. Same input
// bytes → same hash, no global state.
std::size_t fnv1a_64(std::string_view s) noexcept {
    std::size_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= static_cast<std::size_t>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

} // namespace aura::compiler
