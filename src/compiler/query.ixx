module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

export module aura.compiler.query;
import std;
import aura.core;
import aura.core.concepts;
import aura.diag;

namespace aura::compiler {

// ── Generic AST traversal helpers (re-exports) ─────────────
//
// The actual helper definitions live in aura.core.ast
// (moved there in the Phase A hoisting commit to break the
// ast.ixx ↔ query.ixx import cycle). This module re-exports
// them under aura::compiler for backward compat with all
// existing call sites in compiler/.
//
// Adding new helpers here: define the export in
// aura.core.ast first, then add the `export using` line
// below so compiler/ callers see them without an extra
// import.
export using ::aura::ast::walk_children;
export using ::aura::ast::count_nodes_with_predicate;
export using ::aura::ast::find_first_node_with;
export using ::aura::ast::walk_ancestors;
export using ::aura::ast::count_nodes_with_tag;

// ── ASTIndex — zero-copy filter views on FlatAST SoA ───────────
export struct ASTIndex {
    const aura::ast::FlatAST& ast;
    aura::ast::StringPool& pool;

    struct TagIndex {
        static constexpr std::size_t TAG_COUNT = 15;
        std::array<std::vector<aura::ast::NodeId>, TAG_COUNT> tags;
        bool built = false;
        void build(const aura::ast::FlatAST& a) {
            if (built)
                return;
            for (auto& v : tags)
                v.clear();
            for (aura::ast::NodeId id = 0; id < a.size(); ++id) {
                auto t = static_cast<std::size_t>(a.get(id).tag);
                if (t < TAG_COUNT)
                    tags[t].push_back(id);
            }
            built = true;
        }
        std::span<const aura::ast::NodeId> nodes(aura::ast::NodeTag t) const {
            auto idx = static_cast<std::size_t>(t);
            if (idx < TAG_COUNT)
                return tags[idx];
            return {};
        }
    };
    mutable TagIndex tag_index_;
    mutable void* sym_ref_ = nullptr;
    template <typename T> void set_sym_index(T& r) const { sym_ref_ = &r; }

    // Filter nodes by tag
    // Filter nodes by tag — O(1) after first call (lazy-builds TagIndex)
    std::span<const aura::ast::NodeId> by_tag(aura::ast::NodeTag t) const {
        tag_index_.build(ast);
        return tag_index_.nodes(t);
    }

    // Get children of a node as a range
    auto children_of(aura::ast::NodeId id) const { return ast.get(id).children; }

    // Find calls to a specific function by name
    // Find calls to a specific function by name — uses TagIndex
    auto calls_to(std::string_view name) const {
        auto sym = pool.intern(name);
        auto call_nodes = by_tag(aura::ast::NodeTag::Call);
        return call_nodes | std::views::filter([this, sym](aura::ast::NodeId id) {
                   auto v = ast.get(id);
                   if (v.children.empty())
                       return false;
                   auto callee = ast.get(v.child(0));
                   return callee.sym_id == sym;
               });
    }

    // Find all references to a symbol
    auto refs_of(std::string_view name) const {
        auto sym = pool.intern(name);
        // Use TagIndex to only scan nodes with sym_id != INVALID_SYM
        // This is still O(N) but avoids checking nodes with no symbol.
        // Future: integrate with SymRefIndex for O(1) lookups.
        return by_tag(aura::ast::NodeTag::Variable) |
               std::views::filter(
                   [this, sym](aura::ast::NodeId id) { return ast.get(id).sym_id == sym; });
    }

    // Invalidate when AST changes (call after applying patches)
    void invalidate_tag_index() const { tag_index_.built = false; }
};


// ── QueryExpr — parsed query AST ───────────────────────────────
export struct QueryExpr {
    enum class Kind : std::uint8_t {
        NodeType, // (node-type Call)
        Eq,       // (= field value)
        Gt,       // (> field value)
        And,
        Or,
        Not,        // logical
        Child,      // (child N P)
        HasChild,   // (has-child P)
        Exists,     // (exists P)
        Callee,     // (callee "name")
        HasError,   // (has-error?)
        RefCount,   // (= (ref-count :node) N)
        AllNodes,   // wildcard — match everything
        HasType,    // (has-type? Int) — node's type_id matches
        ReturnType, // (return-type Int) — call return type matches
        ArgType,    // (argument-type 0 Int) — call arg type matches
        // Issue #62 Iter 4: global observability queries. Routed
        // through a separate code path (not per-node match); see
        // QueryEngine::execute_global().
        DeoptCount,          // (:deopt-count)
        ArenaUsage,          // (:arena-usage)
        SpecializationCount, // (:specialization-count)
    };

