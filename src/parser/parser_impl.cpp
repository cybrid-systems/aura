module aura.parser.parser;

namespace aura::parser {

using namespace aura::ast;

FlatParseResult FlatParser::parse(std::string_view s) {
    FlatParseResult r;
    try {
    lexer_.emplace(s);

    // Helper: record a parse error and skip to next recoverable point
    // Token kind to readable string for error messages
    auto token_desc = [&](const Token& t) -> std::string {
        switch (t.kind) {
            case TokenKind::Identifier:
                return "identifier '" + std::string(t.text) + "'";
            case TokenKind::Integer:
                return "integer '" + std::string(t.text) + "'";
            case TokenKind::Float:
                return "float '" + std::string(t.text) + "'";
            case TokenKind::String:
                return "string literal '" + std::string(t.text) + "'";
            case TokenKind::Bool:
                return "boolean '" + std::string(t.text) + "'";
            case TokenKind::LParen:
                return "'('";
            case TokenKind::RParen:
                return "')'";
            case TokenKind::Quote:
                return "''";
            case TokenKind::QuasiQuote:
                return "'`'";
            case TokenKind::Unquote:
                return "','";
            case TokenKind::Dot:
                return "'.'";
            case TokenKind::Ellipsis:
                return "'...'";
            case TokenKind::EndOfFile:
                return "end of input";
            case TokenKind::Error:
                return "invalid character";
            default:
                return "token";
        }
    };

    // Record structured parse error with token location
    auto record_error = [&](const std::string& msg, std::optional<Token> err_tok = std::nullopt) {
        auto loc = err_tok ? aura::diag::SourceLocation{err_tok->line, err_tok->column, 0}
                           : aura::diag::SourceLocation{};
        auto formatted = loc.valid()
            ? std::format("{}: {}", loc.format(), msg)
            : msg;
        if (r.error.empty())
            r.error = formatted;
        r.errors.push_back({msg, loc});
        // Skip tokens until we can try parsing again
        int depth = 0;
        while (!lexer_->eof()) {
            auto tok = lexer_->peek();
            if (depth == 0) {
                // At top level, try to find the start of a new expression
                if (tok.kind == TokenKind::LParen || tok.kind == TokenKind::Integer ||
                    tok.kind == TokenKind::Float || tok.kind == TokenKind::String ||
                    tok.kind == TokenKind::Identifier || tok.kind == TokenKind::Bool ||
                    tok.kind == TokenKind::Quote || tok.kind == TokenKind::QuasiQuote ||
                    tok.kind == TokenKind::Unquote) {
                    break;
                }
                lexer_->consume();
            } else {
                // Inside parens, skip until matching close
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
            record_error("expected expression, got " + token_desc(tok), tok);
        } else {
            record_error("expected expression, reached end of input");
        }
        // If we recovered but got nothing, return as failure
        if (r.root == NULL_NODE)
            return r;
    }

    // Check for multiple top-level expressions
    auto next = lexer_->peek();
    if (next.kind == TokenKind::EndOfFile || next.kind == TokenKind::Error) {
        r.success = r.root != NULL_NODE;
        return r;
    }

    // Multiple forms → wrap in begin
    std::vector<NodeId> exprs;
    exprs.push_back(r.root);
    do {
        auto e = parse_expr();
        if (e == NULL_NODE) {
            auto tok = lexer_->peek();
            if (tok.kind != TokenKind::EndOfFile) {
                record_error("expected expression, got " + token_desc(tok), tok);
                e = parse_expr(); // try again after skip
            }
            if (lexer_->eof())
                break;
        }
        if (e != NULL_NODE)
            exprs.push_back(e);
        if (lexer_->eof())
            break;
        next = lexer_->peek();
    } while (next.kind != TokenKind::EndOfFile);

    r.root = flat_.add_begin(exprs);
    r.success = !exprs.empty();
    return r;
    } catch (const std::bad_alloc&) {
        r.success = false;
        r.error = "out of memory during parse";
        return r;
    } catch (const std::exception&) {
        r.success = false;
        r.error = "parse error";
        return r;
    }
}

NodeId FlatParser::parse_expr() {
    if (!lexer_)
        return NULL_NODE;
    // Recursion depth guard: prevent stack overflow on deeply nested expressions
    static constexpr std::size_t MAX_PARSE_DEPTH = 500;
    if (++parse_depth_ > MAX_PARSE_DEPTH) {
        --parse_depth_;
        return NULL_NODE;
    }
    // RAII depth decrement on any exit path
    struct DepthGuard {
        std::size_t& d;
        ~DepthGuard() { --d; }
    } _dg{parse_depth_};
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
            auto tok = lexer_->consume();
            auto s = std::string(tok.text);
            // Process \" → " and \\ → \ in string literals
            std::string unescaped;
            bool has_esc = false;
            for (std::size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    auto n = s[i + 1];
                    if (n == '"') {
                        unescaped += '"';
                        i++;
                        has_esc = true;
                    } else if (n == '\\') {
                        unescaped += '\\';
                        i++;
                        has_esc = true;
                    } else {
                        unescaped += s[i];
                    }
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
            auto text = std::string(tok.text);
            // M4 &x reader macro: &x → (borrow x)
            if (text.size() > 1 && text[0] == '&') {
                auto var_name = text.substr(1);
                auto inner = flat_.add_variable(pool_.intern(var_name));
                auto id = flat_.add_borrow(inner);
                flat_.set_loc(id, tok.line, tok.column);
                return id;
            }
            // M4 &mut-x reader macro: &mut-x → (mut-borrow x)
            if (text.size() > 5 && text.substr(0, 5) == "&mut-") {
                auto var_name = text.substr(5);
                auto inner = flat_.add_variable(pool_.intern(var_name));
                auto id = flat_.add_mut_borrow(inner);
                flat_.set_loc(id, tok.line, tok.column);
                return id;
            }
            auto id = flat_.add_variable(pool_.intern(text));
            flat_.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::Quote: {
            lexer_->consume(); // consume '
            auto quoted = parse_expr();
            if (quoted == NULL_NODE)
                return NULL_NODE;
            auto id = flat_.add_quote(quoted);
            flat_.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::HashLParen: {
            // #(1 2 3) → (vector 1 2 3)
            lexer_->consume(); // consume #(
            auto vec_fun = flat_.add_variable(pool_.intern("vector"));
            std::vector<aura::ast::NodeId> args;
            while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
                auto arg = parse_expr();
                if (arg == NULL_NODE) break;
                args.push_back(arg);
            }
            if (lexer_->peek().kind == TokenKind::RParen)
                lexer_->consume(); // consume )
            auto id = flat_.add_call(vec_fun, args);
            flat_.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::QuasiQuote: {
            lexer_->consume(); // consume `
            auto quoted = parse_expr();
            if (quoted == NULL_NODE)
                return NULL_NODE;
            auto id = expand_qq(quoted, 0);
            flat_.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::Unquote: {
            lexer_->consume(); // consume ,
            auto inner = parse_expr();
            if (inner == NULL_NODE)
                return NULL_NODE;
            // Represent (unquote inner) as a Call to variable 'unquote'
            auto unquote_var = flat_.add_variable(pool_.intern("unquote"));
            auto id = flat_.add_call(unquote_var, std::vector<aura::ast::NodeId>{inner});
            flat_.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::UnquoteSplicing: {
            lexer_->consume(); // consume ,@
            auto inner = parse_expr();
            if (inner == NULL_NODE)
                return NULL_NODE;
            // Represent (unquote-splicing inner) as a Call to variable 'unquote-splicing'
            auto unsplice_var = flat_.add_variable(pool_.intern("unquote-splicing"));
            auto id = flat_.add_call(unsplice_var, std::vector<aura::ast::NodeId>{inner});
            flat_.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::LParen:
            lexer_->consume();
            return parse_list();
        default:
            return NULL_NODE;
    }
}

NodeId FlatParser::parse_number(Token tok) {
    try {
        auto v = std::stoll(std::string(tok.text));
        return flat_.add_literal(v);
    } catch (...) {
        return NULL_NODE;
    }
}

NodeId FlatParser::parse_float(Token tok) {
    try {
        auto v = std::stod(std::string(tok.text));
        return flat_.add_literal_float(v);
    } catch (...) {
        return NULL_NODE;
    }
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
        if (kw == "if")
            return parse_if();
        if (kw == "lambda")
            return parse_lambda();
        if (kw == "let")
            return parse_let(false);
        if (kw == "let*")
            return parse_let_star();
        if (kw == "letrec")
            return parse_let(true);
        if (kw == "define")
            return parse_define();
        if (kw == "begin")
            return parse_begin();
        if (kw == "set!")
            return parse_set();
        if (kw == "quote")
            return parse_quote();
        if (kw == "cond")
            return parse_cond();
        if (kw == "defmacro")
            return parse_defmacro();
        if (kw == "match")
            return parse_match();
        if (kw == "cast")
            return parse_cast();
        if (kw == "check")
            return parse_check();
        if (kw == ":")
            return parse_type_annot();
        if (kw == "define-type")
            return parse_define_type();
        if (kw == "define-module")
            return parse_define_module();
        if (kw == "Linear")
            return parse_linear();
        if (kw == "move")
            return parse_move();
        if (kw == "borrow")
            return parse_borrow();
        if (kw == "mut-borrow")
            return parse_mut_borrow();
        // Note: 'drop' is intentionally NOT a parser special form.
        // It's a C++ primitive (list drop) defined in evaluator_impl.cpp.
        // The linear-type drop is accessible via a different name if needed.
        if (kw == "export") {
            lexer_->consume(); // consume 'export'
            std::vector<aura::ast::NodeId> syms;
            while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
                auto sym = parse_expr();
                if (sym != aura::ast::NULL_NODE)
                    syms.push_back(sym);
                else
                    break;
            }
            if (lexer_->peek().kind == TokenKind::RParen)
                lexer_->consume();
            auto id = flat_.add_export(syms);
            flat_.set_loc(id, tok.line, tok.column);
            return id;
        }
    }

    auto func = parse_expr();
    if (func == NULL_NODE) {
        skip_rparen();
        return NULL_NODE;
    }

    std::vector<NodeId> args;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        // Dotted pair: (a . b) or (a b ... . c)
        if (lexer_->peek().kind == TokenKind::Dot) {
            lexer_->consume(); // consume '.'
            auto cdr = parse_expr();
            if (cdr == NULL_NODE) {
                skip_rparen();
                return NULL_NODE;
            }
            if (lexer_->peek().kind != TokenKind::RParen) {
                skip_rparen();
                return NULL_NODE;
            }
            lexer_->consume(); // ')'

            // Build cons chain: cons(func, cons(arg1, ..., cons(argN, cdr)))
            NodeId tail = cdr;
            for (auto it = args.rbegin(); it != args.rend(); ++it)
                tail = flat_.add_pair(*it, tail);
            tail = flat_.add_pair(func, tail);
            flat_.set_loc(tail, tok.line, tok.column);
            return tail;
        }
        auto a = parse_expr();
        if (a != NULL_NODE)
            args.push_back(a);
        else
            break;
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
    // Check if the next token is the closing paren; if not, there are extra
    // arguments that belong to the else branch. Wrap them in begin.
    if (lexer_->peek().kind == TokenKind::RParen) {
        lexer_->consume(); // ')'
    } else {
        std::vector<NodeId> extra = {e};
        while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
            auto x = parse_expr();
            if (x != aura::ast::NULL_NODE)
                extra.push_back(x);
        }
        lexer_->consume(); // ')'
        e = (extra.size() == 1) ? extra[0] : flat_.add_begin(extra);
    }
    auto id = flat_.add_if(c, t, e);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_lambda() {
    auto tok = lexer_->consume(); // 'lambda'
    if (lexer_->consume().kind != TokenKind::LParen)
        return NULL_NODE;

    std::vector<SymId> params;
    std::vector<NodeId> annots; // parallel annotation node IDs (NULL_NODE = none)
    bool dotted = false;
    while (lexer_->peek().kind != TokenKind::RParen) {
        // Check for dotted rest parameter: (lambda (x . rest) body)
        if (lexer_->peek().kind == TokenKind::Dot) {
            lexer_->consume(); // consume '.'
            if (lexer_->peek().kind == TokenKind::RParen) {
                dotted = true;
                break;
            } // (lambda rest .) = all args
            auto rest = lexer_->consume();
            if (rest.kind != TokenKind::Identifier)
                return NULL_NODE;
            params.push_back(pool_.intern(std::string(rest.text)));
            annots.push_back(NULL_NODE);
            dotted = true;
            break;
        }
        // Check for type-annotated parameter: (lambda ((: x Type)) body)
        if (lexer_->peek().kind == TokenKind::LParen) {
            // Type-annotated parameter: ((: x Int)) or ((: x Int val))
            // The outer '(' before ':' is consumed here, parse_type_annot handles the rest
            lexer_->consume();
            auto annot_node = parse_type_annot();
            if (annot_node == NULL_NODE)
                return NULL_NODE;
            // TypeAnnotation node: child[0] = Variable with the parameter name
            auto annot_tag = flat_.get(annot_node);
            if (annot_tag.children.empty())
                return NULL_NODE;
            auto var_node = flat_.get(annot_tag.child(0));
            params.push_back(var_node.sym_id);
            annots.push_back(annot_node);
        } else {
            auto t = lexer_->consume();
            if (t.kind != TokenKind::Identifier)
                return NULL_NODE;
            params.push_back(pool_.intern(std::string(t.text)));
            annots.push_back(NULL_NODE);
        }
    }
    lexer_->consume(); // ')'

    // Parse multiple body expressions and wrap in begin
    std::vector<NodeId> body_exprs;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto be = parse_expr();
        if (be != NULL_NODE)
            body_exprs.push_back(be);
        if (lexer_->peek().kind == TokenKind::RParen)
            break;
    }
    auto body = body_exprs.empty() ? NULL_NODE
                                   : (body_exprs.size() == 1
                                          ? body_exprs[0]
                                          : flat_.add_begin(body_exprs.data(), body_exprs.size()));
    if (body == NULL_NODE)
        return NULL_NODE;
    lexer_->consume(); // ')'
    auto lid = flat_.add_lambda(params, annots, body, dotted);
    flat_.set_loc(lid, tok.line, tok.column);
    return lid;
}

NodeId FlatParser::parse_define() {
    lexer_->consume(); // 'define'
    auto n = lexer_->peek();
    if (n.kind == TokenKind::LParen) {
        // Shorthand: (define (fn params...) body...)
        lexer_->consume(); // '('
        auto fn = lexer_->consume();
        if (fn.kind != TokenKind::Identifier) {
            skip_rparen();
            return NULL_NODE;
        }
        std::vector<SymId> params;
        std::vector<NodeId> annots;
        bool dotted = false;
        while (lexer_->peek().kind != TokenKind::RParen) {
            // Check for dotted rest parameter: (define (f . rest) body)
            if (lexer_->peek().kind == TokenKind::Dot) {
                lexer_->consume(); // consume '.'
                if (lexer_->peek().kind == TokenKind::RParen) {
                    dotted = true;
                    break;
                }
                auto rest = lexer_->consume();
                if (rest.kind != TokenKind::Identifier) {
                    skip_rparen();
                    return NULL_NODE;
                }
                params.push_back(pool_.intern(std::string(rest.text)));
                annots.push_back(NULL_NODE);
                dotted = true;
                break;
            }
            // Check for type-annotated parameter: (define (f (x : Int)) ...)
            if (lexer_->peek().kind == TokenKind::LParen) {
                lexer_->consume();
                auto annot_node = parse_type_annot();
                if (annot_node == NULL_NODE)
                    return NULL_NODE;
                auto annot_tag = flat_.get(annot_node);
                if (annot_tag.children.empty())
                    return NULL_NODE;
                auto var_node = flat_.get(annot_tag.child(0));
                params.push_back(var_node.sym_id);
                annots.push_back(annot_node);
            } else {
                auto p = lexer_->consume();
                if (p.kind != TokenKind::Identifier) {
                    skip_rparen();
                    return NULL_NODE;
                }
                params.push_back(pool_.intern(std::string(p.text)));
                annots.push_back(NULL_NODE);
            }
        }
        lexer_->consume(); // ')' after params
        // Parse multiple body expressions and wrap in begin
        std::vector<NodeId> body_exprs;
        while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
            auto be = parse_expr();
            if (be == NULL_NODE)
                break;
            body_exprs.push_back(be);
        }
        lexer_->consume(); // ')' closing define
        if (body_exprs.empty())
            return NULL_NODE;
        NodeId body = (body_exprs.size() == 1) ? body_exprs[0] : flat_.add_begin(body_exprs);
        auto lambda = flat_.add_lambda(params, annots, body, dotted);
        return flat_.add_define(pool_.intern(std::string(fn.text)), lambda);
    }
    // Normal: (define name value)
    if (n.kind != TokenKind::Identifier) {
        skip_rparen();
        return NULL_NODE;
    }
    lexer_->consume(); // consume name
    auto v = parse_expr();
    if (v == NULL_NODE)
        return NULL_NODE;
    lexer_->consume(); // ')'
    return flat_.add_define(pool_.intern(std::string(n.text)), v);
}

NodeId FlatParser::parse_define_type() {
    auto tok = lexer_->consume(); // 'define-type'

    // Parse type name (possibly with params): Name or (Name params...)
    std::vector<SymId> params;
    SymId type_name;
    if (lexer_->peek().kind == TokenKind::LParen) {
        lexer_->consume(); // '('
        auto name_tok = lexer_->peek();
        if (name_tok.kind != TokenKind::Identifier) {
            skip_rparen();
            skip_rparen();
            return NULL_NODE;
        }
        type_name = pool_.intern(name_tok.text);
        lexer_->consume(); // type name
        // Parse type parameters
        while (lexer_->peek().kind == TokenKind::Identifier) {
            params.push_back(pool_.intern(lexer_->peek().text));
            lexer_->consume();
        }
        if (lexer_->peek().kind != TokenKind::RParen) {
            skip_rparen();
            return NULL_NODE;
        }
        lexer_->consume(); // ')'
    } else {
        auto name_tok = lexer_->peek();
        if (name_tok.kind != TokenKind::Identifier) {
            skip_rparen();
            return NULL_NODE;
        }
        type_name = pool_.intern(name_tok.text);
        lexer_->consume(); // type name
    }

    // Parse constructor clauses: each is (CtorName field-types...)
    std::vector<NodeId> ctors;
    while (lexer_->peek().kind == TokenKind::LParen) {
        lexer_->consume(); // '('
        auto ctor_name_tok = lexer_->peek();
        if (ctor_name_tok.kind != TokenKind::Identifier) {
            skip_rparen();
            break;
        }
        auto ctor_sym = pool_.intern(ctor_name_tok.text);
        lexer_->consume(); // ctor name

        // Parse field type expressions (identifiers or parenthesized types)
        std::vector<NodeId> field_nodes;
        while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
            auto ft = parse_expr();
            if (ft == NULL_NODE) break;
            field_nodes.push_back(ft);
        }
        lexer_->consume(); // ')'

        // Store constructor as (quote (ctor-name ft1 ft2 ...))
        // Build a list of field type expressions
        NodeId field_list = flat_.add_literal(0); // '() sentinel
        for (auto it = field_nodes.rbegin(); it != field_nodes.rend(); ++it) {
            field_list = flat_.add_pair(*it, field_list);
        }
        // Prepend constructor name
        auto ctor_name_var = flat_.add_variable(ctor_sym);
        auto ctor_list = flat_.add_pair(ctor_name_var, field_list);
        auto ctor_node = flat_.add_quote(ctor_list);
        flat_.set_loc(ctor_node, tok.line, tok.column);
        ctors.push_back(ctor_node);
    }

    if (lexer_->peek().kind != TokenKind::RParen) {
        skip_rparen();
        return NULL_NODE;
    }
    lexer_->consume(); // ')'

    return flat_.add_define_type(type_name, params, ctors);
}

NodeId FlatParser::parse_define_module() {
    auto tok = lexer_->consume(); // 'define-module'

    // (define-module (Name :Param-1 :Param-2 ...) body...)
    // Parse the header: (Name :Param ...)
    if (lexer_->peek().kind != TokenKind::LParen)
        return NULL_NODE;
    lexer_->consume(); // '('

    auto tok_name = lexer_->peek();
    if (tok_name.kind != TokenKind::Identifier) {
        skip_rparen();
        return NULL_NODE;
    }
    lexer_->consume(); // Name
    auto name_sym = pool_.intern(tok_name.text);

    // Parse parameters: plain identifiers (T, K, V) or (cap :Capability)
    std::vector<aura::ast::SymId> type_params;
    std::vector<aura::ast::SymId> cap_params;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto param_tok = lexer_->peek();
        if (param_tok.kind == TokenKind::LParen) {
            // Capability parameter: (cap :Capability)
            lexer_->consume(); // '('
            auto inner_tok = lexer_->peek();
            if (inner_tok.kind == TokenKind::Identifier) {
                lexer_->consume(); // param name (e.g., cap)
                // Check for ':Capability' or ':' 'Capability' annotation
                auto next_tok = lexer_->peek();
                bool is_cap = false;
                if (next_tok.kind == TokenKind::Identifier && next_tok.text == ":Capability") {
                    lexer_->consume(); // ':Capability'
                    is_cap = true;
                } else if (next_tok.kind == TokenKind::Identifier && next_tok.text == ":") {
                    lexer_->consume(); // ':'
                    auto type_tok = lexer_->peek();
                    if (type_tok.kind == TokenKind::Identifier &&
                        (type_tok.text == "Capability" || type_tok.text == "capability")) {
                        lexer_->consume(); // Capability
                        is_cap = true;
                    }
                }
                type_params.push_back(pool_.intern(inner_tok.text));
                if (is_cap)
                    cap_params.push_back(pool_.intern(inner_tok.text));
            }
            skip_rparen(); // skip ')'
        } else if (param_tok.kind == TokenKind::Identifier && !param_tok.text.empty() &&
            param_tok.text[0] != '(') {
            lexer_->consume();
            type_params.push_back(pool_.intern(param_tok.text));
        } else {
            break;
        }
    }
    lexer_->consume(); // ')' after params

