module;
#include <cstdlib>
#include <cerrno>
#include <string>
#include <string_view>

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
        auto* func = parse_expr();
        if (!func) return nullptr;
        auto* arg = parse_expr();
        if (!arg) return nullptr;
        lexer_->consume(); // skip RParen
        return func;
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

} // namespace aura::parser
