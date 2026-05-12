export module aura.compiler.query;
import std;
import aura.core;
import aura.core.ast_flat;
import aura.core.ast_pool;
import aura.diag;

namespace aura::compiler {

// ── ASTIndex — zero-copy filter views on FlatAST SoA ───────────
export struct ASTIndex {
    const aura::ast::FlatAST& ast;
    aura::ast::StringPool& pool;  // intern() is non-const

    // Filter nodes by tag
    auto by_tag(aura::ast::NodeTag t) const {
        return std::views::iota(0u, ast.size())
             | std::views::filter([this, t](aura::ast::NodeId id) {
                   return ast.get(id).tag == t;
               });
    }

    // Get children of a node as a range
    auto children_of(aura::ast::NodeId id) const {
        return ast.get(id).children;
    }

    // Find calls to a specific function by name
    auto calls_to(std::string_view name) const {
        auto sym = pool.intern(name);
        return by_tag(aura::ast::NodeTag::Call)
             | std::views::filter([this, sym](aura::ast::NodeId id) {
                   auto v = ast.get(id);
                   if (v.children.empty()) return false;
                   auto callee = ast.get(v.child(0));
                   return callee.sym_id == sym;
               });
    }

    // Find all references to a symbol
    auto refs_of(std::string_view name) const {
        auto sym = pool.intern(name);
        return std::views::iota(0u, ast.size())
             | std::views::filter([this, sym](aura::ast::NodeId id) {
                   return ast.get(id).sym_id == sym;
               });
    }
};

// ── QueryExpr — parsed query AST ───────────────────────────────
export struct QueryExpr {
    enum class Kind : std::uint8_t {
        NodeType,        // (node-type Call)
        Eq,              // (= field value)
        Gt,              // (> field value)
        And, Or, Not,    // logical
        Child,           // (child N P)
        HasChild,        // (has-child P)
        Exists,          // (exists P)
        Callee,          // (callee "name")
        HasError,        // (has-error?)
        RefCount,        // (= (ref-count :node) N)
        AllNodes,        // wildcard — match everything
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
export class QueryEngine {
public:
    QueryEngine(aura::ast::FlatAST& ast,
                aura::ast::StringPool& pool)
        : index_{ast, pool} {}

    // Parse a query S-expression into a QueryExpr tree
    QueryExpr parse(std::string_view sexpr);

    // Execute a parsed query, returning matching NodeIds
    std::vector<aura::ast::NodeId> execute(const QueryExpr& q);

    // Convenience: parse + execute in one call
    std::vector<aura::ast::NodeId> query(std::string_view sexpr) {
        return execute(parse(sexpr));
    }

private:
    // Internal recursive matching
    bool match(aura::ast::NodeId id, const QueryExpr& q);

    // Parse helpers
    QueryExpr parse_node_type(std::string_view tag);
    QueryExpr parse_call(std::string_view expr);

