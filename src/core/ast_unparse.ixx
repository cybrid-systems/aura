// ast_unparse.ixx — FlatAST → S-expression source (Issue #2922)
//
// Extracted from the inline recursive lambda in kCurrentSource
// (evaluator_primitives_eval.cpp). Callers: (current-source),
// ast:snapshot / ast:diff workspace source, CompilerService
// get_workspace_source_fn, and unit tests without the interpreter.
//
// Non-goals: comment/whitespace fidelity, rustfmt-class layout.
// Default options preserve the historical single-line compact form
// used by (current-source) / roundtrip tests (#2921).

module;

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

export module aura.core.ast_unparse;

import std;
import aura.core.ast;

export namespace aura::ast {

/// Options for `unparse_to_string` (Issue #2922).
struct UnparseOptions {
    /// Multi-line indented output for nested let / lambda / begin / if.
    bool pretty = false;
    /// Spaces per indent level when `pretty` is true (uses the former unused indent).
    int indent_width = 2;
    /// Depth cap; deeper nodes become `"..."`.
    int max_depth = 256;
    /// Prefer `(define (f x) body)` when value is a lambda (agent sugar).
    bool define_fn_sugar = false;
};

namespace detail {

    struct UnparseCtx {
        const FlatAST& flat;
        const StringPool& pool;
        UnparseOptions opts;
        std::string out;

        void append(std::string_view s) { out.append(s); }
        void append(char c) { out.push_back(c); }

        void append_int(std::int64_t v) {
            char buf[32];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
            if (ec == std::errc{})
                out.append(buf, static_cast<std::size_t>(ptr - buf));
            else
                out.append(std::to_string(v));
        }

        void append_float(double v) {
            auto s = std::to_string(v);
            if (s.find('.') == std::string::npos)
                s += ".0";
            out.append(s);
        }

        void indent(int level) {
            if (!opts.pretty || level <= 0)
                return;
            const auto n = static_cast<std::size_t>(level) *
                           static_cast<std::size_t>(opts.indent_width > 0 ? opts.indent_width : 2);
            out.append(n, ' ');
        }

        void sep(int level, bool force_nl = false) {
            if (opts.pretty && force_nl) {
                append('\n');
                indent(level);
            } else {
                append(' ');
            }
        }

        void escape_string(std::string_view raw) {
            append('"');
            for (unsigned char c : raw) {
                switch (c) {
                    case '\\':
                        append("\\\\");
                        break;
                    case '"':
                        append("\\\"");
                        break;
                    case '\n':
                        append("\\n");
                        break;
                    case '\t':
                        append("\\t");
                        break;
                    case '\r':
                        append("\\r");
                        break;
                    default:
                        if (c < 0x20) {
                            append("\\x");
                            static constexpr char hex[] = "0123456789abcdef";
                            append(hex[(c >> 4) & 0xF]);
                            append(hex[c & 0xF]);
                        } else {
                            append(static_cast<char>(c));
                        }
                        break;
                }
            }
            append('"');
        }

        static std::string_view coercion_type_name(std::int64_t type_tag) noexcept {
            switch (type_tag) {
                case 0:
                    return "Int";
                case 1:
                    return "String";
                case 2:
                    return "Bool";
                case 3:
                    return "Any";
                default:
                    return "Any";
            }
        }

        // Compound forms get pretty children on their own lines.
        bool wants_pretty_children(NodeTag tag) const noexcept {
            if (!opts.pretty)
                return false;
            switch (tag) {
                case NodeTag::Lambda:
                case NodeTag::Let:
                case NodeTag::LetRec:
                case NodeTag::Begin:
                case NodeTag::IfExpr:
                case NodeTag::Define:
                case NodeTag::DefineModule:
                case NodeTag::MacroDef:
                case NodeTag::DefineType:
                    return true;
                default:
                    return false;
            }
        }

