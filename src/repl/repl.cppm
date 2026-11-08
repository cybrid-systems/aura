module;
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>
#include <vector>
#include <unistd.h>
#include "../linenoise/linenoise.h"

#ifndef AURA_BUILD_VERSION
#define AURA_BUILD_VERSION "dev"
#endif

export module aura.repl;

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

export namespace aura {

class Repl {
public:
    explicit Repl(aura::compiler::CompilerService& cs)
        : cs_(cs) {
        g_repl_ = this;
        linenoiseSetMultiLine(1);
        linenoiseHistorySetMaxLen(2000);
        linenoiseSetCompletionCallback([](const char* buf, linenoiseCompletions* lc) {
            if (g_repl_ && g_repl_->completion_)
                for (auto& s : g_repl_->completion_(buf))
                    linenoiseAddCompletion(lc, s.c_str());
        });
    }

    ~Repl() {
        if (!history_path_.empty())
            linenoiseHistorySave(history_path_.c_str());
        g_repl_ = nullptr;
    }
    Repl(const Repl&) = delete;
    Repl& operator=(const Repl&) = delete;
    Repl(Repl&&) = delete;
    Repl& operator=(Repl&&) = delete;

    void eval_and_print_(const std::string& line) {
        auto r = cs_.eval(line);
        if (!r) {
            std::fprintf(stderr, "%s: error: %s\n", line.c_str(),
                         r.error().format_with_source(line).c_str());
            std::fflush(stderr);
            // Issue (lyapunov-fact demo 2026-07-28): emit an empty line
            // on stdout so the runner (one readline per eval) can proceed.
            std::fprintf(stdout, "\n");
            std::fflush(stdout);
        } else if (!aura::compiler::types::is_void(*r)) {
            auto val = aura::compiler::format_value(
                *r, cs_.evaluator().primitives().string_heap(), cs_.evaluator().pairs(), 0,
                &cs_.evaluator().primitives(), cs_.evaluator().keyword_table());
            std::fprintf(stdout, "%s\n", val.c_str());
            std::fflush(stdout);
        } else {
            // Void result (e.g. `define`, `set!`): still emit an empty line
            // so the runner's per-form readline gets a response.
            std::fprintf(stdout, "\n");
            std::fflush(stdout);
        }
    }

    void run() {
        const bool is_tty = ::isatty(STDIN_FILENO) != 0;

        // Issue #2292 / lyapunov-fact: pipe-mode drivers send forms one line
        // at a time over a long-lived subprocess PIPE. linenoise's non-TTY
        // path hangs on such pipes (no response, no prompt). Use plain
        // getline + fprintf for pipe mode; keep linenoise only for real TTYs.
        if (!is_tty) {
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line == "(quit)" || line == "(exit)")
                    break;
                if (line.empty())
                    continue;
                if (line.front() == ':') {
                    command_(line);
                    continue;
                }
                eval_and_print_(line);
            }
            return;
        }

        const char* home = std::getenv("HOME");
        if (!home)
            home = ".";
        history_path_ = std::string(home) + "/.config/aura/history";
        std::filesystem::create_directories(std::filesystem::path(history_path_).parent_path());
        linenoiseHistoryLoad(history_path_.c_str());

        // Issue #5036: the banner carries the same version token as
        // `aura --version` and the same tagline as `aura --help`. The old
        // hand-written "v0.2" never moved with AURA_BUILD_VERSION.
        std::fprintf(stdout, "aura %s \u2014 an AI-native Lisp\n", AURA_BUILD_VERSION);
        std::fprintf(stdout, "  (quit) to exit, Ctrl+D for EOF, :help for REPL commands\n");
        std::fflush(stdout);

