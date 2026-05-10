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
    ParseResult r;
    r.arena = &arena_;
    r.root = parse_expr();
    if (r.root) r.success = true;
    else r.error = "parse error";
    return r;
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
    case TokenKind::LParen:
        lexer_->consume();
        return parse_list();
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

ast::Expr* Parser::parse_list() {
    auto first = lexer_->peek();
    if (first.kind == TokenKind::Identifier) {
        std::string_view kw = first.text;
        if (kw == "if")     return parse_if();
        if (kw == "lambda") return parse_lambda();
        if (kw == "let")    return parse_let(false);
        if (kw == "letrec") return parse_let(true);
        if (kw == "define") return parse_define();
    }
    auto* func = parse_expr();
    if (!func) { skip_to_rparen(); return nullptr; }
    ast::CallNode call;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto* arg = parse_expr();
        if (arg) call.args.push_back(arg);
        else break;
    }
    lexer_->consume();
    call.function = func;
    return arena_.create<ast::Expr>(std::move(call));
}

ast::Expr* Parser::parse_if() {
    lexer_->consume();
    auto* c = parse_expr();
    auto* t = parse_expr();
    auto* e = parse_expr();
    lexer_->consume();
    return arena_.create<ast::Expr>(ast::IfExprNode{{}, c, t, e});
}

ast::Expr* Parser::parse_lambda() {
    lexer_->consume();
    if (lexer_->consume().kind != TokenKind::LParen) return nullptr;
    ast::LambdaNode lambda;
    while (lexer_->peek().kind != TokenKind::RParen) {
        auto t = lexer_->consume();
        if (t.kind != TokenKind::Identifier) return nullptr;
        lambda.params.push_back(std::string(t.text));
    }
    lexer_->consume();
    lambda.body = parse_expr();
    if (!lambda.body) return nullptr;
    lexer_->consume();
    return arena_.create<ast::Expr>(std::move(lambda));
}

ast::Expr* Parser::parse_define() {
    lexer_->consume(); // 'define'
    auto name = lexer_->consume();
    if (name.kind != TokenKind::Identifier) return nullptr;
    auto* val = parse_expr_value();
    if (!val) return nullptr;
    lexer_->consume(); // ')'
    return arena_.create<ast::Expr>(
        ast::DefineNode{{}, std::string(name.text), val});
}

ast::Expr* Parser::parse_let(bool is_letrec) {
    lexer_->consume();
    if (lexer_->consume().kind != TokenKind::LParen) return nullptr;

    struct Binding { std::string name; ast::Expr* value; };
    std::vector<Binding> bindings;

    while (lexer_->peek().kind != TokenKind::RParen) {
        if (lexer_->consume().kind != TokenKind::LParen) return nullptr;
        auto name = lexer_->consume();
        if (name.kind != TokenKind::Identifier) return nullptr;
        auto* val = parse_expr_value();
        if (!val) return nullptr;
        bindings.push_back({std::string(name.text), val});
        if (lexer_->consume().kind != TokenKind::RParen) return nullptr;
    }
    lexer_->consume();

    auto* body = parse_expr();
    if (!body) return nullptr;
    if (lexer_->peek().kind == TokenKind::RParen) lexer_->consume();

    if (is_letrec) {
        for (auto it = bindings.rbegin(); it != bindings.rend(); ++it)
            body = arena_.create<ast::Expr>(
                ast::LetRecNode{{}, it->name, it->value, body});
        return body;
    }
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it)
        body = arena_.create<ast::Expr>(
            ast::LetNode{{}, it->name, it->value, body});
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
    case TokenKind::LParen: {
        lexer_->consume();
        return parse_list();
    }
    default: return nullptr;
    }
}

void Parser::skip_to_rparen() {
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof())
        lexer_->consume();
    lexer_->consume();
}

} // namespace aura::parser
