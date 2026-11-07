module;
#include <cctype>
#include <cstdint>
#include <expected>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "reflect/diag_error_kind_names.hh"

export module aura.diag;
import std;

namespace aura::diag {

// Error categories — each maps to a distinct failure mode
export enum class ErrorKind : std::uint8_t {
    // Parse errors
    ParseError,
    UnexpectedToken,
    UnterminatedSExpr,
    // Semantic errors
    UnboundVariable,
    DivisionByZero,
    InvalidClosure,
    ArityMismatch,
    TypeError,
    // IR pipeline errors
    IRCorruption,
    IRNoReturn,
    // Internal
    InternalError,
    OutOfMemory,
    // Issue #124: uncaught exception (Raise without TryBegin on
    // the exception stack). Reported by the IR interpreter when
    // an exception propagates past the top of the call stack.
    UncaughtException,
    // Informational (not an error)
    Note,
    // Issue #103: Warning — the inference degrades to Dynamic
    // (or otherwise produces a sound-but-loose result). Not a
    // hard error; the program type-checks but the result type
    // is wider than the user expected.
    Warning,
    // Issue #4349: the workspace changed under compiled state (ast:restore
    // over a post-mutate snapshot leaves env closures holding rebind-era
    // node ids that are dead in the restored flat). Not an internal
    // error — a documented, recoverable face: run (eval-current) to
    // re-bind, then retry.
    StaleWorkspace,
    // Issue #4909: a sandbox / capability refusal. Not a type error: the
    // program is fine, the run did not grant an effect. format() adds a
    // `hint:` line with the smallest grant that unblocks it.
    EffectDenied,
    // Issue #4909: the program's own (error ...) / (raise ...) escaped.
    UserError,
    // Issue #4918: a (require ...) / (load ...) that could not resolve,
    // parse or evaluate its module. Not an interpreter bug.
    ModuleError,
    // Issue #4917: a host I/O operation failed (path + reason).
    IoError,
    // Issue #4909: a heap mutator (set-car! / vector-set! / hash-set! ...)
    // whose MutationBoundaryGuard admit was refused (AdmissionRejected: ...).
    // A runtime admission decision, not a type error in the program.
    MutationRefused,
};

// ── Blame Info (design §6.3) ────────────────────────────────────
export enum class BlameParty : std::uint8_t {
    Caller,     // 调用者 — 参数类型不匹配
    Annotation, // 类型标注 — 标注与推断冲突
    Implicit,   // 隐式边界 — 无标注的默认边界
    System,     // 基元/系统类型错误
    Narrowing,  // Occurrence Typing narrowing —
                // 谓词对变量的类型细化，后
                // 续诊断中可附加 narrowing
                // 来源（predicate name + 源
                // cond NodeId）。Issue #342。
};

export struct BlameInfo {
    BlameParty party = BlameParty::Implicit;
    std::string annotation_src;    // 标注来源，如 ": x Int"
    std::string phase = "compile"; // "compile" | "runtime"
};

// Convert ErrorKind to human-readable string (display phrases table).
// Identifiers: kDiagErrorKindIdents (P2996-aligned). Wave C1 wire.
export constexpr std::string_view kind_name(ErrorKind k) {
    return diag_error_kind_display(static_cast<std::size_t>(k));
}

export constexpr std::string_view kind_ident(ErrorKind k) {
    return diag_error_kind_ident(static_cast<std::size_t>(k));
}

export constexpr std::string_view party_name(BlameParty p) {
    return blame_party_name(static_cast<std::size_t>(p));
}

// ── Issue #4916: source file table ─────────────────────────────
// A diagnostic names the file it came from. file_id 0 is the entry
// program (the file given on the command line, when there is exactly
// one); every module that (require ...) loads gets its own id. The
// table also maps a module's FlatAST to its id so the evaluator can
// stamp a location that was produced inside that module's code.
export struct SourceFile {
    std::string path;
    std::string content;
};

namespace detail {
    struct SourceFileTable {
        std::mutex mu;
        std::vector<SourceFile> files; // file_id - 1
        std::vector<std::pair<const void*, std::uint32_t>> flats;
        std::uint32_t entry = 0;
    };
    inline SourceFileTable& source_file_table() {
        static SourceFileTable table;
        return table;
    }
} // namespace detail

// Register (or refresh) a file; returns its id (>= 1).
export inline std::uint32_t register_source_file(std::string path, std::string content) {
    auto& table = detail::source_file_table();
    const std::scoped_lock lock(table.mu);
    for (std::size_t idx = 0; idx < table.files.size(); ++idx) {
        if (table.files[idx].path == path) {
            table.files[idx].content = std::move(content);
            return static_cast<std::uint32_t>(idx + 1);
        }
    }
    table.files.push_back(SourceFile{std::move(path), std::move(content)});
    return static_cast<std::uint32_t>(table.files.size());
}

// The entry program: locations with file_id 0 are shown in this file.
export inline void set_entry_source_file(std::uint32_t file_id) {
    auto& table = detail::source_file_table();
    const std::scoped_lock lock(table.mu);
    table.entry = file_id;
}

export inline std::optional<SourceFile> source_file(std::uint32_t file_id) {
    auto& table = detail::source_file_table();
    const std::scoped_lock lock(table.mu);
    if (file_id == 0)
        file_id = table.entry;
    if (file_id == 0 || file_id > table.files.size())
        return std::nullopt;
    return table.files[file_id - 1];
}

// Issue #4916: the path a report shows — relative to the cwd when the
// file is under it (helper.aura, kits/x/main.aura), else as registered.
export inline std::string display_source_path(std::string_view path) {
    if (path.empty() || path[0] != '/')
        return std::string(path);
    std::error_code err;
    auto cwd = std::filesystem::current_path(err).string();
    if (err || cwd.empty())
        return std::string(path);
    if (!cwd.ends_with('/'))
        cwd += '/';
    if (path.starts_with(cwd) && path.size() > cwd.size())
        return std::string(path.substr(cwd.size()));
    return std::string(path);
}

export inline std::string source_file_path(std::uint32_t file_id) {
    auto& table = detail::source_file_table();
    const std::scoped_lock lock(table.mu);
    if (file_id == 0)
        file_id = table.entry;
    if (file_id == 0 || file_id > table.files.size())
        return {};
    return table.files[file_id - 1].path;
}

// Bind a module FlatAST to its file. id 0 drops the binding.
export inline void bind_flat_to_source_file(const void* flat, std::uint32_t file_id) {
    auto& table = detail::source_file_table();
    const std::scoped_lock lock(table.mu);
    for (auto pos = table.flats.begin(); pos != table.flats.end(); ++pos) {
        if (pos->first == flat) {
            if (file_id == 0)
                table.flats.erase(pos);
            else
                pos->second = file_id;
            return;
        }
    }
    if (file_id != 0)
        table.flats.emplace_back(flat, file_id);
}

// A module arena is being freed: drop every FlatAST bound to that file
// so a later FlatAST at a reused address is not shown as that module.
export inline void unbind_source_file_flats(std::string_view path) {
    auto& table = detail::source_file_table();
    const std::scoped_lock lock(table.mu);
    std::uint32_t file_id = 0;
    for (std::size_t idx = 0; idx < table.files.size(); ++idx)
        if (table.files[idx].path == path)
            file_id = static_cast<std::uint32_t>(idx + 1);
    if (file_id == 0)
        return;
    std::erase_if(table.flats, [file_id](const auto& bound) { return bound.second == file_id; });
}

export inline std::uint32_t source_file_for_flat(const void* flat) {
    auto& table = detail::source_file_table();
    const std::scoped_lock lock(table.mu);
    for (const auto& [bound_flat, bound_id] : table.flats)
        if (bound_flat == flat)
            return bound_id;
    return 0;
}

// Source location (line/column, 1-indexed)
export struct SourceLocation {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t file_id = 0;

