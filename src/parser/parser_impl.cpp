module aura.parser.parser;

import <cstdlib>;
import <cerrno>;
import <string>;
import <string_view>;
import <vector>;
import <utility>;

namespace aura::parser {

ParseResult Parser::parse(std::string_view source) {
    lexer_.emplace(source);
    ParseResult result;
    result.arena = &arena_;
    result.root = parse_expr();
    if (result.root) result.success = true;
    else result.error = "parse error";
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
        if (first.kind == TokenKind::Identifier && first.text == "let")
            return parse_let();
        // Default: skip until RParen
        while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof())
            lexer_->consume();
        lexer_->consume();
        return nullptr;
    }

    case TokenKind::EndOfFile: return nullptr;
    default: return nullptr;
    }
}

ast::Expr* Parser::parse_literal_int(Token tok) {
    errno = 0;
    char* end = nullptr;
    int64_t val = std::strtoll(std::string(tok.text).c_str(), &end, 10);
    if (errno == ERANGE) return nullptr;
    return arena_.create<ast::Expr>(ast::LiteralIntNode{{}, val});
}

ast::Expr* Parser::parse_let() {
    lexer_->consume(); // 'let'
    if (lexer_->consume().kind != TokenKind::LParen) return nullptr;

    struct Binding { std::string name; ast::Expr* value; };
    std::vector<Binding> bindings;

    while (lexer_->peek().kind != TokenKind::RParen) {
        if (lexer_->consume().kind != TokenKind::LParen) return nullptr;
        auto name_tok = lexer_->consume();
        if (name_tok.kind != TokenKind::Identifier) return nullptr;
        auto* value = parse_expr_value();
        if (!value) return nullptr;
        bindings.push_back({std::string(name_tok.text), value});
        if (lexer_->consume().kind != TokenKind::RParen) return nullptr;
    }
    lexer_->consume(); // ')'

    auto* body = parse_expr();
    if (!body) return nullptr;
    if (lexer_->peek().kind == TokenKind::RParen) lexer_->consume();

    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it)
        body = arena_.create<ast::Expr>(ast::LetNode{{}, it->name, it->value, body});

    return body;
}

ast::Expr* Parser::parse_expr_value() {
    Token tok = lexer_->peek();
    switch (tok.kind) {
    case TokenKind::Integer:
        return parse_literal_int(lexer_->consume());
    case TokenKind::Identifier:
        return arena_.create<ast::Expr>(
            ast::VariableNode{{}, std::string(lexer_->consume().text)});
    default:
        return nullptr;
    }
}

} // namespace aura::parser
