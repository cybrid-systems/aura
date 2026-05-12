module aura.parser.parser;

namespace aura::parser {

using namespace aura::ast;

FlatParseResult FlatParser::parse(std::string_view s) {
    lexer_.emplace(s);
    FlatParseResult r;
    r.root = parse_expr();
    if (r.root == NULL_NODE) { r.error = "parse error"; return r; }
    // Check for multiple top-level expressions
    auto next = lexer_->peek();
    if (next.kind == TokenKind::EndOfFile || next.kind == TokenKind::Error) {
        r.success = true; return r;
    }
    // Multiple forms → wrap in begin
    std::vector<NodeId> exprs;
    exprs.push_back(r.root);
    do {
        auto e = parse_expr();
        if (e != NULL_NODE) exprs.push_back(e);
        if (lexer_->eof()) break;
        next = lexer_->peek();
    } while (next.kind != TokenKind::EndOfFile);
    r.root = flat_.add_begin(exprs.data(), static_cast<std::uint32_t>(exprs.size()));
    r.success = true;
    return r;
}

NodeId FlatParser::parse_expr() {
    if (!lexer_) return NULL_NODE;
    auto tok = lexer_->peek();
    switch (tok.kind) {
    case TokenKind::Integer: return parse_int(lexer_->consume());
    case TokenKind::String:
        return flat_.add_literalstring(pool_.intern(std::string(lexer_->consume().text)));
    case TokenKind::Identifier:
        return flat_.add_variable(pool_.intern(std::string(lexer_->consume().text)));
    case TokenKind::LParen: lexer_->consume(); return parse_list();
    default: return NULL_NODE;
    }
}

NodeId FlatParser::parse_int(Token tok) {
    try {
        auto v = std::stoll(std::string(tok.text));
        return flat_.add_literal(v);
    } catch (...) { return NULL_NODE; }
}

NodeId FlatParser::parse_list() {
    // () → null sentinel (0)
    if (lexer_->peek().kind == TokenKind::RParen) {
        lexer_->consume();
        return flat_.add_literal(0);
    }
    auto f = lexer_->peek();
    if (f.kind == TokenKind::Identifier) {
        auto kw = f.text;
        if (kw == "if")     return parse_if();
        if (kw == "lambda") return parse_lambda();
        if (kw == "let")    return parse_let(false);
        if (kw == "letrec") return parse_let(true);
        if (kw == "define") return parse_define();
        if (kw == "begin")  return parse_begin();
        if (kw == "set!")   return parse_set();
        if (kw == "quote")  return parse_quote();
        if (kw == "cond")   return parse_cond();
        if (kw == "defmacro") return parse_defmacro();
    }

    auto func = parse_expr();
    if (func == NULL_NODE) { skip_rparen(); return NULL_NODE; }

    std::vector<NodeId> args;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto a = parse_expr();
        if (a != NULL_NODE) args.push_back(a);
        else break;
    }
    lexer_->consume(); // ')'
    return flat_.add_call(func, args);
}

NodeId FlatParser::parse_if() {
    lexer_->consume(); // 'if'
    auto c = parse_expr();
    auto t = parse_expr();
    auto e = parse_expr();
    lexer_->consume(); // ')'
    return flat_.add_if(c, t, e);
}

NodeId FlatParser::parse_lambda() {
    lexer_->consume(); // 'lambda'
    if (lexer_->consume().kind != TokenKind::LParen) return NULL_NODE;

    std::vector<SymId> params;
    while (lexer_->peek().kind != TokenKind::RParen) {
        auto t = lexer_->consume();
        if (t.kind != TokenKind::Identifier) return NULL_NODE;
        params.push_back(pool_.intern(std::string(t.text)));
    }
    lexer_->consume(); // ')'

    auto body = parse_expr();
    if (body == NULL_NODE) return NULL_NODE;
    lexer_->consume(); // ')'
    return flat_.add_lambda(params, body);
}

NodeId FlatParser::parse_define() {
    lexer_->consume(); // 'define'
    auto n = lexer_->consume();
    if (n.kind != TokenKind::Identifier) return NULL_NODE;
    auto v = parse_val();
    if (v == NULL_NODE) return NULL_NODE;
    lexer_->consume(); // ')'
    return flat_.add_define(pool_.intern(std::string(n.text)), v);
}

