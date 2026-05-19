module aura.parser.lexer;
import std;

namespace aura::parser {

Token Lexer::advance() {
    skip_ws();
    if (pos_ >= source_.size()) return make_tok(TokenKind::EndOfFile, "");
    char c = source_[pos_];
    switch (c) {
    case '(': return make_tok(TokenKind::LParen, source_.substr(pos_++, 1));
    case ')': return make_tok(TokenKind::RParen, source_.substr(pos_++, 1));
    case '\'': return make_tok(TokenKind::Quote, source_.substr(pos_++, 1));
    case '`': return make_tok(TokenKind::QuasiQuote, source_.substr(pos_++, 1));
    case ',': {
        if (pos_ + 1 < source_.size() && source_[pos_ + 1] == '@') {
            pos_ += 2;
            return make_tok(TokenKind::UnquoteSplicing, ",@");
        }
        return make_tok(TokenKind::Unquote, source_.substr(pos_++, 1));
    }
    case '.': {
        if (pos_ + 2 < source_.size() && source_[pos_+1] == '.' && source_[pos_+2] == '.') {
            auto tok = make_tok(TokenKind::Ellipsis, "...");
            pos_ += 3;
            return tok;
        }
        return make_tok(TokenKind::Dot, source_.substr(pos_++, 1));
    }
    default:
        if (std::isdigit((unsigned char)c) || (c == '-' && pos_+1<source_.size() && std::isdigit((unsigned char)source_[pos_+1]))) return read_number();
        if (std::isalpha((unsigned char)c) || c == '_' || c == '+' || c == '*' || c == '-' || c == '/' || c == '=' || c == '<' || c == '>' || c == '!' || c == '?' || c == ':') return read_identifier();
        if (c == '"') return read_string();
        // Ghuloum Step 9: #f, #t boolean literals and # identifiers (eq?) 
        if (c == '#' && pos_ + 1 < source_.size()) {
            auto next = source_[pos_ + 1];
            if (next == 'f' || next == 't') {
                pos_ += 2;
                return make_tok(TokenKind::Bool, (next == 't') ? "1" : "0");
            }
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
            switch (next) {
            case 'n':  string_buf_ += '\n'; break;
            case 't':  string_buf_ += '\t'; break;
            case 'r':  string_buf_ += '\r'; break;
            case '"': string_buf_ += '"'; break;
            case '\\': string_buf_ += '\\'; break;
            default:   string_buf_ += next; break;
            }
            pos_ += 2;
        } else {
            string_buf_ += source_[pos_];
            pos_++;
        }
    }
    if (pos_ < source_.size()) pos_++; // skip closing "
    return make_tok(TokenKind::String, string_buf_);
}

Token Lexer::read_number() {
    std::size_t s = pos_;
    if (source_[pos_] == '-') pos_++;
    while (pos_ < source_.size() && std::isdigit((unsigned char)source_[pos_])) pos_++;
    // Check for decimal point followed by digit
    if (pos_ < source_.size() && source_[pos_] == '.' && pos_ + 1 < source_.size() && std::isdigit((unsigned char)source_[pos_ + 1])) {
        pos_++; // consume '.'
        while (pos_ < source_.size() && std::isdigit((unsigned char)source_[pos_])) pos_++;
        return make_tok(TokenKind::Float, source_.substr(s, pos_-s));
    }
    return make_tok(TokenKind::Integer, source_.substr(s, pos_-s));
}
Token Lexer::read_identifier() { std::size_t s = pos_; while (pos_ < source_.size()) { char c = source_[pos_]; if (std::isalnum((unsigned char)c) || c == '_' || c == '+' || c == '*' || c == '-' || c == '/' || c == '=' || c == '<' || c == '>' || c == '!' || c == '?' || c == ':') pos_++; else break; } return make_tok(TokenKind::Identifier, source_.substr(s, pos_-s)); }
void Lexer::skip_ws() { while (pos_ < source_.size()) { char c = source_[pos_]; if (c == ' '||c=='\t') { pos_++; col_++; } else if (c == '\n') { pos_++; line_++; col_=1; } else if (c == ';') { while (pos_<source_.size() && source_[pos_]!='\n') pos_++; } else break; } }
Token Lexer::peek() { if (!peeked_) { peeked_=true; peek_token_=advance(); } return peek_token_; }
Token Lexer::consume() { if (peeked_) { peeked_=false; auto t=peek_token_; peek_token_={}; return t; } return advance(); }
Token Lexer::make_tok(TokenKind k, std::string_view t) { Token r{k,t,line_,col_}; col_+=(std::uint32_t)t.size(); return r; }

} // namespace aura::parser