    // Create the define-module AST node
    auto id = flat_.add_define_module(name_sym, type_params, cap_params);

    // Parse body expressions
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto body_expr = parse_expr();
        if (body_expr != NULL_NODE)
            flat_.insert_child(id, 1000000, body_expr);
    }
    lexer_->consume(); // ')' after body
    flat_.set_loc(id, tok.line, tok.column);

    return id;
}

NodeId FlatParser::parse_let(bool rec) {
    auto tok = lexer_->consume(); // 'let' or 'letrec'

    // Named let: (let name ((binding...) body)
    if (!rec && lexer_->peek().kind == TokenKind::Identifier) {
        return parse_named_let();
    }

    if (lexer_->consume().kind != TokenKind::LParen)
        return NULL_NODE; // ((

    struct Binding {
        SymId name;
        NodeId val;
    };
    std::vector<Binding> bs;

    while (lexer_->peek().kind != TokenKind::RParen) {
        if (lexer_->consume().kind != TokenKind::LParen)
            return NULL_NODE;
        auto n = lexer_->consume();
        if (n.kind != TokenKind::Identifier)
            return NULL_NODE;
        auto v = parse_expr();
        if (v == NULL_NODE)
            return NULL_NODE;
        bs.push_back({pool_.intern(std::string(n.text)), v});
        if (lexer_->consume().kind != TokenKind::RParen)
            return NULL_NODE;
    }
    lexer_->consume(); // ')'

    auto body = parse_expr();
    if (body == NULL_NODE)
        return NULL_NODE;
    // Collect additional body expressions until closing paren
    std::vector<NodeId> body_exprs = {body};
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto be = parse_expr();
        if (be == NULL_NODE)
            break;
        body_exprs.push_back(be);
    }
    if (lexer_->peek().kind == TokenKind::RParen)
        lexer_->consume();
    if (body_exprs.size() > 1)
        body = flat_.add_begin(body_exprs);

    // Multi-binding letrec: desugar to pre-allocated cells + set!
    // (letrec ((a v1) (b v2)) body) → (begin (define a 0) (define b 0) (set! a v1) (set! b v2)
    // body)
    if (rec && bs.size() > 1) {
        std::vector<NodeId> exprs;
        for (auto& b : bs)
            exprs.push_back(flat_.add_define(b.name, flat_.add_literal(0)));
        for (auto& b : bs)
            exprs.push_back(flat_.add_set(b.name, b.val));
        exprs.push_back(body);
        body = flat_.add_begin(exprs);
    } else {
        // Wrap bindings: innermost first (so outer wraps inner)
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
    auto name_tok = lexer_->peek(); // already peeked
    if (name_tok.kind != TokenKind::Identifier)
        return NULL_NODE;
    lexer_->consume(); // consume the name
    auto name = pool_.intern(std::string(name_tok.text));

    // Expect '(' for binding list
    if (lexer_->consume().kind != TokenKind::LParen)
        return NULL_NODE;

    struct Binding {
        SymId name;
        NodeId val;
    };
    std::vector<Binding> bs;

    while (lexer_->peek().kind != TokenKind::RParen) {
        if (lexer_->consume().kind != TokenKind::LParen)
            return NULL_NODE;
        auto n = lexer_->consume();
        if (n.kind != TokenKind::Identifier)
            return NULL_NODE;
        auto v = parse_expr();
        if (v == NULL_NODE)
            return NULL_NODE;
        bs.push_back({pool_.intern(std::string(n.text)), v});
        if (lexer_->consume().kind != TokenKind::RParen)
            return NULL_NODE;
    }
    lexer_->consume(); // ')'

    // Read all body expressions and wrap in begin if >1
    std::vector<NodeId> body_exprs;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto be = parse_expr();
        if (be == NULL_NODE)
            break;
        body_exprs.push_back(be);
    }
    lexer_->consume(); // ')'
    if (body_exprs.empty())
        return NULL_NODE;
    NodeId body = (body_exprs.size() == 1) ? body_exprs[0] : flat_.add_begin(body_exprs);

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
    if (lexer_->consume().kind != TokenKind::LParen)
        return NULL_NODE;

    struct Binding {
        SymId name;
        NodeId val;
    };
    std::vector<Binding> bs;

    while (lexer_->peek().kind != TokenKind::RParen) {
        if (lexer_->consume().kind != TokenKind::LParen)
            return NULL_NODE;
        auto n = lexer_->consume();
        if (n.kind != TokenKind::Identifier)
            return NULL_NODE;
        auto v = parse_expr();
        if (v == NULL_NODE)
            return NULL_NODE;
        bs.push_back({pool_.intern(std::string(n.text)), v});
        if (lexer_->consume().kind != TokenKind::RParen)
            return NULL_NODE;
    }
    lexer_->consume(); // ')'

    // Read all body expressions and wrap in begin if >1
    std::vector<NodeId> body_exprs;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto be = parse_expr();
        if (be == NULL_NODE)
            break;
        body_exprs.push_back(be);
    }
    lexer_->consume(); // ')'
    if (body_exprs.empty())
        return NULL_NODE;
    NodeId body = (body_exprs.size() == 1) ? body_exprs[0] : flat_.add_begin(body_exprs);

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
                    if (n == '"') {
                        unesc2 += '"';
                        i++;
                        has_e2 = true;
                    } else if (n == '\\') {
                        unesc2 += '\\';
                        i++;
                        has_e2 = true;
                    } else {
                        unesc2 += s2[i];
                    }
                } else {
                    unesc2 += s2[i];
                }
            }
            return flat_.add_literalstring(pool_.intern(has_e2 ? unesc2 : s2));
        }
        case TokenKind::Identifier:
            return flat_.add_variable(pool_.intern(std::string(lexer_->consume().text)));
        case TokenKind::LParen:
            lexer_->consume();
            return parse_list();
        case TokenKind::Quote: {
            lexer_->consume();
            auto quoted = parse_val();
            if (quoted == NULL_NODE)
                return NULL_NODE;
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
    auto tok = lexer_->consume(); // 'begin'
    std::vector<NodeId> exprs;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto e = parse_expr();
        if (e != NULL_NODE)
            exprs.push_back(e);
        else
            break;
    }
    lexer_->consume(); // ')'
    auto bid = flat_.add_begin(exprs);
    flat_.set_loc(bid, tok.line, tok.column);
    return bid;
}