    Kind kind = Kind::AllNodes;
    aura::ast::NodeTag node_tag = aura::ast::NodeTag::LiteralInt;
    std::string field_name;
    std::string str_value;
    std::int64_t int_value = 0;
    std::uint32_t child_index = 0;
    std::vector<QueryExpr> children;
};

// ── QueryEngine — parse + execute queries ──────────────────────
export class SymRefIndex;

export class QueryEngine {
public:
    // Issue #62 Iter 4: read-only metrics source. The QueryEngine
    // doesn't own a CompilerService; this interface lets the
    // --query CLI inject the live counters from outside. Callers
    // implement this on the metrics struct (just return the
    // atomic.load()). Non-virtual; stack-only.
    struct MetricsProvider {
        std::uint64_t deopt_count() const { return 0; }
        std::uint64_t arena_bytes_used() const { return 0; }
        std::uint64_t jit_compilations() const { return 0; }
    };

    QueryEngine(aura::ast::FlatAST& ast, aura::ast::StringPool& pool)
        : index_{ast, pool} {}

    // Parse a query S-expression into a QueryExpr tree
    QueryExpr parse(std::string_view sexpr);

    // Execute a parsed query, returning matching NodeIds
    std::vector<aura::ast::NodeId> execute(const QueryExpr& q) pre(index_.ast.size() > 0);

    // Issue #62 Iter 4: global observability query. Returns the
    // counter value for one of the 3 new kinds (DeoptCount,
    // ArenaUsage, SpecializationCount), or
    // std::numeric_limits<uint64_t>::max() for other kinds.
    std::uint64_t execute_global(const QueryExpr& q) const;

    // Issue #62 Iter 4: install an external metrics source. The
    // provider is non-owning; lifetime must outlive this engine.
    void set_metrics_provider(MetricsProvider* p) { metrics_provider = p; }

    // Convenience: parse + execute in one call
    std::vector<aura::ast::NodeId> query(std::string_view sexpr) { return execute(parse(sexpr)); }

private:
    // Internal recursive matching (depth-limited)
    bool match(aura::ast::NodeId id, const QueryExpr& q, int depth = 0) pre(depth >= 0);

    // Parse helpers
    QueryExpr parse_node_type(std::string_view tag);
    QueryExpr parse_call(std::string_view expr);

    ASTIndex index_;
    mutable SymRefIndex* sym_index_ = nullptr;
    mutable bool sym_index_built_ = false;
    MetricsProvider* metrics_provider = nullptr;

    void ensure_sym_index() const;
};

// ── SymRefIndex — inverted index: SymId → all referencing nodes ─
// Built by scanning FlatAST once. Enables fast "find all refs" and
// "unused definition" queries.
export class SymRefIndex {
public:
    SymRefIndex(const aura::ast::FlatAST& ast, aura::ast::StringPool& pool)
        : ast_(ast)
        , pool_(pool) {}

    // Build the inverted index (O(N) scan). Call once after AST is built.
    void build() {
        refs_.clear();
        for (aura::ast::NodeId id = 0; id < ast_.size(); ++id) {
            auto v = ast_.get(id);
            if (v.sym_id != aura::ast::INVALID_SYM)
                refs_[v.sym_id].push_back(id);
        }
    }

    // All references to a given symbol
    std::span<const aura::ast::NodeId> refs_of(aura::ast::SymId sym) const {
        auto it = refs_.find(sym);
        if (it != refs_.end())
            return it->second;
        return {};
    }

    // All references to a named symbol
    std::span<const aura::ast::NodeId> refs_of(std::string_view name) {
        auto sym = pool_.intern(name);
        return refs_of(sym);
    }

    // Number of references to a symbol
    std::size_t count(aura::ast::SymId sym) const {
        auto it = refs_.find(sym);
        return it != refs_.end() ? it->second.size() : 0;
    }

    // Which nodes have zero references (potential dead code)
    std::vector<aura::ast::NodeId> unused_defs() const {
        std::vector<aura::ast::NodeId> result;
        for (aura::ast::NodeId id = 0; id < ast_.size(); ++id) {
            auto v = ast_.get(id);
            if ((v.tag == aura::ast::NodeTag::Define || v.tag == aura::ast::NodeTag::Let) &&
                v.sym_id != aura::ast::INVALID_SYM && count(v.sym_id) <= 1) {
                result.push_back(id);
            }
        }
        return result;
    }

