module;

module aura.parser.parser;
import std;

namespace aura::parser::detail {

using namespace aura::ast;

FlatParseResult parse(ParserState& s, std::string_view src) {
    FlatParseResult r;
    try {
        s.lex = Lexer(src);

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
        auto record_error = [&](const std::string& msg,
                                std::optional<Token> err_tok = std::nullopt) {
            auto loc = err_tok ? aura::diag::SourceLocation{err_tok->line, err_tok->column, 0}
                               : aura::diag::SourceLocation{};
            auto formatted = loc.valid() ? std::format("{}: {}", loc.format(), msg) : msg;
            if (r.error.empty())
                r.error = formatted;
            r.errors.push_back({msg, loc});
            // Skip tokens until we can try parsing again
            int depth = 0;
            while (!s.lex.eof()) {
                auto tok = s.lex.peek();
                if (depth == 0) {
                    // At top level, try to find the start of a new expression
                    if (tok.kind == TokenKind::LParen || tok.kind == TokenKind::Integer ||
                        tok.kind == TokenKind::Float || tok.kind == TokenKind::String ||
                        tok.kind == TokenKind::Identifier || tok.kind == TokenKind::Bool ||
                        tok.kind == TokenKind::Quote || tok.kind == TokenKind::QuasiQuote ||
                        tok.kind == TokenKind::Unquote) {
                        break;
                    }
                    s.lex.consume();
                } else {
                    // Inside parens, skip until matching close
                    if (tok.kind == TokenKind::RParen) {
                        depth--;
                    } else if (tok.kind == TokenKind::LParen) {
                        depth++;
                    }
                    s.lex.consume();
                }
            }
        };

        r.root = parse_expr(s);
        if (r.root == NULL_NODE) {
            auto tok = s.lex.peek();
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
        auto next = s.lex.peek();
        if (next.kind == TokenKind::EndOfFile || next.kind == TokenKind::Error) {
            r.success = r.root != NULL_NODE;
            return r;
        }

        // Multiple forms → wrap in begin
        std::vector<NodeId> exprs;
        exprs.push_back(r.root);
        do {
            auto e = parse_expr(s);
            if (e == NULL_NODE) {
                auto tok = s.lex.peek();
                if (tok.kind != TokenKind::EndOfFile) {
                    record_error("expected expression, got " + token_desc(tok), tok);
                    e = parse_expr(s); // try again after skip
                }
                if (s.lex.eof())
                    break;
            }
            if (e != NULL_NODE)
                exprs.push_back(e);
            if (s.lex.eof())
                break;
            next = s.lex.peek();
        } while (next.kind != TokenKind::EndOfFile);

        r.root = s.flat.add_begin(exprs);
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

NodeId parse_expr(ParserState& s) {
    // Recursion depth guard: prevent stack overflow on deeply nested expressions
    static constexpr std::size_t MAX_PARSE_DEPTH = 500;
    if (++s.depth > MAX_PARSE_DEPTH) {
        --s.depth;
        return NULL_NODE;
    }
    // RAII depth decrement on any exit path
    struct DepthGuard {
        std::size_t& d;
        ~DepthGuard() { --d; }
    } _dg{s.depth};
    auto tok = s.lex.peek();
    switch (tok.kind) {
        case TokenKind::Integer:
            return parse_number(s, s.lex.consume());
        case TokenKind::Bool: {
            auto tok = s.lex.consume();
            auto v = std::stoll(std::string(tok.text));
            auto id = s.flat.add_literal(v);
            s.flat.set_marker(id, aura::ast::SyntaxMarker::BoolLiteral);
            s.flat.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::Float:
            return parse_float(s, s.lex.consume());
        case TokenKind::String: {
            auto tok = s.lex.consume();
            auto text = std::string(tok.text);
            // Process \" → " and \\ → \ in string literals
            std::string unescaped;
            bool has_esc = false;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    auto n = text[i + 1];
                    if (n == '"') {
                        unescaped += '"';
                        i++;
                        has_esc = true;
                    } else if (n == '\\') {
                        unescaped += '\\';
                        i++;
                        has_esc = true;
                    } else {
                        unescaped += text[i];
                    }
                } else {
                    unescaped += text[i];
                }
            }
            auto id = s.flat.add_literalstring(s.pool.intern(has_esc ? unescaped : text));
            s.flat.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::Ellipsis: {
            s.lex.consume();
            return s.flat.add_variable(s.pool.intern("..."));
        }
        case TokenKind::Identifier: {
            auto tok = s.lex.consume();
            auto text = std::string(tok.text);
            // M4 &x reader macro: &x → (borrow x)
            if (text.size() > 1 && text[0] == '&') {
                auto var_name = text.substr(1);
                auto inner = s.flat.add_variable(s.pool.intern(var_name));
                auto id = s.flat.add_borrow(inner);
                s.flat.set_loc(id, tok.line, tok.column);
                return id;
            }
            // M4 &mut-x reader macro: &mut-x → (mut-borrow x)
            if (text.size() > 5 && text.substr(0, 5) == "&mut-") {
                auto var_name = text.substr(5);
                auto inner = s.flat.add_variable(s.pool.intern(var_name));
                auto id = s.flat.add_mut_borrow(inner);
                s.flat.set_loc(id, tok.line, tok.column);
                return id;
            }
            auto id = s.flat.add_variable(s.pool.intern(text));
            s.flat.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::Quote: {
            s.lex.consume(); // consume '
            auto quoted = parse_expr(s);
            if (quoted == NULL_NODE)
                return NULL_NODE;
            auto id = s.flat.add_quote(quoted);
            s.flat.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::HashLParen: {
            // #(1 2 3) → (vector 1 2 3)
            s.lex.consume(); // consume #(
            auto vec_fun = s.flat.add_variable(s.pool.intern("vector"));
            std::vector<aura::ast::NodeId> args;
            while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
                auto arg = parse_expr(s);
                if (arg == NULL_NODE)
                    break;
                args.push_back(arg);
            }
            if (s.lex.peek().kind == TokenKind::RParen)
                s.lex.consume(); // consume )
            auto id = s.flat.add_call(vec_fun, args);
            s.flat.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::QuasiQuote: {
            s.lex.consume(); // consume `
            auto quoted = parse_expr(s);
            if (quoted == NULL_NODE)
                return NULL_NODE;
            auto id = expand_qq(s, quoted, 0);
            s.flat.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::Unquote: {
            s.lex.consume(); // consume ,
            auto inner = parse_expr(s);
            if (inner == NULL_NODE)
                return NULL_NODE;
            // Represent (unquote inner) as a Call to variable 'unquote'
            auto unquote_var = s.flat.add_variable(s.pool.intern("unquote"));
            auto id = s.flat.add_call(unquote_var, std::vector<aura::ast::NodeId>{inner});
            s.flat.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::UnquoteSplicing: {
            s.lex.consume(); // consume ,@
            auto inner = parse_expr(s);
            if (inner == NULL_NODE)
                return NULL_NODE;
            // Represent (unquote-splicing inner) as a Call to variable 'unquote-splicing'
            auto unsplice_var = s.flat.add_variable(s.pool.intern("unquote-splicing"));
            auto id = s.flat.add_call(unsplice_var, std::vector<aura::ast::NodeId>{inner});
            s.flat.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::LParen:
            s.lex.consume();
            return parse_list(s);
        default:
            return NULL_NODE;
    }
}

NodeId parse_number(ParserState& s, Token tok) {
    try {
        auto v = std::stoll(std::string(tok.text));
        return s.flat.add_literal(v);
    } catch (...) {
        return NULL_NODE;
    }
}

NodeId parse_float(ParserState& s, Token tok) {
    try {
        auto v = std::stod(std::string(tok.text));
        return s.flat.add_literal_float(v);
    } catch (...) {
        return NULL_NODE;
    }
}

NodeId parse_list(ParserState& s) {
    auto tok = s.lex.peek(); // '(' or first token
    // () → null sentinel (0)
    if (s.lex.peek().kind == TokenKind::RParen) {
        s.lex.consume();
        auto id = s.flat.add_literal(0);
        s.flat.set_loc(id, tok.line, tok.column);
        return id;
    }
    auto f = s.lex.peek();
    if (f.kind == TokenKind::Identifier) {
        auto kw = f.text;
        if (kw == "if")
            return parse_if(s);
        if (kw == "lambda")
            return parse_lambda(s);
        if (kw == "let")
            return parse_let(s, false);
        if (kw == "let*")
            return parse_let_star(s);
        if (kw == "letrec")
            return parse_let(s, true);
        if (kw == "define")
            return parse_define(s);
        if (kw == "begin")
            return parse_begin(s);
        if (kw == "set!")
            return parse_set(s);
        if (kw == "quote")
            return parse_quote(s);
        if (kw == "cond")
            return parse_cond(s);
        if (kw == "defmacro")
            return parse_defmacro(s);
        if (kw == "define-hygienic-macro")
            return parse_defmacro(s, /*hygienic=*/true);
        // Issue #230 #2: the `*` suffix variant. Behaves like
        // define-hygienic-macro (gensyms all macro-introduced locals)
        // BUT params prefixed with `.` are kept as their literal
        // names instead of being gensym'd. Use this for
        // symbol-generating macros like define-struct, where the
        // macro body needs the user's actual struct name to build
        // (make-struct field1 field2 ...) at expansion time.
        if (kw == "define-hygienic-macro*")
            return parse_defmacro(s, /*hygienic=*/true,
                                  /*preserve_dotted=*/true);
        if (kw == "match")
            return parse_match(s);
        if (kw == "cast")
            return parse_cast(s);
        if (kw == "check")
            return parse_check(s);
        if (kw == ":")
            return parse_type_annot(s);
        if (kw == "define-type")
            return parse_define_type(s);
        if (kw == "define-module")
            return parse_define_module(s);
        if (kw == "Linear")
            return parse_linear(s);
        if (kw == "move")
            return parse_move(s);
        if (kw == "borrow")
            return parse_borrow(s);
        if (kw == "mut-borrow")
            return parse_mut_borrow(s);
        if (kw == "datatype")
            return parse_datatype(s);
        // Note: 'drop' is intentionally NOT a parser special form.
        // It's a C++ primitive (list drop) defined in evaluator_primitives_list.cpp.
        // The linear-type drop is accessible via a different name if needed.
        if (kw == "export") {
            s.lex.consume(); // consume 'export'
            std::vector<aura::ast::NodeId> syms;
            while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
                auto sym = parse_expr(s);
                if (sym != aura::ast::NULL_NODE)
                    syms.push_back(sym);
                else
                    break;
            }
            if (s.lex.peek().kind == TokenKind::RParen)
                s.lex.consume();
            auto id = s.flat.add_export(syms);
            s.flat.set_loc(id, tok.line, tok.column);
            return id;
        }
    }

    auto func = parse_expr(s);
    if (func == NULL_NODE) {
        skip_rparen(s);
        return NULL_NODE;
    }

    std::vector<NodeId> args;
    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
        // Dotted pair: (a . b) or (a b ... . c)
        if (s.lex.peek().kind == TokenKind::Dot) {
            s.lex.consume(); // consume '.'
            auto cdr = parse_expr(s);
            if (cdr == NULL_NODE) {
                skip_rparen(s);
                return NULL_NODE;
            }
            if (s.lex.peek().kind != TokenKind::RParen) {
                skip_rparen(s);
                return NULL_NODE;
            }
            s.lex.consume(); // ')'

            // Build cons chain: cons(func, cons(arg1, ..., cons(argN, cdr)))
            NodeId tail = cdr;
            for (auto it = args.rbegin(); it != args.rend(); ++it)
                tail = s.flat.add_pair(*it, tail);
            tail = s.flat.add_pair(func, tail);
            s.flat.set_loc(tail, tok.line, tok.column);
            return tail;
        }
        auto a = parse_expr(s);
        if (a != NULL_NODE)
            args.push_back(a);
        else
            break;
    }
    s.lex.consume(); // ')'
    auto id = s.flat.add_call(func, args);
    s.flat.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId parse_if(ParserState& s) {
    auto tok = s.lex.consume(); // 'if'
    auto c = parse_expr(s);
    auto t = parse_expr(s);
    auto e = parse_expr(s);
    // Check if the next token is the closing paren; if not, there are extra
    // arguments that belong to the else branch. Wrap them in begin.
    if (s.lex.peek().kind == TokenKind::RParen) {
        s.lex.consume(); // ')'
    } else {
        std::vector<NodeId> extra = {e};
        while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
            auto x = parse_expr(s);
            if (x != aura::ast::NULL_NODE)
                extra.push_back(x);
        }
        s.lex.consume(); // ')'
        e = (extra.size() == 1) ? extra[0] : s.flat.add_begin(extra);
    }
    auto id = s.flat.add_if(c, t, e);
    s.flat.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId parse_lambda(ParserState& s) {
    auto tok = s.lex.consume(); // 'lambda'
    if (s.lex.consume().kind != TokenKind::LParen)
        return NULL_NODE;

    std::vector<SymId> params;
    std::vector<NodeId> annots; // parallel annotation node IDs (NULL_NODE = none)
    bool dotted = false;
    while (s.lex.peek().kind != TokenKind::RParen) {
        // Check for dotted rest parameter: (lambda (x . rest) body)
        if (s.lex.peek().kind == TokenKind::Dot) {
            s.lex.consume(); // consume '.'
            if (s.lex.peek().kind == TokenKind::RParen) {
                dotted = true;
                break;
            } // (lambda rest .) = all args
            auto rest = s.lex.consume();
            if (rest.kind != TokenKind::Identifier)
                return NULL_NODE;
            params.push_back(s.pool.intern(std::string(rest.text)));
            annots.push_back(NULL_NODE);
            dotted = true;
            break;
        }
        // Check for type-annotated parameter: (lambda ((: x Type)) body)
        if (s.lex.peek().kind == TokenKind::LParen) {
            // Type-annotated parameter: ((: x Int)) or ((: x Int val))
            // The outer '(' before ':' is consumed here, parse_type_annot handles the rest
            s.lex.consume();
            auto annot_node = parse_type_annot(s);
            if (annot_node == NULL_NODE)
                return NULL_NODE;
            // TypeAnnotation node: child[0] = Variable with the parameter name
            auto annot_tag = s.flat.get(annot_node);
            if (annot_tag.children.empty())
                return NULL_NODE;
            auto var_node = s.flat.get(annot_tag.child(0));
            params.push_back(var_node.sym_id);
            annots.push_back(annot_node);
        } else {
            auto t = s.lex.consume();
            if (t.kind != TokenKind::Identifier)
                return NULL_NODE;
            // Issue #102: shorthand form (x :?) or (x _) for
            // type-hole param. Consume the param name, then look
            // ahead: if the next token is `:?` or `_` (which the
            // lexer reads as a single Identifier token), build a
            // TypeAnnotation wrapping a Variable node for x, with
            // the type-hole name as the sym_id. Falls back to the
            // plain-param path if the next token is anything else
            // (regular `x` followed by `y` → two params).
            auto next = s.lex.peek();
            if (next.kind == TokenKind::Identifier && (next.text == ":?" || next.text == "_")) {
                auto hole_text = std::string(next.text);
                s.lex.consume(); // :? or _
                auto var_node = s.flat.add_variable(s.pool.intern(std::string(t.text)));
                auto hole_sym = s.pool.intern(hole_text);
                auto annot_node = s.flat.add_type_annotation(hole_sym, var_node);
                s.flat.set_loc(annot_node, tok.line, tok.column);
                params.push_back(s.pool.intern(std::string(t.text)));
                annots.push_back(annot_node);
            } else {
                params.push_back(s.pool.intern(std::string(t.text)));
                annots.push_back(NULL_NODE);
            }
        }
    }
    s.lex.consume(); // ')'

    // Parse multiple body expressions and wrap in begin
    std::vector<NodeId> body_exprs;
    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
        auto be = parse_expr(s);
        if (be != NULL_NODE)
            body_exprs.push_back(be);
        if (s.lex.peek().kind == TokenKind::RParen)
            break;
    }
    auto body = body_exprs.empty() ? NULL_NODE
                                   : (body_exprs.size() == 1
                                          ? body_exprs[0]
                                          : s.flat.add_begin(body_exprs.data(), body_exprs.size()));
    if (body == NULL_NODE)
        return NULL_NODE;
    s.lex.consume(); // ')'
    auto lid = s.flat.add_lambda(params, annots, body, dotted);
    s.flat.set_loc(lid, tok.line, tok.column);
    return lid;
}

NodeId parse_define(ParserState& s) {
    s.lex.consume(); // 'define'
    auto n = s.lex.peek();
    if (n.kind == TokenKind::LParen) {
        // Shorthand: (define (fn params...) body...)
        s.lex.consume(); // '('
        auto fn = s.lex.consume();
        if (fn.kind != TokenKind::Identifier) {
            skip_rparen(s);
            return NULL_NODE;
        }
        std::vector<SymId> params;
        std::vector<NodeId> annots;
        bool dotted = false;
        while (s.lex.peek().kind != TokenKind::RParen) {
            // Check for dotted rest parameter: (define (f . rest) body)
            if (s.lex.peek().kind == TokenKind::Dot) {
                s.lex.consume(); // consume '.'
                if (s.lex.peek().kind == TokenKind::RParen) {
                    dotted = true;
                    break;
                }
                auto rest = s.lex.consume();
                if (rest.kind != TokenKind::Identifier) {
                    skip_rparen(s);
                    return NULL_NODE;
                }
                params.push_back(s.pool.intern(std::string(rest.text)));
                annots.push_back(NULL_NODE);
                dotted = true;
                break;
            }
            // Check for type-annotated parameter: (define (f (x : Int)) ...)
            if (s.lex.peek().kind == TokenKind::LParen) {
                s.lex.consume();
                auto annot_node = parse_type_annot(s);
                if (annot_node == NULL_NODE)
                    return NULL_NODE;
                auto annot_tag = s.flat.get(annot_node);
                if (annot_tag.children.empty())
                    return NULL_NODE;
                auto var_node = s.flat.get(annot_tag.child(0));
                params.push_back(var_node.sym_id);
                annots.push_back(annot_node);
            } else {
                auto p = s.lex.consume();
                if (p.kind != TokenKind::Identifier) {
                    skip_rparen(s);
                    return NULL_NODE;
                }
                params.push_back(s.pool.intern(std::string(p.text)));
                annots.push_back(NULL_NODE);
            }
        }
        s.lex.consume(); // ')' after params
        // Parse multiple body expressions and wrap in begin
        std::vector<NodeId> body_exprs;
        while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
            auto be = parse_expr(s);
            if (be == NULL_NODE)
                break;
            body_exprs.push_back(be);
        }
        s.lex.consume(); // ')' closing define
        if (body_exprs.empty())
            return NULL_NODE;
        NodeId body = (body_exprs.size() == 1) ? body_exprs[0] : s.flat.add_begin(body_exprs);
        auto lambda = s.flat.add_lambda(params, annots, body, dotted);
        return s.flat.add_define(s.pool.intern(std::string(fn.text)), lambda);
    }
    // Normal: (define name value)
    if (n.kind != TokenKind::Identifier) {
        skip_rparen(s);
        return NULL_NODE;
    }
    s.lex.consume(); // consume name
    auto v = parse_expr(s);
    if (v == NULL_NODE)
        return NULL_NODE;
    s.lex.consume(); // ')'
    return s.flat.add_define(s.pool.intern(std::string(n.text)), v);
}

NodeId parse_define_type(ParserState& s) {
    auto tok = s.lex.consume(); // 'define-type'

    // Parse type name (possibly with params): Name or (Name params...)
    std::vector<SymId> params;
    SymId type_name;
    if (s.lex.peek().kind == TokenKind::LParen) {
        s.lex.consume(); // '('
        auto name_tok = s.lex.peek();
        if (name_tok.kind != TokenKind::Identifier) {
            skip_rparen(s);
            skip_rparen(s);
            return NULL_NODE;
        }
        type_name = s.pool.intern(name_tok.text);
        s.lex.consume(); // type name
        // Parse type parameters
        while (s.lex.peek().kind == TokenKind::Identifier) {
            params.push_back(s.pool.intern(s.lex.peek().text));
            s.lex.consume();
        }
        if (s.lex.peek().kind != TokenKind::RParen) {
            skip_rparen(s);
            return NULL_NODE;
        }
        s.lex.consume(); // ')'
    } else {
        auto name_tok = s.lex.peek();
        if (name_tok.kind != TokenKind::Identifier) {
            skip_rparen(s);
            return NULL_NODE;
        }
        type_name = s.pool.intern(name_tok.text);
        s.lex.consume(); // type name
    }

    // Parse constructor clauses: each is (CtorName field-types...)
    std::vector<NodeId> ctors;
    while (s.lex.peek().kind == TokenKind::LParen) {
        s.lex.consume(); // '('
        auto ctor_name_tok = s.lex.peek();
        if (ctor_name_tok.kind != TokenKind::Identifier) {
            skip_rparen(s);
            break;
        }
        auto ctor_sym = s.pool.intern(ctor_name_tok.text);
        s.lex.consume(); // ctor name

        // Parse field type expressions (identifiers or parenthesized types)
        std::vector<NodeId> field_nodes;
        while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
            auto ft = parse_expr(s);
            if (ft == NULL_NODE)
                break;
            field_nodes.push_back(ft);
        }
        s.lex.consume(); // ')'

        // Store constructor as (quote (ctor-name ft1 ft2 ...))
        // Build a list of field type expressions
        NodeId field_list = s.flat.add_literal(0); // '() sentinel
        for (auto it = field_nodes.rbegin(); it != field_nodes.rend(); ++it) {
            field_list = s.flat.add_pair(*it, field_list);
        }
        // Prepend constructor name
        auto ctor_name_var = s.flat.add_variable(ctor_sym);
        auto ctor_list = s.flat.add_pair(ctor_name_var, field_list);
        auto ctor_node = s.flat.add_quote(ctor_list);
        s.flat.set_loc(ctor_node, tok.line, tok.column);
        ctors.push_back(ctor_node);
    }

    if (s.lex.peek().kind != TokenKind::RParen) {
        skip_rparen(s);
        return NULL_NODE;
    }
    s.lex.consume(); // ')'

    return s.flat.add_define_type(type_name, params, ctors);
}

NodeId parse_define_module(ParserState& s) {
    auto tok = s.lex.consume(); // 'define-module'

    // (define-module (Name :Param-1 :Param-2 ...) body...)
    // Parse the header: (Name :Param ...)
    if (s.lex.peek().kind != TokenKind::LParen)
        return NULL_NODE;
    s.lex.consume(); // '('

    auto tok_name = s.lex.peek();
    if (tok_name.kind != TokenKind::Identifier) {
        skip_rparen(s);
        return NULL_NODE;
    }
    s.lex.consume(); // Name
    auto name_sym = s.pool.intern(tok_name.text);

    // Parse parameters: plain identifiers (T, K, V) or (cap :Capability)
    std::vector<aura::ast::SymId> type_params;
    std::vector<aura::ast::SymId> cap_params;
    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
        auto param_tok = s.lex.peek();
        if (param_tok.kind == TokenKind::LParen) {
            // Capability parameter: (cap :Capability)
            s.lex.consume(); // '('
            auto inner_tok = s.lex.peek();
            if (inner_tok.kind == TokenKind::Identifier) {
                s.lex.consume(); // param name (e.g., cap)
                // Check for ':Capability' or ':' 'Capability' annotation
                auto next_tok = s.lex.peek();
                bool is_cap = false;
                if (next_tok.kind == TokenKind::Identifier && next_tok.text == ":Capability") {
                    s.lex.consume(); // ':Capability'
                    is_cap = true;
                } else if (next_tok.kind == TokenKind::Identifier && next_tok.text == ":") {
                    s.lex.consume(); // ':'
                    auto type_tok = s.lex.peek();
                    if (type_tok.kind == TokenKind::Identifier &&
                        (type_tok.text == "Capability" || type_tok.text == "capability")) {
                        s.lex.consume(); // Capability
                        is_cap = true;
                    }
                }
                type_params.push_back(s.pool.intern(inner_tok.text));
                if (is_cap)
                    cap_params.push_back(s.pool.intern(inner_tok.text));
            }
            skip_rparen(s); // skip ')'
        } else if (param_tok.kind == TokenKind::Identifier && !param_tok.text.empty() &&
                   param_tok.text[0] != '(') {
            s.lex.consume();
            type_params.push_back(s.pool.intern(param_tok.text));
        } else {
            break;
        }
    }
    s.lex.consume(); // ')' after params

    // Create the define-module AST node
    auto id = s.flat.add_define_module(name_sym, type_params, cap_params);

    // Parse body expressions
    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
        auto body_expr = parse_expr(s);
        if (body_expr != NULL_NODE)
            s.flat.insert_child(id, 1000000, body_expr);
    }
    s.lex.consume(); // ')' after body
    s.flat.set_loc(id, tok.line, tok.column);

    return id;
}

NodeId parse_let(ParserState& s, bool rec) {
    auto tok = s.lex.consume(); // 'let' or 'letrec'

    // Named let: (let name ((binding...) body)
    if (!rec && s.lex.peek().kind == TokenKind::Identifier) {
        return parse_named_let(s);
    }

    if (s.lex.consume().kind != TokenKind::LParen)
        return NULL_NODE; // ((

    struct Binding {
        SymId name;
        NodeId val;
    };
    std::vector<Binding> bs;

    while (s.lex.peek().kind != TokenKind::RParen) {
        if (s.lex.consume().kind != TokenKind::LParen)
            return NULL_NODE;
        auto n = s.lex.consume();
        if (n.kind != TokenKind::Identifier)
            return NULL_NODE;
        auto v = parse_expr(s);
        if (v == NULL_NODE)
            return NULL_NODE;
        bs.push_back({s.pool.intern(std::string(n.text)), v});
        if (s.lex.consume().kind != TokenKind::RParen)
            return NULL_NODE;
    }
    s.lex.consume(); // ')'

    auto body = parse_expr(s);
    if (body == NULL_NODE)
        return NULL_NODE;
    // Collect additional body expressions until closing paren
    std::vector<NodeId> body_exprs = {body};
    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
        auto be = parse_expr(s);
        if (be == NULL_NODE)
            break;
        body_exprs.push_back(be);
    }
    if (s.lex.peek().kind == TokenKind::RParen)
        s.lex.consume();
    if (body_exprs.size() > 1)
        body = s.flat.add_begin(body_exprs);

    // Multi-binding letrec: desugar to pre-allocated cells + set!
    // (letrec ((a v1) (b v2)) body) → (begin (define a 0) (define b 0) (set! a v1) (set! b v2)
    // body)
    if (rec && bs.size() > 1) {
        std::vector<NodeId> exprs;
        for (auto& b : bs)
            exprs.push_back(s.flat.add_define(b.name, s.flat.add_literal(0)));
        for (auto& b : bs)
            exprs.push_back(s.flat.add_set(b.name, b.val));
        exprs.push_back(body);
        body = s.flat.add_begin(exprs);
    } else {
        // Wrap bindings: innermost first (so outer wraps inner)
        for (auto it = bs.rbegin(); it != bs.rend(); ++it) {
            if (rec)
                body = s.flat.add_letrec(it->name, it->val, body);
            else
                body = s.flat.add_let(it->name, it->val, body);
        }
    }
    return body;
}

NodeId parse_named_let(ParserState& s) {
    auto name_tok = s.lex.peek(); // already peeked
    if (name_tok.kind != TokenKind::Identifier)
        return NULL_NODE;
    s.lex.consume(); // consume the name
    auto name = s.pool.intern(std::string(name_tok.text));

    // Expect '(' for binding list
    if (s.lex.consume().kind != TokenKind::LParen)
        return NULL_NODE;

    struct Binding {
        SymId name;
        NodeId val;
    };
    std::vector<Binding> bs;

    while (s.lex.peek().kind != TokenKind::RParen) {
        if (s.lex.consume().kind != TokenKind::LParen)
            return NULL_NODE;
        auto n = s.lex.consume();
        if (n.kind != TokenKind::Identifier)
            return NULL_NODE;
        auto v = parse_expr(s);
        if (v == NULL_NODE)
            return NULL_NODE;
        bs.push_back({s.pool.intern(std::string(n.text)), v});
        if (s.lex.consume().kind != TokenKind::RParen)
            return NULL_NODE;
    }
    s.lex.consume(); // ')'

    // Read all body expressions and wrap in begin if >1
    std::vector<NodeId> body_exprs;
    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
        auto be = parse_expr(s);
        if (be == NULL_NODE)
            break;
        body_exprs.push_back(be);
    }
    s.lex.consume(); // ')'
    if (body_exprs.empty())
        return NULL_NODE;
    NodeId body = (body_exprs.size() == 1) ? body_exprs[0] : s.flat.add_begin(body_exprs);

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
    auto lambda_id = s.flat.add_lambda(params, body);

    // Create call: (name v1 v2 ...)
    auto var_id = s.flat.add_variable(name);
    auto call_id = s.flat.add_call(var_id, init_vals);

    // Create letrec: (letrec ((name lambda)) call)
    return s.flat.add_letrec(name, lambda_id, call_id);
}

NodeId parse_let_star(ParserState& s) {
    s.lex.consume(); // 'let*'
    if (s.lex.consume().kind != TokenKind::LParen)
        return NULL_NODE;

    struct Binding {
        SymId name;
        NodeId val;
    };
    std::vector<Binding> bs;

    while (s.lex.peek().kind != TokenKind::RParen) {
        if (s.lex.consume().kind != TokenKind::LParen)
            return NULL_NODE;
        auto n = s.lex.consume();
        if (n.kind != TokenKind::Identifier)
            return NULL_NODE;
        auto v = parse_expr(s);
        if (v == NULL_NODE)
            return NULL_NODE;
        bs.push_back({s.pool.intern(std::string(n.text)), v});
        if (s.lex.consume().kind != TokenKind::RParen)
            return NULL_NODE;
    }
    s.lex.consume(); // ')'

    // Read all body expressions and wrap in begin if >1
    std::vector<NodeId> body_exprs;
    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
        auto be = parse_expr(s);
        if (be == NULL_NODE)
            break;
        body_exprs.push_back(be);
    }
    s.lex.consume(); // ')'
    if (body_exprs.empty())
        return NULL_NODE;
    NodeId body = (body_exprs.size() == 1) ? body_exprs[0] : s.flat.add_begin(body_exprs);