        void emit_params(std::span<const SymId> params, bool dotted) {
            const auto n = params.size();
            for (std::size_t i = 0; i < n; ++i) {
                if (dotted && n >= 1 && i == n - 1) {
                    if (n > 1)
                        append(' ');
                    append(". ");
                    append(pool.resolve(params[i]));
                } else {
                    if (i > 0)
                        append(' ');
                    append(pool.resolve(params[i]));
                }
            }
        }

        void unparse_proper_list(NodeId list_id, int depth) {
            append('(');
            bool first = true;
            auto cur = list_id;
            while (cur != NULL_NODE && cur < flat.size()) {
                auto lv = flat.get(cur);
                if (lv.tag == NodeTag::Pair && lv.children.size() >= 2) {
                    if (!first)
                        append(' ');
                    first = false;
                    emit(lv.child(0), depth + 1, /*indent_level=*/0);
                    cur = lv.child(1);
                } else if (lv.tag == NodeTag::LiteralInt && lv.int_value == 0 &&
                           flat.marker(cur) != SyntaxMarker::BoolLiteral) {
                    break; // nil sentinel
                } else {
                    if (!first)
                        append(' ');
                    append(". ");
                    emit(cur, depth + 1, /*indent_level=*/0);
                    break;
                }
            }
            append(')');
        }