    // Total unique symbols indexed
    std::size_t unique_symbols() const { return refs_.size(); }

private:
    const aura::ast::FlatAST& ast_;
    aura::ast::StringPool& pool_;
    std::unordered_map<aura::ast::SymId, std::vector<aura::ast::NodeId>> refs_;
};


// ── TypeResolutionIndex — resolved type name per node ──────────
// Built by scanning FlatAST after type-checking. Maps each node to
// its resolved type name (e.g. "Int", "String", "(-> Int Int)").
// Enables type-aware queries like (has-type? Int) by name lookup.
export class TypeResolutionIndex {
public:
    TypeResolutionIndex(const aura::ast::FlatAST& ast, const aura::ast::StringPool& pool)
        : ast_(ast)
        , pool_(pool) {}

    // Build the index: scan all nodes and cache resolved type names.
    // Call once after type-checking is complete.
    void build() {
        type_of_.clear();
        for (aura::ast::NodeId id = 0; id < ast_.size(); ++id) {
            auto tid = ast_.type_id(id);
            if (tid > 0) {
                type_of_[id] = tid; // store raw type_id for fast match
            }
        }
    }

    // Get the resolved type_id for a node (0 = unknown/dynamic)
    std::uint32_t type_of(aura::ast::NodeId id) const {
        auto it = type_of_.find(id);
        return it != type_of_.end() ? it->second : 0;
    }

    // All nodes that resolved to the given type_id
    std::vector<aura::ast::NodeId> nodes_of_type(std::uint32_t type_id) const {
        std::vector<aura::ast::NodeId> result;
        for (auto& [id, tid] : type_of_) {
            if (tid == type_id)
                result.push_back(id);
        }
        return result;
    }

private:
    const aura::ast::FlatAST& ast_;
    const aura::ast::StringPool& pool_;
    std::unordered_map<aura::ast::NodeId, std::uint32_t> type_of_;
};

// ── Transform — pattern → replacement rule ─────────────────────
// Replacement template syntax:
//   (replace-with (Call (child 0) (LiteralInt 42)))
//   - (child N)       → copy Nth child from matched node
//   - (LiteralInt V)  → create new LiteralInt node
//   - Variable[name]  → create new Variable node with name
//   - otherwise       → create node by tag name with child templates
//
export struct ReplaceTemplate {
    enum class Kind : std::uint8_t {
        CopyChild,   // (child N) — copy from match
        NewLiteral,  // (LiteralInt V)
        NewVariable, // Variable[name]
        NewNode,     // other node type with sub-templates as children
    };

    Kind kind = Kind::NewNode;
    aura::ast::NodeTag tag = aura::ast::NodeTag::LiteralInt;
    std::int64_t int_value = 0;
    std::string str_value;
    std::uint32_t child_index = 0;
    std::vector<ReplaceTemplate> children;
};

// Result of a transform operation
export struct TransformResult {
    bool applied = false;
    std::size_t match_count = 0;
    std::size_t patch_count = 0;
    std::string error;
};

// ── TransformEngine — apply transforms to FlatAST ──────────────
export class TransformEngine {
public:
    TransformEngine(aura::ast::FlatAST& ast, aura::ast::StringPool& pool)
        : ast_(ast)
        , pool_(pool) {}

    // Parse a transform rule from S-expression
    // Syntax: (query-and-fix <pattern> (fix <replacement>))
    ReplaceTemplate parse_replace(std::string_view sexpr);

    // Generate patches: for each matched node, produce replacement
    std::vector<aura::ast::Patch> generate_patches(std::span<const aura::ast::NodeId> matches,
                                                   const ReplaceTemplate& replacement);

    // Apply query + replace in one step
    TransformResult query_and_fix(aura::compiler::QueryEngine& engine, std::string_view query_sexpr,
                                  std::string_view replace_sexpr);

private:
    std::unordered_map<std::string, ReplaceTemplate, aura::core::TransparentStringHash,
                       std::equal_to<>>
        parse_cache_;
    aura::ast::NodeId build_node(const ReplaceTemplate& tmpl);