    bool valid() const { return line > 0; }
    std::string format() const {
        if (!valid())
            return "?";
        // Issue #4916: file:line:col whenever the file is known.
        if (auto path = source_file_path(file_id); !path.empty())
            return std::format("{}:{}:{}", display_source_path(std::move(path)), line, column);
        return std::format("{}:{}", line, column);
    }
};

// ── Issue #4909: sandbox refusals are actionable ────────────────
// The refusal text is "effect-denied: <effect> not granted …" (or the
// older "capability denied: io-… required"). Returns the effect name,
// or empty when the message is not a refusal.
export inline std::string effect_denied_effect(std::string_view msg) {
    constexpr std::string_view kEff = "effect-denied: ";
    if (const auto pos = msg.find(kEff); pos != std::string_view::npos) {
        const auto rest = msg.substr(pos + kEff.size());
        const auto end = rest.find_first_of(" ;");
        return std::string(rest.substr(0, end));
    }
    constexpr std::string_view kCap = "capability denied: ";
    if (const auto pos = msg.find(kCap); pos != std::string_view::npos) {
        const auto rest = msg.substr(pos + kCap.size());
        if (rest.starts_with("io-read"))
            return "read";
        if (rest.starts_with("io-write"))
            return "write";
        if (rest.starts_with("exec"))
            return "exec";
        const auto end = rest.find_first_of(" ;");
        return std::string(rest.substr(0, end));
    }
    return {};
}

// One line naming the smallest grant that unblocks the run.
export inline std::string effect_denied_hint(std::string_view effect) {
    auto file = display_source_path(source_file_path(0));
    if (file.empty())
        file = "file.aura";
    if (effect == "mutate")
        return std::format("hint: run 'aura --preset dev {}' (or 'aura soft {}'; grants mutate "
                           "on this workspace); for local development AURA_SANDBOX=off; in "
                           "production grant the mutate effect",
                           file, file);
    if (effect == "write" || effect == "exec" || effect == "network" || effect == "ffi" ||
        effect == "read")
        return std::format("hint: run 'aura --grant {} {}'{}; for local development "
                           "AURA_SANDBOX=off; in production grant the {} effect",
                           effect, file,
                           effect == "write" ? std::string_view(" (or --preset dev)")
                                             : std::string_view(""),
                           effect);
    // Issue #4912: Write is scoped (ci preset / --grant write:<dir>/).
    if (effect == "write-path")
        return std::format("hint: this run writes only under the directories listed above; write "
                           "there, add one with 'aura --grant write:<dir>/ {}', or use "
                           "'--preset dev' to write anywhere",
                           file);
    if (effect == "macro-self-evo")
        return {"hint: user macros need MacroSelfEvo; for local development "
                "AURA_SANDBOX=off ('aura soft' does not grant it); in production "
                "grant MacroSelfEvo"};
    return std::format("hint: for local development AURA_SANDBOX=off; in production grant the "
                       "{} effect",
                       effect.empty() ? std::string_view("missing") : effect);
}

// ── Issue #4909: heap-mutate admission refusals ─────────────────
// The refusal text is "naked mutate: <prim> requires MutationBoundaryGuard
// (AdmissionRejected: <reason>)". One line naming what to do next.
export inline std::string naked_mutate_hint(std::string_view msg) {
    if (msg.find("densify") != std::string_view::npos ||
        msg.find("production-residual-sticky") != std::string_view::npos)
        return {"hint: the live AST is mid-remap after set-code / mutate:rebind; run "
                "(eval-current) before mutating data, or use AURA_SANDBOX=off for local "
                "development"};
    if (msg.find("concurrency-health") != std::string_view::npos ||
        msg.find("hold-budget") != std::string_view::npos ||
        msg.find("mailbox") != std::string_view::npos ||
        msg.find("region-overlap") != std::string_view::npos)
        return {"hint: too many concurrent mutations hold the boundary; retry the step or "
                "run fewer mutating agents at once"};
    if (msg.find("security-schedule") != std::string_view::npos)
        return {"hint: a security audit of an earlier mutation is still running; retry the "
                "step after it finishes"};
    return {"hint: wrap the mutation in a mutation boundary (mutate:* / agent step) or use "
            "AURA_SANDBOX=off for local development"};
}

// Structured diagnostic — typed error with location and context
export struct Diagnostic {
    ErrorKind kind = ErrorKind::InternalError;
    std::string message;
    SourceLocation location;
    std::uint32_t node_id = ~0u; // FlatAST NodeId (~0 = unknown)
    std::string suggestion;      // "did you mean ...?" text
    std::vector<std::string> context_stack;
    std::optional<BlameInfo> blame; // 结构化 blame 信息 (design §6.3)

