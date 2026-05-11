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
    // IR pipeline errors
    IRCorruption,
    IRNoReturn,
    // Internal
    InternalError,
    OutOfMemory,
};

// Source location (line/column, 1-indexed)
export struct SourceLocation {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t file_id = 0;

    bool valid() const { return line > 0; }
    std::string format() const {
        if (!valid()) return "?";
        return std::format("{}:{}", line, column);
    }
};

// Structured diagnostic — typed error with location and context
export struct Diagnostic {
    ErrorKind kind = ErrorKind::InternalError;
    std::string message;
    SourceLocation location;
    std::vector<std::string> context_stack;

    Diagnostic() = default;

    Diagnostic(ErrorKind k, std::string msg,
               SourceLocation loc = {})
        : kind(k), message(std::move(msg)), location(loc) {}

    // Add context frame for error traceback
    Diagnostic& context(std::string_view ctx) & {
        context_stack.push_back(std::string(ctx));
        return *this;
    }

    Diagnostic&& context(std::string_view ctx) && {
        context_stack.push_back(std::string(ctx));
        return std::move(*this);
    }

    // Format as human-readable error string
    std::string format() const {
        std::string out;
        auto kind_str = [](ErrorKind k) -> std::string_view {
            switch (k) {
            case ErrorKind::ParseError:        return "parse error";
            case ErrorKind::UnexpectedToken:   return "unexpected token";
            case ErrorKind::UnterminatedSExpr: return "unterminated s-expression";
            case ErrorKind::UnboundVariable:   return "unbound variable";
            case ErrorKind::DivisionByZero:    return "division by zero";
            case ErrorKind::InvalidClosure:    return "invalid closure";
            case ErrorKind::ArityMismatch:     return "arity mismatch";
            case ErrorKind::IRCorruption:      return "IR corruption";
            case ErrorKind::IRNoReturn:        return "no return from IR function";
            case ErrorKind::InternalError:     return "internal error";
            case ErrorKind::OutOfMemory:       return "out of memory";
            }
            return "unknown error";
        };

        out += std::format("error: {}", message);
        if (location.valid())
            out += std::format(" at {}", location.format());

        // Print context in reverse order (innermost first)
        for (auto it = context_stack.rbegin(); it != context_stack.rend(); ++it)
            out += std::format("\n  while {}", *it);

        return out;
    }
};

// Result type — used throughout the pipeline
// Replace EvalResult, ParseResult, etc. with Result<T>
export template <typename T>
using Result = std::expected<T, Diagnostic>;

// Convenience: Result<void> for operations that don't produce a value
export using VoidResult = Result<void>;

} // namespace aura::diag