    // Desugar: (let* ((a1 v1) (a2 v2)) body...)
    //       → (let ((a1 v1)) (let ((a2 v2)) body...))
    // Build from right to left so outermost wraps innermost
    for (auto it = bs.rbegin(); it != bs.rend(); ++it) {
        body = s.flat.add_let(it->name, it->val, body);
    }
    return body;
}

NodeId parse_val(ParserState& s) {
    auto tok = s.lex.peek();
    switch (tok.kind) {
        case TokenKind::Integer:
            return parse_number(s, s.lex.consume());
        case TokenKind::Bool: {
            auto tok = s.lex.consume();
            auto v = std::stoll(std::string(tok.text));
            auto id = s.flat.add_literal(v);
            s.flat.set_marker(id, aura::ast::SyntaxMarker::BoolLiteral);
            s.flat.set_loc(id, tok.line, tok.column);
            return id;
        }
        case TokenKind::Float:
            return parse_float(s, s.lex.consume());
        case TokenKind::String: {
            auto tok2 = s.lex.consume();
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
            return s.flat.add_literalstring(s.pool.intern(has_e2 ? unesc2 : s2));
        }
        case TokenKind::Identifier:
            return s.flat.add_variable(s.pool.intern(std::string(s.lex.consume().text)));
        case TokenKind::LParen:
            s.lex.consume();
            return parse_list(s);
        case TokenKind::Quote: {
            s.lex.consume();
            auto quoted = parse_val(s);
            if (quoted == NULL_NODE)
                return NULL_NODE;
            return s.flat.add_quote(quoted);
        }
        default:
            return NULL_NODE;
    }
}

