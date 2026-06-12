export module aura.parser.parser;
import std;
import aura.core;
import aura.diag;
import aura.parser.lexer;

namespace aura::parser {

// Result from the parser (direct FlatAST, no Expr* intermediate)
// Structured parse error with source location
export struct ParseError {
    std::string message;
    aura::diag::SourceLocation location;

    std::string format() const {
        if (location.valid())
            return std::format("{}: {}", location.format(), message);
        return message;
    }
};

export struct FlatParseResult {
    aura::ast::NodeId root = aura::ast::NULL_NODE;
    bool success = false;
    std::string error;                 // first error (backward compat, formatted string)
    std::vector<ParseError> errors;    // all parse errors with location
};

// ── Parser (Issue #161 cpp26 P0) ──
//
// Phase 2 of #161: the parser is now a PURE FUNCTION. There
// is no FlatParser class. The public entry point is the free
// function `parse_to_flat` (exported below with [[nodiscard]]).
//
// All parse logic lives in `aura::parser::detail` as free
// functions. They share mutable parse state through a
// `ParserState` struct that is constructed once in
// `parse_to_flat` and threaded through the parse. The state
// struct is intentionally NOT exported — it is an internal
// implementation detail. From the caller's perspective, the
// API is:
//
//     FlatParseResult r = parse_to_flat(source, flat, pool);
//
// …with all state hidden inside the call. The caller can call
// it twice on the same input and get the same result
// (determinism — a property that was NOT guaranteed when
// FlatParser held mutable state).
//
// Acceptance for #161: parse_to_flat is a pure function from
// the caller's perspective. All existing tests still pass.
// Performance is identical (free functions inline at least as
// well as member functions).
namespace detail {

// ParserState: the only mutable state the parser needs.
// All parse_X functions take ParserState& as the first
// argument and access fields via `s.field`. This is the
// functional-style "explicit state threading" pattern — no
// hidden `this`, no member access.
struct ParserState {
    aura::ast::FlatAST& flat;
    aura::ast::StringPool& pool;
    Lexer lex;                         // constructed once in parse()
    std::size_t depth = 0;             // recursion depth (matches old parse_depth_)
};

// Free functions (forward declarations). Definitions live in
// parser_impl.cpp. All take ParserState& as the first param.
//
// Note: parse() is the public-ish entry point (still in detail
// namespace — not exported). parse_to_flat (the exported
// public API) constructs a ParserState, calls detail::parse(),
// and returns the result.
FlatParseResult parse(ParserState& s, std::string_view source);

aura::ast::NodeId parse_expr(ParserState& s);
aura::ast::NodeId parse_number(ParserState& s, Token tok);
aura::ast::NodeId parse_float(ParserState& s, Token tok);
aura::ast::NodeId parse_list(ParserState& s);
aura::ast::NodeId parse_if(ParserState& s);
aura::ast::NodeId parse_lambda(ParserState& s);
aura::ast::NodeId parse_let(ParserState& s, bool rec);
aura::ast::NodeId parse_named_let(ParserState& s);
aura::ast::NodeId parse_let_star(ParserState& s);
aura::ast::NodeId parse_define(ParserState& s);
aura::ast::NodeId parse_define_type(ParserState& s);
aura::ast::NodeId parse_define_module(ParserState& s);
aura::ast::NodeId parse_begin(ParserState& s);
aura::ast::NodeId parse_set(ParserState& s);
aura::ast::NodeId parse_quote(ParserState& s);
aura::ast::NodeId parse_cond(ParserState& s);
aura::ast::NodeId parse_defmacro(ParserState& s, bool hygienic = false);
aura::ast::NodeId parse_match(ParserState& s);
aura::ast::NodeId parse_linear(ParserState& s);
aura::ast::NodeId parse_move(ParserState& s);
aura::ast::NodeId parse_borrow(ParserState& s);
aura::ast::NodeId parse_mut_borrow(ParserState& s);
aura::ast::NodeId parse_datatype(ParserState& s);
aura::ast::NodeId parse_drop(ParserState& s);
aura::ast::NodeId parse_cast(ParserState& s);
aura::ast::NodeId parse_check(ParserState& s);
aura::ast::NodeId parse_type_annot(ParserState& s);
aura::ast::NodeId parse_val(ParserState& s);
aura::ast::NodeId expand_qq(ParserState& s, aura::ast::NodeId expr, int depth);
aura::ast::NodeId expand_qq_pair(ParserState& s, aura::ast::NodeId expr, int depth);
std::vector<std::pair<aura::ast::SymId, aura::ast::NodeId>>
compile_pattern(ParserState& s, aura::ast::NodeId pattern_node, aura::ast::SymId tmp,
                aura::ast::NodeId* out_test);
void skip_rparen(ParserState& s);

} // namespace detail

// ── Public API (Issue #161 cpp26 P0) ──
//
// parse_to_flat is THE parser entry point. It is a pure
// function: given (source, flat, pool), it returns a
// FlatParseResult. All mutable state is internal to the
// call (constructed as a stack-allocated detail::ParserState
// and discarded on return). Calling it twice on the same input
// yields identical results — the determinism property that
// the [[deprecated]] FlatParser class could not guarantee.
export [[nodiscard]] FlatParseResult parse_to_flat(std::string_view source, aura::ast::FlatAST& flat,
                                     aura::ast::StringPool& pool);

} // namespace aura::parser
