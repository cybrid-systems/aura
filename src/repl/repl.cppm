module;
#include <cstdlib>
#include <cstdio>
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

    void run() {
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
            if (!raw) break;

            std::string line(raw);
            std::free(raw);

            if (line == "(quit)" || line == "(exit)") break;
            if (line.empty()) continue;

            linenoiseHistoryAdd(line.c_str());

            auto r = cs_.eval(line);
            if (!r) {
                std::fprintf(stderr, "%s: error: %s\n",
                             line.c_str(), r.error().format_with_source(line).c_str());
                std::fflush(stderr);
            } else if (!aura::compiler::types::is_void(*r)) {
                auto val = aura::compiler::format_value(
                    *r, cs_.evaluator().primitives().string_heap(),
                    cs_.evaluator().pairs(), 0, &cs_.evaluator().primitives(),
                    cs_.evaluator().keyword_table());
                std::fprintf(stdout, "%s\n", val.c_str());
                std::fflush(stdout);
            }
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
