export module aura.parser.parser;
import std;
import aura.core;
import aura.core.ast_flat;
import aura.core.ast_pool;
import aura.parser.lexer;

namespace aura::parser {

export struct ParseResult { ast::Expr* root = nullptr; bool success = false; std::string error; ast::ASTArena* arena = nullptr; };

// Result from FlatParser (direct FlatAST, no Expr* intermediate)
export struct FlatParseResult {
    aura::ast::NodeId root = aura::ast::NULL_NODE;
    bool success = false;
    std::string error;
};

// ── Legacy Expr* parser ─────────────────────────────────────────
export class Parser {
    friend ParseResult parse(std::string_view, ast::ASTArena&);
public:
    explicit Parser(ast::ASTArena& a) : arena_(a) {}
    ParseResult parse(std::string_view source);
private:
    ast::Expr* parse_expr(); ast::Expr* parse_int(Token tok); ast::Expr* parse_list();
    ast::Expr* parse_if(); ast::Expr* parse_lambda(); ast::Expr* parse_let(bool r); ast::Expr* parse_define();
    ast::Expr* parse_val(); void skip_rparen();
    ast::ASTArena& arena_; std::optional<Lexer> lexer_;
};

export inline ParseResult parse(std::string_view source, ast::ASTArena& arena) {
    Parser parser(arena);
    return parser.parse(source);
}

// ── FlatParser — writes directly to FlatAST (SoA), bypasses Expr* ─
// Phase 4: new code should use parse_to_flat() instead of parse().
export class FlatParser {
public:
    FlatParser(aura::ast::FlatAST& flat, aura::ast::StringPool& pool)
        : flat_(flat), pool_(pool) {}
    FlatParseResult parse(std::string_view source);
private:
    aura::ast::NodeId parse_expr();
    aura::ast::NodeId parse_int(Token tok);
    aura::ast::NodeId parse_list();
    aura::ast::NodeId parse_if();
    aura::ast::NodeId parse_lambda();
    aura::ast::NodeId parse_let(bool rec);
    aura::ast::NodeId parse_define();
    aura::ast::NodeId parse_val();
    std::vector<aura::ast::NodeId> parse_bindings();
    void skip_rparen();
    aura::ast::FlatAST& flat_;
    aura::ast::StringPool& pool_;
    std::optional<Lexer> lexer_;
};

// Free function — parse directly into FlatAST.
export FlatParseResult parse_to_flat(std::string_view source,
                                      aura::ast::FlatAST& flat,
                                      aura::ast::StringPool& pool);

} // namespace aura::parser
