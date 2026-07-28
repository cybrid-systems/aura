module;
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>
#include "../linenoise/linenoise.h"

export module aura.repl;

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

export namespace aura {

class Repl {
public:
    explicit Repl(aura::compiler::CompilerService& cs) : cs_(cs) {
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
                eval_and_print_(line);
            }
            return;
        }

        const char* home = std::getenv("HOME");
        if (!home)
            home = ".";
        history_path_ = std::string(home) + "/.config/aura/history";
        std::filesystem::create_directories(
            std::filesystem::path(history_path_).parent_path());
        linenoiseHistoryLoad(history_path_.c_str());

        std::fprintf(stdout, "Aura v0.2 \u2014 LLVM JIT / Sound Gradual Typing / C FFI\n");
        std::fprintf(stdout, "  (quit) to exit, Ctrl+D for EOF\n");
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
            eval_and_print_(line);
        }
        std::fprintf(stdout, "\n");
        std::fflush(stdout);
    }

    using CompletionCallback = std::function<std::vector<std::string>(std::string_view)>;
    void set_completion_callback(CompletionCallback cb) { completion_ = std::move(cb); }

private:
    aura::compiler::CompilerService& cs_;
    CompletionCallback completion_;
    std::string history_path_;
    static inline Repl* g_repl_ = nullptr;
};

} // namespace aura