    aura::ast::FlatAST& ast_;
    aura::ast::StringPool& pool_;
};

// ── Inline implementation helpers ──────────────────────────────

// Parse a replacement template from token stream
std::vector<std::string> tokenize_rt(std::string_view s) {
    std::vector<std::string> tokens;
    std::string cur;
    for (auto c : s) {
        if (c == '(' || c == ')') {
            if (!cur.empty()) {
                tokens.push_back(std::move(cur));
            }
            tokens.emplace_back(1, c);
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (!cur.empty()) {
                tokens.push_back(std::move(cur));
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        tokens.push_back(std::move(cur));
    return tokens;
}

struct TokenStream {
    std::vector<std::string> t;
    std::size_t p = 0;
    std::string next() { return p < t.size() ? t[p++] : ""; }
    bool end() const { return p >= t.size(); }
};

ReplaceTemplate parse_rt(TokenStream& ts) {
    if (ts.end())
        return {};
    auto tok = ts.next();
    if (tok == "(") {
        auto op = ts.next();
        ReplaceTemplate r;
        if (op == "child") {
            r.kind = ReplaceTemplate::Kind::CopyChild;
            r.child_index = (std::uint32_t)std::stoul(ts.next());
        } else if (op == "LiteralInt") {
            r.kind = ReplaceTemplate::Kind::NewLiteral;
            r.int_value = std::stoll(ts.next());
        } else if (op == "Variable") {
            r.kind = ReplaceTemplate::Kind::NewVariable;
            r.str_value = ts.next();
        } else {
            r.kind = ReplaceTemplate::Kind::NewNode;
            if (op == "Call")
                r.tag = aura::ast::NodeTag::Call;
            else if (op == "IfExpr" || op == "If")
                r.tag = aura::ast::NodeTag::IfExpr;
            else if (op == "Lambda")
                r.tag = aura::ast::NodeTag::Lambda;
            else if (op == "Begin" || op == "BeginNode")
                r.tag = aura::ast::NodeTag::Begin;
            else if (op == "Set" || op == "SetNode")
                r.tag = aura::ast::NodeTag::Set;
            else if (op == "Quote" || op == "QuoteNode")
                r.tag = aura::ast::NodeTag::Quote;
            else if (op == "LiteralString" || op == "String")
                r.tag = aura::ast::NodeTag::LiteralString;
            else if (op == "Coercion" || op == "CoercionNode")
                r.tag = aura::ast::NodeTag::Coercion;
            while (!ts.end() && ts.t[ts.p] != ")")
                r.children.push_back(parse_rt(ts));
        }
        if (!ts.end() && ts.t[ts.p] == ")")
            ts.next();
        return r;
    }
    // Single token
    try {
        std::size_t pos;
        auto v = std::stoll(tok, &pos);
        if (pos == tok.size()) {
            ReplaceTemplate r;
            r.kind = ReplaceTemplate::Kind::NewLiteral;
            r.int_value = v;
            return r;
        }
    } catch (...) {
        // [SILENCE-PRIM-#615] stoll non-integer token → empty
        // ReplaceTemplate; caller handles nullish (#1669 class A).
    }
    return {};
}

inline ReplaceTemplate TransformEngine::parse_replace(std::string_view sexpr) {
    auto it = parse_cache_.find(std::string(sexpr));
    if (it != parse_cache_.end())
        return it->second;
    auto tokens = tokenize_rt(sexpr);
    TokenStream ts{std::move(tokens), 0};
    auto result = parse_rt(ts);
    parse_cache_[std::string(sexpr)] = result;
    return result;
}

inline aura::ast::NodeId TransformEngine::build_node(const ReplaceTemplate& tmpl) {
    switch (tmpl.kind) {
        case ReplaceTemplate::Kind::NewLiteral:
            return ast_.add_literal(tmpl.int_value);
        case ReplaceTemplate::Kind::NewVariable:
            return ast_.add_variable(pool_.intern(tmpl.str_value));
        case ReplaceTemplate::Kind::NewNode: {
            std::vector<aura::ast::NodeId> kids;
            for (auto& c : tmpl.children)
                kids.push_back(build_node(c));
            if (kids.empty())
                return {};
            switch (tmpl.tag) {
                case aura::ast::NodeTag::Call:
                    return kids.size() == 1 ? ast_.add_call(kids[0], {})
                                            : ast_.add_call(kids[0], std::span(kids.data() + 1,
                                                                               kids.size() - 1));
                case aura::ast::NodeTag::IfExpr:
                    return kids.size() >= 3 ? ast_.add_if(kids[0], kids[1], kids[2])
                                            : aura::ast::NULL_NODE;
                default:
                    return kids[0];
            }
        }
        default:
            return aura::ast::NULL_NODE;
    }
}

inline std::vector<aura::ast::Patch>
TransformEngine::generate_patches(std::span<const aura::ast::NodeId> matches,
                                  const ReplaceTemplate& replacement) {
    std::vector<aura::ast::Patch> patches;
    for (auto mid : matches) {
        auto v = ast_.get(mid);
        switch (replacement.kind) {
            case ReplaceTemplate::Kind::NewLiteral:
                patches.push_back({mid, 0, (std::uint64_t)aura::ast::NodeTag::LiteralInt});
                patches.push_back({mid, 1, (std::uint64_t)replacement.int_value});
                break;
            case ReplaceTemplate::Kind::CopyChild:
                if (replacement.child_index < v.children.size()) {
                    auto cv = ast_.get(v.child(replacement.child_index));
                    patches.push_back({mid, 0, (std::uint64_t)cv.tag});
                    patches.push_back({mid, 1, (std::uint64_t)cv.int_value});
                    patches.push_back({mid, 2, (std::uint64_t)cv.sym_id});
                }
                break;
            case ReplaceTemplate::Kind::NewNode: {
                auto nid = build_node(replacement);
                if (nid != aura::ast::NULL_NODE) {
                    auto nv = ast_.get(nid);
                    patches.push_back({mid, 0, (std::uint64_t)nv.tag});
                    patches.push_back({mid, 1, (std::uint64_t)nv.int_value});
                    patches.push_back({mid, 2, (std::uint64_t)nv.sym_id});
                    auto min_c = std::min(v.children.size(), nv.children.size());
                    for (std::size_t i = 0; i < min_c; ++i)
                        ast_.set_child(mid, (std::uint32_t)i, nv.child((std::uint32_t)i));
                }
                break;
            }
            default:
                break;
        }
    }
    return patches;
}

inline TransformResult TransformEngine::query_and_fix(QueryEngine& engine, std::string_view qs,
                                                      std::string_view rs) {
    TransformResult r;
    auto matches = engine.query(qs);
    r.match_count = matches.size();
    if (matches.empty()) {
        r.applied = true;
        return r;
    }
    auto repl = parse_replace(rs);
    auto patches = generate_patches(matches, repl);
    r.patch_count = patches.size();
    if (!patches.empty()) {
        r.applied = aura::ast::apply_patches(ast_, patches);
        if (!r.applied)
            r.error = "patch failed";
    } else {
        r.applied = true;
    }
    return r;
}

// ── AutoFixEngine — pattern-based automatic fix ────────────────
//
// Holds a set of (query, replacement) rules. When run against a
// FlatAST, finds all matches and applies fixes in batch.
//
// Built-in default rules (#966):
//   (if 0 then else) → else
//   (if 1 then else) → then
//   (+ x x) → (* x 2)   [when both children are the same Variable]
//
export class AutoFixEngine {
public:
    AutoFixEngine(aura::ast::FlatAST& ast, aura::ast::StringPool& pool)
        : ast_(ast)
        , pool_(pool) {}

    // Add a fix rule: when <query> matches, apply <replacement>
    void add_rule(std::string_view query, std::string_view replacement) {
        rules_.push_back({std::string(query), std::string(replacement)});
    }

    // Run all rules against the FlatAST. Returns total patches applied.
    std::size_t run_all() {
        std::size_t total = 0;
        for (auto& rule : rules_) {
            aura::compiler::QueryEngine engine(ast_, pool_);
            aura::compiler::TransformEngine xform(ast_, pool_);
            auto r = xform.query_and_fix(engine, rule.query, rule.replacement);
            if (r.applied)
                total += r.patch_count;
        }
        return total;
    }

    // Add default optimization rules (Issue #966: comment/code alignment)
    void add_default_rules() {
        // (if 0 X Y) → Y
        add_rule("(and (node-type IfExpr) (child 0 (and (node-type LiteralInt) (= int_value 0))))",
                 "(child 2)");
        // (if 1 X Y) → X
        add_rule("(and (node-type IfExpr) (child 0 (and (node-type LiteralInt) (= int_value 1))))",
                 "(child 1)");
        // (+ x x) → (* x 2) when both children are Variable with same name is hard
        // in pure pattern DSL; fold (+ n n) style via two equal LiteralInt is
        // deferred. Keep structural (if) rules as the safe defaults.
    }

    // Add a rule from an error kind (for --fix CLI)
    // Issue #962: never replace every Variable with 0 — that destroyed
    // valid bindings. UnboundVariable needs a name-scoped diagnostic fix
    // (future: bind to a gensym default or report-only). Phase 1: no-op.
    void add_error_fix(aura::diag::ErrorKind kind) {
        switch (kind) {
            case aura::diag::ErrorKind::UnboundVariable:
                // Intentionally empty — catastrophic global Variable→0 removed.
                break;
            default:
                break;
        }
    }

private:
    struct Rule {
        std::string query;
        std::string replacement;
    };
    std::vector<Rule> rules_;
    aura::ast::FlatAST& ast_;
    aura::ast::StringPool& pool_;
};

} // namespace aura::compiler
