module aura.parser.parser;

namespace aura::parser {

using namespace aura::ast;

FlatParseResult FlatParser::parse(std::string_view s) {
    lexer_.emplace(s);
    FlatParseResult r;
    r.root = parse_expr();
    if (r.root == NULL_NODE) {
        auto tok = lexer_->peek();
        if (tok.kind != TokenKind::EndOfFile)
            r.error = "parse error at line " + std::to_string(tok.line)
                    + ":" + std::to_string(tok.column);
        else
            r.error = "parse error";
        return r;
    }
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
    r.root = flat_.add_begin(exprs);
    r.success = true;
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
        // Process \" → " and \\ → \ in string literals
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
    case TokenKind::Identifier: {
        auto tok = lexer_->consume();
        auto id = flat_.add_variable(pool_.intern(std::string(tok.text)));
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    case TokenKind::Quote: {
        lexer_->consume(); // consume '
        auto quoted = parse_expr();
        if (quoted == NULL_NODE) return NULL_NODE;
        auto id = flat_.add_quote(quoted);
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    case TokenKind::QuasiQuote: {
        lexer_->consume(); // consume `
        auto quoted = parse_expr();
        if (quoted == NULL_NODE) return NULL_NODE;
        auto id = expand_qq(quoted, 0);
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    case TokenKind::Unquote: {
        lexer_->consume(); // consume ,
        auto inner = parse_expr();
        if (inner == NULL_NODE) return NULL_NODE;
        // Represent (unquote inner) as a Call to variable 'unquote'
        auto unquote_var = flat_.add_variable(pool_.intern("unquote"));
        auto id = flat_.add_call(unquote_var, {inner});
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    case TokenKind::UnquoteSplicing: {
        lexer_->consume(); // consume ,@
        auto inner = parse_expr();
        if (inner == NULL_NODE) return NULL_NODE;
        // Represent (unquote-splicing inner) as a Call to variable 'unquote-splicing'
        auto unsplice_var = flat_.add_variable(pool_.intern("unquote-splicing"));
        auto id = flat_.add_call(unsplice_var, {inner});
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
    auto tok = lexer_->peek(); // '(' or first token
    // () → null sentinel (0)
    if (lexer_->peek().kind == TokenKind::RParen) {
        lexer_->consume();
        auto id = flat_.add_literal(0);
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }
    auto f = lexer_->peek();
    if (f.kind == TokenKind::Identifier) {
        auto kw = f.text;
        if (kw == "if")     return parse_if();
        if (kw == "lambda") return parse_lambda();
        if (kw == "let")    return parse_let(false);
        if (kw == "let*")   return parse_let_star();
        if (kw == "letrec") return parse_let(true);
        if (kw == "define") return parse_define();
        if (kw == "begin")  return parse_begin();
        if (kw == "set!")   return parse_set();
        if (kw == "quote")  return parse_quote();
        if (kw == "cond")   return parse_cond();
        if (kw == "defmacro") return parse_defmacro();
        if (kw == "match")  return parse_match();
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
    auto id = flat_.add_call(func, args);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_if() {
    auto tok = lexer_->consume(); // 'if'
    auto c = parse_expr();
    auto t = parse_expr();
    auto e = parse_expr();
    lexer_->consume(); // ')'
    auto id = flat_.add_if(c, t, e);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_lambda() {
    auto tok = lexer_->consume(); // 'lambda'
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
    auto lid = flat_.add_lambda(params, body); flat_.set_loc(lid, tok.line, tok.column); return lid;
}

NodeId FlatParser::parse_define() {
    lexer_->consume(); // 'define'
    auto n = lexer_->peek();
    if (n.kind == TokenKind::LParen) {
        // Shorthand: (define (fn params...) body...)
        lexer_->consume(); // '('
        auto fn = lexer_->consume();
        if (fn.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
        std::vector<SymId> params;
        while (lexer_->peek().kind != TokenKind::RParen) {
            auto p = lexer_->consume();
            if (p.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
            params.push_back(pool_.intern(std::string(p.text)));
        }
        lexer_->consume(); // ')' after params
        // Parse multiple body expressions and wrap in begin
        std::vector<NodeId> body_exprs;
        while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
            auto be = parse_expr();
            if (be == NULL_NODE) break;
            body_exprs.push_back(be);
        }
        lexer_->consume(); // ')' closing define
        if (body_exprs.empty()) return NULL_NODE;
        NodeId body = (body_exprs.size() == 1) ? body_exprs[0]
                    : flat_.add_begin(body_exprs);
        auto lambda = flat_.add_lambda(params, body);
        return flat_.add_define(pool_.intern(std::string(fn.text)), lambda);
    }
    // Normal: (define name value)
    if (n.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
    lexer_->consume(); // consume name
    auto v = parse_val();
    if (v == NULL_NODE) return NULL_NODE;
    lexer_->consume(); // ')'
    return flat_.add_define(pool_.intern(std::string(n.text)), v);
}

NodeId FlatParser::parse_let(bool rec) {
    auto tok = lexer_->consume(); // 'let' or 'letrec'
    
    // Named let: (let name ((binding...) body)
    if (!rec && lexer_->peek().kind == TokenKind::Identifier) {
        return parse_named_let();
    }
    
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

NodeId FlatParser::parse_named_let() {
    auto name_tok = lexer_->peek(); // already peeked
    if (name_tok.kind != TokenKind::Identifier) return NULL_NODE;
    lexer_->consume(); // consume the name
    auto name = pool_.intern(std::string(name_tok.text));
    
    // Expect '(' for binding list
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
    lexer_->consume(); // ')'
    
    // Read all body expressions and wrap in begin if >1
    std::vector<NodeId> body_exprs;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto be = parse_expr();
        if (be == NULL_NODE) break;
        body_exprs.push_back(be);
    }
    lexer_->consume(); // ')'
    if (body_exprs.empty()) return NULL_NODE;
    NodeId body = (body_exprs.size() == 1) ? body_exprs[0]
                : flat_.add_begin(body_exprs);
    
    // Desugar: (let name ((a1 v1) (a2 v2)) body...)
    //       → (letrec ((name (lambda (a1 a2) body...))) (name v1 v2))
    
    // Collect param symbols and init values
    std::vector<SymId> params;
    std::vector<NodeId> init_vals;
    for (auto& b : bs) {
        params.push_back(b.name);
        init_vals.push_back(b.val);
    }
    
    // Create lambda: (lambda (a1 a2 ...) body)
    auto lambda_id = flat_.add_lambda(params, body);
    
    // Create call: (name v1 v2 ...)
    auto var_id = flat_.add_variable(name);
    auto call_id = flat_.add_call(var_id, init_vals);
    
    // Create letrec: (letrec ((name lambda)) call)
    return flat_.add_letrec(name, lambda_id, call_id);
}

NodeId FlatParser::parse_let_star() {
    lexer_->consume(); // 'let*'
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
    lexer_->consume(); // ')'
    
    // Read all body expressions and wrap in begin if >1
    std::vector<NodeId> body_exprs;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto be = parse_expr();
        if (be == NULL_NODE) break;
        body_exprs.push_back(be);
    }
    lexer_->consume(); // ')'
    if (body_exprs.empty()) return NULL_NODE;
    NodeId body = (body_exprs.size() == 1) ? body_exprs[0]
                : flat_.add_begin(body_exprs);
    
    // Desugar: (let* ((a1 v1) (a2 v2)) body...)
    //       → (let ((a1 v1)) (let ((a2 v2)) body...))
    // Build from right to left so outermost wraps innermost
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
    auto tok = lexer_->consume(); // 'begin'
    std::vector<NodeId> exprs;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto e = parse_expr();
        if (e != NULL_NODE) exprs.push_back(e);
        else break;
    }
    lexer_->consume(); // ')'
    auto bid = flat_.add_begin(exprs);
    flat_.set_loc(bid, tok.line, tok.column);
    return bid;
}

NodeId FlatParser::parse_set() {
    auto tok = lexer_->consume(); // 'set!'
    auto n = lexer_->consume();
    if (n.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
    auto v = parse_val();
    if (v == NULL_NODE) { skip_rparen(); return NULL_NODE; }
    lexer_->consume(); // ')'
    auto sid = flat_.add_set(pool_.intern(std::string(n.text)), v);
    flat_.set_loc(sid, tok.line, tok.column);
    return sid;
}

NodeId FlatParser::parse_quote() {
    auto tok = lexer_->consume(); // 'quote'
    auto v = parse_val();
    if (v == NULL_NODE) { skip_rparen(); return NULL_NODE; }
    lexer_->consume(); // ')'
    auto qid = flat_.add_quote(v);
    flat_.set_loc(qid, tok.line, tok.column);
    return qid;
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
    auto tok = lexer_->consume(); // 'defmacro'
    if (lexer_->consume().kind != TokenKind::LParen) { skip_rparen(); return NULL_NODE; }
    auto name = lexer_->consume();
    if (name.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
    std::vector<SymId> params;
    bool dotted = false;
    while (lexer_->peek().kind != TokenKind::RParen) {
        // Check for dotted rest parameter: (name . rest)
        if (lexer_->peek().kind == TokenKind::Dot) {
            lexer_->consume(); // consume '.'
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
    lexer_->consume(); // ')'
    auto body = parse_expr();
    if (body == NULL_NODE) { skip_rparen(); return NULL_NODE; }
    lexer_->consume(); // ')'
    auto mid = flat_.add_macrodef(pool_.intern(std::string(name.text)), params, body);
    flat_.set_loc(mid, tok.line, tok.column);
    return mid;
}

// ── Match / pattern matching ─────────────────────────────────
NodeId FlatParser::parse_match() {
    auto tok = lexer_->consume(); // 'match'

    // Parse subject expression
    auto subject = parse_expr();
    if (subject == NULL_NODE) { skip_rparen(); return NULL_NODE; }

    // Temp variable to hold subject (evaluated once)
    auto tmp = pool_.intern("__match_tmp");

    // Parse clauses: (pattern body ...)
    struct Clause { NodeId test; NodeId body; };
    std::vector<Clause> clauses;

    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        if (lexer_->peek().kind != TokenKind::LParen) break;
        lexer_->consume(); // '('

        // Parse pattern (as an s-expression value)
        auto pattern = parse_val();
        if (pattern == NULL_NODE) break;

        // Parse body
        auto body = parse_expr();
        if (body == NULL_NODE) break;

        if (lexer_->peek().kind != TokenKind::RParen) break;
        lexer_->consume(); // ')'

        // Compile pattern into test and bindings, then wrap body in let
        NodeId test;
        auto bindings = compile_pattern(pattern, tmp, &test);

        // Wrap body in let bindings
        for (auto& [name, val] : bindings)
            body = flat_.add_let(name, val, body);

        clauses.push_back({test, body});
    }

    if (lexer_->peek().kind == TokenKind::RParen)
        lexer_->consume(); // ')'

    if (clauses.empty()) return NULL_NODE;

    // Build nested if chain from right to left
    NodeId result = clauses.back().body;
    for (auto it = clauses.rbegin() + 1; it != clauses.rend(); ++it)
        result = flat_.add_if(it->test, it->body, result);

    // Wrap in (let ((__match_tmp subject)) result)
    result = flat_.add_let(tmp, subject, result);
    flat_.set_loc(result, tok.line, tok.column);
    return result;
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

    // Helper: call with args as initializer_list
    auto make_call = [&](SymId func, std::initializer_list<NodeId> args) -> NodeId {
        return flat_.add_call(flat_.add_variable(func), std::vector<NodeId>(args));
    };

    // Variable/wildcard pattern
    if (v.tag == NodeTag::Variable) {
        auto name = pool_.resolve(v.sym_id);
        if (name == "_" || (name.size() > 1 && name[0] == '_' && name != "__match_tmp")) {
            // Wildcard: match anything, no bindings
            *out_test = flat_.add_literal(1);
            return bindings;
        }
        // Variable binding: match anything, bind to whole value
        *out_test = flat_.add_literal(1);
        bindings.emplace_back(v.sym_id, var_tmp);
        return bindings;
    }

    // Empty list () → (null? tmp)
    if (v.tag == NodeTag::LiteralInt && v.int_value == 0) {
        *out_test = make_call(sym_null_q, {var_tmp});
        return bindings;
    }

    // Literal number: (= tmp literal) via equal?
    if (v.tag == NodeTag::LiteralInt || v.tag == NodeTag::LiteralFloat || v.tag == NodeTag::LiteralString) {
        *out_test = make_call(sym_equal_q, {var_tmp, pattern_node});
        return bindings;
    }

    // Quote pattern: (quote data) → (equal? tmp '(data))
    if (v.tag == NodeTag::Quote) {
        *out_test = make_call(sym_equal_q, {var_tmp, pattern_node});
        return bindings;
    }

    // Call: (list ...), (cons ...), or other function-like pattern
    if (v.tag == NodeTag::Call) {
        if (v.children.empty()) { *out_test = flat_.add_literal(0); return bindings; }

        auto callee_v = flat_.get(v.child(0));
        if (callee_v.tag == NodeTag::Variable) {
            auto callee_name = pool_.resolve(callee_v.sym_id);

            // (quote data) pattern — explicit quote in call position
            if (callee_name == "quote" && v.children.size() > 1) {
                auto quoted = v.child(1);
                // Re-wrap as proper quote expression
                auto quoted_expr = flat_.add_quote(quoted);
                *out_test = make_call(sym_equal_q, {var_tmp, quoted_expr});
                return bindings;
            }

            // (list p1 p2 ...) pattern
            if (callee_name == "list") {
                // Build chain: (pair? tmp) && (pair? (cdr tmp)) && ... && (null? (cddr... tmp))
                NodeId accumulated_test = flat_.add_literal(1); // start with #t
                NodeId current = var_tmp;

                for (std::size_t i = 1; i < v.children.size(); ++i) {
                    // (pair? current)
                    auto pair_test = make_call(sym_pair_q, {current});
                    accumulated_test = flat_.add_if(accumulated_test, pair_test, flat_.add_literal(0));

                    auto elem = v.child(i);
                    auto elem_v = flat_.get(elem);

                    // (car current) — extract element value
                    auto car_expr = make_call(sym_car, {current});

                    if (elem_v.tag == NodeTag::Variable) {
                        auto elem_name = pool_.resolve(elem_v.sym_id);
                        if (elem_name != "_" && !(elem_name.size() > 1 && elem_name[0] == '_')) {
                            // Variable binding: bind car value
                            bindings.emplace_back(elem_v.sym_id, car_expr);
                        } // else: wildcard, skip
                    } else if (elem_v.tag == NodeTag::LiteralInt && elem_v.int_value == 0) {
                        // () — exact match car against empty list
                        auto eq_test = make_call(sym_equal_q, {car_expr, elem});
                        accumulated_test = flat_.add_if(accumulated_test, eq_test, flat_.add_literal(0));
                    } else if (elem_v.tag == NodeTag::LiteralInt || elem_v.tag == NodeTag::LiteralFloat || elem_v.tag == NodeTag::LiteralString) {
                        // Literal element match
                        auto eq_test = make_call(sym_equal_q, {car_expr, elem});
                        accumulated_test = flat_.add_if(accumulated_test, eq_test, flat_.add_literal(0));
                    }
                    // For (list ...) sub-patterns or other complex elements,
                    // we fall through and they match anything (no equality check)

                    // Move to next: (cdr current)
                    current = make_call(sym_cdr, {current});
                }

                // Final: (null? current) — proper list length check
                auto null_test = make_call(sym_null_q, {current});
                accumulated_test = flat_.add_if(accumulated_test, null_test, flat_.add_literal(0));

                *out_test = accumulated_test;
                return bindings;
            }

            // (cons p q) pattern
            if (callee_name == "cons" && v.children.size() >= 3) {
                // Test: (pair? tmp)
                *out_test = make_call(sym_pair_q, {var_tmp});

                auto car_pat = v.child(1);
                auto cdr_pat = v.child(2);
                auto car_v = flat_.get(car_pat);
                auto cdr_v = flat_.get(cdr_pat);

                auto car_expr = make_call(sym_car, {var_tmp});
                auto cdr_expr = make_call(sym_cdr, {var_tmp});

                // Car binding
                if (car_v.tag == NodeTag::Variable) {
                    auto ename = pool_.resolve(car_v.sym_id);
                    if (ename != "_" && !(ename.size() > 1 && ename[0] == '_'))
                        bindings.emplace_back(car_v.sym_id, car_expr);
                }

                // Cdr binding
                if (cdr_v.tag == NodeTag::Variable) {
                    auto ename = pool_.resolve(cdr_v.sym_id);
                    if (ename != "_" && !(ename.size() > 1 && ename[0] == '_'))
                        bindings.emplace_back(cdr_v.sym_id, cdr_expr);
                }

                return bindings;
            }
        }
    }

    // Default fallback: exact equality match
    *out_test = make_call(sym_equal_q, {var_tmp, pattern_node});
    return bindings;
}

// ── Quasiquote expansion ───────────────────────────────────────

// Check if a node is (unquote ...)
static bool is_unquote(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool, NodeId id) {
    if (id == NULL_NODE) return false;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Call || v.children.empty()) return false;
    auto callee = flat.get(v.child(0));
    return callee.tag == NodeTag::Variable && std::string(pool.resolve(callee.sym_id)) == "unquote";
}

// Check if a node is (unquote-splicing ...)
static bool is_unquote_splicing(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool, NodeId id) {
    if (id == NULL_NODE) return false;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Call || v.children.empty()) return false;
    auto callee = flat.get(v.child(0));
    return callee.tag == NodeTag::Variable && std::string(pool.resolve(callee.sym_id)) == "unquote-splicing";
}

// Check if a node is (quasiquote ...)
static bool is_quasiquote(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool, NodeId id) {
    if (id == NULL_NODE) return false;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Call || v.children.empty()) return false;
    auto callee = flat.get(v.child(0));
    return callee.tag == NodeTag::Variable && std::string(pool.resolve(callee.sym_id)) == "quasiquote";
}

NodeId FlatParser::expand_qq(NodeId expr, int depth) {
    if (expr == NULL_NODE) {
        // Empty quasiquote: (quote ())
        return flat_.add_quote(flat_.add_literal(0));
    }

    auto v = flat_.get(expr);

    // Non-Call compound nodes: special forms parsed by keyword (IfExpr, Lambda, Let, etc.)
    // The keyword (if, lambda, etc.) is LOST by parse_list — we need to reconstruct it
    if (v.tag != NodeTag::Call) {
        // Variables and literals: (quote expr)
        if (v.tag == NodeTag::Variable || v.tag == NodeTag::LiteralInt 
            || v.tag == NodeTag::LiteralFloat || v.tag == NodeTag::LiteralString
            || v.tag == NodeTag::Quote) {
            return flat_.add_quote(expr);
        }
        // Other compound nodes (IfExpr, Begin, Lambda, etc.) with children:
        // Build a list that starts with (quote <form-name>) followed by child expansions
        if (v.children.size() > 0) {
            // Determine the form name based on node tag
            std::string form_name;
            switch (v.tag) {
            case NodeTag::IfExpr:    form_name = "if"; break;
            case NodeTag::Lambda:    form_name = "lambda"; break;
            case NodeTag::Let:       form_name = "let"; break;
            case NodeTag::LetRec:    form_name = "letrec"; break;
            case NodeTag::Define:    form_name = "define"; break;
            case NodeTag::Begin:     form_name = "begin"; break;
            case NodeTag::Set:       form_name = "set!"; break;
            default: return flat_.add_quote(expr);
            }
            // Build: (cons (quote <form-name>) (cons arg0 (cons arg1 ... (quote ()))))
            // where args are: params (if Lambda) + children + extra elements
            std::vector<NodeId> args_to_expand;
            
            // For Lambda, add quoted parameter list as first arg
            if (v.tag == NodeTag::Lambda) {
                NodeId params_list = flat_.add_quote(flat_.add_literal(0)); // (quote ())
                for (int pi = static_cast<int>(v.params.size()) - 1; pi >= 0; --pi) {
                    auto param_var = flat_.add_variable(pool_.intern(std::string(pool_.resolve(v.params[pi]))));
                    auto param_quoted = flat_.add_quote(param_var);
                    auto cv = flat_.add_variable(pool_.intern("cons"));
                    params_list = flat_.add_call(cv, {param_quoted, params_list});
                }
                args_to_expand.push_back(params_list);
            }
            
            bool has_fn_list = false;  // for define shorthand
            // For Define shorthand (define+lambda child), replace with (fn params) list
            if (v.tag == NodeTag::Define && v.children.size() == 1 && v.sym_id != ast::INVALID_SYM) {
                auto child_v = flat_.get(v.child(0));
                if (child_v.tag == NodeTag::Lambda) {
                    // Build (fn params) list
                    auto fn_var = flat_.add_variable(pool_.intern(std::string(pool_.resolve(v.sym_id))));
                    auto fn_quoted = flat_.add_quote(fn_var);
                    NodeId fn_params_list = flat_.add_quote(flat_.add_literal(0));
                    for (int pi = static_cast<int>(child_v.params.size()) - 1; pi >= 0; --pi) {
                        auto pvar = flat_.add_variable(pool_.intern(std::string(pool_.resolve(child_v.params[pi]))));
                        auto pquoted = flat_.add_quote(pvar);
                        auto cv = flat_.add_variable(pool_.intern("cons"));
                        fn_params_list = flat_.add_call(cv, {pquoted, fn_params_list});
                    }
                    auto cv = flat_.add_variable(pool_.intern("cons"));
                    args_to_expand.push_back(flat_.add_call(cv, {fn_quoted, fn_params_list}));
                    has_fn_list = true;
                    // Also add the lambda body as args
                    for (std::size_t bci = 0; bci < child_v.children.size(); ++bci) {
                        auto expanded = expand_qq(child_v.child(bci), depth);
                        args_to_expand.push_back(expanded);
                    }
                }
            }
            
            // Add children (skip for define shorthand — already handled above)
            if (!has_fn_list) {
                for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                    auto expanded = expand_qq(v.child(ci), depth);
                    args_to_expand.push_back(expanded);
                }
            }
            
            // Build result: (quote ()) then prepend each arg
            NodeId result = flat_.add_quote(flat_.add_literal(0));
            for (int i = static_cast<int>(args_to_expand.size()) - 1; i >= 0; --i) {
                auto cons_var = flat_.add_variable(pool_.intern("cons"));
                result = flat_.add_call(cons_var, {args_to_expand[i], result});
            }
            
            // Prepend (quote <form-name>)
            auto form_var = flat_.add_variable(pool_.intern(form_name));
            auto form_quote = flat_.add_quote(form_var);
            auto cons_var2 = flat_.add_variable(pool_.intern("cons"));
            result = flat_.add_call(cons_var2, {form_quote, result});
            return result;
        }
        return flat_.add_quote(expr);
    }

    // Empty list: (quote ())
    if (v.children.empty()) {
        return flat_.add_quote(expr);
    }

    // Handle unquote at depth 0: just return the inner expression
    if (depth == 0 && is_unquote(flat_, pool_, expr)) {
        if (v.children.size() > 1) return v.child(1);
        return expr;
    }

    // Handle unquote at depth > 0: (quote (unquote ...))
    if (depth > 0 && is_unquote(flat_, pool_, expr)) {
        if (v.children.size() > 1) {
            auto inner = expand_qq(v.child(1), depth - 1);
            auto unq_var = flat_.add_variable(pool_.intern("unquote"));
            return flat_.add_quote(flat_.add_call(unq_var, {inner}));
        }
        return flat_.add_quote(expr);
    }

    // Handle unquote-splicing at depth 0: return the inner expression
    if (depth == 0 && is_unquote_splicing(flat_, pool_, expr)) {
        if (v.children.size() > 1) return v.child(1);
        return expr;
    }

    // Handle unquote-splicing at depth > 0: (quote (unquote-splicing ...))
    if (depth > 0 && is_unquote_splicing(flat_, pool_, expr)) {
        if (v.children.size() > 1) {
            auto inner = expand_qq(v.child(1), depth - 1);
            auto unsplice_var = flat_.add_variable(pool_.intern("unquote-splicing"));
            return flat_.add_quote(flat_.add_call(unsplice_var, {inner}));
        }
        return flat_.add_quote(expr);
    }

    // Handle nested quasiquote
    if (is_quasiquote(flat_, pool_, expr)) {
        if (v.children.size() > 1) {
            auto inner = expand_qq(v.child(1), depth + 1);
            auto qq_var = flat_.add_variable(pool_.intern("quasiquote"));
            return flat_.add_call(qq_var, {inner});
        }
    }

    // Pair/list: expand all children
    return expand_qq_pair(expr, depth);
}

NodeId FlatParser::expand_qq_pair(NodeId expr, int depth) {
    auto v = flat_.get(expr);

    // Build from right to left, starting with (quote ()), consing each element
    NodeId result = flat_.add_quote(flat_.add_literal(0)); // (quote ())

    for (int i = static_cast<int>(v.children.size()) - 1; i >= 0; --i) {
        auto child = v.child(i);

        // Handle unquote-splicing at depth 0: (append expr result)
        if (depth == 0 && is_unquote_splicing(flat_, pool_, child)) {
            auto child_v = flat_.get(child);
            auto spliced = child_v.children.size() > 1 ? child_v.child(1) : child;
            auto append_var = flat_.add_variable(pool_.intern("append"));
            result = flat_.add_call(append_var, {spliced, result});
        } else {
            auto expanded = expand_qq(child, depth);
            auto cons_var = flat_.add_variable(pool_.intern("cons"));
            result = flat_.add_call(cons_var, {expanded, result});
        }
    }

    return result;
}

// ── Free function ──────────────────────────────────────────────
FlatParseResult parse_to_flat(std::string_view source,
                               FlatAST& flat, StringPool& pool) {
    FlatParser fp(flat, pool);
    return fp.parse(source);
}

} // namespace aura::parser
