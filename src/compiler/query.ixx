export module aura.compiler.query;
import std;
import aura.core;
import aura.core.ast_flat;
import aura.core.ast_pool;

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

} // namespace aura::compiler