    ASTIndex index_;
};

// ── SymRefIndex — inverted index: SymId → all referencing nodes ─
// Built by scanning FlatAST once. Enables fast "find all refs" and
// "unused definition" queries.
export class SymRefIndex {
public:
    SymRefIndex(const aura::ast::FlatAST& ast,
                aura::ast::StringPool& pool)
        : ast_(ast), pool_(pool) {}

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
        if (it != refs_.end()) return it->second;
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
            if ((v.tag == aura::ast::NodeTag::Define ||
                 v.tag == aura::ast::NodeTag::Let) &&
                v.sym_id != aura::ast::INVALID_SYM &&
                count(v.sym_id) <= 1) {
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
        CopyChild,      // (child N) — copy from match
        NewLiteral,     // (LiteralInt V)
        NewVariable,    // Variable[name]
        NewNode,        // other node type with sub-templates as children
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
    TransformEngine(aura::ast::FlatAST& ast,
                    aura::ast::StringPool& pool)
        : ast_(ast), pool_(pool) {}

    // Parse a transform rule from S-expression
    // Syntax: (query-and-fix <pattern> (fix <replacement>))
    ReplaceTemplate parse_replace(std::string_view sexpr);

    // Generate patches: for each matched node, produce replacement
    std::vector<aura::ast::Patch>
    generate_patches(const std::vector<aura::ast::NodeId>& matches,
                     const ReplaceTemplate& replacement);

    // Apply query + replace in one step
    TransformResult query_and_fix(
        aura::compiler::QueryEngine& engine,
        std::string_view query_sexpr,
        std::string_view replace_sexpr);

private:
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
            if (!cur.empty()) { tokens.push_back(std::move(cur)); }
            tokens.emplace_back(1, c);
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (!cur.empty()) { tokens.push_back(std::move(cur)); }
        } else { cur += c; }
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
    return tokens;
}

struct TokenStream {
    std::vector<std::string> t;
    std::size_t p = 0;
    std::string next() { return p < t.size() ? t[p++] : ""; }
    bool end() const { return p >= t.size(); }
};

ReplaceTemplate parse_rt(TokenStream& ts) {
    if (ts.end()) return {};
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
            if (op == "Call")      r.tag = aura::ast::NodeTag::Call;
            else if (op == "IfExpr" || op == "If") r.tag = aura::ast::NodeTag::IfExpr;
            else if (op == "Lambda") r.tag = aura::ast::NodeTag::Lambda;
            else if (op == "Begin" || op == "BeginNode") r.tag = aura::ast::NodeTag::Begin;
            else if (op == "Set" || op == "SetNode") r.tag = aura::ast::NodeTag::Set;
            else if (op == "Quote" || op == "QuoteNode") r.tag = aura::ast::NodeTag::Quote;
            while (!ts.end() && ts.t[ts.p] != ")")
                r.children.push_back(parse_rt(ts));
        }
        if (!ts.end() && ts.t[ts.p] == ")") ts.next();
        return r;
    }
    // Single token
    try { std::size_t pos; auto v = std::stoll(tok, &pos);
        if (pos == tok.size()) {
            ReplaceTemplate r; r.kind = ReplaceTemplate::Kind::NewLiteral; r.int_value = v; return r;
        }
    } catch (...) {}
    return {};
}

inline ReplaceTemplate TransformEngine::parse_replace(std::string_view sexpr) {
    auto tokens = tokenize_rt(sexpr);
    TokenStream ts{std::move(tokens), 0};
    return parse_rt(ts);
}

inline aura::ast::NodeId TransformEngine::build_node(const ReplaceTemplate& tmpl) {
    switch (tmpl.kind) {
    case ReplaceTemplate::Kind::NewLiteral:
        return ast_.add_literal(tmpl.int_value);
    case ReplaceTemplate::Kind::NewVariable:
        return ast_.add_variable(pool_.intern(tmpl.str_value));
    case ReplaceTemplate::Kind::NewNode: {
        std::vector<aura::ast::NodeId> kids;
        for (auto& c : tmpl.children) kids.push_back(build_node(c));
        if (kids.empty()) return {};
        switch (tmpl.tag) {
        case aura::ast::NodeTag::Call:
            return kids.size() == 1 ? ast_.add_call(kids[0], {})
                 : ast_.add_call(kids[0], std::span(kids.data() + 1, kids.size() - 1));
        case aura::ast::NodeTag::IfExpr:
            return kids.size() >= 3 ? ast_.add_if(kids[0], kids[1], kids[2]) : aura::ast::NULL_NODE;
        default: return kids[0];
        }
    }
    default: return aura::ast::NULL_NODE;
    }
}

inline std::vector<aura::ast::Patch>
TransformEngine::generate_patches(
    const std::vector<aura::ast::NodeId>& matches,
    const ReplaceTemplate& replacement)
{
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
        default: break;
        }
    }
    return patches;
}

inline TransformResult TransformEngine::query_and_fix(
    QueryEngine& engine,
    std::string_view qs, std::string_view rs)
{
    TransformResult r;
    auto matches = engine.query(qs);
    r.match_count = matches.size();
    if (matches.empty()) { r.applied = true; return r; }
    auto repl = parse_replace(rs);
    auto patches = generate_patches(matches, repl);
    r.patch_count = patches.size();
    if (!patches.empty()) {
        r.applied = aura::ast::apply_patches(ast_, patches);
        if (!r.applied) r.error = "patch failed";
    } else { r.applied = true; }
    return r;
}

// ── AutoFixEngine — pattern-based automatic fix ────────────────
//
// Holds a set of (query, replacement) rules. When run against a
// FlatAST, finds all matches and applies fixes in batch.
//
// Built-in rules:
//   (+ x x) → (* x 2)
//   (if 1 then else) → then
//
export class AutoFixEngine {
public:
    AutoFixEngine(aura::ast::FlatAST& ast,
                  aura::ast::StringPool& pool)
        : ast_(ast), pool_(pool) {}

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
            if (r.applied) total += r.patch_count;
        }
        return total;
    }

    // Add default optimization rules
    void add_default_rules() {
        // (if 0 X Y) → Y
        add_rule(
            "(and (node-type IfExpr) (child 0 (and (node-type LiteralInt) (= int_value 0))))",
            "(child 2)"
        );
    }

    // Add a rule from an error kind (for --fix CLI)
    void add_error_fix(aura::diag::ErrorKind kind) {
        switch (kind) {
        case aura::diag::ErrorKind::UnboundVariable:
            // Generic fallback: any reference to unbound var → LiteralInt 0
            // (the specific var name is in the message)
            add_rule("(node-type Variable)", "(LiteralInt 0)");
            break;
        default:
            break;
        }
    }

private:
    struct Rule { std::string query; std::string replacement; };
    std::vector<Rule> rules_;
    aura::ast::FlatAST& ast_;
    aura::ast::StringPool& pool_;
};

} // namespace aura::compiler