NodeId FlatParser::parse_let(bool rec) {
    lexer_->consume(); // 'let' or 'letrec'
    if (lexer_->consume().kind != TokenKind::LParen) return NULL_NODE; // ((
    
    struct Binding { SymId name; NodeId val; };
    std::vector<Binding> bs;
    
    while (lexer_->peek().kind != TokenKind::RParen) {
        if (lexer_->consume().kind != TokenKind::LParen) return NULL_NODE;
        auto n = lexer_->consume();
        if (n.kind != TokenKind::Identifier) return NULL_NODE;
        auto v = parse_val();
        if (v == NULL_NODE) return NULL_NODE;
        bs.push_back({pool_.intern(std::string(n.text)), v});
        if (lexer_->consume().kind != TokenKind::RParen) return NULL_NODE;
    }
    lexer_->consume(); // ')'
    
    auto body = parse_expr();
    if (body == NULL_NODE) return NULL_NODE;
    if (lexer_->peek().kind == TokenKind::RParen) lexer_->consume();
    
    // Wrap bindings: innermost first (so outer wraps inner)
    for (auto it = bs.rbegin(); it != bs.rend(); ++it) {
        if (rec)
            body = flat_.add_letrec(it->name, it->val, body);
        else
            body = flat_.add_let(it->name, it->val, body);
    }
    return body;
}

NodeId FlatParser::parse_val() {
    auto tok = lexer_->peek();
    switch (tok.kind) {
    case TokenKind::Integer:
        return parse_int(lexer_->consume());
    case TokenKind::String:
        return flat_.add_literalstring(pool_.intern(std::string(lexer_->consume().text)));
    case TokenKind::Identifier:
        return flat_.add_variable(pool_.intern(std::string(lexer_->consume().text)));
    case TokenKind::LParen:
        lexer_->consume(); return parse_list();
    default:
        return NULL_NODE;
    }
}

void FlatParser::skip_rparen() {
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof())
        lexer_->consume();
    lexer_->consume();
}

NodeId FlatParser::parse_begin() {
    lexer_->consume(); // 'begin'
    std::vector<NodeId> exprs;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto e = parse_expr();
        if (e != NULL_NODE) exprs.push_back(e);
        else break;
    }
    lexer_->consume(); // ')'
    return flat_.add_begin(exprs.data(), static_cast<std::uint32_t>(exprs.size()));
}

NodeId FlatParser::parse_set() {
    lexer_->consume(); // 'set!'
    auto n = lexer_->consume();
    if (n.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
    auto v = parse_val();
    if (v == NULL_NODE) { skip_rparen(); return NULL_NODE; }
    lexer_->consume(); // ')'
    return flat_.add_set(pool_.intern(std::string(n.text)), v);
}

NodeId FlatParser::parse_quote() {
    lexer_->consume(); // 'quote'
    auto v = parse_val();
    if (v == NULL_NODE) { skip_rparen(); return NULL_NODE; }
    lexer_->consume(); // ')'
    return flat_.add_quote(v);
}

NodeId FlatParser::parse_cond() {
    lexer_->consume(); // 'cond'
    struct Clause { NodeId test; NodeId val; };
    std::vector<Clause> clauses;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        if (lexer_->peek().kind != TokenKind::LParen) break;
        lexer_->consume(); // '('
        auto cn = parse_expr();
        if (cn == NULL_NODE) { skip_rparen(); break; }
        auto v = parse_expr();
        if (v == NULL_NODE) { skip_rparen(); break; }
        lexer_->consume(); // ')'
        clauses.push_back({cn, v});
    }
    lexer_->consume(); // ')'
    if (clauses.empty()) return NULL_NODE;
    auto result = clauses.back().val;
    for (auto it = clauses.rbegin() + 1; it != clauses.rend(); ++it)
        result = flat_.add_if(it->test, it->val, result);
    return result;
}

NodeId FlatParser::parse_defmacro() {
    lexer_->consume(); // 'defmacro'
    if (lexer_->consume().kind != TokenKind::LParen) { skip_rparen(); return NULL_NODE; }
    auto name = lexer_->consume();
    if (name.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
    std::vector<SymId> params;
    while (lexer_->peek().kind != TokenKind::RParen) {
        auto p = lexer_->consume();
        if (p.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
        params.push_back(pool_.intern(std::string(p.text)));
    }
    lexer_->consume(); // ')'
    auto body = parse_expr();
    if (body == NULL_NODE) { skip_rparen(); return NULL_NODE; }
    lexer_->consume(); // ')'
    return flat_.add_macrodef(pool_.intern(std::string(name.text)), params, body);
}

// ── Free function ──────────────────────────────────────────────
FlatParseResult parse_to_flat(std::string_view source,
                               FlatAST& flat, StringPool& pool) {
    FlatParser fp(flat, pool);
    return fp.parse(source);
}

} // namespace aura::parser
