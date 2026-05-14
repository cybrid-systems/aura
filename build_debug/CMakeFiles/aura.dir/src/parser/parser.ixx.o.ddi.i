# 0 "/home/dev/code/aura/src/parser/parser.ixx"
# 1 "/home/dev/code/aura/build_debug//"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/parser/parser.ixx"
export module aura.parser.parser;
import std;
import aura.core;
import aura.parser.lexer;

namespace aura::parser {


export struct FlatParseResult {
    aura::ast::NodeId root = aura::ast::NULL_NODE;
    bool success = false;
    std::string error;
};


export class FlatParser {
public:
    FlatParser(aura::ast::FlatAST& flat, aura::ast::StringPool& pool)
        : flat_(flat), pool_(pool) {}
    FlatParseResult parse(std::string_view source);
private:
    aura::ast::NodeId parse_expr();
    aura::ast::NodeId parse_int(Token tok);
    aura::ast::NodeId parse_list();
    aura::ast::NodeId parse_if();
    aura::ast::NodeId parse_lambda();
    aura::ast::NodeId parse_let(bool rec);
    aura::ast::NodeId parse_define();
    aura::ast::NodeId parse_begin(); aura::ast::NodeId parse_set();
    aura::ast::NodeId parse_quote(); aura::ast::NodeId parse_cond(); aura::ast::NodeId parse_defmacro();
    aura::ast::NodeId parse_val();
    std::vector<aura::ast::NodeId> parse_bindings();
    void skip_rparen();
    aura::ast::FlatAST& flat_;
    aura::ast::StringPool& pool_;
    std::optional<Lexer> lexer_;
};


export FlatParseResult parse_to_flat(std::string_view source,
                                      aura::ast::FlatAST& flat,
                                      aura::ast::StringPool& pool);

}
