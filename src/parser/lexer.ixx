module;
#include <cstddef>
#include <cstdint>

export module aura.parser.lexer;
import std;

namespace aura::parser {

// One file inside a buffer that may concatenate several (LSP / --typecheck
// follow (require …) by splicing texts). `offset` is the byte where this
// file begins; `line` is that byte's 1-based line in the file. The lexer
// snaps its file-line counter to `line` on entry, so a message that names a
// line names the file. Token::line stays the buffer line — callers already
// map that coordinate back to a range. An empty span is a single file whose
// first line is 1.
export struct SourceLineOrigin {
    std::size_t offset = 0;
    std::uint32_t line = 1;
};

export enum class TokenKind {
    Integer,
    Float,
    String,
    Identifier,
    LParen,
    RParen,
    Quote,
    QuasiQuote,
    Unquote,
    UnquoteSplicing,
    Dot,
    Ellipsis,
    Bool,
    HashLParen,
    Character, // Issue #4129: #\ character literal; token text = decimal code point
    EndOfFile,
    Error
};
// Issue #5199: the '(' that a missing ')' belongs to. followed_by_semi is
// true when the next source character is ';', so "(;" is a real list whose
// rest-of-line was commented out, not a file-header comment.
export struct OpenParenLoc {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    bool followed_by_semi = false;
    // Line in the file that owns this '('. Equals `line` for a single file.
    // #5347: the unclosed-list message prints this, not the buffer line.
    std::uint32_t file_line = 0;
};

export struct Token {
    TokenKind kind;
    std::string_view text;
    std::uint32_t line = 0, column = 0;
    // Issue #4806: Identifier spelled |...|. Same symbol as the bare spelling,
    // but never subject to reader-level syntax such as the &x borrow macro.
    bool quoted = false;
    bool is(TokenKind k) const { return kind == k; }
};

export class Lexer {
public:
    explicit Lexer(std::string_view source, std::span<const SourceLineOrigin> origins = {});
    Token peek();
    Token consume();
    bool eof() { return peek().kind == TokenKind::EndOfFile; }
    // Innermost '(' / "#(" not yet closed. Empty at top level between forms.
    [[nodiscard]] std::optional<OpenParenLoc> innermost_open_paren() const;

private:
    Token advance();
    Token read_string();
    Token read_number();
    Token read_identifier();
    Token read_pipe_symbol();
    Token read_character();
    void skip_ws();
    Token make_tok(TokenKind k, std::string_view t);
    // Enter a SourceLineOrigin: the file-line counter becomes that file's
    // baseline. A newline that merely ended the previous file does not count
    // as a line of the next one.
    void adopt_line_origin();
    void bump_line();
    std::string_view source_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1, col_ = 1; // buffer line (locations / ranges)
    std::uint32_t file_line_ = 1;      // line in the current file (messages)
    std::span<const SourceLineOrigin> origins_{};
    std::size_t origin_i_ = 0;
    bool origin_armed_ = false;
    bool peeked_ = false;
    Token peek_token_ = {};
    // Issue #4875: an unclosed #| block comment is a parse error, not EOF.
    bool unterminated_block_comment_ = false;
    // Start of that comment, so the diagnostic points at the '#|' rather
    // than at end of file (skip_ws advances line_/col_ to EOF first).
    std::uint32_t unterminated_block_comment_line_ = 0;
    std::uint32_t unterminated_block_comment_column_ = 0;
    std::string string_buf_; // temp storage for escape-resolved string tokens
    // Issue #5199: pushed when '(' or "#(" is tokenized, popped on ')'.
    std::vector<OpenParenLoc> open_parens_;
};

} // namespace aura::parser
