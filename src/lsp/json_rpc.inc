// JSON-RPC 2.0 over stdio: message model, parser, framing, writer.
//
// Included by src/cli_lsp_4911.inc; this file opens its own namespace so the
// transport can be changed without touching the method handlers (#5258).
#ifndef AURA_LSP_JSON_RPC_INC
#define AURA_LSP_JSON_RPC_INC

#include <charconv>
#include <cctype>

namespace lsp_4911 {

struct Json {
    // #4936: named member type, not std::pair<std::string, Json> (Clang
    // rejects the pair of an incomplete Json; see cli_evo_4910.inc J).
    struct Member;
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<Member> o;
    [[nodiscard]] const Json* get(std::string_view k) const;
    [[nodiscard]] const Json& at(std::string_view k) const;
};
struct Json::Member {
    std::string first;
    Json second;
};
inline const Json* Json::get(std::string_view k) const {
    for (const auto& [kk, v] : o)
        if (kk == k)
            return &v;
    return nullptr;
}
inline const Json& Json::at(std::string_view k) const {
    static const Json null;
    const auto* p = get(k);
    return p ? *p : null;
}

// Issue #5155: the evolution-log reader refuses past 64 frames. This parser
// used to recurse once per bracket until the native stack died with SIGSEGV.
inline constexpr std::size_t kLspJsonMaxDepth = 64;

struct Parser {
    std::string_view s;
    std::size_t i = 0;
    std::size_t depth = 0;
    bool nesting_refused = false;
    void ws() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
    }
    bool parse(Json& out) {
        // depth > 64 matches cli_evo_4910.inc. Children see depth + 1.
        if (depth > kLspJsonMaxDepth) {
            nesting_refused = true;
            return false;
        }
        struct Guard {
            std::size_t& d;
            ~Guard() { --d; }
        };
        ws();
        if (i >= s.size())
            return false;
        const char c = s[i];
        if (c == '{') {
            out.t = Json::T::Obj;
            ++i;
            ++depth;
            Guard guard{depth};
            ws();
            if (i < s.size() && s[i] == '}')
                return ++i, true;
            while (true) {
                Json k;
                ws();
                if (!parse(k) || k.t != Json::T::Str)
                    return false;
                ws();
                if (i >= s.size() || s[i++] != ':')
                    return false;
                Json v;
                if (!parse(v))
                    return false;
                out.o.emplace_back(std::move(k.s), std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                return i < s.size() && s[i++] == '}';
            }
        }
        if (c == '[') {
            out.t = Json::T::Arr;
            ++i;
            ++depth;
            Guard guard{depth};
            ws();
            if (i < s.size() && s[i] == ']')
                return ++i, true;
            while (true) {
                Json v;
                if (!parse(v))
                    return false;
                out.a.push_back(std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                return i < s.size() && s[i++] == ']';
            }
        }
        if (c == '"') {
            out.t = Json::T::Str;
            for (++i; i < s.size() && s[i] != '"'; ++i) {
                if (s[i] != '\\') {
                    out.s += s[i];
                    continue;
                }
                if (++i >= s.size())
                    return false;
                switch (s[i]) {
                    case 'n':
                        out.s += '\n';
                        break;
                    case 't':
                        out.s += '\t';
                        break;
                    case 'r':
                        out.s += '\r';
                        break;
                    case 'b':
                        out.s += '\b';
                        break;
                    case 'f':
                        out.s += '\f';
                        break;
                    case 'u': {
                        if (i + 4 >= s.size())
                            return false;
                        unsigned cp = 0;
                        std::from_chars(s.data() + i + 1, s.data() + i + 5, cp, 16);
                        i += 4;
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.size() && s[i + 1] == '\\' &&
                            s[i + 2] == 'u') {
                            unsigned lo = 0;
                            std::from_chars(s.data() + i + 3, s.data() + i + 7, lo, 16);
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                        auto put = [&](unsigned v) { out.s += static_cast<char>(v); };
                        if (cp < 0x80) {
                            put(cp);
                        } else if (cp < 0x800) {
                            put(0xC0 | (cp >> 6));
                            put(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            put(0xE0 | (cp >> 12));
                            put(0x80 | ((cp >> 6) & 0x3F));
                            put(0x80 | (cp & 0x3F));
                        } else {
                            put(0xF0 | (cp >> 18));
                            put(0x80 | ((cp >> 12) & 0x3F));
                            put(0x80 | ((cp >> 6) & 0x3F));
                            put(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default:
                        out.s += s[i];
                }
            }
            return i < s.size() && s[i++] == '"';
        }
        if (s.substr(i, 4) == "true")
            return out.t = Json::T::Bool, out.b = true, i += 4, true;
        if (s.substr(i, 5) == "false")
            return out.t = Json::T::Bool, i += 5, true;
        if (s.substr(i, 4) == "null")
            return i += 4, true;
        const std::size_t st = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' ||
                                s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
            ++i;
        if (st == i)
            return false;
        out.t = Json::T::Num;
        out.n = std::strtod(std::string(s.substr(st, i - st)).c_str(), nullptr);
        return true;
    }
};

inline std::string q(std::string_view s) {
    return "\"" + aura::json::escape(s) + "\"";
}

// Re-encode a parsed id (number or string) for the response. A missing id is
// the caller's problem; this never dereferences a null Json (see the empty-id
// guard in the server, #5258).
inline std::string id_text(const Json& id) {
    if (id.t == Json::T::Str)
        return q(id.s);
    if (id.t == Json::T::Num)
        return std::format("{}", static_cast<long long>(id.n));
    return "null";
}

// Content-Length framing. Reads are buffered here instead of through
// std::cin so the server can wait for the next message with a timeout (the
// didChange debounce, #5258).
struct Transport {
    int in_fd = 0;
    int out_fd = 1;
    std::string in_buf;

    // Read at least `want` buffered bytes. false on EOF or a read error.
    bool fill(std::size_t want) {
        while (in_buf.size() < want) {
            char chunk[65536];
            const auto n = ::read(in_fd, chunk, sizeof chunk);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (n == 0)
                return false;
            in_buf.append(chunk, static_cast<std::size_t>(n));
        }
        return true;
    }

    // Issue #5495: the framing parser accepts only a bounded, non-negative
    // decimal Content-Length. The old strtoull path wrapped -23 to
    // 2^64-23, made erase consume zero bytes, and spun the poll loop.
    static constexpr std::size_t kMaxHeaderBytes = 8192;
    static constexpr std::size_t kMaxBodyBytes = 64 * 1024 * 1024;

    struct HeadParse {
        bool complete = false;
        bool valid = false;
        std::size_t head_end = 0;
        std::size_t len = 0;
    };

    static bool header_line_is_length(std::string_view line) {
        constexpr std::string_view kKey = "content-length:";
        if (line.size() < kKey.size())
            return false;
        for (std::size_t i = 0; i < kKey.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(line[i])) != kKey[i])
                return false;
        }
        return true;
    }

    [[nodiscard]] HeadParse parse_head() const {
        HeadParse out{};
        out.head_end = in_buf.find("\r\n\r\n");
        if (out.head_end == std::string::npos)
            return out;
        out.complete = true;
        if (out.head_end > kMaxHeaderBytes)
            return out;
        bool have_len = false;
        std::string_view head(in_buf.data(), out.head_end);
        while (!head.empty()) {
            const auto eol = head.find("\r\n");
            const auto line = head.substr(0, eol);
            if (header_line_is_length(line)) {
                auto value = line.substr(15);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.remove_prefix(1);
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                    value.remove_suffix(1);
                std::size_t parsed = 0;
                const auto [ptr, ec] =
                    std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (ec != std::errc{} || ptr != value.data() + value.size() || parsed == 0 ||
                    parsed > kMaxBodyBytes)
                    return out;
                if (have_len && parsed != out.len)
                    return out;
                out.len = parsed;
                have_len = true;
            }
            if (eol == std::string_view::npos)
                break;
            head.remove_prefix(eol + 2);
        }
        out.valid = have_len;
        return out;
    }

    // Bytes already buffered that form one complete frame. One read() can
    // deliver several frames; the extras must not wait for the pipe. A
    // complete but invalid header returns true so read_message reports it
    // instead of leaving poll to wait forever.
    bool has_framed_message() const {
        const auto head = parse_head();
        if (!head.complete)
            return false;
        if (!head.valid)
            return true;
        return in_buf.size() >= head.head_end + 4 + head.len;
    }

    // Wait up to `timeout_ms` (-1 = forever) for one message. Returns false
    // only on timeout.
    bool wait_input(int timeout_ms) {
        if (has_framed_message())
            return true;
        struct pollfd pfd{in_fd, POLLIN, 0};
        for (;;) {
            const int r = ::poll(&pfd, 1, timeout_ms);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            return r > 0;
        }
    }

    std::optional<std::string> read_message() {
        std::size_t head_end;
        while ((head_end = in_buf.find("\r\n\r\n")) == std::string::npos) {
            if (in_buf.size() > kMaxHeaderBytes)
                return std::nullopt;
            if (!fill(in_buf.size() + 1))
                return std::nullopt;
        }
        const auto head = parse_head();
        if (!head.complete || !head.valid)
            return std::nullopt;
        if (!fill(head.head_end + 4 + head.len))
            return std::nullopt;
        std::string body = in_buf.substr(head.head_end + 4, head.len);
        in_buf.erase(0, head.head_end + 4 + head.len);
        return body;
    }

    bool send(const std::string& body) {
        const std::string msg = std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
        std::size_t off = 0;
        while (off < msg.size()) {
            const auto n = ::write(out_fd, msg.data() + off, msg.size() - off);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (n == 0)
                return false;
            off += static_cast<std::size_t>(n);
        }
        return true;
    }
};

} // namespace lsp_4911

#endif // AURA_LSP_JSON_RPC_INC
