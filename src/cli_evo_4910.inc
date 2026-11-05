// cli_evo_4910.inc — Issue #4910: `aura evo log` / `aura evo report`.
// Included by main.cpp after the #4913 project helpers. Reads the JSONL the
// runtime appends (src/compiler/evo_log_4910.hh); no network, no deps.

namespace evo_cli_4910 {

// ── minimal JSON (records are our own output; tolerate anything) ──
struct J {
    // #4936: a named member type instead of std::pair<std::string, J>.
    // Clang instantiates the pair while J is still incomplete and rejects the
    // vector; a forward-declared struct completed after J is fine for both
    // compilers. Field names stay first/second so the code reads as before.
    struct Member;
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> arr;
    std::vector<Member> obj;
    std::string raw; // source text of this value
    [[nodiscard]] const J* get(std::string_view k) const;
    [[nodiscard]] std::string str(std::string_view k) const {
        const J* v = get(k);
        return v && v->t == T::Str ? v->s : std::string();
    }
    [[nodiscard]] double num(std::string_view k) const {
        const J* v = get(k);
        return v && v->t == T::Num ? v->n : 0.0;
    }
};
struct J::Member {
    std::string first;
    J second;
};
inline const J* J::get(std::string_view k) const {
    for (const auto& [key, v] : obj)
        if (key == k)
            return &v;
    return nullptr;
}

struct Parser {
    std::string_view in;
    std::size_t i = 0;
    bool ok = true;
    void ws() {
        while (i < in.size() && (in[i] == ' ' || in[i] == '\t' || in[i] == '\n' || in[i] == '\r'))
            ++i;
    }
    bool lit(std::string_view w) {
        if (in.substr(i, w.size()) == w) {
            i += w.size();
            return true;
        }
        ok = false;
        return false;
    }
    std::string string() {
        std::string out;
        ++i; // opening quote
        while (i < in.size() && in[i] != '"') {
            char c = in[i++];
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i >= in.size())
                break;
            c = in[i++];
            switch (c) {
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'u': {
                    unsigned cp = 0;
                    for (int k = 0; k < 4 && i < in.size(); ++k, ++i) {
                        const char h = in[i];
                        cp = cp * 16 + static_cast<unsigned>(h >= 'a'   ? h - 'a' + 10
                                                             : h >= 'A' ? h - 'A' + 10
                                                                        : h - '0');
                    }
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default:
                    out.push_back(c);
            }
        }
        if (i < in.size())
            ++i; // closing quote
        else
            ok = false;
        return out;
    }
    J value(int depth = 0) {
        J v;
        ws();
        const std::size_t start = i;
        if (i >= in.size() || depth > 64) {
            ok = false;
            return v;
        }
        const char c = in[i];
        if (c == '"') {
            v.t = J::T::Str;
            v.s = string();
        } else if (c == '{') {
            v.t = J::T::Obj;
            ++i;
            ws();
            if (i < in.size() && in[i] == '}') {
                ++i;
            } else {
                while (ok) {
                    ws();
                    if (i >= in.size() || in[i] != '"') {
                        ok = false;
                        break;
                    }
                    std::string k = string();
                    ws();
                    if (i >= in.size() || in[i] != ':') {
                        ok = false;
                        break;
                    }
                    ++i;
                    v.obj.emplace_back(std::move(k), value(depth + 1));
                    ws();
                    if (i < in.size() && in[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < in.size() && in[i] == '}')
                        ++i;
                    else
                        ok = false;
                    break;
                }
            }
        } else if (c == '[') {
            v.t = J::T::Arr;
            ++i;
            ws();
            if (i < in.size() && in[i] == ']') {
                ++i;
            } else {
                while (ok) {
                    v.arr.push_back(value(depth + 1));
                    ws();
                    if (i < in.size() && in[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < in.size() && in[i] == ']')
                        ++i;
                    else
                        ok = false;
                    break;
                }
            }
        } else if (c == 't') {
            v.t = J::T::Bool;
            v.b = lit("true");
        } else if (c == 'f') {
            v.t = J::T::Bool;
            lit("false");
        } else if (c == 'n') {
            lit("null");
        } else {
            std::size_t j = i;
            while (j < in.size() &&
                   std::string_view("+-0123456789.eE").find(in[j]) != std::string_view::npos)
                ++j;
            if (j == i) {
                ok = false;
                return v;
            }
            v.t = J::T::Num;
            const std::string num(in.substr(i, j - i));
            char* end = nullptr;
            v.n = std::strtod(num.c_str(), &end);
            i = j;
        }
        v.raw = std::string(in.substr(start, i - start));
        return v;
    }
};

struct Log {
    std::vector<J> recs;
    std::size_t bad = 0;
};

static Log load(const std::filesystem::path& p) {
    Log log;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        Parser ps{line};
        J v = ps.value();
        if (!ps.ok || v.t != J::T::Obj) {
            ++log.bad;
            continue;
        }
        log.recs.push_back(std::move(v));
    }
    return log;
}

static std::filesystem::path default_log_path() {
    if (const char* p = std::getenv("AURA_EVO_LOG"); p && *p)
        return p;
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (auto root = find_project_root_4913(cwd))
        return *root / ".aura" / "evo" / "log.jsonl";
    return cwd / ".aura" / "evo" / "log.jsonl";
}

// ── line diff (LCS; small inputs — define-sized texts) ──
struct DiffLine {
    char op; // ' ', '+', '-'
    std::string text;
};

static std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> out;
    std::size_t b = 0;
    while (b <= s.size()) {
        const auto e = s.find('\n', b);
        if (e == std::string_view::npos) {
            if (b < s.size())
                out.emplace_back(s.substr(b));
            break;
        }
        out.emplace_back(s.substr(b, e - b));
        b = e + 1;
    }
    return out;
}

// One-line S-expressions (unparse output) diff poorly by line: break them
// at top-level-ish parens so a changed subform shows as its own line.
static std::string soft_break(std::string_view s) {
    if (s.find('\n') != std::string_view::npos || s.size() < 80)
        return std::string(s);
    std::string out;
    int depth = 0;
    bool in_str = false;
    for (std::size_t k = 0; k < s.size(); ++k) {
        const char c = s[k];
        if (in_str) {
            out.push_back(c);
            if (c == '\\' && k + 1 < s.size())
                out.push_back(s[++k]);
            else if (c == '"')
                in_str = false;
            continue;
        }
        if (c == '"')
            in_str = true;
        if (c == '(' && depth >= 1 && depth <= 3 && !out.empty() && out.back() == ' ') {
            out.back() = '\n';
            out.append(static_cast<std::size_t>(depth * 2), ' ');
        }
        if (c == '(')
            ++depth;
        if (c == ')')
            --depth;
        out.push_back(c);
    }
    return out;
}

static std::vector<DiffLine> diff(std::string_view a_src, std::string_view b_src) {
    const auto a = split_lines(soft_break(a_src));
    const auto b = split_lines(soft_break(b_src));
    std::vector<DiffLine> out;
    if (a.size() * b.size() > 4'000'000) { // too big: show replace
        for (const auto& l : a)
            out.push_back({'-', l});
        for (const auto& l : b)
            out.push_back({'+', l});
        return out;
    }
    const std::size_t n = a.size(), m = b.size();
    std::vector<std::uint32_t> L((n + 1) * (m + 1), 0);
    auto at = [&](std::size_t i, std::size_t j) -> std::uint32_t& { return L[i * (m + 1) + j]; };
    for (std::size_t i = n; i-- > 0;)
        for (std::size_t j = m; j-- > 0;)
            at(i, j) = a[i] == b[j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
    std::size_t i = 0, j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            out.push_back({' ', a[i]});
            ++i;
            ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            out.push_back({'-', a[i++]});
        } else {
            out.push_back({'+', b[j++]});
        }
    }
    while (i < n)
        out.push_back({'-', a[i++]});
    while (j < m)
        out.push_back({'+', b[j++]});
    return out;
}