void skip_rparen(ParserState& s) {
    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof())
        s.lex.consume();
    s.lex.consume();
}

NodeId parse_begin(ParserState& s) {
    auto tok = s.lex.consume(); // 'begin'
    std::vector<NodeId> exprs;
    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
        auto e = parse_expr(s);
        if (e != NULL_NODE)
            exprs.push_back(e);
        else
            break;
    }
    s.lex.consume(); // ')'
    auto bid = s.flat.add_begin(exprs);
    s.flat.set_loc(bid, tok.line, tok.column);
    return bid;
}

NodeId parse_set(ParserState& s) {
    auto tok = s.lex.consume(); // 'set!'
    auto n = s.lex.consume();
    if (n.kind != TokenKind::Identifier) {
        skip_rparen(s);
        return NULL_NODE;
    }
    auto v = parse_expr(s);
    if (v == NULL_NODE) {
        skip_rparen(s);
        return NULL_NODE;
    }
    s.lex.consume(); // ')'
    auto sid = s.flat.add_set(s.pool.intern(std::string(n.text)), v);
    s.flat.set_loc(sid, tok.line, tok.column);
    return sid;
}

NodeId parse_quote(ParserState& s) {
    auto tok = s.lex.consume(); // 'quote'
    auto v = parse_expr(s);
    if (v == NULL_NODE) {
        skip_rparen(s);
        return NULL_NODE;
    }
    s.lex.consume(); // ')'
    auto qid = s.flat.add_quote(v);
    s.flat.set_loc(qid, tok.line, tok.column);
    return qid;
}

