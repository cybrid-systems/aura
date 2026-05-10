module;
#include <string>
#include <string_view>
#include <vector>
#include <cctype>
#include <cstdint>

export module aura.parser.lexer;

namespace aura::parser {

export enum class TokenKind {
    Integer,
    Identifier,
    LParen, RParen,
    EndOfFile,
    Error
};

export struct Token {
    TokenKind kind;
    std::string_view text;
    uint32_t line   = 0;
    uint32_t column = 0;

    bool is(TokenKind k) const { return kind == k; }
};

export class Lexer {
public:
    explicit Lexer(std::string_view source)
        : source_(source)
    {}

    Token peek();
    Token consume();
    bool eof() { return peek().kind == TokenKind::EndOfFile; }

private:
    Token advance();
    Token read_number();
    Token read_identifier();
    void skip_whitespace();
    Token make_token(TokenKind kind, std::string_view text);

    std::string_view source_;
    size_t pos_ = 0;
    uint32_t line_ = 1, col_ = 1;
    bool peeked_ = false;
    Token peek_token_ = {};
};

} // namespace aura::parser