// "old" for a record: its own, else the last KEEP'd "new" of the same target
// in the same run (user evo:record! entries carry only the candidate).
static std::vector<std::string> effective_old(const std::vector<J>& recs) {
    std::vector<std::string> out(recs.size());
    std::map<std::pair<std::string, std::string>, std::string> last;
    for (std::size_t k = 0; k < recs.size(); ++k) {
        const auto& r = recs[k];
        const auto key = std::make_pair(r.str("run"), r.str("target"));
        out[k] = r.str("old");
        if (out[k].empty())
            if (auto it = last.find(key); it != last.end())
                out[k] = it->second;
        if (r.str("decision") == "KEEP" && !r.str("new").empty())
            last[key] = r.str("new");
    }
    return out;
}

// Gate evidence summary: "k=v k=v" for a flat object, raw JSON otherwise.
static std::string evidence_text(const J* e) {
    if (!e || e->t == J::T::Null)
        return {};
    if (e->t != J::T::Obj)
        return e->t == J::T::Str ? e->s : e->raw;
    // Keys sorted: a hash's field order is not the order the program wrote.
    std::vector<const J::Member*> kv;
    for (const auto& p : e->obj)
        kv.push_back(&p);
    std::ranges::sort(kv, {}, [](const auto* p) { return std::string_view(p->first); });
    std::string out;
    for (const auto* p : kv) {
        if (!out.empty())
            out += ' ';
        out += p->first + "=" + (p->second.t == J::T::Str ? p->second.s : p->second.raw);
    }
    return out;
}

