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

static std::string html_escape(std::string_view s); // defined below

// ── objective space: Pareto scatter + hypervolume curve (#4948) ──────
struct ObjPt {
    double x = 0, y = 0, z = 0;
    bool has_z = false;
    std::size_t seq = 0;
    std::string id;
};

static bool objectives_xyz(const J& r, double& x, double& y, double& z, bool& has_z) {
    const J* o = r.get("objectives");
    if (!o)
        return false;
    std::vector<double> vs;
    if (o->t == J::T::Arr) {
        for (const auto& v : o->arr)
            if (v.t == J::T::Num)
                vs.push_back(v.n);
    } else if (o->t == J::T::Obj) {
        for (const auto& [k, v] : o->obj)
            if (v.t == J::T::Num)
                vs.push_back(v.n);
    }
    if (vs.size() < 2)
        return false;
    x = vs[0];
    y = vs[1];
    has_z = vs.size() >= 3;
    z = has_z ? vs[2] : 0.0;
    return true;
}

static bool objectives_xy(const J& r, double& x, double& y) {
    double z = 0;
    bool has_z = false;
    return objectives_xyz(r, x, y, z, has_z);
}

static bool dominates2(const ObjPt& a, const ObjPt& b) {
    return a.x <= b.x && a.y <= b.y && (a.x < b.x || a.y < b.y);
}