        void emit(NodeId id, int depth, int indent_level) {
            if (depth > opts.max_depth) {
                append("...");
                return;
            }
            if (id == NULL_NODE || id >= flat.size()) {
                append("()");
                return;
            }
            auto v = flat.get(id);
            const bool multi = wants_pretty_children(v.tag);
            const int child_indent = indent_level + 1;

            switch (v.tag) {
                case NodeTag::LiteralInt: {
                    if (flat.marker(id) == SyntaxMarker::BoolLiteral)
                        append(v.int_value ? "#t" : "#f");
                    else
                        append_int(v.int_value);
                    return;
                }
                case NodeTag::LiteralFloat: {
                    append_float(v.float_value);
                    return;
                }
                case NodeTag::LiteralString: {
                    escape_string(pool.resolve(v.sym_id));
                    return;
                }
                case NodeTag::Variable:
                    append(pool.resolve(v.sym_id));
                    return;
                case NodeTag::Call: {
                    append('(');
                    for (std::size_t i = 0; i < v.children.size(); ++i) {
                        if (i > 0) {
                            if (opts.pretty && v.children.size() > 2 && i > 0) {
                                // Mild pretty for long calls: break after callee.
                                if (i == 1)
                                    sep(child_indent, /*force_nl=*/true);
                                else
                                    sep(child_indent, /*force_nl=*/false);
                            } else {
                                append(' ');
                            }
                        }
                        emit(v.child(i), depth + 1, child_indent);
                    }
                    append(')');
                    return;
                }
                case NodeTag::Lambda: {
                    const bool dotted = v.int_value != 0;
                    append("(lambda (");
                    emit_params(v.params, dotted);
                    append(')');
                    if (!v.children.empty()) {
                        sep(child_indent, multi);
                        emit(v.child(0), depth + 1, child_indent);
                    }
                    append(')');
                    return;
                }
                case NodeTag::Let:
                case NodeTag::LetRec: {
                    append('(');
                    append(v.tag == NodeTag::LetRec ? "letrec" : "let");
                    append(" ((");
                    append(pool.resolve(v.sym_id));
                    append(' ');
                    if (!v.children.empty())
                        emit(v.child(0), depth + 1, child_indent);
                    append("))");
                    if (v.children.size() > 1) {
                        sep(child_indent, multi);
                        emit(v.child(1), depth + 1, child_indent);
                    }
                    append(')');
                    return;
                }
                case NodeTag::Define: {
                    // Optional sugar: (define (f x) body) when value is lambda.
                    if (opts.define_fn_sugar && !v.children.empty()) {
                        auto cv = flat.get(v.child(0));
                        if (cv.tag == NodeTag::Lambda) {
                            const bool dotted = cv.int_value != 0;
                            append("(define (");
                            append(pool.resolve(v.sym_id));
                            if (!cv.params.empty()) {
                                append(' ');
                                emit_params(cv.params, dotted);
                            }
                            append(')');
                            if (!cv.children.empty()) {
                                sep(child_indent, multi);
                                emit(cv.child(0), depth + 1, child_indent);
                            }
                            append(')');
                            return;
                        }
                    }
                    append("(define ");
                    append(pool.resolve(v.sym_id));
                    if (v.children.empty()) {
                        append(" ()");
                    } else {
                        sep(child_indent, multi && flat.get(v.child(0)).tag == NodeTag::Lambda);
                        emit(v.child(0), depth + 1, child_indent);
                    }
                    append(')');
                    return;
                }
                case NodeTag::IfExpr: {
                    append("(if");
                    for (std::size_t i = 0; i < v.children.size(); ++i) {
                        sep(child_indent, multi && i > 0);
                        emit(v.child(i), depth + 1, child_indent);
                    }
                    append(')');
                    return;
                }
                case NodeTag::Begin: {
                    append("(begin");
                    for (std::size_t i = 0; i < v.children.size(); ++i) {
                        sep(child_indent, multi);
                        emit(v.child(i), depth + 1, child_indent);
                    }
                    append(')');
                    return;
                }
                case NodeTag::Set: {
                    append("(set! ");
                    append(pool.resolve(v.sym_id));
                    append(' ');
                    if (v.children.empty())
                        append("()");
                    else
                        emit(v.child(0), depth + 1, child_indent);
                    append(')');
                    return;
                }
                case NodeTag::Quote: {
                    append("(quote ");
                    if (v.children.empty())
                        append("()");
                    else
                        emit(v.child(0), depth + 1, child_indent);
                    append(')');
                    return;
                }
                case NodeTag::Pair: {
                    append('(');
                    if (v.children.empty()) {
                        append("()");
                    } else {
                        emit(v.child(0), depth + 1, child_indent);
                        append(" . ");
                        emit(v.child(1), depth + 1, child_indent);
                    }
                    append(')');
                    return;
                }
                case NodeTag::DefineModule: {
                    append("(define-module (");
                    append(pool.resolve(v.sym_id));
                    for (auto pid : v.params) {
                        append(' ');
                        append(pool.resolve(pid));
                    }
                    append(')');
                    for (auto cid : v.children) {
                        sep(child_indent, multi);
                        emit(cid, depth + 1, child_indent);
                    }
                    append(')');
                    return;
                }
                case NodeTag::Export: {
                    append("(export");
                    for (auto pid : v.params) {
                        append(' ');
                        append(pool.resolve(pid));
                    }
                    append(')');
                    return;
                }
                case NodeTag::MacroDef: {
                    append("(defmacro (");
                    append(pool.resolve(v.sym_id));
                    for (auto pid : v.params) {
                        append(' ');
                        append(pool.resolve(pid));
                    }
                    append(')');
                    if (!v.children.empty()) {
                        sep(child_indent, multi);
                        emit(v.child(0), depth + 1, child_indent);
                    }
                    append(')');
                    return;
                }
                // ── Issue #2919 P0: type / coercion / linear / define-type ──
                case NodeTag::TypeAnnotation: {
                    const auto type_name = pool.resolve(v.sym_id);
                    if (v.int_value != 0) {
                        auto var_name = pool.resolve(static_cast<SymId>(v.int_value));
                        append("(: ");
                        append(var_name);
                        append(' ');
                        append(type_name);
                        append(' ');
                        if (v.children.empty())
                            append("()");
                        else
                            emit(v.child(0), depth + 1, child_indent);
                        append(')');
                        return;
                    }
                    if (!v.children.empty()) {
                        auto iv = flat.get(v.child(0));
                        if (iv.tag == NodeTag::Variable) {
                            append("(: ");
                            append(pool.resolve(iv.sym_id));
                            append(' ');
                            append(type_name);
                            append(')');
                            return;
                        }
                    }
                    append("(check ");
                    if (v.children.empty())
                        append("()");
                    else
                        emit(v.child(0), depth + 1, child_indent);
                    append(" : ");
                    append(type_name);
                    append(')');
                    return;
                }
                case NodeTag::Coercion: {
                    append("(cast ");
                    if (v.children.empty())
                        append("()");
                    else
                        emit(v.child(0), depth + 1, child_indent);
                    append(" : ");
                    append(coercion_type_name(v.int_value));
                    append(')');
                    return;
                }
                case NodeTag::DefineType: {
                    append("(define-type ");
                    if (v.params.empty()) {
                        append(pool.resolve(v.sym_id));
                    } else {
                        append('(');
                        append(pool.resolve(v.sym_id));
                        for (auto pid : v.params) {
                            append(' ');
                            append(pool.resolve(pid));
                        }
                        append(')');
                    }
                    for (auto cid : v.children) {
                        if (cid == NULL_NODE || cid >= flat.size())
                            continue;
                        auto cv = flat.get(cid);
                        append(' ');
                        if (cv.tag == NodeTag::Quote && !cv.children.empty()) {
                            unparse_proper_list(cv.child(0), depth + 1);
                        } else {
                            emit(cid, depth + 1, child_indent);
                        }
                    }
                    append(')');
                    return;
                }
                case NodeTag::Linear: {
                    append("(Linear ");
                    if (v.children.empty())
                        append("()");
                    else
                        emit(v.child(0), depth + 1, child_indent);
                    append(')');
                    return;
                }
                case NodeTag::Move: {
                    append("(move ");
                    if (v.children.empty())
                        append("()");
                    else
                        emit(v.child(0), depth + 1, child_indent);
                    append(')');
                    return;
                }
                case NodeTag::Borrow: {
                    append("(borrow ");
                    if (v.children.empty())
                        append("()");
                    else
                        emit(v.child(0), depth + 1, child_indent);
                    append(')');
                    return;
                }
                case NodeTag::MutBorrow: {
                    append("(mut-borrow ");
                    if (v.children.empty())
                        append("()");
                    else
                        emit(v.child(0), depth + 1, child_indent);
                    append(')');
                    return;
                }
                case NodeTag::Drop: {
                    append("(drop ");
                    if (v.children.empty())
                        append("()");
                    else
                        emit(v.child(0), depth + 1, child_indent);
                    append(')');
                    return;
                }
                default: {
                    // Fallback: generic node dump for unknown / SV tags (P1).
                    // P0 production tags must not reach here (see #2919 AC).
                    append('<');
                    append_int(static_cast<std::int64_t>(static_cast<int>(v.tag)));
                    append('>');
                    return;
                }
            }
        }
    };

} // namespace detail

/// Unparse `root` under `flat`/`pool` into source text.
/// Allocation: single growing `std::string` (no recursive `operator+`).
[[nodiscard]] inline std::string unparse_to_string(const FlatAST& flat, const StringPool& pool,
                                                   NodeId root, UnparseOptions opts = {}) {
    detail::UnparseCtx ctx{flat, pool, opts, {}};
    // Rough reserve: ~16 bytes per node reduces reallocs on large trees.
    const auto n = flat.size();
    if (n > 0 && n < 1'000'000)
        ctx.out.reserve(static_cast<std::size_t>(n) * 16u);
    ctx.emit(root, /*depth=*/0, /*indent_level=*/0);
    return std::move(ctx.out);
}

/// Unparse the FlatAST root.
[[nodiscard]] inline std::string unparse_to_string(const FlatAST& flat, const StringPool& pool,
                                                   UnparseOptions opts = {}) {
    return unparse_to_string(flat, pool, flat.root, opts);
}

} // namespace aura::ast
