module;
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <optional>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include <unistd.h>
#include "../compiler/eval_budget.h"
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

// Issue #5039: a phrase is still open while a ( / [ or a string is
// unterminated. Comments (`;` to end of line and `#| … |#`) and character
// literals (`#\x`) are skipped, so a paren inside them does not count.
bool repl_phrase_incomplete(std::string_view src) {
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        if (in_string) {
            if (c == '\\' && i + 1 < src.size()) {
                ++i;
                continue;
            }
            if (c == '"')
                in_string = false;
            continue;
        }
        if (c == ';') {
            while (i < src.size() && src[i] != '\n')
                ++i;
            continue;
        }
        if (c == '#' && i + 1 < src.size() && src[i + 1] == '|') {
            for (i += 2; i + 1 < src.size() && !(src[i] == '|' && src[i + 1] == '#'); ++i) {
            }
            if (i + 1 < src.size())
                ++i; // consume '|#'
            continue;
        }
        if (c == '#' && i + 1 < src.size() && src[i + 1] == '\\') {
            i += 2; // the character literal's payload
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '(' || c == '[') {
            ++depth;
            continue;
        }
        if (c == ')' || c == ']') {
            if (depth > 0)
                --depth;
            continue;
        }
    }
    return in_string || depth > 0;
}

// Issue #5043: the pipe face answers one evaluation with one record, so a
// driver can tell a successful define from a failed form. The record is
// text (not an evaluated hash): strings are escaped.
std::string repl_record_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += c;
        }
    }
    return out;
}

