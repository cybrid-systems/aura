module;

module aura.parser.lexer;
import std;

namespace aura::parser {

Token Lexer::advance() {
    skip_ws();
    if (pos_ >= source_.size())
        return make_tok(TokenKind::EndOfFile, "");
    char c = source_[pos_];
    switch (c) {
        case '(':
            return make_tok(TokenKind::LParen, source_.substr(pos_++, 1));
        case ')':
            return make_tok(TokenKind::RParen, source_.substr(pos_++, 1));
        case '\'':
            return make_tok(TokenKind::Quote, source_.substr(pos_++, 1));
        case '`':
            return make_tok(TokenKind::QuasiQuote, source_.substr(pos_++, 1));
        case ',': {
            if (pos_ + 1 < source_.size() && source_[pos_ + 1] == '@') {
                pos_ += 2;
                return make_tok(TokenKind::UnquoteSplicing, ",@");
            }
            return make_tok(TokenKind::Unquote, source_.substr(pos_++, 1));
        }
        case '.': {
            if (pos_ + 2 < source_.size() && source_[pos_ + 1] == '.' && source_[pos_ + 2] == '.') {
                auto tok = make_tok(TokenKind::Ellipsis, "...");
                pos_ += 3;
                return tok;
            }
            return make_tok(TokenKind::Dot, source_.substr(pos_++, 1));
        }
        default:
            if (std::isdigit((unsigned char)c) || (c == '-' && pos_ + 1 < source_.size() &&
                                                   std::isdigit((unsigned char)source_[pos_ + 1])))
                return read_number();
            if (std::isalpha((unsigned char)c) || c == '_' || c == '+' || c == '*' || c == '-' ||
                c == '/' || c == '=' || c == '<' || c == '>' || c == '!' || c == '?' || c == ':' ||
                c == '&')
                return read_identifier();
            if (c == '"')
                return read_string();
            // Ghuloum Step 9: #f, #t boolean literals and # identifiers (eq?)
            if (c == '#' && pos_ + 1 < source_.size()) {
                auto next = source_[pos_ + 1];
                if (next == 'f' || next == 't') {
                    pos_ += 2;
                    return make_tok(TokenKind::Bool, (next == 't') ? "1" : "0");
                }
                if (next == '(') {
                    pos_ += 2;
                    return make_tok(TokenKind::HashLParen, "#(");
                }
                // Issue #4129: #\ character literals — named forms (#\space,
                // #\newline, #\tab, ...), single-character forms (#\a, #\0,
                // #\-), and optional hex #\xHH. A malformed literal (a bare
                // hash + backslash at EOF, or an unknown long name) returns
                // a TokenKind::Error whose text starts with "#\\" so the
                // parser records a parse error instead of silently
                // swallowing the token as a closing ')'.
                if (next == '\\')
                    return read_character();
            }
            return make_tok(TokenKind::Error, source_.substr(pos_++, 1));
    }
}
Token Lexer::read_string() {
    pos_++; // skip opening "
    string_buf_.clear();
    while (pos_ < source_.size() && source_[pos_] != '"') {
        if (source_[pos_] == '\\' && pos_ + 1 < source_.size()) {
            char next = source_[pos_ + 1];
            // Issue #1502: \xHH / \xH — C-style byte escape (1-2 hex digits)
            if (next == 'x' && pos_ + 2 < source_.size()) {
                auto hex_val = [](char c) -> int {
                    if (c >= '0' && c <= '9')
                        return c - '0';
                    if (c >= 'a' && c <= 'f')
                        return 10 + (c - 'a');
                    if (c >= 'A' && c <= 'F')
                        return 10 + (c - 'A');
                    return -1;
                };
                int v1 = hex_val(source_[pos_ + 2]);
                if (v1 >= 0) {
                    int byte_val = v1;
                    pos_ += 3; // past \xH
                    if (pos_ < source_.size()) {
                        int v2 = hex_val(source_[pos_]);
                        if (v2 >= 0) {
                            byte_val = (v1 << 4) | v2;
                            ++pos_;
                        }
                    }
                    string_buf_ += static_cast<char>(byte_val & 0xff);
                    continue;
                }
                // Fall through: \x not followed by hex → legacy (keep 'x')
            }
            switch (next) {
                case 'n':
                    string_buf_ += '\n';
                    break;
                case 't':
                    string_buf_ += '\t';
                    break;
                case 'r':
                    string_buf_ += '\r';
                    break;
                case '"':
                    string_buf_ += '"';
                    break;
                case '\\':
                    string_buf_ += '\\';
                    break;
                // Issue #964: \0 → NUL (not literal '0')
                case '0':
                    string_buf_ += '\0';
                    break;
                default:
                    // Unknown escape: keep the escaped char (legacy)
                    string_buf_ += next;
                    break;
            }
            pos_ += 2;
        } else {
            string_buf_ += source_[pos_];
            pos_++;
        }
    }
    if (pos_ < source_.size())
        pos_++; // skip closing "
    return make_tok(TokenKind::String, string_buf_);
}

Token Lexer::read_number() {
    std::size_t s = pos_;
    if (source_[pos_] == '-')
        pos_++;
    while (pos_ < source_.size() && std::isdigit((unsigned char)source_[pos_]))
        pos_++;
    bool is_float = false;
    // Decimal point followed by digit: 3.14
    if (pos_ < source_.size() && source_[pos_] == '.' && pos_ + 1 < source_.size() &&
        std::isdigit((unsigned char)source_[pos_ + 1])) {
        pos_++; // consume '.'
        while (pos_ < source_.size() && std::isdigit((unsigned char)source_[pos_]))
            pos_++;
        is_float = true;
    }
    // Issue #2941: scientific notation — [eE][+-]?digits
    // e.g. 1e-9, 1.0e-3, 2.5E+2, -1e2. If 'e' is not followed by a valid
    // exponent, leave 'e' for the next token (identifier).
    if (pos_ < source_.size() && (source_[pos_] == 'e' || source_[pos_] == 'E')) {
        const std::size_t exp_mark = pos_;
        pos_++; // consume e/E
        if (pos_ < source_.size() && (source_[pos_] == '+' || source_[pos_] == '-'))
            pos_++;
        if (pos_ < source_.size() && std::isdigit((unsigned char)source_[pos_])) {
            while (pos_ < source_.size() && std::isdigit((unsigned char)source_[pos_]))
                pos_++;
            is_float = true;
        } else {
            pos_ = exp_mark; // not a valid exponent — leave e/E for identifier
        }
    }
    return make_tok(is_float ? TokenKind::Float : TokenKind::Integer, source_.substr(s, pos_ - s));
}

// Issue #4129: Scheme-style character literals.
//   named:   #\space #\newline #\tab #\nul #\null #\alarm #\backspace
//            #\linefeed #\vtab #\page #\return #\escape #\altmode
//            #\delete #\rubout (case-insensitive; R7RS set + aliases)
//   single:  #\a #\0 #\- #\( #\  — one non-alphabetic or lone character
//   hex:     #\x41 / #\X41 (optionally; 'x' + >=1 hex digit)
// The token text carries the DECIMAL CODE POINT: characters ride the same
// "chars are integers" model the char primitives already use (char?,
// char->integer, char=? are int-based; string-ref yields int code points).
// No second character value model is introduced. Malformed literals return
// a TokenKind::Error whose text starts with "#\\" — the parser records a
// deferred parse error from that shape so the literal is never silently
// consumed as a closing ')'.
Token Lexer::read_character() {
    const std::size_t start = pos_;
    pos_ += 2; // consume the '#' and the backslash
    if (pos_ >= source_.size())
        return make_tok(TokenKind::Error, "#\\"); // bare #\ at EOF — malformed
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F')
            return 10 + (c - 'A');
        return -1;
    };
    char c = source_[pos_];
    // Hex form: #\xHH... — 'x'/'X' followed by at least one hex digit.
    if ((c == 'x' || c == 'X') && pos_ + 1 < source_.size() && hex_val(source_[pos_ + 1]) >= 0) {
        ++pos_;
        int v = 0;
        std::size_t digits = 0;
        while (pos_ < source_.size() && digits < 6) {
            int h = hex_val(source_[pos_]);
            if (h < 0)
                break;
            v = (v << 4) | h;
            ++pos_;
            ++digits;
        }
        string_buf_ = std::to_string(v);
        return make_tok(TokenKind::Character, string_buf_);
    }
    // Single-character form for non-alphabetic characters: #\0 #\- #\( #\ ...
    if (!std::isalpha((unsigned char)c)) {
        ++pos_;
        string_buf_ = std::to_string(static_cast<unsigned char>(c));
        return make_tok(TokenKind::Character, string_buf_);
    }
    // Alphabetic start: read the full name run, then named-form lookup.
    const std::size_t ns = pos_;
    while (pos_ < source_.size() &&
           (std::isalnum((unsigned char)source_[pos_]) || source_[pos_] == '-'))
        ++pos_;
    std::string_view name = source_.substr(ns, pos_ - ns);
    if (name.size() == 1) {
        // Lone letter: #\a #\b ...
        string_buf_ = std::to_string(static_cast<unsigned char>(name[0]));
        return make_tok(TokenKind::Character, string_buf_);
    }
    // Named form lookup (case-insensitive).
    std::string lower(name);
    for (auto& ch : lower)
        ch = static_cast<char>(std::tolower((unsigned char)ch));
    int cp = -1;
    if (lower == "nul" || lower == "null")
        cp = 0;
    else if (lower == "alarm")
        cp = 7;
    else if (lower == "backspace")
        cp = 8;
    else if (lower == "tab")
        cp = 9;
    else if (lower == "linefeed" || lower == "newline")
        cp = 10;
    else if (lower == "vtab")
        cp = 11;
    else if (lower == "page")
        cp = 12;
    else if (lower == "return")
        cp = 13;
    else if (lower == "escape" || lower == "altmode")
        cp = 27;
    else if (lower == "space")
        cp = 32;
    else if (lower == "delete" || lower == "rubout")
        cp = 127;
    if (cp < 0)
        return make_tok(TokenKind::Error, source_.substr(start, pos_ - start)); // unknown name
    string_buf_ = std::to_string(cp);
    return make_tok(TokenKind::Character, string_buf_);
}
Token Lexer::read_identifier() {
    std::size_t s = pos_;
    while (pos_ < source_.size()) {
        char c = source_[pos_];
        if (std::isalnum((unsigned char)c) || c == '_' || c == '+' || c == '*' || c == '-' ||
            c == '/' || c == '=' || c == '<' || c == '>' || c == '!' || c == '?' || c == ':' ||
            c == '&')
            pos_++;
        else
            break;
    }
    return make_tok(TokenKind::Identifier, source_.substr(s, pos_ - s));
}
void Lexer::skip_ws() {
    while (pos_ < source_.size()) {
        char c = source_[pos_];
        if (c == ' ' || c == '\t') {
            pos_++;
            col_++;
        } else if (c == '\n') {
            pos_++;
            line_++;
            col_ = 1;
        } else if (c == ';') {
            while (pos_ < source_.size() && source_[pos_] != '\n')
                pos_++;
        } else
            break;
    }
}
Token Lexer::peek() {
    if (!peeked_) {
        peeked_ = true;
        peek_token_ = advance();
    }
    return peek_token_;
}
Token Lexer::consume() {
    if (peeked_) {
        peeked_ = false;
        auto t = peek_token_;
        peek_token_ = {};
        return t;
    }
    return advance();
}
Token Lexer::make_tok(TokenKind k, std::string_view t) {
    Token r{k, t, line_, col_};
    col_ += (std::uint32_t)t.size();
    return r;
}

} // namespace aura::parser
