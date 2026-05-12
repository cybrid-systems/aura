export module aura.parser.lexer;
import std;

namespace aura::parser {

export enum class TokenKind { Integer, String, Identifier, LParen, RParen, EndOfFile, Error };
export struct Token { TokenKind kind; std::string_view text; std::uint32_t line = 0, column = 0; bool is(TokenKind k) const { return kind == k; } };

export class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}
    Token peek(); Token consume(); bool eof() { return peek().kind == TokenKind::EndOfFile; }
private:
    Token advance(); Token read_string(); Token read_number(); Token read_identifier(); void skip_ws(); Token make_tok(TokenKind k, std::string_view t);
    std::string_view source_; std::size_t pos_ = 0; std::uint32_t line_ = 1, col_ = 1; bool peeked_ = false; Token peek_token_ = {};
};

} // namespace aura::parser
