module;
#include <string>
#include <optional>

export module aura.parser.parser;

import aura.core;
import aura.parser.lexer;

namespace aura::parser {

export struct ParseResult {
    ast::Expr* root = nullptr;
    bool success = false;
    std::string error;
    ast::ASTArena* arena = nullptr;
};

export class Parser {
public:
    explicit Parser(ast::ASTArena& arena)
        : arena_(arena)
    {}

    ParseResult parse(std::string_view source);

private:
    ast::Expr* parse_expr();
    ast::Expr* parse_literal_int(Token tok);
    ast::Expr* parse_let();
    ast::Expr* parse_expr_value();

    ast::ASTArena& arena_;
    std::optional<Lexer> lexer_;
};

} // namespace aura::parser