static std::vector<ObjPt> pareto2(std::vector<ObjPt> pts) {
    std::vector<ObjPt> front;
    for (const auto& p : pts) {
        bool beaten = false;
        for (const auto& q : pts)
            if (dominates2(q, p)) {
                beaten = true;
                break;
            }
        if (!beaten)
            front.push_back(p);
    }
    std::ranges::sort(front, [](const ObjPt& a, const ObjPt& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    return front;
}

// Exact 2-D hypervolume (min/min) of a front against a reference corner.
static double hv2(const std::vector<ObjPt>& front, double rx, double ry) {
    if (front.empty())
        return 0.0;
    double hv = 0.0;
    for (std::size_t i = 0; i < front.size(); ++i) {
        const double x1 = i + 1 < front.size() ? front[i + 1].x : rx;
        const double w = x1 - front[i].x;
        const double h = ry - front[i].y;
        if (w > 0 && h > 0)
            hv += w * h;
    }
    return hv;
}

struct Axis2 {
    double x0 = 0, x1 = 1, y0 = 0, y1 = 1;
};

static Axis2 bounds2(const std::vector<ObjPt>& pts) {
    Axis2 b;
    if (pts.empty())
        return b;
    b.x0 = b.x1 = pts[0].x;
    b.y0 = b.y1 = pts[0].y;
    for (const auto& p : pts) {
        b.x0 = std::min(b.x0, p.x);
        b.x1 = std::max(b.x1, p.x);
        b.y0 = std::min(b.y0, p.y);
        b.y1 = std::max(b.y1, p.y);
    }
    if (b.x1 <= b.x0) {
        b.x1 = b.x0 + 1.0;
        b.x0 -= 0.5;
    }
    if (b.y1 <= b.y0) {
        b.y1 = b.y0 + 1.0;
        b.y0 -= 0.5;
    }
    return b;
}

static std::string pareto_svg(const std::vector<ObjPt>& pts, const std::vector<ObjPt>& front) {
    constexpr double W = 460, H = 340, PAD = 38;
    const Axis2 b = bounds2(pts);
    auto X = [&](double v) { return PAD + (W - 2 * PAD) * (v - b.x0) / (b.x1 - b.x0); };
    auto Y = [&](double v) { return H - PAD - (H - 2 * PAD) * (v - b.y0) / (b.y1 - b.y0); };
    std::string svg =
        std::format("<svg viewBox=\"0 0 {} {}\" width=\"{}\" height=\"{}\" role=\"img\" "
                    "aria-label=\"pareto scatter\">",
                    W, H, W, H);
    svg += std::format("<rect x=\"0\" y=\"0\" width=\"{0}\" height=\"{1}\" fill=\"#fff\"/>", W, H);
    svg += std::format("<line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" class=\"ax\"/>"
                       "<line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" class=\"ax\"/>",
                       PAD, H - PAD, W - PAD, H - PAD, PAD, H - PAD, PAD, PAD);
    double z0 = 0, z1 = 0;
    bool any_z = false;
    for (const auto& p : pts)
        if (p.has_z) {
            if (!any_z) {
                z0 = z1 = p.z;
                any_z = true;
            }
            z0 = std::min(z0, p.z);
            z1 = std::max(z1, p.z);
        }
    for (const auto& p : pts) {
        const bool on = std::any_of(front.begin(), front.end(), [&](const ObjPt& f) {
            return f.id == p.id && f.x == p.x && f.y == p.y;
        });
        const double zt = p.has_z && z1 > z0 ? (p.z - z0) / (z1 - z0) : 0.5;
        const double radius = on ? 4.5 : (any_z ? 2.0 + 3.0 * zt : 3.0);
        svg += std::format("<circle cx=\"{:.1f}\" cy=\"{:.1f}\" r=\"{:.1f}\" class=\"{}\" "
                           "fill=\"{}\"><title>{} ({:.6g}, {:.6g})</title></circle>",
                           X(p.x), Y(p.y), radius, on ? "k" : "o", on ? "#1a7f37" : "#9aa4af",
                           html_escape(short_id(p.id)), p.x, p.y);
    }
    svg += std::format("<text x=\"4\" y=\"{}\" class=\"lb\">{:.4g}</text>"
                       "<text x=\"4\" y=\"{}\" class=\"lb\">{:.4g}</text>",
                       PAD + 6, b.y1, H - PAD, b.y0);
    svg += "</svg>";
    return svg;
}

static std::string hv_svg(const std::vector<double>& hv) {
    constexpr double W = 460, H = 200, PAD = 34;
    double hi = 0.0;
    for (const double v : hv)
        hi = std::max(hi, v);
    if (hi <= 0.0)
        hi = 1.0;
    auto X = [&](std::size_t i) {
        return PAD + (W - 2 * PAD) * static_cast<double>(i) /
                         static_cast<double>(std::max<std::size_t>(hv.size() - 1, 1));
    };
    auto Y = [&](double v) { return H - PAD - (H - 2 * PAD) * (v / hi); };
    std::string path;
    for (std::size_t i = 0; i < hv.size(); ++i)
        path += std::format("{}{:.1f},{:.1f} ", i == 0 ? "M" : "L", X(i), Y(hv[i]));
    std::string svg =
        std::format("<svg viewBox=\"0 0 {} {}\" width=\"{}\" height=\"{}\" role=\"img\" "
                    "aria-label=\"hypervolume curve\">",
                    W, H, W, H);
    svg += std::format("<rect x=\"0\" y=\"0\" width=\"{0}\" height=\"{1}\" fill=\"#fff\"/>", W, H);
    svg += std::format("<line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" class=\"ax\"/>"
                       "<line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" class=\"ax\"/>",
                       PAD, H - PAD, W - PAD, H - PAD, PAD, H - PAD, PAD, PAD);
    svg += "<path d=\"" + path + "\" class=\"tip\"/>";
    svg += std::format("<text x=\"4\" y=\"{}\" class=\"lb\">{:.4g}</text>", PAD + 6, hi);
    svg += "</svg>";
    return svg;
}

static bool surrogate_record(const J& r) {
    if (r.get("surrogate"))
        return true;
    const J* e = r.get("evidence");
    if (e && e->t == J::T::Obj)
        for (const char* k : {"surrogate", "mean", "var", "sent"})
            if (e->get(k))
                return true;
    return false;
}


static int usage(const char* argv0) {
    std::println(std::cerr,
                 "usage: {} evo log [--json] [--all] [--run ID|last] [--front] "
                 "[--lineage ID] [--log FILE]",
                 argv0);
    std::println(std::cerr, "       {} evo report --html DIR [--all] [--run ID|last] [--log FILE]",
                 argv0);
    std::println(std::cerr, "note: --front keeps the candidates non-dominated on the first two "
                            "objectives (min/min); report adds Pareto, HV, QD, lineage");
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

// --front and --lineage filters (#4948). The log carries objective values
// but not their directions, so the front uses the same documented
// minimization projection as the report scatter: both objectives smaller is
// better, and a point is on the front when nothing beats it on both.
static std::vector<J> front_filter(const std::vector<J>& recs) {
    std::vector<ObjPt> pts;
    std::vector<std::size_t> at;
    for (std::size_t i = 0; i < recs.size(); ++i) {
        ObjPt p;
        if (objectives_xy(recs[i], p.x, p.y)) {
            p.id = recs[i].str("id");
            pts.push_back(p);
            at.push_back(i);
        }
    }
    const std::vector<ObjPt> front = pareto2(pts);
    std::vector<J> out;
    for (std::size_t k = 0; k < pts.size(); ++k) {
        const bool on = std::any_of(front.begin(), front.end(), [&](const ObjPt& f) {
            return f.x == pts[k].x && f.y == pts[k].y;
        });
        if (on)
            out.push_back(recs[at[k]]);
    }
    return out;
}

// The ancestor chain of one candidate: walk "parent" to the root, then keep
// the records whose id is on the chain (oldest first).
static std::vector<J> lineage_filter(const std::vector<J>& recs, const std::string& target) {
    auto matches = [&](const std::string& id) {
        if (id == target)
            return true;
        const auto h = id.rfind('#');
        return h != std::string::npos && id.substr(h + 1) == target;
    };
    std::string cur;
    for (const auto& r : recs)
        if (matches(r.str("id"))) {
            cur = r.str("id");
            break;
        }
    std::vector<std::string> chain;
    std::size_t guard = 0;
    while (!cur.empty() && guard++ <= recs.size() + 1) {
        chain.push_back(cur);
        std::string next;
        for (const auto& r : recs)
            if (r.str("id") == cur) {
                next = r.str("parent");
                break;
            }
        cur = next;
    }
    std::vector<J> out;
    for (const auto& r : recs)
        if (std::find(chain.begin(), chain.end(), r.str("id")) != chain.end())
            out.push_back(r);
    return out;
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

// ── QD heat map and lineage (issue #4941) ──────────────────────────
// Behaviour descriptors arrive as the v=2 "behavior" member: an array (or
// an object of numbers) from the evaluator. Two of them are the cell
// coordinates; the record's score is the cell quality. The grid is the
// observed bounding box, so the report needs no map configuration.
struct QdCell {
    double x = 0, y = 0, q = 0;
};

static bool behavior_xy(const J& r, double& x, double& y) {
    const J* b = r.get("behavior");
    if (!b)
        return false;
    std::vector<double> xs;
    if (b->t == J::T::Arr) {
        for (const auto& v : b->arr)
            if (v.t == J::T::Num)
                xs.push_back(v.n);
    } else if (b->t == J::T::Obj) {
        for (const auto& [k, v] : b->obj)
            if (v.t == J::T::Num)
                xs.push_back(v.n);
    }
    if (xs.size() < 2)
        return false;
    x = xs[0];
    y = xs[1];
    return true;
}

static double quality_of(const J& r) {
    if (auto s = score_of(r))
        return s->second;
    return 0.0;
}

static double clamp01(double t) {
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

static std::string color_for(double t) {
    // Blue → amber ramp; t in [0,1].
    t = clamp01(t);
    const int r = static_cast<int>(30 + 225 * t);
    const int g = static_cast<int>(90 + 120 * t);
    const int b = static_cast<int>(200 - 140 * t);
    return std::format("rgb({},{},{})", r, g, b);
}

static std::string qd_heatmap_svg(const std::vector<QdCell>& cells, std::size_t& filled) {
    constexpr int BINS = 10;
    constexpr double W = 460, H = 360, PAD = 34;
    if (cells.empty())
        return {};
    double x0 = cells[0].x, x1 = x0, y0 = cells[0].y, y1 = y0, q0 = cells[0].q, q1 = q0;
    for (const auto& c : cells) {
        x0 = std::min(x0, c.x);
        x1 = std::max(x1, c.x);
        y0 = std::min(y0, c.y);
        y1 = std::max(y1, c.y);
        q0 = std::min(q0, c.q);
        q1 = std::max(q1, c.q);
    }
    const double xs = x1 > x0 ? x1 - x0 : 1.0;
    const double ys = y1 > y0 ? y1 - y0 : 1.0;
    std::vector<double> best(BINS * BINS, 0.0);
    std::vector<char> has(BINS * BINS, 0);
    for (const auto& c : cells) {
        int i = static_cast<int>((c.x - x0) / xs * BINS);
        int j = static_cast<int>((c.y - y0) / ys * BINS);
        i = i < 0 ? 0 : (i >= BINS ? BINS - 1 : i);
        j = j < 0 ? 0 : (j >= BINS ? BINS - 1 : j);
        const auto slot = static_cast<std::size_t>(j) * BINS + i;
        if (!has[slot] || c.q > best[slot]) {
            best[slot] = c.q;
            has[slot] = 1;
        }
    }
    filled = 0;
    const double cw = (W - 2 * PAD) / BINS, ch = (H - 2 * PAD) / BINS;
    std::string svg =
        std::format("<svg viewBox=\"0 0 {} {}\" width=\"{}\" height=\"{}\" role=\"img\" "
                    "aria-label=\"QD heat map\">",
                    W, H, W, H);
    svg += std::format("<rect x=\"0\" y=\"0\" width=\"{0}\" height=\"{1}\" fill=\"#fff\"/>", W, H);
    for (int j = 0; j < BINS; ++j)
        for (int i = 0; i < BINS; ++i) {
            const auto slot = static_cast<std::size_t>(j) * BINS + i;
            if (!has[slot])
                continue;
            const double v = best[slot];
            ++filled;
            const double t = q1 > q0 ? (v - q0) / (q1 - q0) : 1.0;
            svg += std::format(
                "<rect x=\"{:.1f}\" y=\"{:.1f}\" width=\"{:.1f}\" height=\"{:.1f}\" "
                "fill=\"{}\" stroke=\"#fff\"><title>x={:.4g} y={:.4g} q={:.6g}</title></rect>",
                PAD + i * cw, PAD + j * ch, cw, ch, color_for(t), x0 + (i + 0.5) * xs / BINS,
                y0 + (j + 0.5) * ys / BINS, v);
        }
    svg += std::format("<text x=\"4\" y=\"{}\" class=\"lb\">{:.4g}</text>"
                       "<text x=\"4\" y=\"{}\" class=\"lb\">{:.4g}</text>",
                       H - PAD, y0, PAD + 8, y1);
    svg += "</svg>";
    return svg;
}

struct LNode {
    std::string id;
    std::string parent;
    std::vector<std::string> parents;
    std::string decision;
};

// Depth of each node from its parent chain (roots at 0). Records are written
// in append order, so a parent normally precedes its child; the loop is
// bounded anyway and stops on a cycle.
static std::size_t node_at(const std::vector<std::pair<std::string, std::size_t>>& idx,
                           const std::string& id) {
    for (const auto& [key, at] : idx)
        if (key == id)
            return at;
    return static_cast<std::size_t>(-1);
}

static std::size_t node_depth(const std::vector<LNode>& ns,
                              const std::vector<std::pair<std::string, std::size_t>>& idx,
                              std::size_t i) {
    std::size_t d = 0;
    std::string cur = ns[i].parent;
    std::size_t guard = 0;
    while (!cur.empty() && guard++ <= ns.size()) {
        const std::size_t at = node_at(idx, cur);
        if (at == static_cast<std::size_t>(-1))
            break;
        cur = ns[at].parent;
        ++d;
    }
    return d;
}

static std::string lineage_svg(const std::vector<LNode>& ns, std::size_t& drawn) {
    if (ns.empty())
        return {};
    std::vector<std::pair<std::string, std::size_t>> idx;
    idx.reserve(ns.size());
    for (std::size_t i = 0; i < ns.size(); ++i)
        idx.emplace_back(ns[i].id, i);
    std::vector<std::size_t> depth(ns.size());
    std::size_t maxd = 0;
    for (std::size_t i = 0; i < ns.size(); ++i) {
        depth[i] = node_depth(ns, idx, i);
        maxd = std::max(maxd, depth[i]);
    }
    std::vector<std::size_t> per_layer(maxd + 1, 0);
    for (const auto d : depth)
        ++per_layer[d];
    std::size_t widest = 0;
    for (const auto n : per_layer)
        widest = std::max(widest, n);
    constexpr double DX = 64, DY = 42, PAD = 30;
    const double W = std::max(320.0, PAD * 2 + DX * static_cast<double>(widest));
    const double H = PAD * 2 + DY * static_cast<double>(maxd + 1);
    std::vector<std::pair<double, double>> pos(ns.size());
    std::vector<std::size_t> seen(maxd + 1, 0);
    for (std::size_t i = 0; i < ns.size(); ++i) {
        const double x = PAD + DX * static_cast<double>(seen[depth[i]]++) + 8;
        pos[i] = {x, PAD + DY * static_cast<double>(depth[i])};
    }
    std::string svg =
        std::format("<svg viewBox=\"0 0 {} {}\" width=\"{}\" height=\"{}\" role=\"img\" "
                    "aria-label=\"lineage tree\">",
                    W, H, W, H);
    svg += std::format("<rect x=\"0\" y=\"0\" width=\"{0}\" height=\"{1}\" fill=\"#fff\"/>", W, H);
    drawn = 0;
    for (std::size_t i = 0; i < ns.size(); ++i) {
        const auto edge = [&](const std::string& pid) {
            const std::size_t at = node_at(idx, pid);
            if (at == static_cast<std::size_t>(-1))
                return;
            svg += std::format("<line x1=\"{:.1f}\" y1=\"{:.1f}\" x2=\"{:.1f}\" y2=\"{:.1f}\" "
                               "stroke=\"#bbb\"/>",
                               pos[at].first, pos[at].second, pos[i].first, pos[i].second);
        };
        if (!ns[i].parent.empty())
            edge(ns[i].parent);
        for (const auto& p : ns[i].parents)
            if (p != ns[i].parent)
                edge(p);
    }
    for (std::size_t i = 0; i < ns.size(); ++i) {
        ++drawn;
        svg += std::format("<circle cx=\"{:.1f}\" cy=\"{:.1f}\" r=\"4\" class=\"{}\" "
                           "fill=\"{}\"><title>{} {}</title></circle>",
                           pos[i].first, pos[i].second, ns[i].decision == "KEEP" ? "k" : "r",
                           ns[i].decision == "KEEP" ? "#1a7f37" : "#cf222e",
                           html_escape(short_id(ns[i].id)), html_escape(ns[i].decision));
    }
    svg += "</svg>";
    return svg;
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
    // QD map: one cell per record that carries a 2-D behaviour descriptor.
    std::vector<QdCell> cells;
    for (const auto& r : recs) {
        QdCell c;
        if (behavior_xy(r, c.x, c.y)) {
            c.q = quality_of(r);
            cells.push_back(c);
        }
    }
    std::size_t qd_filled = 0;
    const std::string qd_svg = qd_heatmap_svg(cells, qd_filled);
    // Lineage: parent links, including the v=2 multi-parent list.
    std::vector<LNode> nodes;
    for (const auto& r : recs) {
        if (r.str("id").empty())
            continue;
        LNode n;
        n.id = r.str("id");
        n.parent = r.str("parent");
        n.decision = r.str("decision");
        if (const J* ps = r.get("parents"); ps && ps->t == J::T::Arr)
            for (const auto& p : ps->arr)
                if (p.t == J::T::Str && !p.s.empty())
                    n.parents.push_back(p.s);
        nodes.push_back(std::move(n));
    }
    std::size_t lineage_nodes = 0;
    const std::string lineage_svg_text = lineage_svg(nodes, lineage_nodes);
    // Objective space: scatter (first two numeric objectives) and the
    // cumulative hypervolume curve of the front after each measurement.
    std::vector<ObjPt> opts;
    for (const auto& r : recs) {
        ObjPt p;
        if (objectives_xyz(r, p.x, p.y, p.z, p.has_z)) {
            p.seq = opts.size();
            p.id = r.str("id");
            opts.push_back(p);
        }
    }
    const std::vector<ObjPt> front2 = pareto2(opts);
    const std::string pareto_svg_text = opts.size() >= 2 ? pareto_svg(opts, front2) : std::string();
    std::vector<double> hv_curve;
    double final_hv = 0.0;
    if (opts.size() >= 2) {
        const Axis2 b = bounds2(opts);
        const double rx = b.x1 + 0.1 * (b.x1 - b.x0);
        const double ry = b.y1 + 0.1 * (b.y1 - b.y0);
        std::vector<ObjPt> prefix;
        for (const auto& r : recs) {
            ObjPt p;
            if (!objectives_xy(r, p.x, p.y))
                continue;
            p.id = r.str("id");
            prefix.push_back(p);
            final_hv = hv2(pareto2(prefix), rx, ry);
            hv_curve.push_back(final_hv);
        }
    }
    const std::string hv_svg_text = hv_curve.size() >= 2 ? hv_svg(hv_curve) : std::string();
    std::vector<std::string> surrogate_rows;
    for (const auto& r : recs) {
        if (!surrogate_record(r))
            continue;
        const J* e = r.get("evidence");
        std::string cells;
        for (const char* k : {"mean", "var", "score", "sent", "surrogate"}) {
            const J* v = e && e->t == J::T::Obj ? e->get(k) : nullptr;
            cells +=
                std::format("<td>{}</td>", v ? html_escape(v->t == J::T::Str ? v->s : v->raw) : "");
        }
        surrogate_rows.push_back(std::format(
            "<tr><td>{}</td><td>{}</td><td>{}</td>{}</tr>", html_escape(short_id(r.str("id"))),
            html_escape(r.str("decision")), html_escape(r.str("reason")), cells));
    }
    if (!qd_svg.empty()) {
        std::ofstream f(dir / "qd-heatmap.svg", std::ios::binary | std::ios::trunc);
        f << qd_svg;
        if (!f) {
            std::println(std::cerr, "error: cannot write {}", (dir / "qd-heatmap.svg").string());
            return 1;
        }
    }
    if (!lineage_svg_text.empty()) {
        std::ofstream f(dir / "lineage.svg", std::ios::binary | std::ios::trunc);
        f << lineage_svg_text;
        if (!f) {
            std::println(std::cerr, "error: cannot write {}", (dir / "lineage.svg").string());
            return 1;
        }
    }
    if (!pareto_svg_text.empty()) {
        std::ofstream f(dir / "pareto.svg", std::ios::binary | std::ios::trunc);
        f << pareto_svg_text;
        if (!f) {
            std::println(std::cerr, "error: cannot write {}", (dir / "pareto.svg").string());
            return 1;
        }
    }
    if (!hv_svg_text.empty()) {
        std::ofstream f(dir / "hv-curve.svg", std::ios::binary | std::ios::trunc);
        f << hv_svg_text;
        if (!f) {
            std::println(std::cerr, "error: cannot write {}", (dir / "hv-curve.svg").string());
            return 1;
        }
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
    if (!pareto_svg_text.empty())
        h += std::format("<h2>Pareto front</h2><p>{} points, {} non-dominated "
                         "(first two objectives; a third is the point size)</p>",
                         opts.size(), front2.size()) +
             pareto_svg_text;
    if (!hv_svg_text.empty())
        h += std::format("<h2>Hypervolume</h2><p>final HV {:.6g} over {} measurements</p>",
                         final_hv, hv_curve.size()) +
             hv_svg_text;
    if (!qd_svg.empty())
        h += std::format("<h2>QD map</h2><p>{} cells filled · {} evaluated candidates</p>",
                         qd_filled, cells.size()) +
             qd_svg;
    if (!lineage_svg_text.empty())
        h += std::format("<h2>Lineage</h2><p>{} nodes</p>", lineage_nodes) + lineage_svg_text;
    if (!surrogate_rows.empty()) {
        h += std::format("<h2>Surrogate pre-screen</h2><p>{} record(s)</p>"
                         "<table><tr><th>id</th><th>decision</th><th>reason</th>"
                         "<th>mean</th><th>var</th><th>score</th><th>sent</th></tr>",
                         surrogate_rows.size());
        for (const auto& row : surrogate_rows)
            h += row;
        h += "</table>";
    }
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
    // Outer fold: one group per generation (v=2 "gen", else the worldline);
    // inner fold: one per candidate, as before.
    std::vector<std::string> gen_keys;
    std::vector<std::vector<std::size_t>> gen_rows;
    for (std::size_t k = 0; k < recs.size(); ++k) {
        std::string key = "worldline";
        if (const J* g = recs[k].get("gen"); g && g->t == J::T::Num)
            key = std::format("gen {}", static_cast<long long>(g->n));
        auto it = std::find(gen_keys.begin(), gen_keys.end(), key);
        if (it == gen_keys.end()) {
            gen_keys.push_back(key);
            gen_rows.emplace_back();
            gen_rows.back().push_back(k);
        } else {
            gen_rows[static_cast<std::size_t>(it - gen_keys.begin())].push_back(k);
        }
    }
    for (std::size_t g = 0; g < gen_keys.size(); ++g) {
        h += std::format("<details class=\"fold\"><summary>{} — {} candidate(s)</summary>",
                         html_escape(gen_keys[g]), gen_rows[g].size());
        for (const std::size_t k : gen_rows[g]) {
            const auto& r = recs[k];
            h +=
                std::format("<details id=\"c{}\"{}><summary><b>{}</b> <span class=\"{}\">{}</span> "
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
    bool as_json = false, all = false, front = false;
    std::string run, html, log_arg, lineage_id;
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
        } else if (auto v = val("--lineage")) {
            lineage_id = *v;
        } else if (a == "--front") {
            front = true;
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
    if (!lineage_id.empty())
        recs = lineage_filter(recs, lineage_id);
    if (front)
        recs = front_filter(recs);
    if (sub == "log")
        return cmd_log(recs, as_json);
    return cmd_report(recs, html, log_path);
}
