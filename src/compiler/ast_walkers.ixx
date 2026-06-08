// ast_walkers.ixx — Pure AST walker helpers extracted from
// CompilerService (Issue #132).
//
// These walkers are pure free functions that take FlatAST
// and StringPool references and return a result. They
// have no `this->` access, no global state, no I/O. They
// can be unit-tested in isolation and used from any
// module without coupling to CompilerService.
//
// Extracted from service.ixx's eval() and compile_module().
// These extractions are the first step of the larger
// Issue #132 refactor: extract focused helpers from the
// monolithic CompilerService methods.

module;

#include <string>
#include <utility>
#include <vector>

export module aura.compiler.ast_walkers;

import aura.core.ast;

export namespace aura::compiler {

// ── find_top_level_defines ────────────────────────────────
// Walk a FlatAST rooted at `root` and collect all
// top-level (define ...) forms. The result is a vector
// of (name, define_node_id) pairs in document order.
// Nested defines inside lambda bodies are NOT collected
// (they're not "top-level" for the purpose of caching).
//
// This was the `DefFinder` struct inside compile_module
// (service.ixx:1880). Extracted as a free function so it
// can be tested in isolation and used from other contexts.
std::vector<std::pair<std::string, aura::ast::NodeId>>
find_top_level_defines(const aura::ast::FlatAST& flat,
                       const aura::ast::StringPool& pool,
                       aura::ast::NodeId root) {
    std::vector<std::pair<std::string, aura::ast::NodeId>> defs;

    if (root == aura::ast::NULL_NODE || root >= flat.size())
        return defs;

    // Recursive lambda walker. Bounded by AST size;
    // never recurses into NULL_NODE or out-of-range ids.
    auto walk = [&](auto self, aura::ast::NodeId id) -> void {
        if (id == aura::ast::NULL_NODE || id >= flat.size())
            return;
        auto v = flat.get(id);
        if (v.tag == aura::ast::NodeTag::Define) {
            auto name = pool.resolve(v.sym_id);
            if (!name.empty())
                defs.emplace_back(std::string(name), id);
        }
        if (v.tag == aura::ast::NodeTag::Begin) {
            for (auto c : v.children)
                self(self, c);
        }
    };

    auto v = flat.get(root);
    if (v.tag == aura::ast::NodeTag::Begin) {
        for (auto c : v.children)
            walk(walk, c);
    } else {
        walk(walk, root);
    }
    return defs;
}

// ── collect_user_bindings ─────────────────────────────────
// Walk a FlatAST rooted at `root` and collect all names
// bound at the top level (via Define or TypeAnnotation).
// These are the names that subsequent eval calls should
// NOT fall through to the IR pipeline (which silently
// returns 0 for unknown variables).
//
// This was the `track_names` lambda inside eval()
// (service.ixx:773). Extracted as a free function.
std::vector<std::string> collect_user_bindings(
    const aura::ast::FlatAST& flat,
    const aura::ast::StringPool& pool,
    aura::ast::NodeId root) {
    std::vector<std::string> names;

    if (root == aura::ast::NULL_NODE || root >= flat.size())
        return names;

    auto walk = [&](auto self, aura::ast::NodeId id) -> void {
        if (id == aura::ast::NULL_NODE || id >= flat.size())
            return;
        auto v = flat.get(id);
        if (v.tag == aura::ast::NodeTag::Define) {
            auto name = pool.resolve(v.sym_id);
            if (!name.empty())
                names.push_back(std::string(name));
        }
        if (v.tag == aura::ast::NodeTag::TypeAnnotation && v.int_value != 0) {
            auto name = pool.resolve(static_cast<aura::ast::SymId>(v.int_value));
            if (!name.empty())
                names.push_back(std::string(name));
        }
        if (v.tag == aura::ast::NodeTag::Begin) {
            for (auto c : v.children)
                self(self, c);
        }
    };

    walk(walk, root);
    return names;
}

} // namespace aura::compiler