// The score plotted for a record: evidence.score, else the first numeric
// evidence field. nullopt when the record carries no number.
static std::optional<std::pair<std::string, double>> score_of(const J& r) {
    const J* e = r.get("evidence");
    if (!e)
        return std::nullopt;
    if (e->t == J::T::Num)
        return std::make_pair(std::string("score"), e->n);
    if (e->t != J::T::Obj)
        return std::nullopt;
    if (const J* s = e->get("score"); s && s->t == J::T::Num)
        return std::make_pair(std::string("score"), s->n);
    for (const auto& [k, v] : e->obj)
        if (v.t == J::T::Num)
            return std::make_pair(k, v.n);
    return std::nullopt;
}

static std::string short_id(const std::string& id) {
    const auto h = id.rfind('#');
    return h == std::string::npos ? id : id.substr(h);
}

static int usage(const char* argv0) {
    std::println(std::cerr, "usage: {} evo log [--json] [--all] [--run ID|last] [--log FILE]",
                 argv0);
    std::println(std::cerr, "       {} evo report --html DIR [--all] [--run ID|last] [--log FILE]",
                 argv0);
    std::println(std::cerr,
                 "hint: records come from AURA_EVO_LOG (aura run in a project defaults it to "
                 ".aura/evo/log.jsonl); see docs/evo.md");
    return 2;
}

static std::string html_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out.push_back(c);
        }
    }
    return out;
}

// A run that records its own verdicts with evo:record! counts those as its
// candidates; the automatic mutate:* / ast:restore records are still listed
// (they carry the diffs) but would double-count every evaluated candidate.
static bool user_verdicts(const std::vector<J>& recs, const std::string* run) {
    for (const auto& r : recs)
        if ((!run || r.str("run") == *run) && r.str("kind") == "record")
            return true;
    return false;
}

