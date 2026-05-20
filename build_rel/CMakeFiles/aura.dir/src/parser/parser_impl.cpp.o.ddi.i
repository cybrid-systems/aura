# 0 "/home/dev/code/aura/src/parser/parser_impl.cpp"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/parser/parser_impl.cpp"
module aura.parser.parser;

namespace aura::parser {

using namespace aura::ast;

FlatParseResult FlatParser::parse(std::string_view s) {
    lexer_.emplace(s);
    FlatParseResult r;


    auto record_error = [&](const std::string& msg) {
        if (r.error.empty()) r.error = msg;
        r.errors.push_back(msg);

        int depth = 0;
        while (!lexer_->eof()) {
            auto tok = lexer_->peek();
            if (depth == 0) {

                if (tok.kind == TokenKind::LParen ||
                    tok.kind == TokenKind::Integer ||
                    tok.kind == TokenKind::Float ||
                    tok.kind == TokenKind::String ||
                    tok.kind == TokenKind::Identifier ||
                    tok.kind == TokenKind::Bool ||
                    tok.kind == TokenKind::Quote ||
                    tok.kind == TokenKind::QuasiQuote ||
                    tok.kind == TokenKind::Unquote) {
                    break;
                }
                lexer_->consume();
            } else {

                if (tok.kind == TokenKind::RParen) {
                    depth--;
                } else if (tok.kind == TokenKind::LParen) {
                    depth++;
                }
                lexer_->consume();
            }
        }
    };

    r.root = parse_expr();
    if (r.root == NULL_NODE) {
        auto tok = lexer_->peek();
        if (tok.kind != TokenKind::EndOfFile) {
            record_error("parse error at line " + std::to_string(tok.line)
                        + ":" + std::to_string(tok.column));
        } else {
            record_error("parse error");
        }

        if (r.root == NULL_NODE) return r;
    }


    auto next = lexer_->peek();
    if (next.kind == TokenKind::EndOfFile || next.kind == TokenKind::Error) {
        r.success = r.root != NULL_NODE;
        return r;
    }


    std::vector<NodeId> exprs;
    exprs.push_back(r.root);
    do {
        auto e = parse_expr();
        if (e == NULL_NODE) {
            auto tok = lexer_->peek();
            if (tok.kind != TokenKind::EndOfFile) {
                record_error("parse error at line " + std::to_string(tok.line)
                            + ":" + std::to_string(tok.column));
                e = parse_expr();
            }
            if (lexer_->eof()) break;
        }
        if (e != NULL_NODE) exprs.push_back(e);
        if (lexer_->eof()) break;
        next = lexer_->peek();
    } while (next.kind != TokenKind::EndOfFile);

    r.root = flat_.add_begin(exprs);
    r.success = !exprs.empty();
    return r;
}

NodeId FlatParser::parse_expr() {
    if (!lexer_) return NULL_NODE;
    auto tok = lexer_->peek();
    switch (tok.kind) {
    case TokenKind::Integer: return parse_number(lexer_->consume());
    case TokenKind::Bool: {
        auto tok = lexer_->consume();
        auto v = std::stoll(std::string(tok.text));
        auto id = flat_.add_literal(v);
        flat_.set_marker(id, aura::ast::SyntaxMarker::BoolLiteral);
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    case TokenKind::Float: return parse_float(lexer_->consume());
    case TokenKind::String: {
        auto tok = lexer_->consume();
        auto s = std::string(tok.text);

        std::string unescaped;
        bool has_esc = false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                auto n = s[i + 1];
                if (n == '"') { unescaped += '"'; i++; has_esc = true; }
                else if (n == '\\') { unescaped += '\\'; i++; has_esc = true; }
                else { unescaped += s[i]; }
            } else {
                unescaped += s[i];
            }
        }
        auto id = flat_.add_literalstring(pool_.intern(has_esc ? unescaped : s));
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    case TokenKind::Ellipsis: {
        lexer_->consume();
        return flat_.add_variable(pool_.intern("..."));
    }
    case TokenKind::Identifier: {
        auto tok = lexer_->consume();
        auto id = flat_.add_variable(pool_.intern(std::string(tok.text)));
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    case TokenKind::Quote: {
        lexer_->consume();
        auto quoted = parse_expr();
        if (quoted == NULL_NODE) return NULL_NODE;
        auto id = flat_.add_quote(quoted);
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    case TokenKind::QuasiQuote: {
        lexer_->consume();
        auto quoted = parse_expr();
        if (quoted == NULL_NODE) return NULL_NODE;
        auto id = expand_qq(quoted, 0);
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    case TokenKind::Unquote: {
        lexer_->consume();
        auto inner = parse_expr();
        if (inner == NULL_NODE) return NULL_NODE;

        auto unquote_var = flat_.add_variable(pool_.intern("unquote"));
        auto id = flat_.add_call(unquote_var, std::vector<aura::ast::NodeId>{inner});
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    case TokenKind::UnquoteSplicing: {
        lexer_->consume();
        auto inner = parse_expr();
        if (inner == NULL_NODE) return NULL_NODE;

        auto unsplice_var = flat_.add_variable(pool_.intern("unquote-splicing"));
        auto id = flat_.add_call(unsplice_var, std::vector<aura::ast::NodeId>{inner});
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    case TokenKind::LParen: lexer_->consume(); return parse_list();
    default: return NULL_NODE;
    }
}

NodeId FlatParser::parse_number(Token tok) {
    try {
        auto v = std::stoll(std::string(tok.text));
        return flat_.add_literal(v);
    } catch (...) { return NULL_NODE; }
}

NodeId FlatParser::parse_float(Token tok) {
    try {
        auto v = std::stod(std::string(tok.text));
        return flat_.add_literal_float(v);
    } catch (...) { return NULL_NODE; }
}

NodeId FlatParser::parse_list() {
    auto tok = lexer_->peek();

    if (lexer_->peek().kind == TokenKind::RParen) {
        lexer_->consume();
        auto id = flat_.add_literal(0);
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    auto f = lexer_->peek();
    if (f.kind == TokenKind::Identifier) {
        auto kw = f.text;
        if (kw == "if") return parse_if();
        if (kw == "lambda") return parse_lambda();
        if (kw == "let") return parse_let(false);
        if (kw == "let*") return parse_let_star();
        if (kw == "letrec") return parse_let(true);
        if (kw == "define") return parse_define();
        if (kw == "begin") return parse_begin();
        if (kw == "set!") return parse_set();
        if (kw == "quote") return parse_quote();
        if (kw == "cond") return parse_cond();
        if (kw == "defmacro") return parse_defmacro();
        if (kw == "match") return parse_match();
        if (kw == "cast") return parse_cast();
        if (kw == "check") return parse_check();
        if (kw == ":") return parse_type_annot();
        if (kw == "export") {
            lexer_->consume();
            std::vector<aura::ast::NodeId> syms;
            while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
                auto sym = parse_expr();
                if (sym != aura::ast::NULL_NODE)
                    syms.push_back(sym);
                else break;
            }
            if (lexer_->peek().kind == TokenKind::RParen) lexer_->consume();
            auto id = flat_.add_export(syms);
            flat_.set_loc(id, tok.line, tok.column);
            return id;
        }
    }

    auto func = parse_expr();
    if (func == NULL_NODE) { skip_rparen(); return NULL_NODE; }

    std::vector<NodeId> args;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {

        if (lexer_->peek().kind == TokenKind::Dot) {
            lexer_->consume();
            auto cdr = parse_expr();
            if (cdr == NULL_NODE) { skip_rparen(); return NULL_NODE; }
            if (lexer_->peek().kind != TokenKind::RParen) { skip_rparen(); return NULL_NODE; }
            lexer_->consume();


            NodeId tail = cdr;
            for (auto it = args.rbegin(); it != args.rend(); ++it)
                tail = flat_.add_pair(*it, tail);
            tail = flat_.add_pair(func, tail);
            flat_.set_loc(tail, tok.line, tok.column);
            return tail;
        }
        auto a = parse_expr();
        if (a != NULL_NODE) args.push_back(a);
        else break;
    }
    lexer_->consume();
    auto id = flat_.add_call(func, args);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_if() {
    auto tok = lexer_->consume();
    auto c = parse_expr();
    auto t = parse_expr();
    auto e = parse_expr();
    lexer_->consume();
    auto id = flat_.add_if(c, t, e);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_lambda() {
    auto tok = lexer_->consume();
    if (lexer_->consume().kind != TokenKind::LParen) return NULL_NODE;

    std::vector<SymId> params;
    bool dotted = false;
    while (lexer_->peek().kind != TokenKind::RParen) {

        if (lexer_->peek().kind == TokenKind::Dot) {
            lexer_->consume();
            if (lexer_->peek().kind == TokenKind::RParen) { dotted = true; break; }
            auto rest = lexer_->consume();
            if (rest.kind != TokenKind::Identifier) return NULL_NODE;
            params.push_back(pool_.intern(std::string(rest.text)));
            dotted = true;
            break;
        }
        auto t = lexer_->consume();
        if (t.kind != TokenKind::Identifier) return NULL_NODE;
        params.push_back(pool_.intern(std::string(t.text)));
    }
    lexer_->consume();


    std::vector<NodeId> body_exprs;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto be = parse_expr();
        if (be != NULL_NODE) body_exprs.push_back(be);
        if (lexer_->peek().kind == TokenKind::RParen) break;
    }
    auto body = body_exprs.empty() ? NULL_NODE
        : (body_exprs.size() == 1 ? body_exprs[0] : flat_.add_begin(body_exprs.data(), body_exprs.size()));
    if (body == NULL_NODE) return NULL_NODE;
    lexer_->consume();
    auto lid = flat_.add_lambda(params, body, dotted); flat_.set_loc(lid, tok.line, tok.column); return lid;
}

NodeId FlatParser::parse_define() {
    lexer_->consume();
    auto n = lexer_->peek();
    if (n.kind == TokenKind::LParen) {

        lexer_->consume();
        auto fn = lexer_->consume();
        if (fn.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
        std::vector<SymId> params;
        bool dotted = false;
        while (lexer_->peek().kind != TokenKind::RParen) {

            if (lexer_->peek().kind == TokenKind::Dot) {
                lexer_->consume();
                if (lexer_->peek().kind == TokenKind::RParen) { dotted = true; break; }
                auto rest = lexer_->consume();
                if (rest.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
                params.push_back(pool_.intern(std::string(rest.text)));
                dotted = true;
                break;
            }
            auto p = lexer_->consume();
            if (p.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
            params.push_back(pool_.intern(std::string(p.text)));
        }
        lexer_->consume();

        std::vector<NodeId> body_exprs;
        while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
            auto be = parse_expr();
            if (be == NULL_NODE) break;
            body_exprs.push_back(be);
        }
        lexer_->consume();
        if (body_exprs.empty()) return NULL_NODE;
        NodeId body = (body_exprs.size() == 1) ? body_exprs[0]
                    : flat_.add_begin(body_exprs);
        auto lambda = flat_.add_lambda(params, body, dotted);
        return flat_.add_define(pool_.intern(std::string(fn.text)), lambda);
    }

    if (n.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
    lexer_->consume();
    auto v = parse_val();
    if (v == NULL_NODE) return NULL_NODE;
    lexer_->consume();
    return flat_.add_define(pool_.intern(std::string(n.text)), v);
}

NodeId FlatParser::parse_let(bool rec) {
    auto tok = lexer_->consume();


    if (!rec && lexer_->peek().kind == TokenKind::Identifier) {
        return parse_named_let();
    }

    if (lexer_->consume().kind != TokenKind::LParen) return NULL_NODE;

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
    lexer_->consume();

    auto body = parse_expr();
    if (body == NULL_NODE) return NULL_NODE;

    std::vector<NodeId> body_exprs = {body};
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto be = parse_expr();
        if (be == NULL_NODE) break;
        body_exprs.push_back(be);
    }
    if (lexer_->peek().kind == TokenKind::RParen) lexer_->consume();
    if (body_exprs.size() > 1)
        body = flat_.add_begin(body_exprs);



    if (rec && bs.size() > 1) {
        std::vector<NodeId> exprs;
        for (auto& b : bs)
            exprs.push_back(flat_.add_define(b.name, flat_.add_literal(0)));
        for (auto& b : bs)
            exprs.push_back(flat_.add_set(b.name, b.val));
        exprs.push_back(body);
        body = flat_.add_begin(exprs);
    } else {

        for (auto it = bs.rbegin(); it != bs.rend(); ++it) {
            if (rec)
                body = flat_.add_letrec(it->name, it->val, body);
            else
                body = flat_.add_let(it->name, it->val, body);
        }
    }
    return body;
}

NodeId FlatParser::parse_named_let() {
    auto name_tok = lexer_->peek();
    if (name_tok.kind != TokenKind::Identifier) return NULL_NODE;
    lexer_->consume();
    auto name = pool_.intern(std::string(name_tok.text));


    if (lexer_->consume().kind != TokenKind::LParen) return NULL_NODE;

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
    lexer_->consume();


    std::vector<NodeId> body_exprs;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto be = parse_expr();
        if (be == NULL_NODE) break;
        body_exprs.push_back(be);
    }
    lexer_->consume();
    if (body_exprs.empty()) return NULL_NODE;
    NodeId body = (body_exprs.size() == 1) ? body_exprs[0]
                : flat_.add_begin(body_exprs);





    std::vector<SymId> params;
    std::vector<NodeId> init_vals;
    for (auto& b : bs) {
        params.push_back(b.name);
        init_vals.push_back(b.val);
    }


    auto lambda_id = flat_.add_lambda(params, body);


    auto var_id = flat_.add_variable(name);
    auto call_id = flat_.add_call(var_id, init_vals);


    return flat_.add_letrec(name, lambda_id, call_id);
}

NodeId FlatParser::parse_let_star() {
    lexer_->consume();
    if (lexer_->consume().kind != TokenKind::LParen) return NULL_NODE;

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
    lexer_->consume();


    std::vector<NodeId> body_exprs;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto be = parse_expr();
        if (be == NULL_NODE) break;
        body_exprs.push_back(be);
    }
    lexer_->consume();
    if (body_exprs.empty()) return NULL_NODE;
    NodeId body = (body_exprs.size() == 1) ? body_exprs[0]
                : flat_.add_begin(body_exprs);




    for (auto it = bs.rbegin(); it != bs.rend(); ++it) {
        body = flat_.add_let(it->name, it->val, body);
    }
    return body;
}

NodeId FlatParser::parse_val() {
    auto tok = lexer_->peek();
    switch (tok.kind) {
    case TokenKind::Integer:
        return parse_number(lexer_->consume());
    case TokenKind::Bool: {
        auto tok = lexer_->consume();
        auto v = std::stoll(std::string(tok.text));
        auto id = flat_.add_literal(v);
        flat_.set_marker(id, aura::ast::SyntaxMarker::BoolLiteral);
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    case TokenKind::Float:
        return parse_float(lexer_->consume());
    case TokenKind::String: {
        auto tok2 = lexer_->consume();
        auto s2 = std::string(tok2.text);
        std::string unesc2;
        bool has_e2 = false;
        for (std::size_t i = 0; i < s2.size(); ++i) {
            if (s2[i] == '\\' && i + 1 < s2.size()) {
                auto n = s2[i + 1];
                if (n == '"') { unesc2 += '"'; i++; has_e2 = true; }
                else if (n == '\\') { unesc2 += '\\'; i++; has_e2 = true; }
                else { unesc2 += s2[i]; }
            } else {
                unesc2 += s2[i];
            }
        }
        return flat_.add_literalstring(pool_.intern(has_e2 ? unesc2 : s2));
    }
    case TokenKind::Identifier:
        return flat_.add_variable(pool_.intern(std::string(lexer_->consume().text)));
    case TokenKind::LParen:
        lexer_->consume(); return parse_list();
    case TokenKind::Quote: {
        lexer_->consume();
        auto quoted = parse_val();
        if (quoted == NULL_NODE) return NULL_NODE;
        return flat_.add_quote(quoted);
    }
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
    auto tok = lexer_->consume();
    std::vector<NodeId> exprs;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto e = parse_expr();
        if (e != NULL_NODE) exprs.push_back(e);
        else break;
    }
    lexer_->consume();
    auto bid = flat_.add_begin(exprs);
    flat_.set_loc(bid, tok.line, tok.column);
    return bid;
}

NodeId FlatParser::parse_set() {
    auto tok = lexer_->consume();
    auto n = lexer_->consume();
    if (n.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
    auto v = parse_val();
    if (v == NULL_NODE) { skip_rparen(); return NULL_NODE; }
    lexer_->consume();
    auto sid = flat_.add_set(pool_.intern(std::string(n.text)), v);
    flat_.set_loc(sid, tok.line, tok.column);
    return sid;
}

NodeId FlatParser::parse_quote() {
    auto tok = lexer_->consume();
    auto v = parse_val();
    if (v == NULL_NODE) { skip_rparen(); return NULL_NODE; }
    lexer_->consume();
    auto qid = flat_.add_quote(v);
    flat_.set_loc(qid, tok.line, tok.column);
    return qid;
}

NodeId FlatParser::parse_cond() {
    lexer_->consume();
    struct Clause { NodeId test; NodeId val; };
    std::vector<Clause> clauses;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        if (lexer_->peek().kind != TokenKind::LParen) break;
        lexer_->consume();
        auto cn = parse_expr();
        if (cn == NULL_NODE) { skip_rparen(); break; }
        auto v = parse_expr();
        if (v == NULL_NODE) { skip_rparen(); break; }
        lexer_->consume();
        clauses.push_back({cn, v});
    }
    lexer_->consume();
    if (clauses.empty()) return NULL_NODE;
    auto result = clauses.back().val;
    for (auto it = clauses.rbegin() + 1; it != clauses.rend(); ++it)
        result = flat_.add_if(it->test, it->val, result);
    return result;
}

NodeId FlatParser::parse_defmacro() {
    auto tok = lexer_->consume();
    if (lexer_->consume().kind != TokenKind::LParen) { skip_rparen(); return NULL_NODE; }
    auto name = lexer_->consume();
    if (name.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
    std::vector<SymId> params;
    bool dotted = false;
    while (lexer_->peek().kind != TokenKind::RParen) {

        if (lexer_->peek().kind == TokenKind::Dot) {
            lexer_->consume();
            if (lexer_->peek().kind != TokenKind::Identifier) break;
            auto rest = lexer_->consume();
            params.push_back(pool_.intern(std::string(rest.text)));
            dotted = true;
            break;
        }
        auto p = lexer_->consume();
        if (p.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
        params.push_back(pool_.intern(std::string(p.text)));
    }
    lexer_->consume();
    auto body = parse_expr();
    if (body == NULL_NODE) { skip_rparen(); return NULL_NODE; }
    lexer_->consume();
    auto mid = flat_.add_macrodef(pool_.intern(std::string(name.text)), params, body, dotted);
    flat_.set_loc(mid, tok.line, tok.column);
    return mid;
}


NodeId FlatParser::parse_match() {
    auto tok = lexer_->consume();


    auto subject = parse_expr();
    if (subject == NULL_NODE) { skip_rparen(); return NULL_NODE; }


    auto tmp = pool_.intern("__match_tmp");


    struct Clause { NodeId test; NodeId body; };
    std::vector<Clause> clauses;

    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        if (lexer_->peek().kind != TokenKind::LParen) break;
        lexer_->consume();


        auto pattern = parse_val();
        if (pattern == NULL_NODE) break;


        auto body = parse_expr();
        if (body == NULL_NODE) break;

        if (lexer_->peek().kind != TokenKind::RParen) break;
        lexer_->consume();


        NodeId test;
        auto bindings = compile_pattern(pattern, tmp, &test);


        for (auto& [name, val] : bindings)
            body = flat_.add_let(name, val, body);

        clauses.push_back({test, body});
    }

    if (lexer_->peek().kind == TokenKind::RParen)
        lexer_->consume();

    if (clauses.empty()) return NULL_NODE;


    NodeId result = clauses.back().body;
    for (auto it = clauses.rbegin() + 1; it != clauses.rend(); ++it)
        result = flat_.add_if(it->test, it->body, result);


    result = flat_.add_let(tmp, subject, result);
    flat_.set_loc(result, tok.line, tok.column);
    return result;
}

NodeId FlatParser::parse_cast() {


    auto tok = lexer_->consume();
    auto expr = parse_expr();
    if (expr == NULL_NODE) { skip_rparen(); return NULL_NODE; }


    if (lexer_->peek().kind == TokenKind::Identifier && lexer_->peek().text == ":") {
        lexer_->consume();
    }

    auto type_tok = lexer_->peek();
    auto type_name = type_tok.text;
    std::uint32_t type_tag = 3;
    if (type_name == "Int") type_tag = 0;
    else if (type_name == "String") type_tag = 1;
    else if (type_name == "Bool") type_tag = 2;
    else if (type_name == "Any") type_tag = 3;
    lexer_->consume();

    if (lexer_->peek().kind == TokenKind::RParen) lexer_->consume();

    auto id = flat_.add_coercion(expr, type_tag, 0);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_check() {


    auto tok = lexer_->consume();
    auto expr = parse_expr();
    if (expr == NULL_NODE) { skip_rparen(); return NULL_NODE; }


    if (lexer_->peek().kind == TokenKind::Identifier && lexer_->peek().text == ":") {
        lexer_->consume();
    }

    auto type_tok = lexer_->peek();
    auto type_sym = pool_.intern(type_tok.text);
    lexer_->consume();

    if (lexer_->peek().kind == TokenKind::RParen) lexer_->consume();

    auto id = flat_.add_type_annotation(type_sym, expr);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_type_annot() {

    auto tok = lexer_->consume();

    auto name_tok = lexer_->peek();
    if (name_tok.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
    auto var_sym = pool_.intern(name_tok.text);
    lexer_->consume();

    auto type_tok = lexer_->peek();
    if (type_tok.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
    auto type_sym = pool_.intern(type_tok.text);
    lexer_->consume();

    if (lexer_->peek().kind == TokenKind::RParen) lexer_->consume();

    auto var_node = flat_.add_variable(var_sym);
    auto id = flat_.add_type_annotation(type_sym, var_node);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

std::vector<std::pair<SymId, NodeId>> FlatParser::compile_pattern(NodeId pattern_node, SymId tmp, NodeId* out_test) {
    auto v = flat_.get(pattern_node);
    std::vector<std::pair<SymId, NodeId>> bindings;
    auto var_tmp = flat_.add_variable(tmp);
    auto sym_null_q = pool_.intern("null?");
    auto sym_pair_q = pool_.intern("pair?");
    auto sym_car = pool_.intern("car");
    auto sym_cdr = pool_.intern("cdr");
    auto sym_equal_q = pool_.intern("equal?");


    auto make_call = [&](SymId func, std::initializer_list<NodeId> args) -> NodeId {
        return flat_.add_call(flat_.add_variable(func), std::vector<NodeId>(args));
    };


    if (v.tag == NodeTag::Variable) {
        auto name = pool_.resolve(v.sym_id);
        if (name == "_" || (name.size() > 1 && name[0] == '_' && name != "__match_tmp")) {

            *out_test = flat_.add_literal(1);
            return bindings;
        }

        *out_test = flat_.add_literal(1);
        bindings.emplace_back(v.sym_id, var_tmp);
        return bindings;
    }


    if (v.tag == NodeTag::LiteralInt && v.int_value == 0) {
        *out_test = make_call(sym_null_q, {var_tmp});
        return bindings;
    }


    if (v.tag == NodeTag::LiteralInt || v.tag == NodeTag::LiteralFloat || v.tag == NodeTag::LiteralString) {
        *out_test = make_call(sym_equal_q, {var_tmp, pattern_node});
        return bindings;
    }


    if (v.tag == NodeTag::Quote) {
        *out_test = make_call(sym_equal_q, {var_tmp, pattern_node});
        return bindings;
    }


    if (v.tag == NodeTag::Call) {
        if (v.children.empty()) { *out_test = flat_.add_literal(0); return bindings; }

        auto callee_v = flat_.get(v.child(0));
        if (callee_v.tag == NodeTag::Variable) {
            auto callee_name = pool_.resolve(callee_v.sym_id);


            if (callee_name == "quote" && v.children.size() > 1) {
                auto quoted = v.child(1);

                auto quoted_expr = flat_.add_quote(quoted);
                *out_test = make_call(sym_equal_q, {var_tmp, quoted_expr});
                return bindings;
            }


            if (callee_name == "list") {

                NodeId accumulated_test = flat_.add_literal(1);
                NodeId current = var_tmp;

                for (std::size_t i = 1; i < v.children.size(); ++i) {

                    auto pair_test = make_call(sym_pair_q, {current});
                    accumulated_test = flat_.add_if(accumulated_test, pair_test, flat_.add_literal(0));

                    auto elem = v.child(i);
                    auto elem_v = flat_.get(elem);


                    auto car_expr = make_call(sym_car, {current});

                    if (elem_v.tag == NodeTag::Variable) {
                        auto elem_name = pool_.resolve(elem_v.sym_id);
                        if (elem_name != "_" && !(elem_name.size() > 1 && elem_name[0] == '_')) {

                            bindings.emplace_back(elem_v.sym_id, car_expr);
                        }
                    } else if (elem_v.tag == NodeTag::LiteralInt && elem_v.int_value == 0) {

                        auto eq_test = make_call(sym_equal_q, {car_expr, elem});
                        accumulated_test = flat_.add_if(accumulated_test, eq_test, flat_.add_literal(0));
                    } else if (elem_v.tag == NodeTag::LiteralInt || elem_v.tag == NodeTag::LiteralFloat || elem_v.tag == NodeTag::LiteralString) {

                        auto eq_test = make_call(sym_equal_q, {car_expr, elem});
                        accumulated_test = flat_.add_if(accumulated_test, eq_test, flat_.add_literal(0));
                    }




                    current = make_call(sym_cdr, {current});
                }


                auto null_test = make_call(sym_null_q, {current});
                accumulated_test = flat_.add_if(accumulated_test, null_test, flat_.add_literal(0));

                *out_test = accumulated_test;
                return bindings;
            }


            if (callee_name == "cons" && v.children.size() >= 3) {

                *out_test = make_call(sym_pair_q, {var_tmp});

                auto car_pat = v.child(1);
                auto cdr_pat = v.child(2);
                auto car_v = flat_.get(car_pat);
                auto cdr_v = flat_.get(cdr_pat);

                auto car_expr = make_call(sym_car, {var_tmp});
                auto cdr_expr = make_call(sym_cdr, {var_tmp});


                if (car_v.tag == NodeTag::Variable) {
                    auto ename = pool_.resolve(car_v.sym_id);
                    if (ename != "_" && !(ename.size() > 1 && ename[0] == '_'))
                        bindings.emplace_back(car_v.sym_id, car_expr);
                }


                if (cdr_v.tag == NodeTag::Variable) {
                    auto ename = pool_.resolve(cdr_v.sym_id);
                    if (ename != "_" && !(ename.size() > 1 && ename[0] == '_'))
                        bindings.emplace_back(cdr_v.sym_id, cdr_expr);
                }

                return bindings;
            }
        }
    }


    *out_test = make_call(sym_equal_q, {var_tmp, pattern_node});
    return bindings;
}




static bool is_unquote(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool, NodeId id) {
    if (id == NULL_NODE) return false;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Call || v.children.empty()) return false;
    auto callee = flat.get(v.child(0));
    return callee.tag == NodeTag::Variable && std::string(pool.resolve(callee.sym_id)) == "unquote";
}


static bool is_unquote_splicing(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool, NodeId id) {
    if (id == NULL_NODE) return false;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Call || v.children.empty()) return false;
    auto callee = flat.get(v.child(0));
    return callee.tag == NodeTag::Variable && std::string(pool.resolve(callee.sym_id)) == "unquote-splicing";
}


static bool is_quasiquote(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool, NodeId id) {
    if (id == NULL_NODE) return false;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Call || v.children.empty()) return false;
    auto callee = flat.get(v.child(0));
    return callee.tag == NodeTag::Variable && std::string(pool.resolve(callee.sym_id)) == "quasiquote";
}

NodeId FlatParser::expand_qq(NodeId expr, int depth) {
    if (expr == NULL_NODE) {

        return flat_.add_quote(flat_.add_literal(0));
    }

    auto v = flat_.get(expr);



    if (v.tag != NodeTag::Call) {

        if (v.tag == NodeTag::Variable || v.tag == NodeTag::LiteralInt
            || v.tag == NodeTag::LiteralFloat || v.tag == NodeTag::LiteralString
            || v.tag == NodeTag::Quote) {
            return flat_.add_quote(expr);
        }


        if (v.children.size() > 0) {

            std::string form_name;
            switch (v.tag) {
            case NodeTag::IfExpr: form_name = "if"; break;
            case NodeTag::Lambda: form_name = "lambda"; break;
            case NodeTag::Let: form_name = "let"; break;
            case NodeTag::LetRec: form_name = "letrec"; break;
            case NodeTag::Define: form_name = "define"; break;
            case NodeTag::Begin: form_name = "begin"; break;
            case NodeTag::Set: form_name = "set!"; break;
            default: return flat_.add_quote(expr);
            }


            std::vector<NodeId> args_to_expand;


            if (v.tag == NodeTag::Lambda) {
                NodeId params_list = flat_.add_quote(flat_.add_literal(0));
                for (int pi = static_cast<int>(v.params.size()) - 1; pi >= 0; --pi) {
                    auto param_var = flat_.add_variable(pool_.intern(std::string(pool_.resolve(v.params[pi]))));
                    auto param_quoted = flat_.add_quote(param_var);
                    auto cv = flat_.add_variable(pool_.intern("cons"));
                    params_list = flat_.add_call(cv, std::vector<aura::ast::NodeId>{param_quoted, params_list});
                }
                args_to_expand.push_back(params_list);
            }

            bool has_fn_list = false;

            if (v.tag == NodeTag::Define && v.children.size() == 1 && v.sym_id != ast::INVALID_SYM) {
                auto child_v = flat_.get(v.child(0));
                if (child_v.tag == NodeTag::Lambda) {

                    auto fn_var = flat_.add_variable(pool_.intern(std::string(pool_.resolve(v.sym_id))));
                    auto fn_quoted = flat_.add_quote(fn_var);
                    NodeId fn_params_list = flat_.add_quote(flat_.add_literal(0));
                    for (int pi = static_cast<int>(child_v.params.size()) - 1; pi >= 0; --pi) {
                        auto pvar = flat_.add_variable(pool_.intern(std::string(pool_.resolve(child_v.params[pi]))));
                        auto pquoted = flat_.add_quote(pvar);
                        auto cv = flat_.add_variable(pool_.intern("cons"));
                        fn_params_list = flat_.add_call(cv, std::vector<aura::ast::NodeId>{pquoted, fn_params_list});
                    }
                    auto cv = flat_.add_variable(pool_.intern("cons"));
                    args_to_expand.push_back(flat_.add_call(cv, std::vector<aura::ast::NodeId>{fn_quoted, fn_params_list}));
                    has_fn_list = true;

                    for (std::size_t bci = 0; bci < child_v.children.size(); ++bci) {
                        auto expanded = expand_qq(child_v.child(bci), depth);
                        args_to_expand.push_back(expanded);
                    }
                }
            }


            if (!has_fn_list) {
                for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                    auto expanded = expand_qq(v.child(ci), depth);
                    args_to_expand.push_back(expanded);
                }
            }


            NodeId result = flat_.add_quote(flat_.add_literal(0));
            for (int i = static_cast<int>(args_to_expand.size()) - 1; i >= 0; --i) {
                auto cons_var = flat_.add_variable(pool_.intern("cons"));
                result = flat_.add_call(cons_var, std::vector<aura::ast::NodeId>{args_to_expand[i], result});
            }


            auto form_var = flat_.add_variable(pool_.intern(form_name));
            auto form_quote = flat_.add_quote(form_var);
            auto cons_var2 = flat_.add_variable(pool_.intern("cons"));
            result = flat_.add_call(cons_var2, std::vector<aura::ast::NodeId>{form_quote, result});
            return result;
        }
        return flat_.add_quote(expr);
    }


    if (v.children.empty()) {
        return flat_.add_quote(expr);
    }


    if (depth == 0 && is_unquote(flat_, pool_, expr)) {
        if (v.children.size() > 1) return v.child(1);
        return expr;
    }


    if (depth > 0 && is_unquote(flat_, pool_, expr)) {
        if (v.children.size() > 1) {
            auto inner = expand_qq(v.child(1), depth - 1);
            auto unq_var = flat_.add_variable(pool_.intern("unquote"));
            return flat_.add_quote(flat_.add_call(unq_var, std::vector<aura::ast::NodeId>{inner}));
        }
        return flat_.add_quote(expr);
    }


    if (depth == 0 && is_unquote_splicing(flat_, pool_, expr)) {
        if (v.children.size() > 1) return v.child(1);
        return expr;
    }


    if (depth > 0 && is_unquote_splicing(flat_, pool_, expr)) {
        if (v.children.size() > 1) {
            auto inner = expand_qq(v.child(1), depth - 1);
            auto unsplice_var = flat_.add_variable(pool_.intern("unquote-splicing"));
            return flat_.add_quote(flat_.add_call(unsplice_var, std::vector<aura::ast::NodeId>{inner}));
        }
        return flat_.add_quote(expr);
    }


    if (is_quasiquote(flat_, pool_, expr)) {
        if (v.children.size() > 1) {
            auto inner = expand_qq(v.child(1), depth + 1);
            auto qq_var = flat_.add_variable(pool_.intern("quasiquote"));
            return flat_.add_call(qq_var, std::vector<aura::ast::NodeId>{inner});
        }
    }


    return expand_qq_pair(expr, depth);
}

NodeId FlatParser::expand_qq_pair(NodeId expr, int depth) {
    auto v = flat_.get(expr);


    NodeId result = flat_.add_quote(flat_.add_literal(0));

    for (int i = static_cast<int>(v.children.size()) - 1; i >= 0; --i) {
        auto child = v.child(i);


        if (depth == 0 && is_unquote_splicing(flat_, pool_, child)) {
            auto child_v = flat_.get(child);
            auto spliced = child_v.children.size() > 1 ? child_v.child(1) : child;
            auto append_var = flat_.add_variable(pool_.intern("append"));
            result = flat_.add_call(append_var, std::vector<aura::ast::NodeId>{spliced, result});
        } else {
            auto expanded = expand_qq(child, depth);
            auto cons_var = flat_.add_variable(pool_.intern("cons"));
            result = flat_.add_call(cons_var, std::vector<aura::ast::NodeId>{expanded, result});
        }
    }

    return result;
}


FlatParseResult parse_to_flat(std::string_view source,
                               FlatAST& flat, StringPool& pool) {
    FlatParser fp(flat, pool);
    return fp.parse(source);
}

}