NodeId FlatParser::parse_set() {
    auto tok = lexer_->consume(); // 'set!'
    auto n = lexer_->consume();
    if (n.kind != TokenKind::Identifier) {
        skip_rparen();
        return NULL_NODE;
    }
    auto v = parse_expr();
    if (v == NULL_NODE) {
        skip_rparen();
        return NULL_NODE;
    }
    lexer_->consume(); // ')'
    auto sid = flat_.add_set(pool_.intern(std::string(n.text)), v);
    flat_.set_loc(sid, tok.line, tok.column);
    return sid;
}

NodeId FlatParser::parse_quote() {
    auto tok = lexer_->consume(); // 'quote'
    auto v = parse_expr();
    if (v == NULL_NODE) {
        skip_rparen();
        return NULL_NODE;
    }
    lexer_->consume(); // ')'
    auto qid = flat_.add_quote(v);
    flat_.set_loc(qid, tok.line, tok.column);
    return qid;
}

NodeId FlatParser::parse_cond() {
    lexer_->consume(); // 'cond'
    struct Clause {
        NodeId test;
        NodeId val;
    };
    std::vector<Clause> clauses;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        if (lexer_->peek().kind != TokenKind::LParen)
            break;
        lexer_->consume(); // '('
        auto cn = parse_expr();
        if (cn == NULL_NODE) {
            skip_rparen();
            break;
        }
        auto v = parse_expr();
        if (v == NULL_NODE) {
            skip_rparen();
            break;
        }
        // R5RS: (cond (test expr1 expr2 ...)) — read ALL exprs, wrap in begin
        std::vector<NodeId> exprs;
        exprs.push_back(v);
        while (lexer_->peek().kind != TokenKind::RParen) {
            auto more = parse_expr();
            if (more == NULL_NODE)
                break;
            exprs.push_back(more);
        }
        lexer_->consume(); // ')'
        clauses.push_back(
            {cn, exprs.size() == 1 ? v : flat_.add_begin(exprs.data(), exprs.size())});
    }
    lexer_->consume(); // ')'
    if (clauses.empty())
        return NULL_NODE;
    auto result = clauses.back().val;
    for (auto it = clauses.rbegin() + 1; it != clauses.rend(); ++it)
        result = flat_.add_if(it->test, it->val, result);
    return result;
}