        while (true) {
            char* raw = linenoise("> ");
            if (!raw)
                break;

            std::string line(raw);
            std::free(raw);

            if (line == "(quit)" || line == "(exit)")
                break;
            if (line.empty())
                continue;

            linenoiseHistoryAdd(line.c_str());
            if (line.front() == ':') {
                command_(line);
                continue;
            }
            eval_and_print_(line);
        }
        std::fprintf(stdout, "\n");
        std::fflush(stdout);
    }


    // ── Issue #4911: hot reload + worldline commands ─────────────────────
    // `:load FILE` evaluates every top-level form and remembers each define's
    // text; `:reload [FILE]` re-reads it and only touches defines whose text
    // changed (via mutate:rebind, so live data such as generated prices stays
    // bound). Top-level expressions are not re-run on :reload.
    struct Form {
        std::string text;
        int line = 1;
    };
    struct Def {
        std::string file;
        int line = 0;
        std::string text;  // whitespace-normalized define form
        std::string value; // rebind payload: value expr, or (lambda …) for sugar
        bool is_fn = false;
        std::string src; // the define form as written (re-evaluated on redefine)
    };

    // Split source into balanced top-level forms (strings, ; and #| |#
    // comments, and #\x character literals are skipped).
    static std::vector<Form> split_forms(std::string_view src) {
        std::vector<Form> out;
        int depth = 0, line = 1, start_line = 1;
        std::size_t start = std::string_view::npos;
        for (std::size_t i = 0; i < src.size(); ++i) {
            const char c = src[i];
            if (c == '\n') {
                ++line;
                continue;
            }
            if (c == ';') {
                while (i + 1 < src.size() && src[i + 1] != '\n')
                    ++i;
                continue;
            }
            if (c == '#' && i + 1 < src.size() && src[i + 1] == '|') {
                for (i += 2; i + 1 < src.size() && !(src[i] == '|' && src[i + 1] == '#'); ++i)
                    if (src[i] == '\n')
                        ++line;
                ++i;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c)) && depth == 0)
                continue;
            if (start == std::string_view::npos) {
                start = i;
                start_line = line;
            }
            if (c == '#' && i + 2 < src.size() && src[i + 1] == '\\') {
                i += 2;
                continue;
            }
            if (c == '"') {
                for (++i; i < src.size() && src[i] != '"'; ++i) {
                    if (src[i] == '\\')
                        ++i;
                    else if (src[i] == '\n')
                        ++line;
                }
            } else if (c == '(' || c == '[') {
                ++depth;
            } else if (c == ')' || c == ']') {
                --depth;
            }
            const bool atom_end =
                depth == 0 &&
                (i + 1 >= src.size() || std::isspace(static_cast<unsigned char>(src[i + 1])) ||
                 src[i + 1] == '(' || src[i + 1] == ')' || src[i + 1] == ';');
            const bool prefix = c == '\'' || c == '`' || c == ',' || c == '@' || c == '#';
            if (depth <= 0 && (c == ')' || c == ']' || c == '"' || (atom_end && !prefix))) {
                out.push_back({std::string(src.substr(start, i + 1 - start)), start_line});
                start = std::string_view::npos;
                depth = 0;
            }
        }
        if (start != std::string_view::npos)
            out.push_back({std::string(src.substr(start)), start_line});
        return out;
    }

    // Collapse whitespace outside strings so re-indenting is not a "change".
    static std::string normalize(std::string_view s) {
        std::string out;
        bool space = false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c == ';') {
                while (i + 1 < s.size() && s[i + 1] != '\n')
                    ++i;
                space = true;
                continue;
            }
            if (c == '"') {
                if (space && !out.empty())
                    out += ' ';
                space = false;
                out += c;
                for (++i; i < s.size() && s[i] != '"'; ++i) {
                    out += s[i];
                    if (s[i] == '\\' && i + 1 < s.size())
                        out += s[++i];
                }
                if (i < s.size())
                    out += '"';
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c))) {
                space = true;
                continue;
            }
            if (space && !out.empty() && out.back() != '(' && c != ')')
                out += ' ';
            space = false;
            out += c;
        }
        return out;
    }

    // Skip whitespace/comments; return index of next token start.
    static std::size_t skip_ws(std::string_view s, std::size_t i) {
        while (i < s.size()) {
            if (std::isspace(static_cast<unsigned char>(s[i]))) {
                ++i;
            } else if (s[i] == ';') {
                while (i < s.size() && s[i] != '\n')
                    ++i;
            } else {
                break;
            }
        }
        return i;
    }
    // End index (exclusive) of the datum starting at i.
    static std::size_t datum_end(std::string_view s, std::size_t i) {
        if (i >= s.size())
            return i;
        if (s[i] == '(' || s[i] == '[') {
            int d = 0;
            for (; i < s.size(); ++i) {
                if (s[i] == '"') {
                    for (++i; i < s.size() && s[i] != '"'; ++i)
                        if (s[i] == '\\')
                            ++i;
                } else if (s[i] == ';') {
                    while (i < s.size() && s[i] != '\n')
                        ++i;
                } else if (s[i] == '#' && i + 2 < s.size() && s[i + 1] == '\\') {
                    i += 2;
                } else if (s[i] == '(' || s[i] == '[') {
                    ++d;
                } else if ((s[i] == ')' || s[i] == ']') && --d == 0) {
                    return i + 1;
                }
            }
            return s.size();
        }
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '(' &&
               s[i] != ')')
            ++i;
        return i;
    }

    // (define name expr) / (define (name . params) body…) → name + rebind value.
    static std::optional<std::pair<std::string, Def>> parse_define(const Form& f) {
        std::string_view s = f.text;
        std::size_t i = skip_ws(s, 0);
        if (i >= s.size() || s[i] != '(')
            return std::nullopt;
        i = skip_ws(s, i + 1);
        const std::size_t kw_end = datum_end(s, i);
        if (s.substr(i, kw_end - i) != "define")
            return std::nullopt;
        i = skip_ws(s, kw_end);
        Def d;
        d.line = f.line;
        d.src = f.text;
        d.text = normalize(s);
        std::string name;
        if (i < s.size() && s[i] == '(') {
            const std::size_t sig_end = datum_end(s, i);
            std::size_t j = skip_ws(s, i + 1);
            const std::size_t n_end = datum_end(s, j);
            name = std::string(s.substr(j, n_end - j));
            std::string_view ps = s.substr(n_end, sig_end - 1 - n_end);
            ps.remove_prefix(std::min(ps.size(), skip_ws(ps, 0)));
            // (define (f . rest) …) → (lambda rest …)
            std::string params =
                ps.starts_with(". ") ? std::string(ps.substr(2)) : "(" + std::string(ps) + ")";
            std::size_t body_end = s.rfind(')');
            if (body_end == std::string_view::npos || body_end < sig_end)
                return std::nullopt;
            d.value = "(lambda " + params + " " +
                      std::string(s.substr(sig_end, body_end - sig_end)) + ")";
            d.is_fn = true;
        } else {
            const std::size_t n_end = datum_end(s, i);
            name = std::string(s.substr(i, n_end - i));
            const std::size_t v = skip_ws(s, n_end);
            const std::size_t v_end = datum_end(s, v);
            d.value = std::string(s.substr(v, v_end - v));
            d.is_fn = d.value.starts_with("(lambda");
        }
        if (name.empty())
            return std::nullopt;
        return std::pair{std::move(name), std::move(d)};
    }

    static std::string quote_(std::string_view s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\')
                out += '\\';
            if (c == '\n') {
                out += "\\n";
                continue;
            }
            out += c;
        }
        return out + "\"";
    }

    // Evaluate without printing; returns "" on success, else the error text.
    std::string eval_quiet_(const std::string& code, std::string* shown = nullptr) {
        auto r = cs_.eval(code);
        if (!r)
            return r.error().format_with_source(code);
        if (shown && !aura::compiler::types::is_void(*r))
            *shown = format_(*r);
        return {};
    }
    std::string format_(aura::compiler::types::EvalValue v) {
        return aura::compiler::format_value(
            v, cs_.evaluator().primitives().string_heap(), cs_.evaluator().pairs(), 0,
            &cs_.evaluator().primitives(), cs_.evaluator().keyword_table());
    }

    static std::optional<std::string> slurp_(const std::string& path) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
            return std::nullopt;
        std::string s;
        char buf[8192];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
            s.append(buf, n);
        std::fclose(f);
        return s;
    }

    static std::string canon_(const std::string& p) {
        std::error_code ec;
        auto c = std::filesystem::weakly_canonical(p, ec);
        return ec ? p : c.string();
    }

    // Record (without evaluating) the defines of every file `file` requires,
    // so `:reload lib.aura` on a module that was loaded through (require …)
    // diffs against what is live and rebinds only the changed defines.
    void baseline_requires_(const std::string& file, const std::string& src, int depth = 0) {
        namespace fs = std::filesystem;
        if (depth > 32)
            return;
        const fs::path dir = fs::path(file).parent_path();
        for (auto& f : split_forms(src)) {
            if (!f.text.starts_with("(require"))
                continue;
            const auto a = f.text.find('"');
            const auto b = a == std::string::npos ? a : f.text.find('"', a + 1);
            if (b == std::string::npos)
                continue;
            const std::string spec = f.text.substr(a + 1, b - a - 1);
            std::string found;
            for (const fs::path& cand : {dir / spec, dir / (spec + ".aura")}) {
                std::error_code ec;
                if (fs::is_regular_file(cand, ec)) {
                    found = canon_(cand.string());
                    break;
                }
            }
            if (found.empty() || !baselined_.insert(found).second)
                continue;
            auto msrc = slurp_(found);
            if (!msrc)
                continue;
            for (auto& mf : split_forms(*msrc)) {
                auto d = parse_define(mf);
                if (!d || defs_.contains(d->first))
                    continue;
                d->second.file = found;
                def_order_.push_back(d->first);
                defs_[d->first] = std::move(d->second);
            }
            baseline_requires_(found, *msrc, depth + 1);
        }
    }

