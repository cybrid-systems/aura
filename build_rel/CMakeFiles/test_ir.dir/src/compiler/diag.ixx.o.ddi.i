# 0 "/home/dev/code/aura/src/compiler/diag.ixx"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/diag.ixx"
export module aura.diag;
import std;

namespace aura::diag {


export enum class ErrorKind : std::uint8_t {

    ParseError,
    UnexpectedToken,
    UnterminatedSExpr,

    UnboundVariable,
    DivisionByZero,
    InvalidClosure,
    ArityMismatch,
    TypeError,

    IRCorruption,
    IRNoReturn,

    InternalError,
    OutOfMemory,

    Note,
};


export constexpr std::string_view kind_name(ErrorKind k) {
    switch (k) {
    case ErrorKind::ParseError: return "parse error";
    case ErrorKind::UnexpectedToken: return "unexpected token";
    case ErrorKind::UnterminatedSExpr: return "unterminated s-expr";
    case ErrorKind::UnboundVariable: return "unbound variable";
    case ErrorKind::DivisionByZero: return "division by zero";
    case ErrorKind::InvalidClosure: return "invalid closure";
    case ErrorKind::ArityMismatch: return "arity mismatch";
    case ErrorKind::TypeError: return "type error";
    case ErrorKind::IRCorruption: return "IR corruption";
    case ErrorKind::IRNoReturn: return "no return";
    case ErrorKind::InternalError: return "internal error";
    case ErrorKind::OutOfMemory: return "out of memory";
    case ErrorKind::Note: return "note";
    }
    return "unknown";
}


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


export struct Diagnostic {
    ErrorKind kind = ErrorKind::InternalError;
    std::string message;
    SourceLocation location;
    std::uint32_t node_id = ~0u;
    std::string suggestion;
    std::vector<std::string> context_stack;

    Diagnostic() = default;

    Diagnostic(ErrorKind k, std::string msg,
               SourceLocation loc = {}, std::uint32_t nid = ~0u)
        : kind(k), message(std::move(msg)), location(loc), node_id(nid) {}


    Diagnostic& with_suggestion(std::string s) & {
        suggestion = std::move(s);
        return *this;
    }
    Diagnostic&& with_suggestion(std::string s) && {
        suggestion = std::move(s);
        return std::move(*this);
    }


    Diagnostic& context(std::string_view ctx) & {
        context_stack.push_back(std::string(ctx));
        return *this;
    }
    Diagnostic&& context(std::string_view ctx) && {
        context_stack.push_back(std::string(ctx));
        return std::move(*this);
    }


    std::string format() const {
        std::string out;
        if (location.valid())
            out += std::format("{}: ", location.format());
        else if (node_id != ~0u)
            out += std::format("node[{}]: ", node_id);

        out += std::string(kind_name(kind));
        out += ": " + message;

        if (!suggestion.empty())
            out += "\n  " + suggestion;


        for (auto it = context_stack.rbegin(); it != context_stack.rend(); ++it)
            out += std::format("\n  while {}", *it);

        return out;
    }


    std::string summary() const {
        return std::string(kind_name(kind)) + ": " + message;
    }
};


export template <typename T>
using Result = std::expected<T, Diagnostic>;


export using VoidResult = Result<void>;


export class DiagnosticCollector {
public:
    void report(aura::diag::Diagnostic d) {
        diagnostics_.push_back(std::move(d));
    }

    bool has_errors() const {
        for (auto& d : diagnostics_)
            if (d.kind != ErrorKind::Note)
                return true;
        return false;
    }

    std::span<const aura::diag::Diagnostic> diagnostics() const {
        return diagnostics_;
    }

    void clear() {
        diagnostics_.clear();
    }

private:
    std::vector<aura::diag::Diagnostic> diagnostics_;
};

}
