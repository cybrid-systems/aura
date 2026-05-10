module;
#include <string>
#include <string_view>
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
    explicit Parser(ast::ASTArena& arena) : arena_(arena) {}
    ParseResult parse(std::string_view source);

private:
    ast::Expr* parse_expr();
    ast::Expr* parse_literal_int(Token tok);
    ast::Expr* parse_list();
    ast::Expr* parse_if();
    ast::Expr* parse_lambda();
    ast::Expr* parse_let(bool is_letrec);
    ast::Expr* parse_define();
    ast::Expr* parse_expr_value();
    void skip_to_rparen();

    ast::ASTArena& arena_;
    std::optional<Lexer> lexer_;
};

} // namespace aura::parser