NodeId parse_cond(ParserState& s) {
    s.lex.consume(); // 'cond'
    struct Clause {
        NodeId test;
        NodeId val;
    };
    std::vector<Clause> clauses;
    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
        if (s.lex.peek().kind != TokenKind::LParen)
            break;
        s.lex.consume(); // '('
        auto cn = parse_expr(s);
        if (cn == NULL_NODE) {
            skip_rparen(s);
            break;
        }
        auto v = parse_expr(s);
        if (v == NULL_NODE) {
            skip_rparen(s);
            break;
        }
        // R5RS: (cond (test expr1 expr2 ...)) — read ALL exprs, wrap in begin
        std::vector<NodeId> exprs;
        exprs.push_back(v);
        while (s.lex.peek().kind != TokenKind::RParen) {
            auto more = parse_expr(s);
            if (more == NULL_NODE)
                break;
            exprs.push_back(more);
        }
        s.lex.consume(); // ')'
        clauses.push_back(
            {cn, exprs.size() == 1 ? v : s.flat.add_begin(exprs.data(), exprs.size())});
    }
    s.lex.consume(); // ')'
    if (clauses.empty())
        return NULL_NODE;
    auto result = clauses.back().val;
    for (auto it = clauses.rbegin() + 1; it != clauses.rend(); ++it)
        result = s.flat.add_if(it->test, it->val, result);
    return result;
}