static int cmd_log(const std::vector<J>& recs, bool as_json) {
    if (as_json) {
        std::string out = "[";
        for (std::size_t k = 0; k < recs.size(); ++k) {
            if (k)
                out += ",\n ";
            out += recs[k].raw;
        }
        out += "]";
        std::println("{}", out);
        return 0;
    }
    const auto olds = effective_old(recs);
    std::string run;
    for (std::size_t k = 0; k < recs.size(); ++k) {
        const auto& r = recs[k];
        if (r.str("run") != run) {
            run = r.str("run");
            std::size_t keep = 0, rb = 0, n = 0;
            const bool only_user = user_verdicts(recs, &run);
            for (const auto& x : recs) {
                if (x.str("run") != run || (only_user && x.str("kind") != "record"))
                    continue;
                ++n;
                keep += x.str("decision") == "KEEP";
                rb += x.str("decision") == "ROLLBACK";
            }
            std::println("run {}  ({} candidates: {} KEEP, {} ROLLBACK)", run, n, keep, rb);
        }
        const std::string parent = r.str("parent").empty() ? "-" : short_id(r.str("parent"));
        std::println("  {:<5} {:<8} {:<16} {:<20} {:>9.3f}ms  parent {}", short_id(r.str("id")),
                     r.str("decision"), r.str("kind"), r.str("target"), r.num("dur_ms"), parent);
        if (const auto why = r.str("reason"); !why.empty())
            std::println("        reason:   {}", why);
        if (const auto ev = evidence_text(r.get("evidence")); !ev.empty())
            std::println("        evidence: {}", ev);
        const auto& neu = r.str("new");
        if (!neu.empty() || !olds[k].empty()) {
            std::size_t add = 0, del = 0;
            for (const auto& d : diff(olds[k], neu)) {
                add += d.op == '+';
                del += d.op == '-';
            }
            std::println("        diff:     +{} -{} lines", add, del);
        }
    }
    if (recs.empty())
        std::println("(no evolution records)");
    return 0;
}

