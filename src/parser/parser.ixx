export module aura.parser.parser;
import std;
import aura.core;
import aura.diag;
import aura.parser.lexer;

namespace aura::parser {

// Result from FlatParser (direct FlatAST, no Expr* intermediate)
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

// ── FlatParser — writes directly to FlatAST (SoA), bypasses Expr* ─
export class FlatParser {
public:
    FlatParser(aura::ast::FlatAST& flat, aura::ast::StringPool& pool)
        : flat_(flat)
        , pool_(pool) {}
    FlatParseResult parse(std::string_view source);

private:
    aura::ast::NodeId parse_expr();
    aura::ast::NodeId parse_number(Token tok);
    aura::ast::NodeId parse_float(Token tok);
    aura::ast::NodeId parse_list();
    aura::ast::NodeId parse_if();
    aura::ast::NodeId parse_lambda();
    aura::ast::NodeId parse_let(bool rec);
    aura::ast::NodeId parse_named_let();
    aura::ast::NodeId parse_let_star();
    aura::ast::NodeId parse_define();
    aura::ast::NodeId parse_define_type();
    aura::ast::NodeId parse_define_module();
    aura::ast::NodeId parse_begin();
    aura::ast::NodeId parse_set();
    aura::ast::NodeId parse_quote();
    aura::ast::NodeId parse_cond();
    aura::ast::NodeId parse_defmacro();
    aura::ast::NodeId parse_match();
    aura::ast::NodeId parse_linear();
    aura::ast::NodeId parse_move();
    aura::ast::NodeId parse_borrow();
    aura::ast::NodeId parse_mut_borrow();
    aura::ast::NodeId parse_drop();
    aura::ast::NodeId parse_cast();
    aura::ast::NodeId parse_check();
    aura::ast::NodeId parse_type_annot();
    aura::ast::NodeId parse_val();
    aura::ast::NodeId expand_qq(aura::ast::NodeId expr, int depth);
    aura::ast::NodeId expand_qq_pair(aura::ast::NodeId expr, int depth);
    std::vector<std::pair<aura::ast::SymId, aura::ast::NodeId>>
    compile_pattern(aura::ast::NodeId pattern_node, aura::ast::SymId tmp,
                    aura::ast::NodeId* out_test);
    std::vector<aura::ast::NodeId> parse_bindings();
    void skip_rparen();
    aura::ast::FlatAST& flat_;
    aura::ast::StringPool& pool_;
    std::optional<Lexer> lexer_;
    std::size_t parse_depth_ = 0;
};

// Free function — parse directly into FlatAST.
export FlatParseResult parse_to_flat(std::string_view source, aura::ast::FlatAST& flat,
                                     aura::ast::StringPool& pool);

} // namespace aura::parser