NodeId FlatParser::parse_defmacro() {
    auto tok = lexer_->consume(); // 'defmacro'
    if (lexer_->consume().kind != TokenKind::LParen) {
        skip_rparen();
        return NULL_NODE;
    }
    auto name = lexer_->consume();
    if (name.kind != TokenKind::Identifier) {
        skip_rparen();
        return NULL_NODE;
    }
    std::vector<SymId> params;
    bool dotted = false;
    while (lexer_->peek().kind != TokenKind::RParen) {
        // Check for dotted rest parameter: (name . rest)
        if (lexer_->peek().kind == TokenKind::Dot) {
            lexer_->consume(); // consume '.'
            if (lexer_->peek().kind != TokenKind::Identifier)
                break;
            auto rest = lexer_->consume();
            params.push_back(pool_.intern(std::string(rest.text)));
            dotted = true;
            break;
        }
        auto p = lexer_->consume();
        if (p.kind != TokenKind::Identifier) {
            skip_rparen();
            return NULL_NODE;
        }
        params.push_back(pool_.intern(std::string(p.text)));
    }
    lexer_->consume(); // ')'
    auto body = parse_expr();
    if (body == NULL_NODE) {
        skip_rparen();
        return NULL_NODE;
    }
    lexer_->consume(); // ')'
    auto mid = flat_.add_macrodef(pool_.intern(std::string(name.text)), params, body, dotted);
    flat_.set_loc(mid, tok.line, tok.column);
    return mid;
}