NodeId parse_defmacro(ParserState& s, bool hygienic, bool preserve_dotted) {
    auto tok = s.lex.consume(); // 'defmacro'
    if (s.lex.consume().kind != TokenKind::LParen) {
        skip_rparen(s);
        return NULL_NODE;
    }
    auto name = s.lex.consume();
    if (name.kind != TokenKind::Identifier) {
        skip_rparen(s);
        return NULL_NODE;
    }
    std::vector<SymId> params;
    bool preserved_macro = false;
    bool dotted = false;
    while (s.lex.peek().kind != TokenKind::RParen) {
        // Issue #230 #2: with preserve_dotted=true, a param prefixed
        // with '&' (e.g. '&name') is marked as "do not gensym" — its
        // literal name flows through to the expanded body. The '&'
        // is consumed and stripped from the interned name. This is
        // the er-macro-style escape hatch for symbol-generating macros
        // that need access to the user's actual identifier (struct
        // name, port name, etc.) rather than a gensym'd shadow.
        //
        // We use '&' rather than '.' to avoid conflict with the
        // dotted-rest-parameter syntax `(name . rest)` where '.' is
        // a separate token between params.
        if (preserve_dotted && s.lex.peek().kind == TokenKind::Identifier) {
            std::string_view ptext = s.lex.peek().text;
            if (ptext.size() > 1 && ptext[0] == '&') {
                s.lex.consume(); // consume '&name'
                preserved_macro = true;
                // Strip the leading '&' and intern the rest.
                params.push_back(s.pool.intern(std::string(ptext.substr(1))));
                continue;
            }
        }
        // Check for dotted rest parameter: (name . rest)
        if (s.lex.peek().kind == TokenKind::Dot) {
            s.lex.consume(); // consume '.'
            if (s.lex.peek().kind != TokenKind::Identifier)
                break;
            auto rest = s.lex.consume();
            params.push_back(s.pool.intern(std::string(rest.text)));
            dotted = true;
            break;
        }
        auto p = s.lex.consume();
        if (p.kind != TokenKind::Identifier) {
            skip_rparen(s);
            return NULL_NODE;
        }
        params.push_back(s.pool.intern(std::string(p.text)));
    }
    s.lex.consume(); // ')'
    // Issue #137: macro body may be a multi-form block (e.g. (define ...)
    // followed by a final expression). The previous parser only collected
    // the first expression via `parse_expr()` then consumed `)`, silently
    // dropping the rest of the body. The dropped expressions ended up at
    // the program top level, where they could reference template-introduced
    // names (e.g. (helper x) after a local (define (helper v) ...))
    // and produce a spurious "unbound variable: helper" error.
    //
    // Fix: parse_expr() once, then collect any additional body
    // expressions until the closing `)`. If more than one, wrap them
    // in a Begin node. This matches the behavior of parse_let /
    // parse_defun / parse_define.
    auto body = parse_expr(s);
    if (body == NULL_NODE) {
        skip_rparen(s);
        return NULL_NODE;
    }
    std::vector<NodeId> body_exprs = {body};
    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
        auto be = parse_expr(s);
        if (be == NULL_NODE)
            break;
        body_exprs.push_back(be);
    }
    if (s.lex.peek().kind == TokenKind::RParen)
        s.lex.consume();
    if (body_exprs.size() > 1)
        body = s.flat.add_begin(body_exprs);
    auto mid = s.flat.add_macrodef(s.pool.intern(std::string(name.text)), params, body, dotted,
                                   hygienic, preserved_macro);
    s.flat.set_loc(mid, tok.line, tok.column);
    return mid;
}

// ── Match / pattern matching ─────────────────────────────────
NodeId parse_match(ParserState& s) {
    auto tok = s.lex.consume(); // 'match'

    // Parse subject expression
    auto subject = parse_expr(s);
    if (subject == NULL_NODE) {
        skip_rparen(s);
        return NULL_NODE;
    }

    // Temp variable to hold subject (evaluated once)
    auto tmp = s.pool.intern("__match_tmp");

    // Parse clauses: (pattern body ...)
    struct Clause {
        NodeId pattern;
        NodeId test;
        NodeId body;
    };
    std::vector<Clause> clauses;

    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
        if (s.lex.peek().kind != TokenKind::LParen)
            break;
        s.lex.consume(); // '('

        // Parse pattern (as an s-expression value)
        auto pattern = parse_val(s);
        if (pattern == NULL_NODE)
            break;

        // Parse body
        auto body = parse_expr(s);
        if (body == NULL_NODE)
            break;

        if (s.lex.peek().kind != TokenKind::RParen)
            break;
        s.lex.consume(); // ')'

        // Compile pattern into test and bindings, then wrap body in let
        NodeId test;
        auto bindings = compile_pattern(s, pattern, tmp, &test);

        // Wrap body in let bindings
        for (auto& [name, val] : bindings)
            body = s.flat.add_let(name, val, body);

        clauses.push_back({pattern, test, body});
    }

    if (s.lex.peek().kind == TokenKind::RParen)
        s.lex.consume(); // ')'

    if (clauses.empty())
        return NULL_NODE;

    // ── Collect match clause metadata for exhaustiveness checking ──
    aura::ast::MatchClauseInfo minfo;
    for (auto& c : clauses) {
        auto pv = s.flat.get(c.pattern);
        if (pv.tag == NodeTag::Variable) {
            auto pname = s.pool.resolve(pv.sym_id);
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
            auto callee_v = s.flat.get(callee_id);
            if (callee_v.tag == NodeTag::Variable)
                minfo.used_constructors.push_back(callee_v.sym_id);
        }
    }

    // Build nested if chain from right to left
    NodeId result = clauses.back().body;
    for (auto it = clauses.rbegin() + 1; it != clauses.rend(); ++it)
        result = s.flat.add_if(it->test, it->body, result);

    // Wrap in (let ((__match_tmp subject)) result)
    result = s.flat.add_let(tmp, subject, result);
    s.flat.set_loc(result, tok.line, tok.column);

    // Store match metadata on the let node
    if (!minfo.used_constructors.empty() || !minfo.candidate_constructors.empty() ||
        minfo.has_wildcard)
        s.flat.set_match_info(result, std::move(minfo));

    return result;
}