    Diagnostic() = default;

    Diagnostic(ErrorKind k, std::string msg, SourceLocation loc = {}, std::uint32_t nid = ~0u)
        : kind(k)
        , message(std::move(msg))
        , location(loc)
        , node_id(nid) {}

    // Set suggestion text (e.g., "did you mean 'foo'?")
    Diagnostic& with_suggestion(std::string s) & {
        suggestion = std::move(s);
        return *this;
    }
    Diagnostic&& with_suggestion(std::string s) && {
        suggestion = std::move(s);
        return std::move(*this);
    }

    // Set structured blame info
    Diagnostic& with_blame(BlameInfo b) & {
        blame = std::move(b);
        return *this;
    }
    Diagnostic&& with_blame(BlameInfo b) && {
        blame = std::move(b);
        return std::move(*this);
    }

    // Add context frame for error traceback
    Diagnostic& context(std::string_view ctx) & {
        context_stack.push_back(std::string(ctx));
        return *this;
    }
    Diagnostic&& context(std::string_view ctx) && {
        context_stack.push_back(std::string(ctx));
        return std::move(*this);
    }

    // Format as human-readable error string (single line)
    // Issue #4909: the kind a reader sees. A refusal raised through the
    // generic primitive-error path still arrives as TypeError; it is a
    // sandbox decision, not a type error.
    ErrorKind display_kind() const {
        if (kind == ErrorKind::TypeError &&
            (message.starts_with("effect-denied:") || message.starts_with("capability denied:")))
            return ErrorKind::EffectDenied;
        if (kind == ErrorKind::TypeError && message.starts_with("naked mutate: "))
            return ErrorKind::MutationRefused;
        return kind;
    }