public:
    // `:load FILE` — evaluate every form, remember define texts. Returns false
    // when the file is missing or a form fails (later forms still run).
    bool load(const std::string& shown_path) {
        const std::string path = canon_(shown_path);
        auto src = slurp_(path);
        if (!src) {
            std::fprintf(stderr, ":load: cannot read %s\n", shown_path.c_str());
            std::fflush(stderr);
            return false;
        }
        last_file_ = path;
        bool ok = true;
        int defs = 0;
        for (auto& f : split_forms(*src)) {
            auto d = parse_define(f);
            if (auto err = eval_quiet_(f.text); !err.empty()) {
                std::fprintf(stderr, "%s:%d: error: %s\n", path.c_str(), f.line, err.c_str());
                ok = false;
                continue;
            }
            if (d) {
                d->second.file = path;
                if (!defs_.contains(d->first))
                    def_order_.push_back(d->first);
                defs_[d->first] = std::move(d->second);
                ++defs;
            }
        }
        baselined_.insert(path);
        baseline_requires_(path, *src);
        std::fprintf(stdout, "loaded %s: %d define(s)\n", shown_path.c_str(), defs);
        events_.push_back(std::format("load {} ({} define(s))", shown_path, defs));
        std::fflush(stdout);
        std::fflush(stderr);
        return ok;
    }

    // Replace the live binding of an existing define `name` with `def`.
    // Procedures go through mutate:rebind (every other live binding, e.g.
    // generated data, stays). A define that is not a live workspace node
    // (one that came in via (require …)) is redefined in place instead
    // (`redefined` = true); a sandbox denial stays an error. Values
    // (mutate:rebind is procedure-only) re-evaluate just this define.
    std::string apply_def_(const std::string& name, const Def& def, bool live_is_fn,
                           bool& redefined) {
        redefined = false;
        if (!def.is_fn || !live_is_fn)
            return eval_quiet_(def.src);
        // Only a define that is a node of the live workspace goes through
        // mutate:rebind. Once the program has a workspace (set-code), rebind
        // of a name outside it still answers #t, but a later ast:snapshot /
        // ast:restore + eval-current then lowers a stale node id (abort in
        // lower_flat_expr) — the kit's `:reload backtest.aura` + evolve!.
        std::string node;
        eval_quiet_("(workspace:find-define " + quote_(name) + ")", &node);
        if (node.empty()) {
            auto err = eval_quiet_(def.src);
            redefined = err.empty();
            return err;
        }
        std::string shown;
        auto err = eval_quiet_("(mutate:rebind " + quote_(name) + " " + quote_(def.value) +
                                   " \"repl :reload\")",
                               &shown);
        if (!err.empty() || shown == "#t")
            return err;
        if (shown.find("effect-denied") != std::string::npos)
            return "mutate:rebind refused: " + shown;
        err = eval_quiet_(def.src);
        redefined = err.empty();
        return err;
    }

    // `:reload [FILE]` — rebind changed defines, add new ones, keep the rest.
    bool reload(std::string path) {
        path = path.empty() ? last_file_ : canon_(path);
        if (path.empty()) {
            std::fprintf(stderr, ":reload: no file loaded yet (use :load FILE)\n");
            std::fflush(stderr);
            return false;
        }
        if (!std::ranges::any_of(defs_, [&](auto& kv) { return kv.second.file == path; }) &&
            path != last_file_)
            return load(path);
        auto src = slurp_(path);
        if (!src) {
            std::fprintf(stderr, ":reload: cannot read %s\n", path.c_str());
            std::fflush(stderr);
            return false;
        }
        last_file_ = path;
        std::vector<std::string> changed, added, failed, redefined;
        int same = 0, skipped = 0;
        for (auto& f : split_forms(*src)) {
            auto d = parse_define(f);
            if (!d) {
                ++skipped;
                continue;
            }
            auto& [name, def] = *d;
            def.file = path;
            auto it = defs_.find(name);
            if (it != defs_.end() && it->second.text == def.text) {
                it->second.line = def.line;
                ++same;
                continue;
            }
            std::string err;
            if (it == defs_.end()) {
                err = eval_quiet_(f.text);
                if (err.empty()) {
                    added.push_back(name);
                    def_order_.push_back(name);
                }
            } else {
                bool redef = false;
                err = apply_def_(name, def, it->second.is_fn, redef);
                if (err.empty()) {
                    changed.push_back(name);
                    if (redef)
                        redefined.push_back(name);
                }
            }
            if (!err.empty()) {
                std::fprintf(stderr, "%s:%d: error: %s: %s\n", path.c_str(), def.line, name.c_str(),
                             err.c_str());
                failed.push_back(name);
                continue;
            }
            defs_[name] = std::move(def);
        }
        auto join = [](const std::vector<std::string>& v) {
            std::string s;
            for (auto& x : v)
                s += (s.empty() ? "" : " ") + x;
            return s.empty() ? std::string("-") : s;
        };
        std::fprintf(stdout,
                     "reloaded %s: changed [%s] added [%s] unchanged %d, skipped %d top-level "
                     "expression(s)%s\n",
                     path.c_str(), join(changed).c_str(), join(added).c_str(), same, skipped,
                     failed.empty() ? "" : (" failed [" + join(failed) + "]").c_str());
        if (!redefined.empty())
            std::fprintf(
                stdout,
                "  (not live workspace nodes, redefined in place: [%s]; :snapshot/:restore "
                "do not cover them)\n",
                join(redefined).c_str());
        events_.push_back(std::format("reload {}: changed [{}] added [{}]{}", path, join(changed),
                                      join(added),
                                      failed.empty() ? "" : " failed [" + join(failed) + "]"));
        std::fflush(stdout);
        std::fflush(stderr);
        return failed.empty();
    }