NodeId parse_linear(ParserState& s) {
    auto tok = s.lex.consume(); // 'Linear'
    auto inner = parse_expr(s);
    if (inner == NULL_NODE) {
        skip_rparen(s);
        return NULL_NODE;
    }
    if (s.lex.peek().kind == TokenKind::RParen)
        s.lex.consume();
    auto id = s.flat.add_linear(inner);
    s.flat.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId parse_move(ParserState& s) {
    auto tok = s.lex.consume(); // 'move'
    auto inner = parse_expr(s);
    if (inner == NULL_NODE) {
        skip_rparen(s);
        return NULL_NODE;
    }
    if (s.lex.peek().kind == TokenKind::RParen)
        s.lex.consume();
    auto id = s.flat.add_move(inner);
    s.flat.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId parse_borrow(ParserState& s) {
    auto tok = s.lex.consume(); // 'borrow'
    auto inner = parse_expr(s);
    if (inner == NULL_NODE) {
        skip_rparen(s);
        return NULL_NODE;
    }
    if (s.lex.peek().kind == TokenKind::RParen)
        s.lex.consume();
    auto id = s.flat.add_borrow(inner);
    s.flat.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId parse_mut_borrow(ParserState& s) {
    auto tok = s.lex.consume(); // 'mut-borrow'
    auto inner = parse_expr(s);
    if (inner == NULL_NODE) {
        skip_rparen(s);
        return NULL_NODE;
    }
    if (s.lex.peek().kind == TokenKind::RParen)
        s.lex.consume();
    auto id = s.flat.add_mut_borrow(inner);
    s.flat.set_loc(id, tok.line, tok.column);
    return id;
}

// (#108 part 4 Phase 1) parse_datatype
//
// Syntax: (datatype (Name : TypeParam) (Ctor1 f1 f2 ...) (Ctor2 g1 ...) ...)
//
// Emits a single call to the runtime primitive
// (adt:register-constructors (list "Ctor1" "Ctor2" ...)). The
// primitive populates the g_adt_constructors table; Env::lookup
// then falls back to this table, so constructors are visible
// across top-level forms (bypassing Begin's lexical scope).
//
// Why this design: a (datatype ...) form is one top-level
// expression, so the parser can only return one root node. If
// we emitted a Begin of Defines (the natural macro expansion),
// the inner Defines would be scoped to the Begin and invisible
// to subsequent top-level expressions. The global table +
// lookup fallback is the only way to make ctor names globally
// visible from a single parsed form.
//
// The : TypeParam declaration is parsed but ignored — Aura's
// gradual type system doesn't track ADT type parameters.
NodeId parse_datatype(ParserState& s) {
    auto tok = s.lex.consume(); // 'datatype'

    // Parse spec: (Name : TypeParam)
    if (s.lex.peek().kind != TokenKind::LParen) {
        skip_rparen(s);
        return NULL_NODE;
    }
    s.lex.consume(); // '('
    auto name_tok = s.lex.peek();
    if (name_tok.kind != TokenKind::Identifier) {
        skip_rparen(s);
        return NULL_NODE;
    }
    s.lex.consume();
    // Skip the ": TypeParam..." suffix (any number of identifiers
    // after the colon; Phase 1 ignores them). (Either : a b)
    // is parsed as spec = (Either) and the ": a b" suffix is
    // skipped entirely.
    if (s.lex.peek().kind == TokenKind::Identifier && s.lex.peek().text == ":") {
        s.lex.consume(); // ':'
        // Consume any number of type-param identifiers
        while (s.lex.peek().kind == TokenKind::Identifier) {
            s.lex.consume();
        }
    }
    if (s.lex.peek().kind == TokenKind::RParen)
        s.lex.consume(); // ')'

    // Collect ctor names + arities. Phase 2 wire format:
    // (adt:register-constructors (cons "Ctor1" 1 (cons "Ctor2" 2 ...)))
    // i.e. each entry is a (cons name arity) pair; the cdr is
    // the next entry.
    std::vector<std::pair<aura::ast::NodeId, std::size_t>> ctor_entries;
    while (s.lex.peek().kind == TokenKind::LParen && !s.lex.eof()) {
        s.lex.consume(); // '('
        auto ctor_tok = s.lex.peek();
        if (ctor_tok.kind != TokenKind::Identifier) {
            skip_rparen(s);
            return NULL_NODE;
        }
        s.lex.consume();
        // Build a LiteralString node holding the ctor name.
        auto ctor_str_sym = s.pool.intern(ctor_tok.text);
        auto ctor_name_node = s.flat.add_literalstring(ctor_str_sym);

        // Count field type names (arity = number of identifiers
        // until the closing RParen). Phase 2 doesn't validate
        // that these are real types, just counts them.
        std::size_t arity = 0;
        while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
            // Only count simple identifiers; nested parens (e.g.
            // parametric types like (List T)) count as one for
            // arity purposes. Phase 2 keeps it simple.
            if (s.lex.peek().kind == TokenKind::Identifier) {
                ++arity;
                s.lex.consume();
            } else {
                // Skip non-identifier tokens (LParen, RParen, etc.)
                s.lex.consume();
            }
        }
        if (s.lex.peek().kind == TokenKind::RParen)
            s.lex.consume();

        ctor_entries.push_back({ctor_name_node, arity});
    }

    // Consume final ')'
    if (s.lex.peek().kind == TokenKind::RParen)
        s.lex.consume();

    // Build the alist of (name . arity) entries as a list of
    // pairs. Each entry is a single cons cell:
    //   (cons "Name" arity_int)
    // and the list itself is a chain of cons cells:
    //   (cons entry1 (cons entry2 ... ()))
    //
    // We use Call nodes (not raw Pair nodes) because the AST
    // doesn't expose a "create a Pair" — pair cells are only
    // produced at eval time by calling `cons`. (add_call wants
    // a NodeId for the func, not a bare SymId.)
    auto cons_var = s.flat.add_variable(s.pool.intern("cons"));
    auto nil_lit = s.flat.add_literal(0); // () — empty list
    aura::ast::NodeId name_list = nil_lit;
    for (auto it = ctor_entries.rbegin(); it != ctor_entries.rend(); ++it) {
        // Each entry: (cons "Name" arity)  — a single cons cell
        auto arity_lit = s.flat.add_literal(static_cast<std::int64_t>(it->second));
        std::array<aura::ast::NodeId, 2> entry_args{it->first, arity_lit};
        auto entry = s.flat.add_call(cons_var, entry_args);
        // cons the entry onto the running list
        std::array<aura::ast::NodeId, 2> list_args{entry, name_list};
        name_list = s.flat.add_call(cons_var, list_args);
    }

    // Build (adt:register-constructors <name_list>)
    auto prim_sym = s.pool.intern("adt:register-constructors");
    auto prim_var = s.flat.add_variable(prim_sym);
    std::array<aura::ast::NodeId, 1> call_args{name_list};
    auto call_id = s.flat.add_call(prim_var, call_args);
    s.flat.set_loc(call_id, tok.line, tok.column);
    return call_id;
}

NodeId parse_drop(ParserState& s) {
    auto tok = s.lex.consume(); // 'drop'
    auto inner = parse_expr(s);
    if (inner == NULL_NODE) {
        skip_rparen(s);
        return NULL_NODE;
    }
    if (s.lex.peek().kind == TokenKind::RParen)
        s.lex.consume();
    auto id = s.flat.add_drop(inner);
    s.flat.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId parse_cast(ParserState& s) {
    // Syntax: (cast expr : TypeName) or (cast expr TypeName)
    // Creates Coercion node: child[0]=expr, int_val=type_tag
    auto tok = s.lex.consume(); // 'cast'
    auto expr = parse_expr(s);
    if (expr == NULL_NODE) {
        skip_rparen(s);
        return NULL_NODE;
    }

    // Parse optional : then type name
    if (s.lex.peek().kind == TokenKind::Identifier && s.lex.peek().text == ":") {
        s.lex.consume(); // :
    }

    auto type_tok = s.lex.peek();
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
    s.lex.consume(); // TypeName

    if (s.lex.peek().kind == TokenKind::RParen)
        s.lex.consume();

    auto id = s.flat.add_coercion(expr, type_tag, 0);
    s.flat.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId parse_check(ParserState& s) {
    // Syntax: (check expr : TypeName)
    // Creates TypeAnnotation node: child[0]=expr, sym_id=TypeName
    // If not followed by ': TypeName', treat as regular function call
    auto tok = s.lex.consume(); // 'check'
    auto expr = parse_expr(s);
    if (expr == NULL_NODE) {
        skip_rparen(s);
        return NULL_NODE;
    }

    // Verify this is truly (check ... : Type), not (check x) as function call.
    // Issue #102: also accept `:?` and `_` as the type hole sentinels
    // directly, without requiring the `:` separator. The lexer treats
    // `:?` and `_` as identifier tokens, so the type name is the
    // token text itself. This lets LLM-generated code use either
    // `(check x :?)` or `(check x _)` interchangeably.
    if (s.lex.peek().kind == TokenKind::Identifier &&
        (s.lex.peek().text == ":" || s.lex.peek().text == ":?" || s.lex.peek().text == "_")) {
        auto type_text = std::string(s.lex.peek().text);
        if (type_text == ":") {
            s.lex.consume(); // ':'
            // Now read the actual type name token.
            auto type_tok = s.lex.peek();
            if (type_tok.kind == TokenKind::Identifier) {
                type_text = std::string(type_tok.text);
                s.lex.consume(); // TypeName
            }
        } else {
            s.lex.consume(); // the `:?` or `_` token itself
        }
        if (s.lex.peek().kind == TokenKind::RParen)
            s.lex.consume();
        auto type_sym = s.pool.intern(type_text);
        auto id = s.flat.add_type_annotation(type_sym, expr);
        s.flat.set_loc(id, tok.line, tok.column);
        return id;
    }
    // Token after `check` and the expr is something else — also
    // accept the type hole directly when the next token after the
    // expr is `:?` or `_`. The `:` separator was the historical
    // form; the hole-only form is the LLM-friendly form.
    if (s.lex.peek().kind == TokenKind::Identifier &&
        (s.lex.peek().text == ":?" || s.lex.peek().text == "_")) {
        auto type_text = std::string(s.lex.peek().text);
        s.lex.consume(); // the `:?` or `_` token
        if (s.lex.peek().kind == TokenKind::RParen)
            s.lex.consume();
        auto type_sym = s.pool.intern(type_text);
        auto id = s.flat.add_type_annotation(type_sym, expr);
        s.flat.set_loc(id, tok.line, tok.column);
        return id;
    }

    // Not a valid type annotation — treat (check ...) as regular function call
    // Build a Call node with 'check' as the operator
    auto check_sym = s.pool.intern("check");
    auto func = s.flat.add_variable(check_sym);
    std::vector<NodeId> args;
    args.push_back(expr);
    // Collect any remaining args until )
    while (s.lex.peek().kind != TokenKind::RParen && !s.lex.eof()) {
        auto arg = parse_expr(s);
        if (arg == NULL_NODE)
            break;
        args.push_back(arg);
    }
    if (s.lex.peek().kind == TokenKind::RParen)
        s.lex.consume();
    auto id = s.flat.add_call(func, args);
    s.flat.set_loc(id, tok.line, tok.column);
    return id;
}

NodeId parse_type_annot(ParserState& s) {
    // Syntax:
    //   (: name TypeName)       — annotate variable with type (no-op at runtime)
    //   (: name Type val)        — bind, annotate, and return val
    auto tok = s.lex.consume(); // :

    auto name_tok = s.lex.peek();
    if (name_tok.kind != TokenKind::Identifier) {
        skip_rparen(s);
        return NULL_NODE;
    }
    auto var_sym = s.pool.intern(name_tok.text);
    s.lex.consume(); // name

    auto type_tok = s.lex.peek();
    // Compound type expression: (: name (List :T)) or (: name (Maybe :T))
    if (type_tok.kind != TokenKind::Identifier) {
        skip_rparen(s);
        return NULL_NODE;
    }
    auto type_sym = s.pool.intern(type_tok.text);
    s.lex.consume(); // TypeName

    // Check for 3-arg form: (: name Type value-expr)
    if (s.lex.peek().kind != TokenKind::RParen) {
        // Consume value and return it with type annotation wrapping
        auto val = parse_expr(s);
        if (s.lex.peek().kind == TokenKind::RParen)
            s.lex.consume();
        if (val == NULL_NODE)
            return NULL_NODE;
        // Store var_sym in int_val_ so eval_flat can bind the variable.
        // The type checker will see the TypeAnnotation as the root and report the
        // correct type (Int, String, etc.).
        auto id = s.flat.add_type_annotation(type_sym, val, var_sym);
        s.flat.set_loc(id, tok.line, tok.column);
        return id;
    }

    s.lex.consume(); // RParen

    auto var_node = s.flat.add_variable(var_sym);
    auto id = s.flat.add_type_annotation(type_sym, var_node);
    s.flat.set_loc(id, tok.line, tok.column);
    return id;
}

std::vector<std::pair<SymId, NodeId>> compile_pattern(ParserState& s, NodeId pattern_node,
                                                      SymId tmp, NodeId* out_test) {
    auto v = s.flat.get(pattern_node);
    std::vector<std::pair<SymId, NodeId>> bindings;
    auto var_tmp = s.flat.add_variable(tmp);
    auto sym_null_q = s.pool.intern("null?");
    auto sym_pair_q = s.pool.intern("pair?");
    auto sym_car = s.pool.intern("car");
    auto sym_cdr = s.pool.intern("cdr");
    auto sym_equal_q = s.pool.intern("equal?");

    // Helper: call with args as initializer_list
    auto make_call = [&](SymId func, std::initializer_list<NodeId> args) -> NodeId {
        return s.flat.add_call(s.flat.add_variable(func), std::vector<NodeId>(args));
    };

    // Variable/wildcard pattern
    if (v.tag == NodeTag::Variable) {
        auto name = s.pool.resolve(v.sym_id);
        if (name == "_" || (name.size() > 1 && name[0] == '_' && name != "__match_tmp")) {
            // Wildcard: match anything, no bindings
            *out_test = s.flat.add_literal(1);
            return bindings;
        }
        // Variable binding: match anything, bind to whole value
        *out_test = s.flat.add_literal(1);
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
            *out_test = s.flat.add_literal(0);
            return bindings;
        }

        auto callee_v = s.flat.get(v.child(0));
        if (callee_v.tag == NodeTag::Variable) {
            auto callee_name = s.pool.resolve(callee_v.sym_id);

            // (quote data) pattern — explicit quote in call position
            if (callee_name == "quote" && v.children.size() > 1) {
                auto quoted = v.child(1);
                // Re-wrap as proper quote expression
                auto quoted_expr = s.flat.add_quote(quoted);
                *out_test = make_call(sym_equal_q, {var_tmp, quoted_expr});
                return bindings;
            }

            // (list p1 p2 ...) pattern
            if (callee_name == "list") {
                // Build chain: (pair? tmp) && (pair? (cdr tmp)) && ... && (null? (cddr... tmp))
                NodeId accumulated_test = s.flat.add_literal(1); // start with #t
                NodeId current = var_tmp;

                for (std::size_t i = 1; i < v.children.size(); ++i) {
                    // (pair? current)
                    auto pair_test = make_call(sym_pair_q, {current});
                    accumulated_test =
                        s.flat.add_if(accumulated_test, pair_test, s.flat.add_literal(0));

                    auto elem = v.child(i);
                    auto elem_v = s.flat.get(elem);

                    // (car current) — extract element value
                    auto car_expr = make_call(sym_car, {current});

                    if (elem_v.tag == NodeTag::Variable) {
                        auto elem_name = s.pool.resolve(elem_v.sym_id);
                        if (elem_name != "_" && !(elem_name.size() > 1 && elem_name[0] == '_')) {
                            // Variable binding: bind car value
                            bindings.emplace_back(elem_v.sym_id, car_expr);
                        } // else: wildcard, skip
                    } else if (elem_v.tag == NodeTag::LiteralInt && elem_v.int_value == 0) {
                        // () — exact match car against empty list
                        auto eq_test = make_call(sym_equal_q, {car_expr, elem});
                        accumulated_test =
                            s.flat.add_if(accumulated_test, eq_test, s.flat.add_literal(0));
                    } else if (elem_v.tag == NodeTag::LiteralInt ||
                               elem_v.tag == NodeTag::LiteralFloat ||
                               elem_v.tag == NodeTag::LiteralString) {
                        // Literal element match
                        auto eq_test = make_call(sym_equal_q, {car_expr, elem});
                        accumulated_test =
                            s.flat.add_if(accumulated_test, eq_test, s.flat.add_literal(0));
                    }
                    // For (list ...) sub-patterns or other complex elements,
                    // we fall through and they match anything (no equality check)

                    // Move to next: (cdr current)
                    current = make_call(sym_cdr, {current});
                }

                // Final: (null? current) — proper list length check
                auto null_test = make_call(sym_null_q, {current});
                accumulated_test =
                    s.flat.add_if(accumulated_test, null_test, s.flat.add_literal(0));

                *out_test = accumulated_test;
                return bindings;
            }

            // (cons p q) pattern
            if (callee_name == "cons" && v.children.size() >= 3) {
                // Test: (pair? tmp)
                *out_test = make_call(sym_pair_q, {var_tmp});

                auto car_pat = v.child(1);
                auto cdr_pat = v.child(2);
                auto car_v = s.flat.get(car_pat);
                auto cdr_v = s.flat.get(cdr_pat);

                auto car_expr = make_call(sym_car, {var_tmp});
                auto cdr_expr = make_call(sym_cdr, {var_tmp});

                // Car binding
                if (car_v.tag == NodeTag::Variable) {
                    auto ename = s.pool.resolve(car_v.sym_id);
                    if (ename != "_" && !(ename.size() > 1 && ename[0] == '_'))
                        bindings.emplace_back(car_v.sym_id, car_expr);
                }

                // Cdr binding
                if (cdr_v.tag == NodeTag::Variable) {
                    auto ename = s.pool.resolve(cdr_v.sym_id);
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
                auto ctor_name = s.pool.resolve(callee_v.sym_id);
                // Ignore list/cons/pair/quote — handled above
                if (ctor_name != "list" && ctor_name != "cons" && ctor_name != "pair") {
                    // Zero-arg constructor: (CtorName) with no sub-patterns
                    // The value IS the constructor function (a closure), not a pair.
                    // Compare directly using equal?.
                    if (v.children.size() == 1) {
                        // Zero-arg constructor: compare subject against a built pair.
                        // Build (cons "CtorName" ()) which is how zero-arg constructors are stored.
                        auto ctor_str_node = s.flat.add_literalstring(s.pool.intern(ctor_name));
                        auto null_lit = s.flat.add_literal(0);
                        auto ctor_val = make_call(s.pool.intern("cons"), {ctor_str_node, null_lit});
                        *out_test = make_call(sym_equal_q, {var_tmp, ctor_val});
                        return bindings;
                    }
                    // Test 1: (pair? tmp) — value must be a pair
                    NodeId accumulated_test = make_call(sym_pair_q, {var_tmp});

                    // Test 2: (equal? (car tmp) "CtorName") — tag match as string
                    // The constructor stores the tag as a string in the pair heap
                    auto ctor_str = s.flat.add_literalstring(s.pool.intern(ctor_name));
                    auto tag_test = make_call(s.pool.intern("string=?"),
                                              {make_call(sym_car, {var_tmp}), ctor_str});
                    accumulated_test =
                        s.flat.add_if(accumulated_test, tag_test, s.flat.add_literal(0));

                    // Walk sub-patterns starting from (cdr tmp)
                    auto current = make_call(sym_cdr, {var_tmp});
                    for (std::size_t i = 1; i < v.children.size(); ++i) {
                        auto elem = v.child(i);
                        auto elem_v = s.flat.get(elem);

                        // (pair? current) — ensure list structure
                        auto pair_check = make_call(sym_pair_q, {current});
                        accumulated_test =
                            s.flat.add_if(accumulated_test, pair_check, s.flat.add_literal(0));

                        // Null-guarded field extraction for type checker safety
                        auto null_check = make_call(sym_null_q, {current});
                        auto zero_lit = s.flat.add_literal(0);
                        auto car_expr = make_call(sym_car, {current});
                        auto elem_car = s.flat.add_if(null_check, zero_lit, car_expr);

                        if (elem_v.tag == NodeTag::Variable) {
                            auto elem_name = s.pool.resolve(elem_v.sym_id);
                            if (elem_name != "_" &&
                                !(elem_name.size() > 1 && elem_name[0] == '_')) {
                                bindings.emplace_back(elem_v.sym_id, elem_car);
                            }
                        } else if (elem_v.tag == NodeTag::LiteralInt ||
                                   elem_v.tag == NodeTag::LiteralFloat ||
                                   elem_v.tag == NodeTag::LiteralString) {
                            auto eq_test = make_call(sym_equal_q, {elem_car, elem});
                            accumulated_test =
                                s.flat.add_if(accumulated_test, eq_test, s.flat.add_literal(0));
                        }

                        current = make_call(sym_cdr, {current});
                    }

                    // Final: (null? current) — proper length
                    auto null_test = make_call(sym_null_q, {current});
                    accumulated_test =
                        s.flat.add_if(accumulated_test, null_test, s.flat.add_literal(0));

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

NodeId expand_qq(ParserState& s, NodeId expr, int depth) {
    // Prevent stack overflow from exceessive quasiquote nesting
    if (depth > 4)
        return NULL_NODE;
    if (expr == NULL_NODE) {
        // Empty quasiquote: (quote ())
        return s.flat.add_quote(s.flat.add_literal(0));
    }

    auto v = s.flat.get(expr);

    // Non-Call compound nodes: special forms parsed by keyword (IfExpr, Lambda, Let, etc.)
    // The keyword (if, lambda, etc.) is LOST by parse_list — we need to reconstruct it
    if (v.tag != NodeTag::Call) {
        // Variables and literals: (quote expr)
        if (v.tag == NodeTag::Variable || v.tag == NodeTag::LiteralInt ||
            v.tag == NodeTag::LiteralFloat || v.tag == NodeTag::LiteralString ||
            v.tag == NodeTag::Quote) {
            return s.flat.add_quote(expr);
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
                    return s.flat.add_quote(expr);
            }
            // Build: (cons (quote <form-name>) (cons arg0 (cons arg1 ... (quote ()))))
            // where args are: params (if Lambda) + children + extra elements
            std::vector<NodeId> args_to_expand;

            // For Lambda, add quoted parameter list as first arg
            if (v.tag == NodeTag::Lambda) {
                NodeId params_list = s.flat.add_quote(s.flat.add_literal(0)); // (quote ())
                for (int pi = static_cast<int>(v.params.size()) - 1; pi >= 0; --pi) {
                    auto param_var = s.flat.add_variable(
                        s.pool.intern(std::string(s.pool.resolve(v.params[pi]))));
                    auto param_quoted = s.flat.add_quote(param_var);
                    auto cv = s.flat.add_variable(s.pool.intern("cons"));
                    params_list = s.flat.add_call(
                        cv, std::vector<aura::ast::NodeId>{param_quoted, params_list});
                }
                args_to_expand.push_back(params_list);
            }

            bool has_fn_list = false; // for define shorthand
            // For Define shorthand (define+lambda child), replace with (fn params) list
            if (v.tag == NodeTag::Define && v.children.size() == 1 &&
                v.sym_id != ast::INVALID_SYM) {
                auto child_v = s.flat.get(v.child(0));
                if (child_v.tag == NodeTag::Lambda) {
                    // Build (fn params) list
                    auto fn_var =
                        s.flat.add_variable(s.pool.intern(std::string(s.pool.resolve(v.sym_id))));
                    auto fn_quoted = s.flat.add_quote(fn_var);
                    NodeId fn_params_list = s.flat.add_quote(s.flat.add_literal(0));
                    for (int pi = static_cast<int>(child_v.params.size()) - 1; pi >= 0; --pi) {
                        auto pvar = s.flat.add_variable(
                            s.pool.intern(std::string(s.pool.resolve(child_v.params[pi]))));
                        auto pquoted = s.flat.add_quote(pvar);
                        auto cv = s.flat.add_variable(s.pool.intern("cons"));
                        fn_params_list = s.flat.add_call(
                            cv, std::vector<aura::ast::NodeId>{pquoted, fn_params_list});
                    }
                    auto cv = s.flat.add_variable(s.pool.intern("cons"));
                    args_to_expand.push_back(s.flat.add_call(
                        cv, std::vector<aura::ast::NodeId>{fn_quoted, fn_params_list}));
                    has_fn_list = true;
                    // Also add the lambda body as args
                    for (std::size_t bci = 0; bci < child_v.children.size(); ++bci) {
                        auto expanded = expand_qq(s, child_v.child(bci), depth);
                        args_to_expand.push_back(expanded);
                    }
                }
            }

            // Add children (skip for define shorthand — already handled above)
            if (!has_fn_list) {
                for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                    auto expanded = expand_qq(s, v.child(ci), depth);
                    args_to_expand.push_back(expanded);
                }
            }

            // Build result: (quote ()) then prepend each arg
            NodeId result = s.flat.add_quote(s.flat.add_literal(0));
            for (int i = static_cast<int>(args_to_expand.size()) - 1; i >= 0; --i) {
                auto cons_var = s.flat.add_variable(s.pool.intern("cons"));
                result = s.flat.add_call(cons_var,
                                         std::vector<aura::ast::NodeId>{args_to_expand[i], result});
            }

            // Prepend (quote <form-name>)
            auto form_var = s.flat.add_variable(s.pool.intern(form_name));
            auto form_quote = s.flat.add_quote(form_var);
            auto cons_var2 = s.flat.add_variable(s.pool.intern("cons"));
            result = s.flat.add_call(cons_var2, std::vector<aura::ast::NodeId>{form_quote, result});
            return result;
        }
        return s.flat.add_quote(expr);
    }

    // Empty list: (quote ())
    if (v.children.empty()) {
        return s.flat.add_quote(expr);
    }

    // Handle unquote at depth 0: just return the inner expression
    if (depth == 0 && is_unquote(s.flat, s.pool, expr)) {
        if (v.children.size() > 1)
            return v.child(1);
        return expr;
    }

    // Handle unquote at depth > 0: (quote (unquote ...))
    if (depth > 0 && is_unquote(s.flat, s.pool, expr)) {
        if (v.children.size() > 1) {
            auto inner = expand_qq(s, v.child(1), depth - 1);
            auto unq_var = s.flat.add_variable(s.pool.intern("unquote"));
            return s.flat.add_quote(
                s.flat.add_call(unq_var, std::vector<aura::ast::NodeId>{inner}));
        }
        return s.flat.add_quote(expr);
    }

    // Handle unquote-splicing at depth 0: return the inner expression
    if (depth == 0 && is_unquote_splicing(s.flat, s.pool, expr)) {
        if (v.children.size() > 1)
            return v.child(1);
        return expr;
    }

    // Handle unquote-splicing at depth > 0: (quote (unquote-splicing ...))
    if (depth > 0 && is_unquote_splicing(s.flat, s.pool, expr)) {
        if (v.children.size() > 1) {
            auto inner = expand_qq(s, v.child(1), depth - 1);
            auto unsplice_var = s.flat.add_variable(s.pool.intern("unquote-splicing"));
            return s.flat.add_quote(
                s.flat.add_call(unsplice_var, std::vector<aura::ast::NodeId>{inner}));
        }
        return s.flat.add_quote(expr);
    }

    // Handle nested quasiquote
    if (is_quasiquote(s.flat, s.pool, expr)) {
        if (v.children.size() > 1) {
            auto inner = expand_qq(s, v.child(1), depth + 1);
            auto qq_var = s.flat.add_variable(s.pool.intern("quasiquote"));
            return s.flat.add_call(qq_var, std::vector<aura::ast::NodeId>{inner});
        }
    }

    // Pair/list: expand all children
    return expand_qq_pair(s, expr, depth);
}

NodeId expand_qq_pair(ParserState& s, NodeId expr, int depth) {
    auto v = s.flat.get(expr);

    // Build from right to left, starting with (quote ()), consing each element
    NodeId result = s.flat.add_quote(s.flat.add_literal(0)); // (quote ())

    for (int i = static_cast<int>(v.children.size()) - 1; i >= 0; --i) {
        auto child = v.child(i);

        // Handle unquote-splicing at depth 0: (append expr result)
        if (depth == 0 && is_unquote_splicing(s.flat, s.pool, child)) {
            auto child_v = s.flat.get(child);
            auto spliced = child_v.children.size() > 1 ? child_v.child(1) : child;
            auto append_var = s.flat.add_variable(s.pool.intern("append"));
            result = s.flat.add_call(append_var, std::vector<aura::ast::NodeId>{spliced, result});
        } else {
            auto expanded = expand_qq(s, child, depth);
            auto cons_var = s.flat.add_variable(s.pool.intern("cons"));
            result = s.flat.add_call(cons_var, std::vector<aura::ast::NodeId>{expanded, result});
        }
    }

    return result;
}

// ── Free function ──────────────────────────────────────────────
} // namespace aura::parser::detail

namespace aura::parser {

using namespace aura::ast;

// Public API: pure function. Constructs a ParserState (stack
// allocation, no hidden state visible to caller), walks the
// source, returns the result. The ParserState is destroyed on
// return — no state escapes this function.
FlatParseResult parse_to_flat(std::string_view source, FlatAST& flat, StringPool& pool) {
    detail::ParserState s{flat, pool, Lexer(source), 0};
    auto result = detail::parse(s, source);
    // Issue #273: define-module (and similar) append body exprs via
    // insert_child, which bumps generation_ per op. Refresh all live
    // node_gen_ entries so eval_flat / typecheck see valid NodeIds.
    if (result.success && result.root != aura::ast::NULL_NODE)
        flat.restamp_all_node_generations();
    return result;
}

} // namespace aura::parser