    std::string format() const {
        std::string out;
        if (location.valid())
            out += std::format("{}: ", location.format());
        else if (node_id != ~0u)
            out += std::format("node[{}]: ", node_id);

        const auto shown = display_kind();
        out += std::string(kind_name(shown));
        if (shown == ErrorKind::EffectDenied && message.starts_with("effect-denied: "))
            out += ": " + message.substr(std::string_view("effect-denied: ").size());
        else if (shown == ErrorKind::MutationRefused)
            out += ": " + message.substr(std::string_view("naked mutate: ").size());
        else
            out += ": " + message;

        // Blame info (design §6.3)
        if (blame) {
            // Display party in lowercase (historical format strings).
            // Issue #342: Narrowing — annotation_src carries predicate + cond NodeId.
            const auto party_ident = party_name(blame->party);
            std::string party_str(party_ident);
            for (char& c : party_str)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            if (party_str == "unknown")
                party_str = "?";
            out += std::format("\n  blamed: {} ({})", party_str, blame->phase);
            if (!blame->annotation_src.empty())
                out += std::format("\n  annotation: {}", blame->annotation_src);
        }

        if (!suggestion.empty())
            out += "\n  " + suggestion;

        // Issue #4909: every refusal carries one actionable hint line
        // (also when it is wrapped, e.g. inside a require failure).
        if (message.find("hint:") == std::string::npos) {
            if (const auto eff = effect_denied_effect(message); !eff.empty())
                out += "\n  " + effect_denied_hint(eff);
            else if (shown == ErrorKind::MutationRefused)
                out += "\n  " + naked_mutate_hint(message);
        }

        // Print context in reverse order (innermost first)
        for (auto it = context_stack.rbegin(); it != context_stack.rend(); ++it)
            out += std::format("\n  {}", it->starts_with("at ") ? *it : "while " + *it);

        return out;
    }

