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
        auto id = flat_.add_literalstring(pool_.intern(std::string(tok.text)));
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
        if (kw == "define-struct") return parse_define_struct();
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

NodeId FlatParser::parse_define_struct() {
    auto tok = lexer_->consume(); // 'define-struct'
    
    // Parse struct name
    auto name_tok = lexer_->consume();
    if (name_tok.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
    auto struct_name = pool_.intern(std::string(name_tok.text));
    
    // Parse field list: (field1 field2 ...)
    if (lexer_->consume().kind != TokenKind::LParen) { skip_rparen(); return NULL_NODE; }
    std::vector<SymId> fields;
    while (lexer_->peek().kind != TokenKind::RParen) {
        auto f = lexer_->consume();
        if (f.kind != TokenKind::Identifier) { skip_rparen(); return NULL_NODE; }
        fields.push_back(pool_.intern(std::string(f.text)));
    }
    lexer_->consume(); // ')' after field list
    
    lexer_->consume(); // ')' closing define-struct
    
    // ── Generate constructor: (define make-Name (lambda (f1 f2 ...)
    //                               (vector 'Name f1 f2 ...)))
    auto make_name = pool_.intern(std::string("make-") + std::string(name_tok.text));
    
    // Build the quote for the type tag: 'Name
    auto tag_var = flat_.add_variable(struct_name);
    auto tag_quote = flat_.add_quote(tag_var);
    
    // Build: (vector 'Name f1 f2 ...)
    std::vector<NodeId> vec_args;
    vec_args.push_back(tag_quote);
    for (auto f : fields) {
        vec_args.push_back(flat_.add_variable(f));
    }
    auto vector_call = flat_.add_call(flat_.add_variable(pool_.intern("vector")), vec_args);
    auto constructor = flat_.add_lambda(fields, vector_call);
    auto make_define = flat_.add_define(make_name, constructor);
    
    // ── Generate predicate: (define Name? (lambda (obj)
    //                               (and (vector? obj)
    //                                    (equal? (vector-ref obj 0) 'Name))))
    auto pred_name = pool_.intern(std::string(name_tok.text) + "?");
    auto obj_sym = pool_.intern("__obj");
    auto obj_var = flat_.add_variable(obj_sym);
    
    // (vector? obj)
    auto vec_check = flat_.add_call(
        flat_.add_variable(pool_.intern("vector?")),
        std::vector<NodeId>{obj_var}
    );
    // (vector-ref obj 0)
    auto ref_call = flat_.add_call(
        flat_.add_variable(pool_.intern("vector-ref")),
        std::vector<NodeId>{obj_var, flat_.add_literal(0)}
    );
    // (equal? ref 'Name)
    auto eq_check = flat_.add_call(
        flat_.add_variable(pool_.intern("equal?")),
        std::vector<NodeId>{ref_call, tag_quote}
    );
    // (and vec-check eq-check)
    auto and_call = flat_.add_call(
        flat_.add_variable(pool_.intern("and")),
        std::vector<NodeId>{vec_check, eq_check}
    );
    
    auto pred_lambda = flat_.add_lambda({obj_sym}, and_call);
    auto pred_define = flat_.add_define(pred_name, pred_lambda);
    
    // ── Generate accessors: (define Name-field (lambda (rec) (vector-ref rec N)))
    std::vector<NodeId> all_defines;
    all_defines.push_back(make_define);
    all_defines.push_back(pred_define);
    
    for (std::size_t i = 0; i < fields.size(); ++i) {
        auto fname = pool_.resolve(fields[i]);
        auto acc_name = pool_.intern(std::string(name_tok.text) + "-" + std::string(fname));
        auto rec_sym = pool_.intern("__rec");
        auto rec_var = flat_.add_variable(rec_sym);
        
        // (vector-ref obj i+1)  (index 0 is the tag)
        auto ref = flat_.add_call(
            flat_.add_variable(pool_.intern("vector-ref")),
            std::vector<NodeId>{rec_var, flat_.add_literal(static_cast<std::int64_t>(i + 1))}
        );
        auto acc_lambda = flat_.add_lambda({rec_sym}, ref);
        auto acc_define = flat_.add_define(acc_name, acc_lambda);
        all_defines.push_back(acc_define);
    }
    
    // Wrap everything in begin
    auto id = flat_.add_begin(all_defines.data(), static_cast<std::uint32_t>(all_defines.size()));
    flat_.set_loc(id, tok.line, tok.column);
    return id;
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
    while (lexer_->peek().kind != TokenKind::RParen) {
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

// ── Free function ──────────────────────────────────────────────
FlatParseResult parse_to_flat(std::string_view source,
                               FlatAST& flat, StringPool& pool) {
    FlatParser fp(flat, pool);
    return fp.parse(source);
}

} // namespace aura::parser
