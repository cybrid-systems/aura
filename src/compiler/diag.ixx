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
};

// ── Blame Info (design §6.3) ────────────────────────────────────
export enum class BlameParty : std::uint8_t {
    Caller,     // 调用者 — 参数类型不匹配
    Annotation, // 类型标注 — 标注与推断冲突
    Implicit,   // 隐式边界 — 无标注的默认边界
    System,     // 基元/系统类型错误
};

export struct BlameInfo {
    BlameParty party = BlameParty::Implicit;
    std::string annotation_src;    // 标注来源，如 ": x Int"
    std::string phase = "compile"; // "compile" | "runtime"
};

// Convert ErrorKind to human-readable string
export constexpr std::string_view kind_name(ErrorKind k) {
    switch (k) {
        case ErrorKind::ParseError:
            return "parse error";
        case ErrorKind::UnexpectedToken:
            return "unexpected token";
        case ErrorKind::UnterminatedSExpr:
            return "unterminated s-expr";
        case ErrorKind::UnboundVariable:
            return "unbound variable";
        case ErrorKind::DivisionByZero:
            return "division by zero";
        case ErrorKind::InvalidClosure:
            return "invalid closure";
        case ErrorKind::ArityMismatch:
            return "arity mismatch";
        case ErrorKind::TypeError:
            return "type error";
        case ErrorKind::IRCorruption:
            return "IR corruption";
        case ErrorKind::IRNoReturn:
            return "no return";
        case ErrorKind::InternalError:
            return "internal error";
        case ErrorKind::OutOfMemory:
            return "out of memory";
        case ErrorKind::Note:
            return "note";
    }
    return "unknown";
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
        return std::format("{}:{}", line, column);
    }
};

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
    std::string format() const {
        std::string out;
        if (location.valid())
            out += std::format("{}: ", location.format());
        else if (node_id != ~0u)
            out += std::format("node[{}]: ", node_id);

        out += std::string(kind_name(kind));
        out += ": " + message;

        // Blame info (design §6.3)
        if (blame) {
            const char* party_str = "?";
            switch (blame->party) {
                case BlameParty::Caller:
                    party_str = "caller";
                    break;
                case BlameParty::Annotation:
                    party_str = "annotation";
                    break;
                case BlameParty::Implicit:
                    party_str = "implicit";
                    break;
                case BlameParty::System:
                    party_str = "system";
                    break;
            }
            out += std::format("\n  blamed: {} ({})", party_str, blame->phase);
            if (!blame->annotation_src.empty())
                out += std::format("\n  annotation: {}", blame->annotation_src);
        }

        if (!suggestion.empty())
            out += "\n  " + suggestion;

        // Print context in reverse order (innermost first)
        for (auto it = context_stack.rbegin(); it != context_stack.rend(); ++it)
            out += std::format("\n  while {}", *it);

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
                        while (tok_end < line_end && !std::isspace(static_cast<unsigned char>(*tok_end)) &&
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
