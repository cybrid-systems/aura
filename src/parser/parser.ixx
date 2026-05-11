export module aura.parser.parser;
import std;
import aura.core;
import aura.parser.lexer;

namespace aura::parser {

export struct ParseResult { ast::Expr* root = nullptr; bool success = false; std::string error; ast::ASTArena* arena = nullptr; };

// Internal parser class (implementation detail).
// For most use cases, call the free function parse() instead.
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

// Free function — preferred API.
// Creates a temporary parser, parses the source, returns the result.
export inline ParseResult parse(std::string_view source, ast::ASTArena& arena) {
    Parser parser(arena);
    return parser.parse(source);
}

} // namespace aura::parser