    // Format with source line context and caret pointing to error location
    // Example:
    //   5:1: parse error: expected expression, got ')'
    //     |
    //   5 | (+ 1 2)
    //     | ^^^^^^^
    //     |   suggestion: ...
    std::string format_with_source(std::string_view source) const {
        auto out = format();

        // Issue #4916: the excerpt comes from the file the error is in,
        // not from whatever program text the caller holds.
        std::optional<SourceFile> own;
        if (location.file_id != 0) {
            own = source_file(location.file_id);
            if (own)
                source = own->content;
        }
        if (location.valid() && !source.empty()) {
            // Extract the relevant line from source (1-indexed)
            std::uint32_t current_line = 1;
            const char* line_start = source.data();
            const char* line_end = nullptr;
            const char* p = source.data();
            const char* end = source.data() + source.size();

            while (p < end && current_line < location.line) {
                if (*p == '\n') {
                    current_line++;
                    line_start = p + 1;
                }
                p++;
            }

            if (current_line == location.line) {
                // Find end of this line
                line_end = line_start;
                while (line_end < end && *line_end != '\n' && *line_end != '\r')
                    line_end++;

                auto line_str = std::string_view(line_start, line_end - line_start);
                out += std::format("\n   |");
                out += std::format("\n{:>3} | {}", location.line, line_str);

                // Caret line: underline the error span
                out += "\n   | ";
                if (location.column > 0) {
                    for (std::uint32_t i = 1; i < location.column; ++i)
                        out += ' ';
                    out += '^';
                    // Multi-character span: assume token length from message
                    // Simple heuristic: underline from column to end of word
                    auto tok_start = line_start + (location.column - 1);
                    if (tok_start < line_end) {
                        auto tok_end = tok_start;
                        while (tok_end < line_end &&
                               !std::isspace(static_cast<unsigned char>(*tok_end)) &&
                               *tok_end != ')' && *tok_end != '(')
                            tok_end++;
                        for (auto c = tok_start + 1; c < tok_end; ++c)
                            out += '^';
                    }
                }
            }
        }

        return out;
    }

    // Short one-line summary (no location, no context)
    std::string summary() const { return std::string(kind_name(kind)) + ": " + message; }
};

// Result type — used throughout the pipeline
export template <typename T> using Result = std::expected<T, Diagnostic>;

// Convenience: Result<void> for operations that don't produce a value
export using VoidResult = Result<void>;

// ── Pipeline-stage Result aliases (Issue #127) ────────────────
//
// Each pipeline stage has its own Result<T> alias for
// readability and to make the stage boundaries explicit at
// the type level. The aliases are all just
// `Result<T>` instantiations; they do not introduce new
// types.
//
//   ParseResult<T>    — for the lexer/parser stage
//   LowerResult<T>    — for the lowering stage
//   CompileResult<T>  — for the full compile pipeline
//                       (parse + typecheck + lower + IR)
//
// New code should prefer the most specific alias. Legacy
// code may use `Result<T>` directly, which is equivalent.
export template <typename T> using ParseResult = Result<T>;
export template <typename T> using LowerResult = Result<T>;
export template <typename T> using CompileResult = Result<T>;

// ── DiagnosticCollector — collects diagnostics during compilation ─
export class DiagnosticCollector {
public:
    void report(aura::diag::Diagnostic d) { diagnostics_.push_back(std::move(d)); }

    bool has_errors() const {
        for (auto& d : diagnostics_)
            if (d.kind != ErrorKind::Note)
                return true;
        return false;
    }

    std::span<const aura::diag::Diagnostic> diagnostics() const { return diagnostics_; }

    void clear() { diagnostics_.clear(); }

private:
    std::vector<aura::diag::Diagnostic> diagnostics_;
};

} // namespace aura::diag