static int cmd_report(const std::vector<J>& recs, const std::filesystem::path& dir,
                      const std::filesystem::path& log_path) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::println(std::cerr, "error: cannot create {}: {}", dir.string(), ec.message());
        return 1;
    }
    const auto olds = effective_old(recs);
    std::size_t keep = 0, rb = 0;
    const bool only_user = user_verdicts(recs, nullptr);
    for (const auto& r : recs) {
        if (only_user && r.str("kind") != "record")
            continue;
        keep += r.str("decision") == "KEEP";
        rb += r.str("decision") == "ROLLBACK";
    }
    // Score curve: one point per scored record, KEEP green / ROLLBACK red,
    // plus the best-so-far (worldline) step line through the KEEPs.
    std::vector<std::tuple<std::size_t, double, std::string>> pts;
    std::string metric;
    for (std::size_t k = 0; k < recs.size(); ++k)
        if (auto s = score_of(recs[k])) {
            if (metric.empty())
                metric = s->first;
            pts.emplace_back(k, s->second, recs[k].str("decision"));
        }
    std::string svg;
    if (!pts.empty()) {
        double lo = std::get<1>(pts[0]), hi = lo;
        for (const auto& p : pts) {
            lo = std::min(lo, std::get<1>(p));
            hi = std::max(hi, std::get<1>(p));
        }
        if (hi == lo) {
            hi += 1;
            lo -= 1;
        }
        const double W = 720, H = 240, pad = 36;
        const double nx = static_cast<double>(std::max<std::size_t>(recs.size() - 1, 1));
        auto X = [&](std::size_t k) { return pad + (W - 2 * pad) * static_cast<double>(k) / nx; };
        auto Y = [&](double v) { return H - pad - (H - 2 * pad) * (v - lo) / (hi - lo); };
        svg = std::format("<svg viewBox=\"0 0 {} {}\" width=\"{}\" height=\"{}\" role=\"img\" "
                          "aria-label=\"score curve\">",
                          W, H, W, H);
        svg += std::format("<line x1=\"{0}\" y1=\"{1}\" x2=\"{2}\" y2=\"{1}\" class=\"ax\"/>"
                           "<line x1=\"{0}\" y1=\"{3}\" x2=\"{0}\" y2=\"{1}\" class=\"ax\"/>",
                           pad, H - pad, W - pad, pad);
        svg += std::format("<text x=\"4\" y=\"{}\" class=\"lb\">{:.4g}</text>"
                           "<text x=\"4\" y=\"{}\" class=\"lb\">{:.4g}</text>",
                           pad + 4, hi, H - pad, lo);
        std::string path;
        for (const auto& [k, v, d] : pts)
            if (d == "KEEP")
                path += std::format("{}{:.1f},{:.1f} ", path.empty() ? "M" : "L", X(k), Y(v));
        if (!path.empty())
            svg += "<path d=\"" + path + "\" class=\"tip\"/>";
        for (const auto& [k, v, d] : pts)
            svg += std::format("<circle cx=\"{:.1f}\" cy=\"{:.1f}\" r=\"4\" class=\"{}\">"
                               "<title>{} {} {}={:.6g}</title></circle>",
                               X(k), Y(v), d == "KEEP" ? "k" : "r",
                               html_escape(short_id(recs[k].str("id"))), html_escape(d),
                               html_escape(metric), v);
        svg += "</svg>";
    }
    std::string h;
    h += "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
         "<title>aura evo report</title><style>"
         "body{font:14px/1.45 system-ui,sans-serif;margin:24px;color:#1b1f24}"
         "h1{font-size:20px}table{border-collapse:collapse;width:100%;margin:12px 0}"
         "td,th{border-bottom:1px solid #d8dee4;padding:4px 8px;text-align:left;vertical-align:top}"
         ".KEEP{color:#1a7f37;font-weight:600}.ROLLBACK{color:#cf222e;font-weight:600}"
         "pre{background:#f6f8fa;padding:8px;overflow:auto;margin:4px 0}"
         ".add{background:#dafbe1}.del{background:#ffebe9}"
         ".ax{stroke:#8c959f}.lb{font-size:11px;fill:#57606a}.tip{fill:none;stroke:#1a7f37;"
         "stroke-width:2}.k{fill:#1a7f37}.r{fill:#cf222e}"
         "details{margin:6px 0}summary{cursor:pointer}</style></head><body>";
    h += "<h1>aura evo report</h1>";
    h += std::format("<p>log <code>{}</code> &middot; {} candidates &middot; <span "
                     "class=\"KEEP\">{} KEEP</span> &middot; <span class=\"ROLLBACK\">{} "
                     "ROLLBACK</span></p>",
                     html_escape(log_path.string()), recs.size(), keep, rb);
    if (!svg.empty())
        h += "<h2>Score (" + html_escape(metric) + ")</h2>" + svg;
    h += "<h2>Timeline</h2><table><tr><th>id</th><th>parent</th><th>kind</th><th>target</th>"
         "<th>decision</th><th>reason</th><th>evidence</th><th>ms</th></tr>";
    for (const auto& r : recs)
        h += std::format("<tr><td><a href=\"#c{}\">{}</a></td><td>{}</td><td>{}</td><td>{}</td>"
                         "<td class=\"{}\">{}</td><td>{}</td><td>{}</td><td>{:.3f}</td></tr>",
                         html_escape(r.str("id")), html_escape(short_id(r.str("id"))),
                         html_escape(short_id(r.str("parent"))), html_escape(r.str("kind")),
                         html_escape(r.str("target")), html_escape(r.str("decision")),
                         html_escape(r.str("decision")), html_escape(r.str("reason")),
                         html_escape(evidence_text(r.get("evidence"))), r.num("dur_ms"));
    h += "</table><h2>Candidates</h2>";
    for (std::size_t k = 0; k < recs.size(); ++k) {
        const auto& r = recs[k];
        h += std::format("<details id=\"c{}\"{}><summary><b>{}</b> <span class=\"{}\">{}</span> "
                         "{} {}</summary>",
                         html_escape(r.str("id")), r.str("decision") == "KEEP" ? " open" : "",
                         html_escape(short_id(r.str("id"))), html_escape(r.str("decision")),
                         html_escape(r.str("decision")), html_escape(r.str("kind")),
                         html_escape(r.str("target")));
        if (const auto why = r.str("reason"); !why.empty())
            h += "<p>reason: " + html_escape(why) + "</p>";
        if (const J* e = r.get("evidence"); e && e->t != J::T::Null)
            h += "<pre>" + html_escape(e->raw) + "</pre>";
        if (!r.str("new").empty() || !olds[k].empty()) {
            h += "<pre>";
            for (const auto& d : diff(olds[k], r.str("new"))) {
                const char* cls = d.op == '+' ? "add" : d.op == '-' ? "del" : "";
                h += std::format("<span class=\"{}\">{} {}</span>\n", cls, d.op,
                                 html_escape(d.text));
            }
            h += "</pre>";
        }
        h += "</details>";
    }
    h += "</body></html>\n";
    const auto out = dir / "index.html";
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f << h;
    if (!f) {
        std::println(std::cerr, "error: cannot write {}", out.string());
        return 1;
    }
    std::println("wrote {} ({} candidates)", out.string(), recs.size());
    return 0;
}

} // namespace evo_cli_4910