// ── Match / pattern matching ─────────────────────────────────
NodeId FlatParser::parse_match() {
    auto tok = lexer_->consume(); // 'match'

    // Parse subject expression
    auto subject = parse_expr();
    if (subject == NULL_NODE) {
        skip_rparen();
        return NULL_NODE;
    }

    // Temp variable to hold subject (evaluated once)
    auto tmp = pool_.intern("__match_tmp");

    // Parse clauses: (pattern body ...)
    struct Clause {
        NodeId pattern;
        NodeId test;
        NodeId body;
    };
    std::vector<Clause> clauses;

    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        if (lexer_->peek().kind != TokenKind::LParen)
            break;
        lexer_->consume(); // '('

        // Parse pattern (as an s-expression value)
        auto pattern = parse_val();
        if (pattern == NULL_NODE)
            break;

        // Parse body
        auto body = parse_expr();
        if (body == NULL_NODE)
            break;

        if (lexer_->peek().kind != TokenKind::RParen)
            break;
        lexer_->consume(); // ')'

        // Compile pattern into test and bindings, then wrap body in let
        NodeId test;
        auto bindings = compile_pattern(pattern, tmp, &test);

        // Wrap body in let bindings
        for (auto& [name, val] : bindings)
            body = flat_.add_let(name, val, body);

        clauses.push_back({pattern, test, body});
    }

    if (lexer_->peek().kind == TokenKind::RParen)
        lexer_->consume(); // ')'

    if (clauses.empty())
        return NULL_NODE;

    // ── Collect match clause metadata for exhaustiveness checking ──
    aura::ast::MatchClauseInfo minfo;
    for (auto& c : clauses) {
        auto pv = flat_.get(c.pattern);
        if (pv.tag == NodeTag::Variable) {
            auto pname = pool_.resolve(pv.sym_id);
            if (pname == "_" || (pname.size() > 1 && pname[0] == '_')) {
                minfo.has_wildcard = true;
            } else {
                // Bare-identifier pattern: ambiguous — could be a constructor
                // (e.g. `Nil`, `Red`) or a variable binding (e.g. `x`).
                // Record as a candidate; the type checker resolves against
                // the actual subject type at exhaustiveness time.
                minfo.candidate_constructors.push_back(pv.sym_id);
            }
        } else if (pv.tag == NodeTag::Call && !pv.children.empty()) {
            // Constructor pattern: (Ctor args...) -> callee is constructor name
            auto callee_id = pv.child(0);
            auto callee_v = flat_.get(callee_id);
            if (callee_v.tag == NodeTag::Variable)
                minfo.used_constructors.push_back(callee_v.sym_id);
        }
    }

    // Build nested if chain from right to left
    NodeId result = clauses.back().body;
    for (auto it = clauses.rbegin() + 1; it != clauses.rend(); ++it)
        result = flat_.add_if(it->test, it->body, result);

    // Wrap in (let ((__match_tmp subject)) result)
    result = flat_.add_let(tmp, subject, result);
    flat_.set_loc(result, tok.line, tok.column);

    // Store match metadata on the let node
    if (!minfo.used_constructors.empty() || !minfo.candidate_constructors.empty() ||
        minfo.has_wildcard)
        flat_.set_match_info(result, std::move(minfo));

    return result;
}