// Issue #5043: the top-level names this phrase defines or rebinds.
std::vector<std::string> repl_defined_names(std::string_view src) {
    std::vector<std::string> out;
    std::size_t i = 0;
    auto skip_ws = [&src](std::size_t& p) {
        while (p < src.size()) {
            const unsigned char c = static_cast<unsigned char>(src[p]);
            if (std::isspace(c)) {
                ++p;
                continue;
            }
            if (src[p] == ';') {
                while (p < src.size() && src[p] != '\n')
                    ++p;
                continue;
            }
            if (src[p] == '#' && p + 1 < src.size() && src[p + 1] == '|') {
                p += 2;
                while (p + 1 < src.size() && !(src[p] == '|' && src[p + 1] == '#'))
                    ++p;
                p = p + 1 < src.size() ? p + 2 : src.size();
                continue;
            }
            break;
        }
    };
    auto read_atom = [&src](std::size_t& p) {
        const std::size_t b = p;
        while (p < src.size()) {
            const unsigned char c = static_cast<unsigned char>(src[p]);
            if (std::isspace(c) || src[p] == '(' || src[p] == ')' || src[p] == '[' || src[p] == ']')
                break;
            ++p;
        }
        return std::string(src.substr(b, p - b));
    };
    auto skip_form = [&src](std::size_t& p) {
        int depth = 0;
        bool in_string = false;
        for (; p < src.size(); ++p) {
            const char c = src[p];
            if (in_string) {
                if (c == '\\' && p + 1 < src.size())
                    ++p;
                else if (c == '"')
                    in_string = false;
                continue;
            }
            if (c == ';') {
                while (p < src.size() && src[p] != '\n')
                    ++p;
                continue;
            }
            if (c == '"') {
                in_string = true;
                continue;
            }
            if (c == '(' || c == '[')
                ++depth;
            else if (c == ')' || c == ']') {
                --depth;
                if (depth <= 0) {
                    ++p;
                    return;
                }
            }
        }
    };
    while (i < src.size()) {
        skip_ws(i);
        if (i >= src.size())
            break;
        if (src[i] != '(') {
            ++i;
            continue;
        }
        const std::size_t form_start = i;
        ++i;
        skip_ws(i);
        const std::string head = read_atom(i);
        if (head == "define" || head == "mutate:rebind") {
            skip_ws(i);
            std::string name;
            if (i < src.size() && (src[i] == '(' || src[i] == '[')) {
                ++i;
                skip_ws(i);
                name = read_atom(i);
            } else if (i < src.size() && src[i] == '"') {
                ++i;
                const std::size_t b = i;
                while (i < src.size() && src[i] != '"') {
                    if (src[i] == '\\' && i + 1 < src.size())
                        ++i;
                    ++i;
                }
                name = std::string(src.substr(b, i - b));
            } else if (i < src.size() && src[i] == '\'') {
                ++i;
                name = read_atom(i);
            } else {
                name = read_atom(i);
            }
            if (!name.empty())
                out.push_back(std::move(name));
        }
        i = form_start;
        skip_form(i);
    }
    return out;
}

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
        // Issue #5040: complete the word at the cursor, not the line end.
        linenoiseSetCompletionCallbackPos(
            [](const char* buf, size_t pos, linenoiseCompletions* lc) {
                if (!g_repl_ || g_repl_->completion_)
                    return; // a caller-supplied callback owns completion
                for (auto& s : g_repl_->complete_(buf, pos))
                    linenoiseAddCompletion(lc, s.c_str());
            });
        // Issue #5038: the grey hint after the line names the word's type or
        // the primitive's arity. linenoise owns no user data, so route it
        // through the active instance.
        linenoiseSetHintsCallback([](const char* buf, int* color, int* bold) -> char* {
            return g_repl_ ? g_repl_->hint_(buf, color, bold) : nullptr;
        });
        linenoiseSetFreeHintsCallback([](void* p) { std::free(p); });
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

    // Issue #5042: the live workspace text, and one definition out of it.
    std::string workspace_source_() {
        std::string shown;
        if (auto err = eval_quiet_("(current-source :workspace)", &shown); !err.empty())
            return {};
        if (shown.size() >= 2 && shown.front() == '"' && shown.back() == '"')
            shown = shown.substr(1, shown.size() - 2);
        std::string out;
        for (std::size_t i = 0; i < shown.size(); ++i) {
            if (shown[i] == '\\' && i + 1 < shown.size()) {
                const char next = shown[++i];
                if (next == 'n')
                    out += '\n';
                else if (next == 't')
                    out += '\t';
                else
                    out += next;
                continue;
            }
            out += shown[i];
        }
        return out;
    }

    std::string live_source_of_(const std::string& name) {
        const std::string src = workspace_source_();
        if (src.empty())
            return {};
        for (const auto& form : split_forms(src)) {
            if (auto d = parse_define(form); d && d->first == name)
                return d->second.src;
        }
        return {};
    }

    bool in_workspace_(const std::string& name) {
        std::string node;
        eval_quiet_("(workspace:find-define " + quote_(name) + ")", &node);
        return !node.empty();
    }

    static std::string quote_name_(const std::string& name) {
        std::string out = "\"";
        out += repl_record_escape(name);
        out += '"';
        return out;
    }

    // Issue #5042: snapshot -> rebind -> eval + gate -> KEEP, else restore.
    bool propose_(const std::string& arg) {
        const auto sp = arg.find_first_of(" \t");
        if (sp == std::string::npos) {
            std::fprintf(stdout, ":propose: usage: :propose NAME CODE\n");
            return false;
        }
        const std::string name = arg.substr(0, sp);
        std::string code = arg.substr(sp + 1);
        // A quoted candidate is the common CLI shape; a bare one (from
        // :ask or a paste) is accepted too.
        if (code.size() >= 2 && code.front() == '"' && code.back() == '"') {
            std::string unescaped;
            for (std::size_t i = 1; i + 1 < code.size(); ++i) {
                if (code[i] == '\\' && i + 1 < code.size() - 1) {
                    const char next = code[++i];
                    if (next == 'n')
                        unescaped += '\n';
                    else if (next == 't')
                        unescaped += '\t';
                    else
                        unescaped += next;
                } else {
                    unescaped += code[i];
                }
            }
            code = std::move(unescaped);
        }
        std::optional<std::pair<std::string, Def>> def;
        for (const auto& form : split_forms(code)) {
            if (auto d = parse_define(form); d) {
                def = std::move(d);
                break;
            }
        }
        if (!def) {
            std::fprintf(stdout, ":propose: the candidate has no define\n");
            return false;
        }
        const std::string before = live_source_of_(name);
        // The snapshot value is a handle: bind it and pass the binding back,
        // the printed form is not a literal the reader can restore from.
        static int snap_seq = 0;
        const std::string snap_var = "__repl_snap_" + std::to_string(++snap_seq);
        if (auto err = eval_quiet_("(define " + snap_var + " (ast:snapshot \"repl-propose\"))");
            !err.empty()) {
            std::fprintf(stdout, ":propose: snapshot failed: %s\n", err.c_str());
            return false;
        }
        // Issue #5042: the candidate evaluation runs under the step budget,
        // and Ctrl-C during it sets the interrupt flag instead of killing the
        // REPL. Either way the proposal rolls back.
        struct SigGuard {
            explicit SigGuard(long steps) { aura::compiler::eval_budget::arm(steps); }
            ~SigGuard() { aura::compiler::eval_budget::disarm(); }
        };
        static auto on_sigint = [](int) { aura::compiler::eval_budget::request_interrupt(); };
        const auto prev_int = std::signal(SIGINT, on_sigint);
        std::string err;
        std::string shown;
        bool ok = false;
        {
        SigGuard guard(fuel_);
        if (in_workspace_(name))
            err = eval_quiet_("(mutate:rebind " + quote_(name) + " " + quote_(def->second.value) +
                                  " \"repl :propose\")",
                              &shown);
        if (err.empty())
            err = eval_quiet_(def->second.src);
        ok = err.empty();
        if (ok && !gate_.empty()) {
            std::string gv;
            auto gerr = eval_quiet_(gate_, &gv);
            ok = gerr.empty() && gv != "#f";
            if (!ok && !gerr.empty())
                err = gerr;
        }
        }
        std::signal(SIGINT, prev_int);
        if (!ok) {
            eval_quiet_("(ast:restore " + snap_var + ")");
            eval_quiet_("(eval-current)");
            // The workspace text cache can still hold the rejected candidate;
            // re-evaluating the pre-proposal definition keeps the live source
            // and the environment on the same text.
            if (!before.empty())
                eval_quiet_(before);
        }
        const std::string after = ok ? live_source_of_(name) : before;
        if (ok) {
            focus_ = name;
            focus_steps_[name] = steps_for_(name) + 1;
        }
        if (pipe_mode_) {
            std::string rec = "(hash \"status\" \"ok\" \"value\" \"\" \"type\" \"void\" \"wrote\" (";
            rec += quote_name_(name);
            rec += ") \"focus\" ";
            rec += quote_name_(name);
            rec += " \"decision\" \"";
            rec += ok ? "KEEP" : "ROLLBACK";
            rec += "\" \"source\" \"";
            rec += repl_record_escape(after);
            rec += "\")";
            std::fprintf(stdout, "%s\n", rec.c_str());
        } else {
            std::fprintf(stdout, "%s\n- %s\n+ %s\n%s\n", name.c_str(),
                         before.empty() ? "(no-workspace)" : before.c_str(), def->second.src.c_str(),
                         ok ? "KEEP" : "ROLLBACK");
        }
        std::fflush(stdout);
        return ok;
    }

    // Issue #5048: the prompt names the definition in focus and its step.
    std::string prompt_() const {
        if (!focus_)
            return "> ";
        return *focus_ + " " + std::to_string(steps_for_(*focus_)) + "> ";
    }

    int steps_for_(const std::string& name) const {
        const auto hit = focus_steps_.find(name);
        return hit == focus_steps_.end() ? 0 : hit->second;
    }

    // The name a command means when it is given without one.
    std::optional<std::string> focus_name_() const { return focus_; }

    // Issue #5040: Tab candidates for the word at `pos`. Candidates are
    // whole replacement lines (linenoise swaps the buffer for one), so the
    // word at the cursor is replaced and the rest of the line stays.
    std::vector<std::string> complete_(std::string_view buf, std::size_t pos) {
        std::vector<std::string> out;
        if (pos > buf.size())
            pos = buf.size();
        auto ident = [](char c) {
            const auto u = static_cast<unsigned char>(c);
            return std::isalnum(u) != 0 || c == '-' || c == '?' || c == '!' || c == '*' ||
                   c == '/' || c == '>' || c == '<' || c == '=' || c == '_' || c == ':';
        };
        std::size_t start = pos;
        while (start > 0 && ident(buf[start - 1]))
            --start;
        const std::string word(buf.substr(start, pos - start));
        const std::string head(buf.substr(0, start));
        const std::string tail(buf.substr(pos));
        auto emit = [&out, &head, &tail](const std::string& cand, const std::string& prefix) {
            if (!cand.starts_with(prefix))
                return;
            out.push_back(head + cand + tail);
        };
        if (word.starts_with(":")) {
            static constexpr std::string_view kCommands[] = {
                ":load",  ":reload", ":ws",      ":worldline", ":history", ":snapshot",
                ":restore", ":help",  ":type",   ":doc",       ":src",     ":propose",
                ":gate",  ":fuel",    ":context", ":ask",      ":focus",   ":undo",
                ":redo",  ":at"};
            for (const auto cmd : kCommands)
                emit(std::string(cmd), word);
            return out;
        }
        if (in_require_string_(buf, pos)) {
            for (const auto& mod : std_modules_())
                emit(mod, word);
            return out;
        }
        if (word.empty())
            return out;
        for (const auto& [name, def] : defs_)
            emit(name, word);
        for (const auto& name : exports_())
            emit(name, word);
        const auto& prims = cs_.evaluator().primitives();
        for (std::size_t slot = 0; slot < prims.slot_count(); ++slot)
            emit(prims.name_for_slot(slot), word);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    static bool in_require_string_(std::string_view buf, std::size_t pos) {
        int quotes = 0;
        std::size_t open = 0;
        for (std::size_t i = 0; i < pos && i < buf.size(); ++i) {
            if (static_cast<unsigned char>(buf[i]) == 92 && i + 1 < pos) {
                ++i;
                continue;
            }
            if (buf[i] == '"') {
                ++quotes;
                open = i;
            }
        }
        if (quotes % 2 == 0)
            return false;
        auto head = buf.substr(0, open);
        while (!head.empty() && std::isspace(static_cast<unsigned char>(head.back())))
            head.remove_suffix(1);
        return head.ends_with("(require");
    }

    std::vector<std::string> std_modules_() const {
        std::vector<std::string> out;
        std::vector<std::filesystem::path> dirs{std::filesystem::path("lib/std")};
        if (const char* rt = std::getenv("AURA_RUNTIME_DIR"); rt && *rt)
            dirs.emplace_back(std::filesystem::path(rt) / "lib" / "std");
        for (const auto& dir : dirs) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec))
                continue;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file(ec))
                    continue;
                const auto name = entry.path().filename().string();
                if (!name.ends_with(".aura"))
                    continue;
                out.push_back("std/" + name.substr(0, name.size() - 5));
            }
            if (!out.empty())
                break;
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // Names installed by a required module: read the module's own file and
    // take its top-level (export …) form. Cached per module.
    std::vector<std::string> module_exports_(const std::string& mod) {
        if (const auto hit = export_cache_.find(mod); hit != export_cache_.end())
            return hit->second;
        std::vector<std::string> out;
        std::vector<std::filesystem::path> dirs{std::filesystem::path("lib/std")};
        if (const char* rt = std::getenv("AURA_RUNTIME_DIR"); rt && *rt)
            dirs.emplace_back(std::filesystem::path(rt) / "lib" / "std");
        for (const auto& dir : dirs) {
            std::ifstream in(dir / (mod + ".aura"));
            if (!in)
                continue;
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            const auto pos = src.find("(export");
            if (pos == std::string::npos)
                break;
            std::size_t i = pos + 7;
            while (i < src.size() && src[i] != ')') {
                while (i < src.size() && std::isspace(static_cast<unsigned char>(src[i])))
                    ++i;
                if (i >= src.size() || src[i] == ')')
                    break;
                if (src[i] == ';') {
                    while (i < src.size() && src[i] != '\n')
                        ++i;
                    continue;
                }
                const std::size_t start = i;
                while (i < src.size() && !std::isspace(static_cast<unsigned char>(src[i])) &&
                       src[i] != ')')
                    ++i;
                if (i > start)
                    out.emplace_back(src.substr(start, i - start));
            }
            if (!out.empty())
                break;
        }
        export_cache_[mod] = out;
        return out;
    }

    const std::vector<std::string>& exports_() {
        std::vector<std::string> mods{"list", "string", "hash", "math"};
        for (const auto& m : required_mods_)
            mods.push_back(m);
        std::sort(mods.begin(), mods.end());
        mods.erase(std::unique(mods.begin(), mods.end()), mods.end());
        std::string key;
        for (const auto& m : mods)
            key += m + ",";
        if (key == exports_key_)
            return exports_cache_;
        exports_key_ = key;
        exports_cache_.clear();
        for (const auto& m : mods)
            for (const auto& n : module_exports_(m))
                exports_cache_.push_back(n);
        std::sort(exports_cache_.begin(), exports_cache_.end());
        exports_cache_.erase(std::unique(exports_cache_.begin(), exports_cache_.end()),
                             exports_cache_.end());
        return exports_cache_;
    }

    // The modules this session loaded through (require "std/X" ...).
    void note_requires_(std::string_view code) {
        std::size_t p = 0;
        while ((p = code.find("(require", p)) != std::string_view::npos) {
            p += 8;
            while (p < code.size() && std::isspace(static_cast<unsigned char>(code[p])))
                ++p;
            if (p >= code.size() || code[p] != '"')
                continue;
            ++p;
            const std::size_t start = p;
            while (p < code.size() && code[p] != '"')
                ++p;
            std::string mod(code.substr(start, p - start));
            if (mod.starts_with("std/"))
                mod = mod.substr(4);
            if (!mod.empty()) {
                required_mods_.insert(mod);
                exports_key_.clear();
            }
        }
    }

    // Issue #5038: a grey hint for the word being typed at the end of the
    // buffer: the primitive's arity, else the binding's type. Cached per
    // word so a keystroke does not re-run the checker for the same text.
    char* hint_(const char* buf, int* color, int* bold) {
        if (!buf)
            return nullptr;
        const std::string_view line(buf);
        const std::size_t end = line.size();
        std::size_t start = end;
        auto ident = [](char c) {
            const auto u = static_cast<unsigned char>(c);
            return std::isalnum(u) != 0 || c == '-' || c == '?' || c == '!' || c == '*' ||
                   c == '/' || c == '>' || c == '<' || c == '=' || c == '_';
        };
        while (start > 0 && ident(line[start - 1]))
            --start;
        if (start == end)
            return nullptr;
        const std::string word(line.substr(start, end - start));
        if (const auto hit = hint_cache_.find(word); hit != hint_cache_.end())
            return hit->second.empty() ? nullptr : ::strdup(hit->second.c_str());
        std::string hint;
        const auto& prims = cs_.evaluator().primitives();
        const auto slot = prims.slot_for_name(word);
        if (slot < prims.slot_count()) {
            const auto arity = prims.meta_for_slot(slot).arity;
            hint = arity == 255 ? "  " + word + "/*" : "  " + word + "/" + std::to_string(arity);
        } else if (defs_.contains(word)) {
            // Only names this session defined: a bare unknown word must not
            // draw a checker guess.
            if (const auto ty = type_of_(word); ty != "unknown")
                hint = "  : " + ty;
        }
        if (color)
            *color = 90; // bright black
        if (bold)
            *bold = 0;
        hint_cache_[word] = hint;
        return hint.empty() ? nullptr : ::strdup(hint.c_str());
    }

    // Issue #5043: the type the checker reports for this phrase, or
    // "unknown" when it cannot give one. No new inference here.
    std::string type_of_(std::string_view code) {
        const auto tr = cs_.typecheck_full(code);
        constexpr std::string_view tag = "type: ";
        if (!tr.output.starts_with(tag))
            return "unknown";
        const auto nl = tr.output.find('\n');
        const auto end = nl == std::string::npos ? tr.output.size() : nl;
        std::string t = tr.output.substr(tag.size(), end - tag.size());
        // A lone unresolved type variable (__tNNN) is not a type the user can
        // use; inside a compound type it is an ordinary polymorphic variable.
        const bool bare_var = t.starts_with("__t") && t.find_first_of(" ()[]->") == std::string::npos;
        if (t.empty() || bare_var)
            return "unknown";
        return t;
    }

    template <class R>
    void print_record_(const std::string& code, const R& r) {
        std::string wrote = "(";
        for (const auto& n : repl_defined_names(code)) {
            if (wrote.size() > 1)
                wrote += ' ';
            wrote += '"';
            wrote += repl_record_escape(n);
            wrote += '"';
        }
        wrote += ')';
        std::string rec;
        if (!r) {
            const auto& d = r.error();
            rec = "(hash \"status\" \"error\" \"message\" \"";
            rec += repl_record_escape(d.message);
            rec += '"';
            if (d.location.line > 0) {
                rec += " \"line\" ";
                rec += std::to_string(d.location.line);
            }
            if (d.location.column > 0) {
                rec += " \"column\" ";
                rec += std::to_string(d.location.column);
            }
        } else {
            const auto& v = *r;
            // A top-level define answers the driver with void: the per-form
            // face hands back the bound cell value, which is not the form's.
            const bool statement = !repl_defined_names(code).empty() &&
                                   !(aura::compiler::types::is_error(v) &&
                                     !aura::compiler::types::is_string(v));
            std::string val;
            if (statement || aura::compiler::types::is_void(v))
                val.clear();
            else if (aura::compiler::types::is_error(v) &&
                     !aura::compiler::types::is_string(v))
                val = cs_.evaluator().soft_error_message(v);
            else
                val = aura::compiler::format_value(
                    v, cs_.evaluator().primitives().string_heap(), cs_.evaluator().pairs(), 0,
                    &cs_.evaluator().primitives(), cs_.evaluator().keyword_table());
            const std::string ty =
                (statement || aura::compiler::types::is_void(v)) ? "void" : type_of_(code);
            rec = "(hash \"status\" \"ok\" \"value\" \"";
            rec += repl_record_escape(val);
            rec += "\" \"type\" \"";
            rec += repl_record_escape(ty);
            rec += '"';
        }
        rec += " \"wrote\" ";
        rec += wrote;
        rec += " \"focus\" ";
        if (focus_)
            rec += '"' + repl_record_escape(*focus_) + '"';
        else
            rec += "#f";
        rec += ')';
        std::fprintf(stdout, "%s\n", rec.c_str());
        std::fflush(stdout);
    }

    void eval_and_print_(const std::string& line) {
        note_requires_(line);
        auto r = cs_.eval(line);
        // Issue #5043: the pipe face answers one evaluation with exactly one
        // record on stdout and nothing on stderr. A TTY keeps the human line
        // and adds the record only under AURA_REPL_RECORD.
        if (pipe_mode_ || std::getenv("AURA_REPL_RECORD") != nullptr)
            print_record_(line, r);
        if (pipe_mode_)
            return;
        if (!r) {
            std::fprintf(stderr, "%s: error: %s\n", line.c_str(),
                         r.error().format_with_source(line).c_str());
            std::fflush(stderr);
            // Issue (lyapunov-fact demo 2026-07-28): emit an empty line
            // on stdout so the runner (one readline per eval) can proceed.
            std::fprintf(stdout, "\n");
            std::fflush(stdout);
        } else if (!aura::compiler::types::is_void(*r)) {
            // Issue #5049: an error VALUE (a refused `set-code`, `(car 5)`)
            // printed a bare `<error>` and swallowed its cause. The file
            // faces already read the cause through soft_error_message.
            std::string val;
            if (aura::compiler::types::is_error(*r) &&
                !aura::compiler::types::is_string(*r))
                val = cs_.evaluator().soft_error_message(*r);
            else
                val = aura::compiler::format_value(
                    *r, cs_.evaluator().primitives().string_heap(), cs_.evaluator().pairs(), 0,
                    &cs_.evaluator().primitives(), cs_.evaluator().keyword_table());
            // Issue #5038: the TTY prints the checker's type with the value,
            // in the OCaml shape: "- : Int = 3".
            const auto names = repl_defined_names(line);
            if (!names.empty() &&
                !(aura::compiler::types::is_error(*r) &&
                  !aura::compiler::types::is_string(*r))) {
                // A define answers with its name, not the bound cell value.
                const std::string ty = type_of_(names.front());
                if (ty == "unknown")
                    std::fprintf(stdout, "\n");
                else
                    std::fprintf(stdout, "- : %s = %s\n", ty.c_str(), names.front().c_str());
            } else {
                std::fprintf(stdout, "- : %s = %s\n", type_of_(line).c_str(), val.c_str());
            }
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
        pipe_mode_ = !is_tty; // Issue #5043

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

        // Issue #5039: keep reading while the phrase is open, with a `..`
        // prompt, and evaluate the joined lines once. Ctrl-C (errno EAGAIN)
        // drops the open phrase; Ctrl-D keeps its end-of-file meaning.
        std::string pending;
        while (true) {
            errno = 0;
            prompt_buf_ = prompt_();
            char* raw = linenoise(pending.empty() ? prompt_buf_.c_str() : ".. ");
            if (!raw) {
                if (errno == EAGAIN) {
                    pending.clear();
                    continue;
                }
                break;
            }

            std::string line(raw);
            std::free(raw);

            if (pending.empty() && (line == "(quit)" || line == "(exit)"))
                break;
            if (pending.empty() && line.empty())
                continue;
            if (pending.empty() && line.front() == ':') {
                linenoiseHistoryAdd(line.c_str());
                command_(line);
                continue;
            }

            pending += line;
            pending += '\n';
            if (repl_phrase_incomplete(pending))
                continue;
            while (!pending.empty() && (pending.back() == '\n' || pending.back() == '\r'))
                pending.pop_back();
            std::string phrase = pending;
            pending.clear();
            linenoiseHistoryAdd(phrase.c_str());
            eval_and_print_(phrase);
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
        } else if (cmd == ":src") {
            const auto name = arg.empty() ? focus_name_() : std::optional<std::string>(arg);
            if (!name) {
                std::fprintf(stdout, "no-focus\n");
            } else {
                const auto src = live_source_of_(*name);
                std::fprintf(stdout, "%s\n", src.empty() ? "no-workspace" : src.c_str());
            }
        } else if (cmd == ":gate") {
            if (!arg.empty())
                gate_ = arg;
            if (gate_.empty())
                std::fprintf(stdout, "gate: (none)\n");
            else
                std::fprintf(stdout, "gate: %s\n", gate_.c_str());
        } else if (cmd == ":fuel") {
            if (!arg.empty()) {
                try {
                    fuel_ = std::stol(arg);
                } catch (...) {
                }
            }
            std::fprintf(stdout, "fuel: %ld\n", fuel_);
        } else if (cmd == ":propose") {
            propose_(arg);
        } else if (cmd == ":focus") {
            // Issue #5048: the screen names the definition being edited.
            if (arg.empty() && !focus_) {
                std::fprintf(stdout, "no-focus\n");
            } else if (arg.empty()) {
                std::fprintf(stdout, "%s %d\n", focus_->c_str(), steps_for_(*focus_));
            } else {
                focus_ = arg;
                std::fprintf(stdout, "%s %d\n", arg.c_str(), steps_for_(arg));
            }
        } else if (cmd == ":doc") {
            // Issue #5037: the description a primitive already carries.
            const auto& prims = cs_.evaluator().primitives();
            const auto slot = prims.slot_for_name(arg);
            if (arg.empty()) {
                std::fprintf(stdout, ":doc: usage: :doc NAME\n");
            } else if (slot >= prims.slot_count()) {
                std::fprintf(stdout, ":doc %s: no such primitive (try :type)\n", arg.c_str());
            } else if (const auto& doc = prims.meta_for_slot(slot).doc; doc.empty()) {
                std::fprintf(stdout, ":doc %s: no description (try :type)\n", arg.c_str());
            } else {
                std::fprintf(stdout, "%s\n", doc.c_str());
            }
        } else if (cmd == ":type") {
            // Issue #5038: ask the checker only; the argument is never run.
            if (arg.empty()) {
                std::fprintf(stdout, ":type: usage: :type EXPR\n");
            } else {
                const auto& prims = cs_.evaluator().primitives();
                const auto slot = prims.slot_for_name(arg);
                if (slot < prims.slot_count()) {
                    const auto arity = prims.meta_for_slot(slot).arity;
                    std::printf("- : %s/%s\n", arg.c_str(), arity == 255 ? "*" : std::to_string(arity).c_str());
                } else {
                    std::fprintf(stdout, "- : %s\n", type_of_(arg).c_str());
                }
            }
        } else {
            std::fprintf(stdout,
                         "REPL commands:\n"
                         "  :load FILE      evaluate FILE, remember its defines\n"
                         "  :reload [FILE]  re-read FILE; rebind only changed defines\n"
                         "  :ws             list live definitions and where they came from\n"
                         "  :worldline      mutation-log summary + snapshots (:history)\n"
                         "  :snapshot [LBL] remember every tracked define\n"
                         "  :restore ID     put changed defines back as of snapshot ID\n"
                         "  :type EXPR      print EXPR's type without running it\n"
                         "  :doc NAME       print a primitive's description\n"
                         "  :focus [NAME]   name the definition the prompt shows\n"
                         "  :src [NAME]     live workspace source of NAME (or the focus)\n"
                         "  :propose NAME CODE  snapshot, rebind, eval, keep or roll back\n"
                         "  :gate EXPR      check an accepted proposal must not return #f\n"
                         "  :fuel N         step budget for a proposal (default 100000)\n");
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
    bool pipe_mode_ = false; // Issue #5043: pipe answers with a record
    std::string prompt_buf_; // Issue #5048: linenoise keeps the prompt pointer
    std::optional<std::string> focus_;            // Issue #5048
    std::map<std::string, int> focus_steps_;      // Issue #5048
    std::string gate_;                            // Issue #5042
    long fuel_ = 100000;                          // Issue #5042
    std::unordered_map<std::string, std::string> hint_cache_; // Issue #5038
    std::set<std::string> required_mods_; // Issue #5040
    std::unordered_map<std::string, std::vector<std::string>> export_cache_; // Issue #5040
    std::string exports_key_;                  // Issue #5040
    std::vector<std::string> exports_cache_;   // Issue #5040
    static inline Repl* g_repl_ = nullptr;
};

} // namespace aura