private:
    void ws_() {
        if (def_order_.empty()) {
            std::fprintf(stdout, "(no definitions loaded via :load)\n");
        }
        for (auto& n : def_order_) {
            auto it = defs_.find(n);
            if (it == defs_.end())
                continue;
            std::string node;
            eval_quiet_("(workspace:find-define " + quote_(n) + ")", &node);
            std::fprintf(stdout, "%-24s %-5s %s:%d%s\n", n.c_str(),
                         it->second.is_fn ? "fn" : "value", it->second.file.c_str(),
                         it->second.line, it->second.is_fn && node.empty() ? "  (not live)" : "");
        }
        std::fflush(stdout);
    }

    void history_() {
        for (std::size_t i = 0; i < events_.size(); ++i)
            std::fprintf(stdout, "#%zu %s\n", i + 1, events_[i].c_str());
        std::string out;
        if (auto err = eval_quiet_(
                "(let ((h (mutation-log:summary)))"
                " (if (hash? h)"
                "  (format \"total ~a, committed ~a, rolled-back ~a, last #~a ~a\""
                "   (hash-ref h \"total\") (hash-ref h \"committed\") (hash-ref h \"rolled-back\")"
                "   (hash-ref h \"last-mutation-id\") (hash-ref h \"last-operator\"))"
                "  \"(no workspace)\"))",
                &out);
            !err.empty())
            out = "error: " + err;
        if (out.size() >= 2 && out.front() == '"' && out.back() == '"')
            out = out.substr(1, out.size() - 2);
        std::fprintf(stdout, "mutation-log: %s\n", out.empty() ? "(empty)" : out.c_str());
        if (snapshots_.empty())
            std::fprintf(stdout, "snapshots: (none; take one with :snapshot)\n");
        for (auto& sn : snapshots_)
            std::fprintf(stdout, "snapshot %s  %s (%zu define(s))\n", sn.id.c_str(),
                         sn.label.c_str(), sn.defs.size());
        std::fflush(stdout);
    }

    void command_(const std::string& line) {
        std::string_view rest = line;
        const auto sp = rest.find_first_of(" \t");
        const std::string cmd(rest.substr(0, sp));
        std::string arg;
        if (sp != std::string_view::npos) {
            auto a = rest.substr(sp);
            const auto b = a.find_first_not_of(" \t");
            const auto e = a.find_last_not_of(" \t");
            if (b != std::string_view::npos)
                arg = std::string(a.substr(b, e + 1 - b));
        }
        if (cmd == ":load" && !arg.empty()) {
            load(arg);
        } else if (cmd == ":reload") {
            reload(arg);
        } else if (cmd == ":ws") {
            ws_();
        } else if (cmd == ":worldline" || cmd == ":history") {
            history_();
        } else if (cmd == ":snapshot") {
            // REPL-level snapshot of every define this session tracks. (A raw
            // ast:restore leaves REPL-compiled definitions on stale node ids.)
            const std::string id = std::to_string(snapshots_.size() + 1);
            snapshots_.push_back({id, arg, defs_});
            events_.push_back("snapshot " + id + (arg.empty() ? "" : " " + arg));
            std::fprintf(stdout, "snapshot %s\n", id.c_str());
        } else if (cmd == ":restore" && !arg.empty()) {
            auto sn = std::ranges::find(snapshots_, arg, &Snapshot::id);
            if (sn == snapshots_.end()) {
                std::fprintf(stderr, ":restore %s: no such snapshot (see :worldline)\n",
                             arg.c_str());
            } else {
                std::vector<std::string> back, failed;
                for (auto& [name, def] : sn->defs) {
                    auto it = defs_.find(name);
                    if (it != defs_.end() && it->second.text == def.text)
                        continue;
                    bool redef = false;
                    auto err = apply_def_(name, def, it == defs_.end() || it->second.is_fn, redef);
                    if (!err.empty()) {
                        std::fprintf(stderr, ":restore %s: %s: %s\n", arg.c_str(), name.c_str(),
                                     err.c_str());
                        failed.push_back(name);
                        continue;
                    }
                    defs_[name] = def;
                    back.push_back(name);
                }
                std::ranges::sort(back);
                std::string list;
                for (auto& n : back)
                    list += (list.empty() ? "" : " ") + n;
                std::fprintf(stdout, "restored snapshot %s: [%s]%s\n", arg.c_str(),
                             list.empty() ? "-" : list.c_str(),
                             failed.empty() ? "" : " (some failed)");
                events_.push_back("restore " + arg + " [" + (list.empty() ? "-" : list) + "]");
            }
        } else {
            std::fprintf(stdout,
                         "REPL commands:\n"
                         "  :load FILE      evaluate FILE, remember its defines\n"
                         "  :reload [FILE]  re-read FILE; rebind only changed defines\n"
                         "  :ws             list live definitions and where they came from\n"
                         "  :worldline      mutation-log summary + snapshots (:history)\n"
                         "  :snapshot [LBL] remember every tracked define\n"
                         "  :restore ID     put changed defines back as of snapshot ID\n");
        }
        // Pipe drivers read one response line per input line.
        std::fflush(stdout);
        std::fflush(stderr);
    }

    std::string last_file_;
    std::unordered_map<std::string, Def> defs_;
    std::vector<std::string> def_order_;
    struct Snapshot {
        std::string id, label;
        std::unordered_map<std::string, Def> defs;
    };
    std::vector<Snapshot> snapshots_;
    std::vector<std::string> events_; // this session's :load/:reload/:snapshot/:restore
    std::unordered_set<std::string> baselined_;

public:
    using CompletionCallback = std::function<std::vector<std::string>(std::string_view)>;
    void set_completion_callback(CompletionCallback cb) { completion_ = std::move(cb); }

private:
    aura::compiler::CompilerService& cs_;
    CompletionCallback completion_;
    std::string history_path_;
    static inline Repl* g_repl_ = nullptr;
};

} // namespace aura