NodeId FlatParser::parse_linear() {
    auto tok = lexer_->consume(); // 'Linear'
    auto inner = parse_expr();
    if (inner == NULL_NODE) {
        skip_rparen();
        return NULL_NODE;
    }
    if (lexer_->peek().kind == TokenKind::RParen)
        lexer_->consume();
    auto id = flat_.add_linear(inner);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_move() {
    auto tok = lexer_->consume(); // 'move'
    auto inner = parse_expr();
    if (inner == NULL_NODE) {
        skip_rparen();
        return NULL_NODE;
    }
    if (lexer_->peek().kind == TokenKind::RParen)
        lexer_->consume();
    auto id = flat_.add_move(inner);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_borrow() {
    auto tok = lexer_->consume(); // 'borrow'
    auto inner = parse_expr();
    if (inner == NULL_NODE) {
        skip_rparen();
        return NULL_NODE;
    }
    if (lexer_->peek().kind == TokenKind::RParen)
        lexer_->consume();
    auto id = flat_.add_borrow(inner);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_mut_borrow() {
    auto tok = lexer_->consume(); // 'mut-borrow'
    auto inner = parse_expr();
    if (inner == NULL_NODE) {
        skip_rparen();
        return NULL_NODE;
    }
    if (lexer_->peek().kind == TokenKind::RParen)
        lexer_->consume();
    auto id = flat_.add_mut_borrow(inner);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_drop() {
    auto tok = lexer_->consume(); // 'drop'
    auto inner = parse_expr();
    if (inner == NULL_NODE) {
        skip_rparen();
        return NULL_NODE;
    }
    if (lexer_->peek().kind == TokenKind::RParen)
        lexer_->consume();
    auto id = flat_.add_drop(inner);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_cast() {
    // Syntax: (cast expr : TypeName) or (cast expr TypeName)
    // Creates Coercion node: child[0]=expr, int_val=type_tag
    auto tok = lexer_->consume(); // 'cast'
    auto expr = parse_expr();
    if (expr == NULL_NODE) {
        skip_rparen();
        return NULL_NODE;
    }

    // Parse optional : then type name
    if (lexer_->peek().kind == TokenKind::Identifier && lexer_->peek().text == ":") {
        lexer_->consume(); // :
    }

    auto type_tok = lexer_->peek();
    auto type_name = type_tok.text;
    std::uint32_t type_tag = 3; // default: Dynamic
    if (type_name == "Int")
        type_tag = 0;
    else if (type_name == "String")
        type_tag = 1;
    else if (type_name == "Bool")
        type_tag = 2;
    else if (type_name == "Any")
        type_tag = 3;
    lexer_->consume(); // TypeName

    if (lexer_->peek().kind == TokenKind::RParen)
        lexer_->consume();

    auto id = flat_.add_coercion(expr, type_tag, 0);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_check() {
    // Syntax: (check expr : TypeName)
    // Creates TypeAnnotation node: child[0]=expr, sym_id=TypeName
    // If not followed by ': TypeName', treat as regular function call
    auto tok = lexer_->consume(); // 'check'
    auto expr = parse_expr();
    if (expr == NULL_NODE) {
        skip_rparen();
        return NULL_NODE;
    }

    // Verify this is truly (check ... : Type), not (check x) as function call
    if (lexer_->peek().kind == TokenKind::Identifier && lexer_->peek().text == ":") {
        lexer_->consume(); // ':'
        auto type_tok = lexer_->peek();
        if (type_tok.kind == TokenKind::Identifier) {
            // Consume the type name
            auto type_sym = pool_.intern(type_tok.text);
            lexer_->consume(); // TypeName
            if (lexer_->peek().kind == TokenKind::RParen)
                lexer_->consume();
            auto id = flat_.add_type_annotation(type_sym, expr);
            flat_.set_loc(id, tok.line, tok.column);
            return id;
        }
        // Token after : is not an identifier — treat check as function call
    }

    // Not a valid type annotation — treat (check ...) as regular function call
    // Build a Call node with 'check' as the operator
    auto check_sym = pool_.intern("check");
    auto func = flat_.add_variable(check_sym);
    std::vector<NodeId> args;
    args.push_back(expr);
    // Collect any remaining args until )
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto arg = parse_expr();
        if (arg == NULL_NODE)
            break;
        args.push_back(arg);
    }
    if (lexer_->peek().kind == TokenKind::RParen)
        lexer_->consume();
    auto id = flat_.add_call(func, args);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId FlatParser::parse_type_annot() {
    // Syntax:
    //   (: name TypeName)       — annotate variable with type (no-op at runtime)
    //   (: name Type val)        — bind, annotate, and return val
    auto tok = lexer_->consume(); // :

    auto name_tok = lexer_->peek();
    if (name_tok.kind != TokenKind::Identifier) {
        skip_rparen();
        return NULL_NODE;
    }
    auto var_sym = pool_.intern(name_tok.text);
    lexer_->consume(); // name

    auto type_tok = lexer_->peek();
    // Compound type expression: (: name (List :T)) or (: name (Maybe :T))
    if (type_tok.kind != TokenKind::Identifier) {
        skip_rparen();
        return NULL_NODE;
    }
    auto type_sym = pool_.intern(type_tok.text);
    lexer_->consume(); // TypeName

    // Check for 3-arg form: (: name Type value-expr)
    if (lexer_->peek().kind != TokenKind::RParen) {
        // Consume value and return it with type annotation wrapping
        auto val = parse_expr();
        if (lexer_->peek().kind == TokenKind::RParen)
            lexer_->consume();
        if (val == NULL_NODE)
            return NULL_NODE;
        // Store var_sym in int_val_ so eval_flat can bind the variable.
        // The type checker will see the TypeAnnotation as the root and report the
        // correct type (Int, String, etc.).
        auto id = flat_.add_type_annotation(type_sym, val, var_sym);
        flat_.set_loc(id, tok.line, tok.column);
        return id;
    }

    lexer_->consume(); // RParen

    auto var_node = flat_.add_variable(var_sym);
    auto id = flat_.add_type_annotation(type_sym, var_node);
    flat_.set_loc(id, tok.line, tok.column);
    return id;
}

std::vector<std::pair<SymId, NodeId>> FlatParser::compile_pattern(NodeId pattern_node, SymId tmp,
                                                                  NodeId* out_test) {
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
    if (v.tag == NodeTag::LiteralInt || v.tag == NodeTag::LiteralFloat ||
        v.tag == NodeTag::LiteralString) {
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
        if (v.children.empty()) {
            *out_test = flat_.add_literal(0);
            return bindings;
        }

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
                    accumulated_test =
                        flat_.add_if(accumulated_test, pair_test, flat_.add_literal(0));

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
                        accumulated_test =
                            flat_.add_if(accumulated_test, eq_test, flat_.add_literal(0));
                    } else if (elem_v.tag == NodeTag::LiteralInt ||
                               elem_v.tag == NodeTag::LiteralFloat ||
                               elem_v.tag == NodeTag::LiteralString) {
                        // Literal element match
                        auto eq_test = make_call(sym_equal_q, {car_expr, elem});
                        accumulated_test =
                            flat_.add_if(accumulated_test, eq_test, flat_.add_literal(0));
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

            // Constructor pattern: (CtorName sub-patterns...)
            // Matches tagged lists: (cons 'tag (cons field1 (cons field2 ...)))
            // Tests: (pair? tmp) && (equal? (car tmp) 'CtorName)
            // Then sub-patterns match against (car (cdr tmp)), (car (cdr (cdr tmp))), ...
            {
                auto ctor_name = pool_.resolve(callee_v.sym_id);
                // Ignore list/cons/pair/quote — handled above
                if (ctor_name != "list" && ctor_name != "cons" && ctor_name != "pair") {
                    // Zero-arg constructor: (CtorName) with no sub-patterns
                    // The value IS the constructor function (a closure), not a pair.
                    // Compare directly using equal?.
                    if (v.children.size() == 1) {
                        // Zero-arg constructor: compare subject against a built pair.
                        // Build (cons "CtorName" ()) which is how zero-arg constructors are stored.
                        auto ctor_str_node = flat_.add_literalstring(pool_.intern(ctor_name));
                        auto null_lit = flat_.add_literal(0);
                        auto ctor_val = make_call(pool_.intern("cons"), {ctor_str_node, null_lit});
                        *out_test = make_call(sym_equal_q, {var_tmp, ctor_val});
                        return bindings;
                    }
                    // Test 1: (pair? tmp) — value must be a pair
                    NodeId accumulated_test = make_call(sym_pair_q, {var_tmp});

                    // Test 2: (equal? (car tmp) "CtorName") — tag match as string
                    // The constructor stores the tag as a string in the pair heap
                    auto ctor_str = flat_.add_literalstring(pool_.intern(ctor_name));
                    auto tag_test = make_call(pool_.intern("string=?"),
                                              {make_call(sym_car, {var_tmp}), ctor_str});
                    accumulated_test =
                        flat_.add_if(accumulated_test, tag_test, flat_.add_literal(0));

                    // Walk sub-patterns starting from (cdr tmp)
                    auto current = make_call(sym_cdr, {var_tmp});
                    for (std::size_t i = 1; i < v.children.size(); ++i) {
                        auto elem = v.child(i);
                        auto elem_v = flat_.get(elem);

                        // (pair? current) — ensure list structure
                        auto pair_check = make_call(sym_pair_q, {current});
                        accumulated_test =
                            flat_.add_if(accumulated_test, pair_check, flat_.add_literal(0));

                        // Null-guarded field extraction for type checker safety
                        auto null_check = make_call(sym_null_q, {current});
                        auto zero_lit = flat_.add_literal(0);
                        auto car_expr = make_call(sym_car, {current});
                        auto elem_car = flat_.add_if(null_check, zero_lit, car_expr);

                        if (elem_v.tag == NodeTag::Variable) {
                            auto elem_name = pool_.resolve(elem_v.sym_id);
                            if (elem_name != "_" &&
                                !(elem_name.size() > 1 && elem_name[0] == '_')) {
                                bindings.emplace_back(elem_v.sym_id, elem_car);
                            }
                        } else if (elem_v.tag == NodeTag::LiteralInt ||
                                   elem_v.tag == NodeTag::LiteralFloat ||
                                   elem_v.tag == NodeTag::LiteralString) {
                            auto eq_test = make_call(sym_equal_q, {elem_car, elem});
                            accumulated_test =
                                flat_.add_if(accumulated_test, eq_test, flat_.add_literal(0));
                        }

                        current = make_call(sym_cdr, {current});
                    }

                    // Final: (null? current) — proper length
                    auto null_test = make_call(sym_null_q, {current});
                    accumulated_test =
                        flat_.add_if(accumulated_test, null_test, flat_.add_literal(0));

                    *out_test = accumulated_test;
                    return bindings;
                }
            }
        }
    }

    // Default fallback: exact equality match
    *out_test = make_call(sym_equal_q, {var_tmp, pattern_node});
    return bindings;
}

// ── Quasiquote expansion ───────────────────────────────────────

// Check if a node is (unquote ...)
static bool is_unquote(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                       NodeId id) {
    if (id == NULL_NODE)
        return false;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Call || v.children.empty())
        return false;
    auto callee = flat.get(v.child(0));
    return callee.tag == NodeTag::Variable && std::string(pool.resolve(callee.sym_id)) == "unquote";
}

// Check if a node is (unquote-splicing ...)
static bool is_unquote_splicing(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                                NodeId id) {
    if (id == NULL_NODE)
        return false;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Call || v.children.empty())
        return false;
    auto callee = flat.get(v.child(0));
    return callee.tag == NodeTag::Variable &&
           std::string(pool.resolve(callee.sym_id)) == "unquote-splicing";
}

// Check if a node is (quasiquote ...)
static bool is_quasiquote(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                          NodeId id) {
    if (id == NULL_NODE)
        return false;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Call || v.children.empty())
        return false;
    auto callee = flat.get(v.child(0));
    return callee.tag == NodeTag::Variable &&
           std::string(pool.resolve(callee.sym_id)) == "quasiquote";
}

NodeId FlatParser::expand_qq(NodeId expr, int depth) {
    // Prevent stack overflow from exceessive quasiquote nesting
    if (depth > 4)
        return NULL_NODE;
    if (expr == NULL_NODE) {
        // Empty quasiquote: (quote ())
        return flat_.add_quote(flat_.add_literal(0));
    }

    auto v = flat_.get(expr);

    // Non-Call compound nodes: special forms parsed by keyword (IfExpr, Lambda, Let, etc.)
    // The keyword (if, lambda, etc.) is LOST by parse_list — we need to reconstruct it
    if (v.tag != NodeTag::Call) {
        // Variables and literals: (quote expr)
        if (v.tag == NodeTag::Variable || v.tag == NodeTag::LiteralInt ||
            v.tag == NodeTag::LiteralFloat || v.tag == NodeTag::LiteralString ||
            v.tag == NodeTag::Quote) {
            return flat_.add_quote(expr);
        }
        // Other compound nodes (IfExpr, Begin, Lambda, etc.) with children:
        // Build a list that starts with (quote <form-name>) followed by child expansions
        if (v.children.size() > 0) {
            // Determine the form name based on node tag
            std::string form_name;
            switch (v.tag) {
                case NodeTag::IfExpr:
                    form_name = "if";
                    break;
                case NodeTag::Lambda:
                    form_name = "lambda";
                    break;
                case NodeTag::Let:
                    form_name = "let";
                    break;
                case NodeTag::LetRec:
                    form_name = "letrec";
                    break;
                case NodeTag::Define:
                    form_name = "define";
                    break;
                case NodeTag::Begin:
                    form_name = "begin";
                    break;
                case NodeTag::Set:
                    form_name = "set!";
                    break;
                default:
                    return flat_.add_quote(expr);
            }
            // Build: (cons (quote <form-name>) (cons arg0 (cons arg1 ... (quote ()))))
            // where args are: params (if Lambda) + children + extra elements
            std::vector<NodeId> args_to_expand;

            // For Lambda, add quoted parameter list as first arg
            if (v.tag == NodeTag::Lambda) {
                NodeId params_list = flat_.add_quote(flat_.add_literal(0)); // (quote ())
                for (int pi = static_cast<int>(v.params.size()) - 1; pi >= 0; --pi) {
                    auto param_var =
                        flat_.add_variable(pool_.intern(std::string(pool_.resolve(v.params[pi]))));
                    auto param_quoted = flat_.add_quote(param_var);
                    auto cv = flat_.add_variable(pool_.intern("cons"));
                    params_list = flat_.add_call(
                        cv, std::vector<aura::ast::NodeId>{param_quoted, params_list});
                }
                args_to_expand.push_back(params_list);
            }

            bool has_fn_list = false; // for define shorthand
            // For Define shorthand (define+lambda child), replace with (fn params) list
            if (v.tag == NodeTag::Define && v.children.size() == 1 &&
                v.sym_id != ast::INVALID_SYM) {
                auto child_v = flat_.get(v.child(0));
                if (child_v.tag == NodeTag::Lambda) {
                    // Build (fn params) list
                    auto fn_var =
                        flat_.add_variable(pool_.intern(std::string(pool_.resolve(v.sym_id))));
                    auto fn_quoted = flat_.add_quote(fn_var);
                    NodeId fn_params_list = flat_.add_quote(flat_.add_literal(0));
                    for (int pi = static_cast<int>(child_v.params.size()) - 1; pi >= 0; --pi) {
                        auto pvar = flat_.add_variable(
                            pool_.intern(std::string(pool_.resolve(child_v.params[pi]))));
                        auto pquoted = flat_.add_quote(pvar);
                        auto cv = flat_.add_variable(pool_.intern("cons"));
                        fn_params_list = flat_.add_call(
                            cv, std::vector<aura::ast::NodeId>{pquoted, fn_params_list});
                    }
                    auto cv = flat_.add_variable(pool_.intern("cons"));
                    args_to_expand.push_back(flat_.add_call(
                        cv, std::vector<aura::ast::NodeId>{fn_quoted, fn_params_list}));
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
                result = flat_.add_call(cons_var,
                                        std::vector<aura::ast::NodeId>{args_to_expand[i], result});
            }

            // Prepend (quote <form-name>)
            auto form_var = flat_.add_variable(pool_.intern(form_name));
            auto form_quote = flat_.add_quote(form_var);
            auto cons_var2 = flat_.add_variable(pool_.intern("cons"));
            result = flat_.add_call(cons_var2, std::vector<aura::ast::NodeId>{form_quote, result});
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
        if (v.children.size() > 1)
            return v.child(1);
        return expr;
    }

    // Handle unquote at depth > 0: (quote (unquote ...))
    if (depth > 0 && is_unquote(flat_, pool_, expr)) {
        if (v.children.size() > 1) {
            auto inner = expand_qq(v.child(1), depth - 1);
            auto unq_var = flat_.add_variable(pool_.intern("unquote"));
            return flat_.add_quote(flat_.add_call(unq_var, std::vector<aura::ast::NodeId>{inner}));
        }
        return flat_.add_quote(expr);
    }

    // Handle unquote-splicing at depth 0: return the inner expression
    if (depth == 0 && is_unquote_splicing(flat_, pool_, expr)) {
        if (v.children.size() > 1)
            return v.child(1);
        return expr;
    }

    // Handle unquote-splicing at depth > 0: (quote (unquote-splicing ...))
    if (depth > 0 && is_unquote_splicing(flat_, pool_, expr)) {
        if (v.children.size() > 1) {
            auto inner = expand_qq(v.child(1), depth - 1);
            auto unsplice_var = flat_.add_variable(pool_.intern("unquote-splicing"));
            return flat_.add_quote(
                flat_.add_call(unsplice_var, std::vector<aura::ast::NodeId>{inner}));
        }
        return flat_.add_quote(expr);
    }

    // Handle nested quasiquote
    if (is_quasiquote(flat_, pool_, expr)) {
        if (v.children.size() > 1) {
            auto inner = expand_qq(v.child(1), depth + 1);
            auto qq_var = flat_.add_variable(pool_.intern("quasiquote"));
            return flat_.add_call(qq_var, std::vector<aura::ast::NodeId>{inner});
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
            result = flat_.add_call(append_var, std::vector<aura::ast::NodeId>{spliced, result});
        } else {
            auto expanded = expand_qq(child, depth);
            auto cons_var = flat_.add_variable(pool_.intern("cons"));
            result = flat_.add_call(cons_var, std::vector<aura::ast::NodeId>{expanded, result});
        }
    }

    return result;
}

// ── Free function ──────────────────────────────────────────────
FlatParseResult parse_to_flat(std::string_view source, FlatAST& flat, StringPool& pool) {
    FlatParser fp(flat, pool);
    return fp.parse(source);
}

} // namespace aura::parser
