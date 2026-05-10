module;
#include <cstdlib>
#include <cerrno>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

module aura.parser.parser;

namespace aura::parser {

ParseResult Parser::parse(std::string_view source) {
    lexer_.emplace(source);
    ParseResult result;
    result.arena = &arena_;

    result.root = parse_expr();
    if (result.root) {
        result.success = true;
    } else {
        result.error = "parse error";
    }
    return result;
}

ast::Expr* Parser::parse_expr() {
    if (!lexer_) return nullptr;
    Token tok = lexer_->peek();

    switch (tok.kind) {
    case TokenKind::Integer:
        return parse_literal_int(lexer_->consume());

    case TokenKind::Identifier: {
        auto t = lexer_->consume();
        return arena_.create<ast::Expr>(ast::VariableNode{{}, std::string(t.text)});
    }

    case TokenKind::LParen: {
        lexer_->consume();
        auto first = lexer_->peek();

        if (first.kind == TokenKind::Identifier) {
            std::string_view kw = first.text;
            if (kw == "let") return parse_let();
        }

        // Default: skip until RParen (placeholder for function calls)
        while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof())
            lexer_->consume();
        lexer_->consume();
        return nullptr;
    }

    case TokenKind::EndOfFile:
        return nullptr;

    case TokenKind::Error:
    default:
        return nullptr;
    }
}

ast::Expr* Parser::parse_literal_int(Token tok) {
    errno = 0;
    char* end = nullptr;
    int64_t val = std::strtoll(std::string(tok.text).c_str(), &end, 10);
    if (errno == ERANGE)
        return nullptr;
    return arena_.create<ast::Expr>(ast::LiteralIntNode{{}, val});
}

ast::Expr* Parser::parse_let() {
    lexer_->consume(); // consume 'let'

    // Binding list: '(' ((name value) ...) ')'
    if (lexer_->consume().kind != TokenKind::LParen) return nullptr;

    // Collect binding pairs: (name, value_expr)
    struct Binding { std::string name; ast::Expr* value; };
    std::vector<Binding> bindings;

    while (lexer_->peek().kind != TokenKind::RParen) {
        // Each binding: '(' name value ')'
        if (lexer_->consume().kind != TokenKind::LParen) return nullptr;

        auto name_tok = lexer_->consume();
        if (name_tok.kind != TokenKind::Identifier) return nullptr;

        auto* value = parse_expr_value();
        if (!value) return nullptr;

        bindings.push_back({std::string(name_tok.text), value});

        if (lexer_->consume().kind != TokenKind::RParen) return nullptr;
    }
    lexer_->consume(); // consume closing ')'

    // Parse body expression
    auto* body = parse_expr();
    if (!body) return nullptr;

    // Consume the outer ')'
    if (lexer_->peek().kind == TokenKind::RParen)
        lexer_->consume();

    // Build nested let chain: (let ((x 1) (y 2)) body)
    // → (let ((x 1)) (let ((y 2)) body))
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
        body = arena_.create<ast::Expr>(
            ast::LetNode{{}, it->name, it->value, body});
    }

    return body;
}

// Parse a value expression inside a let binding (literal, variable, or sub-expr)
ast::Expr* Parser::parse_expr_value() {
    Token tok = lexer_->peek();
    switch (tok.kind) {
    case TokenKind::Integer:
        return parse_literal_int(lexer_->consume());
    case TokenKind::Identifier:
        return arena_.create<ast::Expr>(
            ast::VariableNode{{}, std::string(lexer_->consume().text)});
    case TokenKind::LParen: {
        // Sub-expression as value: parse recursively
        // For now skip this (full recursion added in later steps)
        return nullptr;
    }
    default:
        return nullptr;
    }
}

} // namespace aura::parser