// `aura evo log|report …`
static int aura_evo_4910(int argc, char** argv) {
    using namespace evo_cli_4910;
    if (argc < 3)
        return usage(argv[0]);
    const std::string_view sub(argv[2]);
    bool as_json = false, all = false;
    std::string run, html, log_arg;
    for (int i = 3; i < argc; ++i) {
        const std::string_view a(argv[i]);
        auto val = [&](std::string_view flag) -> std::optional<std::string> {
            if (a == flag && i + 1 < argc)
                return std::string(argv[++i]);
            if (a.starts_with(std::string(flag) + "="))
                return std::string(a.substr(flag.size() + 1));
            return std::nullopt;
        };
        if (a == "--json") {
            as_json = true;
        } else if (a == "--all") {
            all = true;
        } else if (auto v = val("--run")) {
            run = *v;
        } else if (auto v = val("--html")) {
            html = *v;
        } else if (auto v = val("--log")) {
            log_arg = *v;
        } else {
            std::println(std::cerr, "error: unknown argument '{}'", a);
            return usage(argv[0]);
        }
    }
    if (sub != "log" && sub != "report")
        return usage(argv[0]);
    if (sub == "report" && html.empty()) {
        std::println(std::cerr, "error: evo report needs --html DIR");
        return usage(argv[0]);
    }
    const std::filesystem::path log_path =
        log_arg.empty() ? default_log_path() : std::filesystem::path(log_arg);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(log_path, ec)) {
        std::println(std::cerr, "error: no evolution log at {}", log_path.string());
        std::println(std::cerr,
                     "hint: run with AURA_EVO_LOG=<file> (or 'aura run' inside a project) so "
                     "mutate:rebind / ast:restore / evo:record! are recorded");
        return 1;
    }
    Log log = load(log_path);
    if (log.bad)
        std::println(std::cerr, "warning: skipped {} malformed line(s) in {}", log.bad,
                     log_path.string());
    if (run == "last" && !log.recs.empty())
        run = log.recs.back().str("run");
    std::vector<J> recs;
    for (auto& r : log.recs)
        if (run.empty() || r.str("run") == run)
            recs.push_back(std::move(r));
    // A run that records its verdicts with evo:record! is shown as those
    // candidates; its automatic mutate:* / ast:restore records (several per
    // candidate) are kept in --json and shown with --all.
    if (!all && !as_json) {
        std::set<std::string> with_verdicts;
        for (const auto& r : recs)
            if (r.str("kind") == "record")
                with_verdicts.insert(r.str("run"));
        std::vector<J> shown;
        for (auto& r : recs)
            if (r.str("kind") == "record" || !with_verdicts.contains(r.str("run")))
                shown.push_back(std::move(r));
        recs = std::move(shown);
    }
    if (sub == "log")
        return cmd_log(recs, as_json);
    return cmd_report(recs, html, log_path);
}
