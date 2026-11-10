#include <cstdlib>
#include "../lib/aura_value_abi.h" // #4502: single encoding source
#define AURA_ABI_STR2(x) #x
#define AURA_ABI_STR(x) AURA_ABI_STR2(x)
#include "compiler/aura_jit.h"
#include "compiler/messaging_bridge.h"
#include "compiler/runtime_shared.h"
#include "compiler/observability_snapshot.h"
#include "compiler/observability_metrics.h"
#include "compiler/observability_logger.h" // for snapshot_to_json
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#if defined(__linux__) && !defined(__APPLE__)
#include <execinfo.h>
#include <dlfcn.h>
#include <fcntl.h> // Issue #4931: open(2) for /proc/self/maps in the crash handler
#endif
#include "serve/serve_async.h"
#include "serve/scheduler.h"
#include "serve/http_health.h"
#include "serve/aura_platform.h"
#include "serve/runtime_production_abi.h" // #2955 production ABI self-check
#include "compiler/runtime_paths.h"       // #906
#include "core/parse_number.hh"           // #4936 exception-free parse
#include "core/diag_print.hh"             // #4936 eprintln_nothrow
#include "core/ownership.hh"              // #4936 unique_file
#include "compiler/ir_buffer_abi.h"       // #4936 IrBufferPtr
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>
// Issue #2076 / #2053: production security defaults (must be before import std).
#include "compiler/security_defaults.hh"
#include "compiler/jit_assumptions.hh" // #4569 --jit-session stats
#include "compiler/write_scope.hh"     // #4912 write:out/

import std;
import aura.core;
import aura.compiler.service;
import aura.compiler.query;
import aura.compiler.lowering;
import aura.compiler.pass_manager;
import aura.compiler.ir_executor;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.core.type;
import aura.parser.parser;
import aura.compiler.ir;
import aura.diag;
import aura.compiler.cache;
import aura.compiler.value;
import aura.repl;

// C-linkage bridge to reflection-based --inspect
// (implemented in ir_reflect_serialize.cpp, compiled with -freflection).
// #4936: the returned buffer is held in aura::ir::IrBufferPtr
// (compiler/ir_buffer_abi.h, included above import std).
extern "C" bool aura_emit_object_file(const void* mod, const char* path);
extern "C" bool aura_emit_native_file(const char* source, const char* out_path,
                                      const void* functions, unsigned int num_functions);

extern "C" void aura_reset_runtime(); // Issue #137: cleanup at exit
extern "C" int aura_hygiene_expand_deny_blocks_eval(void) noexcept;
extern "C" void aura_set_prim_registration(const char* c_code);
extern "C" std::uint64_t aura_jit_float_pool_size(); // Issue #4570 --jit-session
extern "C" void aura_set_string_pool(const char** strings, unsigned int count);
// Issue #243: defuse_version_ setter/getter so the AOT
// bridge can mangle + register with the current emit epoch.
extern "C" void aura_set_aot_defuse_version(std::uint64_t v);
extern "C" std::uint64_t aura_get_aot_defuse_version(void);

// Issue #157 Phase 5: jit:metrics --serve command accessors.
// Exposes the runtime telemetry counters (bypass_count /
// unchecked_fastpath_count / deopt_count) to ops dashboards.
// After Phase 2, bypass_count stays at 0 in steady state and
// acts as a regression detector. unchecked_fastpath_count and
// deopt_count together show how often concurrent mutate defeats
// the L2 fastpath (multi-fiber serve).
extern "C" std::uint64_t aura_bypass_count();
extern "C" std::uint64_t aura_unchecked_fastpath_count();
extern "C" std::uint64_t aura_deopt_count();
extern "C" void aura_counters_reset();


// JSON pretty-printer (no external dependencies, safe for module TU)
static std::string prettify_json(const std::string& compact) {
    std::string out;
    out.reserve(compact.size() * 2);
    int indent = 0;
    bool in_string = false;
    bool escaped = false;

    for (std::size_t i = 0; i < compact.size(); ++i) {
        char c = compact[i];
        if (escaped) {
            escaped = false;
            out += c;
            continue;
        }
        if (c == '\\') {
            out += c;
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            out += c;
            continue;
        }
        if (in_string) {
            out += c;
            continue;
        }

        if (c == '{' || c == '[') {
            out += c;
            out += '\n';
            ++indent;
            out.append(static_cast<std::size_t>(indent * 2), ' ');
        } else if (c == '}' || c == ']') {
            out += '\n';
            --indent;
            if (indent < 0)
                indent = 0;
            out.append(static_cast<std::size_t>(indent * 2), ' ');
            out += c;
        } else if (c == ',') {
            out += c;
            out += '\n';
            out.append(static_cast<std::size_t>(indent * 2), ' ');
        } else if (c == ':') {
            out += ": ";
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            out += c;
        }
    }
    out += '\n';
    return out;
}

// Format helper: format EvalValue with access to string heap
static std::string fmt_val(const aura::compiler::types::EvalValue& v,
                           aura::compiler::CompilerService& cs) {
    return aura::compiler::format_value(
        v, cs.evaluator().primitives().string_heap(), cs.evaluator().pairs(), 0,
        &cs.evaluator().primitives(), cs.evaluator().keyword_table(), cs.evaluator().vector_heap(),
        cs.evaluator().symbol_table());
}

// JSON helper: wrap a string value for JSON (escape quotes and backslashes)
static std::string json_escape(std::string_view s) {
    std::string out;
    for (auto c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

// Issue #147: stringify InvariantStatus enum for JSON output.
static std::string invariant_status_to_string(aura::ast::InvariantStatus st) {
    switch (st) {
        case aura::ast::InvariantStatus::Ok:
            return "Ok";
        case aura::ast::InvariantStatus::Warnings:
            return "Warnings";
        case aura::ast::InvariantStatus::Violations:
            return "Violations";
        case aura::ast::InvariantStatus::NotChecked:
        default:
            return "NotChecked";
    }
}

// Issue #147: serialize the diagnostics vector as a JSON array. Each
// entry is {"node":N,"kind":"...","message":"..."}. Returns "[]" when
// the vector is empty so callers can always concat the result.
static std::string
invariant_diagnostics_to_json(const std::vector<aura::compiler::OwnershipNote>& notes) {
    if (notes.empty())
        return "[]";
    std::string out = "[";
    bool first = true;
    for (auto& n : notes) {
        if (!first)
            out += ",";
        first = false;
        out += std::format(R"({{"node":{},"kind":"{}","message":"{}")", n.node, json_escape(n.kind),
                           json_escape(n.message));
        if (n.source_mutation_id)
            out += std::format(R"(,"mutation_id":{})", *n.source_mutation_id);
        if (n.blame) {
            const char* party = "implicit";
            switch (n.blame->party) {
                case aura::diag::BlameParty::Caller:
                    party = "caller";
                    break;
                case aura::diag::BlameParty::Annotation:
                    party = "annotation";
                    break;
                case aura::diag::BlameParty::System:
                    party = "system";
                    break;
                case aura::diag::BlameParty::Implicit:
                default:
                    break;
            }
            out += std::format(R"(,"blame":{{"party":"{}","annotation_src":"{}","phase":"{}"}})",
                               party, json_escape(n.blame->annotation_src),
                               json_escape(n.blame->phase));
        }
        out += "}";
    }
    out += "]";
    return out;
}

// Issue #147: print a typed-mutate JSON response with invariant
// diagnostics. Used by both --serve and --serve-async handlers so
// the protocol stays in sync.
static void print_typed_mutate_response(const aura::compiler::CompilerService::MutationResult& r) {
    auto ist = invariant_status_to_string(r.invariant_status);
    auto diags = invariant_diagnostics_to_json(r.invariant_diagnostics);
    if (r.success) {
        std::println(
            R"({{"status":"ok","mutation_id":{},"invariant_status":"{}","invariant_diagnostics":{}}})",
            r.mutation_id, ist, diags);
    } else {
        std::println(
            R"({{"status":"error","msg":"{}","invariant_status":"{}","invariant_diagnostics":{}}})",
            json_escape(r.error.empty() ? "mutation failed" : r.error), ist, diags);
    }
}

// Minimal JSON parser for --serve protocol messages.
// Only supports flat objects: {"key":"value","key2":"value2"}
// Returns an empty map on parse failure.
static std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                          std::equal_to<>>
parse_json_command(std::string_view line) {
    std::unordered_map<std::string, std::string, aura::core::TransparentStringHash, std::equal_to<>>
        result;
    if (line.empty())
        return result;

    auto p = line.data();
    auto end = p + line.size();

    // Skip whitespace and opening brace
    auto skip_ws = [&]() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    };

    skip_ws();
    if (p >= end || *p != '{')
        return result;
    ++p;

    while (p < end) {
        skip_ws();
        if (p >= end || *p == '}')
            break;

        // Parse key
        if (*p != '"')
            return result;
        ++p;
        std::string key;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                key += *p;
            } else {
                key += *p;
            }
            ++p;
        }
        if (p >= end)
            return result;
        ++p; // skip closing quote

        skip_ws();
        if (p >= end || *p != ':')
            return result;
        ++p;

        skip_ws();
        // Parse value (string or number/bool/null)
        std::string value;
        if (*p == '"') {
            // Quoted string
            ++p;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) {
                    ++p;
                    if (*p == 'n')
                        value += '\n';
                    else if (*p == 't')
                        value += '\t';
                    else if (*p == 'r')
                        value += '\r';
                    else if (*p == '"')
                        value += '"';
                    else if (*p == '\\')
                        value += '\\';
                    else
                        value += *p;
                } else {
                    value += *p;
                }
                ++p;
            }
            if (p >= end)
                return result;
            ++p; // skip closing quote
        } else if (*p == 't' || *p == 'f' || *p == 'n') {
            // true / false / null
            while (p < end && (*p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\n' &&
                               *p != '\r')) {
                value += *p++;
            }
        } else {
            // Number (int or float)
            while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\n' &&
                   *p != '\r') {
                value += *p++;
            }
        }

        result[std::move(key)] = std::move(value);

        skip_ws();
        if (p >= end)
            break;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '}')
            break;
        // Unexpected character
        return result;
    }

    return result;
}

// ── Lazy soft oneshot std prelude (#4178–#4219 follow-up) ───────────────
// The prelude auto-loads std/list, std/string, std/hash and std/math for
// every oneshot invocation (`aura -e` / file / pipe / --load). Loading all
// four eagerly costs ~28ms even when a program references none of them.
// This scan over-approximates which modules a program can possibly need:
//   * any identifier token equal to one of the module's (export …) names
//     triggers that module's require;
//   * programs using dynamic evaluation or module loading (eval/require/
//     import/use/read/load/string->symbol/macro forms) fall back to the
//     full eager prelude.
// Over-approximation only ever loads a module that the eager prelude would
// have loaded anyway, so observable semantics are unchanged; the relative
// load order (list, string, hash, math) is preserved. Any read/parse
// failure is treated as "may use" so the eager behaviour is the fallback.
static bool tokenize_identifiers(const std::string& s, std::set<std::string>& out) {
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            out.insert(cur);
            cur.clear();
        }
    };
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        const bool ident = std::isalnum(uc) || c == '_' || c == '-' || c == '?' || c == '!' ||
                           c == '*' || c == '<' || c == '>' || c == '=' || c == '+' || c == '%' ||
                           c == '&' || c == '|' || c == '^' || c == '~';
        if (ident)
            cur += c;
        else
            flush();
    }
    flush();
    return !out.empty();
}

static bool extract_module_export_names(const std::string& text, std::set<std::string>& out) {
    const auto p = text.find("(export");
    if (p == std::string::npos)
        return false;
    std::size_t i = p + 7; // past "(export"
    int depth = 1;
    bool in_str = false;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            out.insert(cur);
            cur.clear();
        }
    };
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (in_str) {
            if (c == '\\') {
                ++i;
                continue;
            }
            if (c == '"')
                in_str = false;
            continue;
        }
        if (c == ';') {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (c == '"') {
            in_str = true;
            flush();
            continue;
        }
        if (c == '(' || c == '[') {
            ++depth;
            flush();
            continue;
        }
        if (c == ')' || c == ']') {
            flush();
            if (--depth == 0)
                break;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            flush();
            continue;
        }
        cur += c;
    }
    return !out.empty();
}

static bool oneshot_source_may_use_module(const std::set<std::string>& source_tokens,
                                          const std::string& module_path) {
    std::ifstream mf(module_path);
    if (!mf)
        return true; // cannot inspect → keep eager (old semantics)
    std::string text((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
    std::set<std::string> exports;
    if (!extract_module_export_names(text, exports))
        return true;
    for (const auto& name : exports) {
        if (source_tokens.count(name))
            return true;
    }
    return false;
}

// Build the oneshot std prelude, requiring only the modules the program can
// reference. Returns empty when no module is needed. Dynamic / module-loading
// programs keep the full eager prelude.
static std::string build_lazy_oneshot_std_prelude(const std::string& source) {
    static const std::string std_root = aura::compiler::paths::resolve_stdlib_root();
    static const std::set<std::string> kEagerFallbackTokens = {"eval",
                                                               "eval-current",
                                                               "read",
                                                               "load",
                                                               "require",
                                                               "import",
                                                               "use",
                                                               "string->symbol",
                                                               "symbol->string",
                                                               "macro",
                                                               "defmacro",
                                                               "define-macro",
                                                               "macroexpand",
                                                               "macroexpand-1",
                                                               "syntax-rules",
                                                               "define-syntax",
                                                               "quasiquote",
                                                               "unquote",
                                                               "unquote-splicing"};
    std::set<std::string> toks;
    tokenize_identifiers(source, toks);
    for (const auto& t : kEagerFallbackTokens) {
        if (toks.count(t)) {
            return "(require \"std/list\" all:)(require \"std/string\" all:)"
                   "(require \"std/hash\" all:)(require \"std/math\" all:)";
        }
    }
    std::string prelude;
    for (const char* mod : {"list", "string", "hash", "math"}) {
        if (oneshot_source_may_use_module(toks, std_root + mod + ".aura"))
            prelude += std::string("(require \"std/") + mod + "\" all:)";
    }
    return prelude;
}

// Issue #4497: defined in evaluator_primitives_runtime.cpp.
extern "C" std::uint64_t aura_displayed_error_count() noexcept;
// #5066: cap concurrent --load fiber bodies. 0 keeps them unlimited.
extern "C" void aura_set_cli_fiber_body_limit(int n) noexcept;

// Issue #4622: length of the Scheme-style character literal that starts at
// 'i' in 'src' (src[i] == '#' and src[i+1] == '\\'), or 0 when those bytes
// do not open one. The lexer (src/parser/lexer_impl.cpp, read_character,
// #4129) reads a character literal as ONE atomic token:
//
//   #\x41 / #\X41  hex form -- 'x' plus up to six hex digits
//   #\( #\) #\" #\; #\[ #\] #\- #\0
//                   single-character form -- one non-alphabetic character
//   #\a #\space #\newline
//                   name form -- an alphabetic run of alphanumerics and '-'
//
// The CLI's top-level form splitter used to count the delimiter inside a
// literal as a real paren / quote / comment start: #\( unbalanced the scan
// (the whole file was refused with "no complete S-expression in program
// source"), #\" opened a string that never closed, and #\; started a comment
// that swallowed the rest of the line.
static std::size_t character_literal_length(std::string_view src, std::size_t i) {
    if (i + 1 >= src.size() || src[i] != '#' || src[i + 1] != '\\')
        return 0;
    std::size_t p = i + 2; // past the '#' and the '\'
    if (p >= src.size())
        return 0; // bare #\ at EOF -- malformed, the lexer reports it
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F')
            return 10 + (c - 'A');
        return -1;
    };
    const char c = src[p];
    if ((c == 'x' || c == 'X') && p + 1 < src.size() && hex_val(src[p + 1]) >= 0) {
        ++p; // the 'x'
        int digits = 0;
        while (p < src.size() && digits < 6 && hex_val(src[p]) >= 0) {
            ++p;
            ++digits;
        }
        return p - i;
    }
    if (!std::isalpha(static_cast<unsigned char>(c)))
        return p + 1 - i; // single-character form
    while (p < src.size() && (std::isalnum(static_cast<unsigned char>(src[p])) || src[p] == '-'))
        ++p;
    return p - i;
}

// Issue #4892 (string_ref_walk_4320 bench regression): the #4865 form-by-form face used the
// permissive (Allow) policy for EVERY top-level form, so a top-level `while`
// / named-let loop that the IR used to keep (ForceSoa → ContinueIr) silently
// dropped to the tree-walker (string_ref_walk_4320: 11ms → 40ms on CI).
// Only a top-level VALUE define needs the walker escape hatch (#4865: a
// fiber:spawn / agent value define that is not IR-lowerable); a procedure
// define `(define (f ...) ...)` / `(define f (lambda ...))` and every plain
// expression stay on ForceSoa exactly as before #4865.
static bool is_value_define_form_4892(std::string_view f) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    constexpr std::string_view kDef = "(define";
    if (f.size() <= kDef.size() || f.substr(0, kDef.size()) != kDef || !ws(f[kDef.size()]))
        return false;
    std::size_t i = kDef.size();
    while (i < f.size() && ws(f[i]))
        ++i;
    if (i >= f.size() || f[i] == '(')
        return false; // (define (f args) ...) — procedure define
    while (i < f.size() && !ws(f[i]) && f[i] != '(' && f[i] != ')')
        ++i; // the bound symbol
    while (i < f.size() && ws(f[i]))
        ++i;
    const std::string_view rest = f.substr(i);
    auto starts = [&](std::string_view k) {
        return rest.size() > k.size() && rest.substr(0, k.size()) == k &&
               (ws(rest[k.size()]) || rest[k.size()] == '(');
    };
    if (starts("(lambda") || starts("(λ"))
        return false; // (define f (lambda ...)) — procedure define
    return true;
}

// Issue #4892 (string_ref_walk_4320 bench regression): textual guard for the Begin-wrap below. A
// top-level form that creates a closure or a fiber / agent / scope task is
// left on the unwrapped #4865 path.
static bool form_builds_closure_or_task_4892(std::string_view f) {
    for (std::string_view k : {"lambda", "\xCE\xBB" /* λ */, "fiber:", "orch:", "agent", "spawn",
                               "thread", "future", "delay", "call/cc", "call-with"}) {
        if (f.find(k) != std::string_view::npos)
            return true;
    }
    return false;
}

// Issue #4893: the Begin-wrap below must never wrap a definitional top-level
// form. Wrapping `(define-hygienic-macro ...)` / `(defmacro ...)` in a Begin
// made the macro registration a nested form, so later top-level forms saw
// `cannot call` / `unbound variable`. Decide on the form's head symbol (not a
// "(define " prefix): any def* head, plus the module/import/macro forms.
static bool form_head_is_definitional_4893(std::string_view f) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    if (f.empty() || f[0] != '(')
        return false;
    std::size_t i = 1;
    while (i < f.size() && ws(f[i]))
        ++i;
    std::size_t j = i;
    while (j < f.size() && !ws(f[j]) && f[j] != '(' && f[j] != ')')
        ++j;
    const std::string_view head = f.substr(i, j - i);
    if (head.starts_with("def"))
        return true; // define, define-*, defmacro, defun, defrecord, ...
    for (std::string_view k : {"begin", "let-syntax", "letrec-syntax", "import", "require",
                               "module", "library", "include", "load", "export", "provide"}) {
        if (head == k)
            return true;
    }
    return false;
}

// Issue #4893: name bound by a top-level user macro definition
// (`(defmacro (m x) ...)`, `(define-hygienic-macro[*] (m a b) ...)`, or the
// `(defmacro m ...)` symbol spelling); empty for any other form.
static std::string user_macro_def_name_4893(std::string_view f) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::string_view rest;
    for (std::string_view k : {"(defmacro", "(define-hygienic-macro*", "(define-hygienic-macro"}) {
        if (f.size() > k.size() && f.starts_with(k) && (ws(f[k.size()]) || f[k.size()] == '(')) {
            rest = f.substr(k.size());
            break;
        }
    }
    if (rest.empty())
        return {};
    std::size_t i = 0;
    while (i < rest.size() && (ws(rest[i]) || rest[i] == '('))
        ++i;
    std::size_t j = i;
    while (j < rest.size() && !ws(rest[j]) && rest[j] != '(' && rest[j] != ')')
        ++j;
    return std::string(rest.substr(i, j - i));
}

// Issue #4893: does `f` mention symbol `name` as a whole token? A top-level
// form that calls a user macro must keep the #4865 per-form face (Allow, no
// Begin-wrap): the ForceSoa IR route lowers the call as an ordinary
// application (`cannot call: m`) instead of expanding it.
static bool form_mentions_symbol_4893(std::string_view f, std::string_view name) {
    if (name.empty())
        return false;
    auto delim = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '(' || c == ')' ||
               c == '\'' || c == '`' || c == ',' || c == '@';
    };
    for (std::size_t p = f.find(name); p != std::string_view::npos; p = f.find(name, p + 1)) {
        const bool left_ok = p == 0 || delim(f[p - 1]);
        const std::size_t e = p + name.size();
        const bool right_ok = e >= f.size() || delim(f[e]);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

// Issue #4899 follow-up: form-relative location for a top-level form's error
// VALUE on the force-soa per-form faces. The value's recorded location can
// belong to another form's flat (a procedure defined by an earlier form), so
// the per-form coordinates would point at the wrong text; name the failing
// top-level form's head instead (1:2 for "(define x (car 5))", the same
// column the whole-program face reports for a statement).
// Issue #4916 / #4918: the program file given on the command line is the
// entry file. Its diagnostics read "main.aura:L:C" and "./x" requires in
// it resolve against its directory (not the cwd).
static void register_entry_file_4916(const char* path, std::string content) {
    std::string p(path);
    // #4936: canonical() is realpath(3) without the malloc'd buffer.
    std::error_code ec;
    if (auto real = std::filesystem::canonical(path, ec); !ec)
        p = real.string();
    aura::diag::set_entry_source_file(
        aura::diag::register_source_file(std::move(p), std::move(content)));
}

// Issue #4916: an error inside a required module also names the entry form
// that called into it ("at helper.aura:4:4 <- main.aura:4:2"), so a call chain
// across files is not reduced to the innermost frame. The entry position is
// the head of that top-level form.
static void print_module_backtrace_4916(const aura::diag::SourceLocation& err, std::uint32_t line,
                                        std::uint32_t col, std::string_view form_text) {
    if (!err.valid() || err.file_id == 0 || line == 0)
        return;
    const auto entry = aura::diag::display_source_path(aura::diag::source_file_path(0));
    if (entry.empty())
        return;
    if (!form_text.empty() && form_text[0] == '(')
        ++col;
    std::println(std::cerr, "  at {} \u2190 {}:{}:{}", err.format(), entry, line, col);
}

// Same, for a whole-program run: `at` is the stamped top-level form.
static void print_module_backtrace_at_4916(const aura::diag::SourceLocation& err,
                                           const aura::diag::SourceLocation& at,
                                           std::string_view src) {
    if (!at.valid() || at.file_id != 0)
        return;
    std::size_t off = 0;
    for (std::uint32_t ln = 1; off < src.size() && ln < at.line; ++off)
        if (src[off] == '\n')
            ++ln;
    off += at.column > 0 ? at.column - 1 : 0;
    if (off > src.size())
        return;
    print_module_backtrace_4916(err, at.line, at.column, src.substr(off));
}

static aura::diag::SourceLocation per_form_error_value_location_4899(std::string_view form) {
    return aura::diag::SourceLocation{1u, (!form.empty() && form[0] == '(') ? 2u : 1u, 0};
}

// Issue #4865: split top-level forms so force-soa can evaluate them one at a
// time. The define must run/register its evaluator value cell before a later
// form's inline lambda is lowered; a single whole-file eval lowers every form
// first and the lambda captured a different cell.
struct TopLevelForm4865 {
    std::string text;
    // Issue #4875: 1-based position of text[0] in the whole source, so a
    // form-relative diagnostic can be mapped back when the splitter skipped
    // comments/whitespace before it.
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};
// Issue #4908: the form's text preceded by the newlines/spaces that put it
// at its real file position. Evaluating this instead of the bare text makes
// every location the parser records (this form's, and those of procedures
// it defines that fail in a later form) file-absolute, so force-soa reports
// the same line:col as the whole-program faces.
static std::string positioned_form_4908(std::uint32_t line, std::uint32_t column,
                                        std::string_view prefix, std::string_view text,
                                        std::string_view suffix) {
    std::string out(static_cast<std::size_t>(line > 0 ? line - 1 : 0), '\n');
    out.append(static_cast<std::size_t>(column > 0 ? column - 1 : 0), ' ');
    out += prefix;
    out += text;
    out += suffix;
    return out;
}

// Issue #4912: `preset = "dev"` from ./aura.toml, else from the aura.toml next
// to the entry file. Only that one key is read; anything else is ignored.
static std::string read_toml_preset_4912(const std::string& entry_path) {
    std::vector<std::filesystem::path> candidates{std::filesystem::path("aura.toml")};
    if (!entry_path.empty())
        candidates.push_back(std::filesystem::path(entry_path).parent_path() / "aura.toml");
    for (const auto& c : candidates) {
        std::ifstream in(c);
        if (!in)
            continue;
        std::string line;
        while (std::getline(in, line)) {
            auto t = std::string_view(line);
            while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
                t.remove_prefix(1);
            if (!t.starts_with("preset"))
                continue;
            t.remove_prefix(6);
            while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
                t.remove_prefix(1);
            if (t.empty() || t.front() != '=')
                continue;
            t.remove_prefix(1);
            auto q1 = t.find('"');
            auto q2 = q1 == std::string_view::npos ? q1 : t.find('"', q1 + 1);
            if (q2 != std::string_view::npos)
                return std::string(t.substr(q1 + 1, q2 - q1 - 1));
        }
    }
    return {};
}

// Issue #4913: one flat `key = "value"` from an aura.toml.
static std::string read_toml_key_4913(const std::filesystem::path& toml, std::string_view key) {
    std::ifstream in(toml);
    std::string line;
    while (in && std::getline(in, line)) {
        std::string_view t(line);
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
            t.remove_prefix(1);
        if (!t.starts_with(key))
            continue;
        t.remove_prefix(key.size());
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
            t.remove_prefix(1);
        if (t.empty() || t.front() != '=')
            continue;
        const auto q1 = t.find('"');
        const auto q2 = q1 == std::string_view::npos ? q1 : t.find('"', q1 + 1);
        if (q2 != std::string_view::npos)
            return std::string(t.substr(q1 + 1, q2 - q1 - 1));
    }
    return {};
}

// Issue #4913: the nearest directory at or above `start` holding aura.toml.
static std::optional<std::filesystem::path> find_project_root_4913(std::filesystem::path start) {
    std::error_code ec;
    start = std::filesystem::weakly_canonical(std::filesystem::absolute(start, ec), ec);
    for (auto p = start;; p = p.parent_path()) {
        if (std::filesystem::is_regular_file(p / "aura.toml", ec))
            return p;
        if (p == p.parent_path())
            return std::nullopt;
    }
}

static std::string sh_quote_4913(std::string_view s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += '\'';
    return out;
}

static std::string slurp_4913(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

#include "cli_templates_4913.inc"

#ifndef AURA_BUILD_VERSION
#define AURA_BUILD_VERSION "dev"
#endif

// Issue #4913: the image tag a new project's CI runs and its aura.toml pins:
// a release build (`X.Y.Z`) uses its own tag; nightly and local builds use
// `nightly` (published on every main push, unlike `latest`, which exists only
// once a release does).
static std::string project_channel_4913() {
    const std::string_view v(AURA_BUILD_VERSION);
    int dots = 0;
    bool ok = !v.empty();
    for (const char c : v) {
        if (c == '.')
            ++dots;
        else if (c < '0' || c > '9')
            ok = false;
    }
    return ok && dots == 2 && v.front() != '.' && v.back() != '.' ? std::string(v) : "nightly";
}
#include "cli_evo_4910.inc"
#include "cli_lsp_4911.inc"

// Write one template's files under dir, substituting {{name}} / {{channel}}.
// Returns the file count, or -1 after printing the write error.
static int write_template_4913(std::string_view tpl, const std::filesystem::path& dir,
                               std::string_view name) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto channel = project_channel_4913();
    int n = 0;
    for (const auto& f : kTemplates4913) {
        if (f.tpl != tpl)
            continue;
        std::string body(f.body);
        for (std::size_t p = 0; (p = body.find("{{name}}", p)) != std::string::npos;)
            body.replace(p, 8, name);
        for (std::size_t p = 0; (p = body.find("{{channel}}", p)) != std::string::npos;)
            body.replace(p, 11, channel);
        const fs::path dst = dir / fs::path(f.path);
        fs::create_directories(dst.parent_path(), ec);
        std::ofstream out(dst, std::ios::binary);
        out << body;
        if (!out) {
            std::println(std::cerr, "error: cannot write {}", dst.string());
            return -1;
        }
        ++n;
    }
    return n;
}

// Temporary project for `aura demo`; removed on exit.
static std::string g_demo_dir_4913;
static void cleanup_demo_4913() {
    if (!g_demo_dir_4913.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(g_demo_dir_4913, ec);
    }
}

// Materialise a template outside the user's tree so `aura demo` runs the
// same source as `aura new <tpl> && aura run`.
static std::optional<std::filesystem::path> make_demo_4913(std::string_view tpl) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto dir =
        fs::temp_directory_path(ec) / std::format("aura-demo-{}", static_cast<long>(::getpid()));
    if (ec)
        return std::nullopt;
    fs::remove_all(dir, ec);
    if (write_template_4913(tpl, dir, "demo") < 0)
        return std::nullopt;
    g_demo_dir_4913 = dir.string();
    std::atexit(cleanup_demo_4913);
    return dir;
}

// `aura new [app|evolve|agent] <dir>`
static int aura_new_4913(int argc, char** argv) {
    auto known = [](std::string_view t) {
        for (const auto& f : kTemplates4913)
            if (f.tpl == t)
                return true;
        return false;
    };
    std::string tpl = "app";
    std::string dir;
    if (argc == 3 && !known(argv[2])) {
        dir = argv[2];
    } else if (argc == 4) {
        tpl = argv[2];
        dir = argv[3];
    } else {
        std::println(std::cerr, "usage: aura new [app|evolve|evolve-moo|agent] <dir>");
        return 1;
    }
    if (dir.starts_with("-") || tpl.starts_with("-")) {
        std::println(std::cerr, "error: unknown option '{}'", dir.starts_with("-") ? dir : tpl);
        std::println(std::cerr, "usage: aura new [app|evolve|evolve-moo|agent] <dir>");
        return 1;
    }
    if (!known(tpl)) {
        std::println(std::cerr, "error: unknown template '{}'", tpl);
        std::println(std::cerr, "hint: app (modules + tests), evolve (prints KEEP / ROLLBACK "
                                "in the terminal), evolve-moo (multi-objective + QD, no LLM), "
                                "agent (two orch roles in a dialogue)");
        return 1;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(dir, ec) && !fs::is_empty(dir, ec)) {
        std::println(std::cerr, "error: '{}' already exists and is not empty", dir);
        return 1;
    }
    auto name = fs::weakly_canonical(fs::absolute(dir, ec), ec).filename().string();
    if (name.empty())
        name = "aura-project";
    const int n = write_template_4913(tpl, fs::path(dir), name);
    if (n < 0)
        return 1;
    std::println("created {} ({} template, {} files)", dir, tpl, n);
    std::println("next: cd {} && {} run", dir, "aura");
    return 0;
}

static bool caps_flag_with_value_4913(std::string_view a) {
    return a == "--preset" || a == "--grant" || a == "--deny" || a == "--allow";
}

// `aura test [--update] [options] [tests/x.aura …]`: run each test with
// cwd = project root and compare its stdout with the .golden beside it.
static int aura_test_4913(const std::filesystem::path& root, const std::vector<std::string>& opts,
                          const std::vector<std::string>& picks, bool update) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> tests;
    if (!picks.empty()) {
        for (const auto& p : picks)
            tests.push_back(fs::relative(fs::absolute(p, ec), root, ec));
    } else if (fs::is_directory(root / "tests", ec)) {
        for (const auto& e : fs::recursive_directory_iterator(root / "tests", ec))
            if (e.is_regular_file() && e.path().extension() == ".aura")
                tests.push_back(fs::relative(e.path(), root, ec));
        std::ranges::sort(tests);
    }
    if (tests.empty()) {
        std::println("no tests (tests/*.aura)");
        return 0;
    }
    // Issue #4907: in the prebuilt bundle /proc/self/exe is the bundled
    // loader (rt/ld.so); its bin/aura launcher exports itself as AURA_SELF.
    std::string self;
    if (const char* s = std::getenv("AURA_SELF"); s && *s && ::access(s, X_OK) == 0)
        self = s;
    else if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec)
        self = p.string();
    else if (const char* b = std::getenv("AURA_BIN"))
        self = b;
    const auto tmp =
        fs::temp_directory_path(ec) / std::format("aura-test-{}", static_cast<long>(::getpid()));
    fs::create_directories(tmp, ec);
    int failed = 0;
    for (std::size_t i = 0; i < tests.size(); ++i) {
        const auto& t = tests[i];
        const auto out_p = tmp / std::format("{}.out", i);
        const auto err_p = tmp / std::format("{}.err", i);
        std::string cmd = sh_quote_4913(self) + " --quiet";
        for (const auto& o : opts)
            cmd += " " + sh_quote_4913(o);
        cmd += " " + sh_quote_4913(t.string()) + " </dev/null >" + sh_quote_4913(out_p.string()) +
               " 2>" + sh_quote_4913(err_p.string());
        const auto t0 = std::chrono::steady_clock::now();
        const int st = std::system(cmd.c_str());
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        const int rc = st == -1 ? -1 : WEXITSTATUS(st);
        const std::string got = slurp_4913(out_p);
        auto golden = t;
        golden.replace_extension(".golden");
        std::string why;
        if (update && rc == 0) {
            std::ofstream(golden, std::ios::binary) << got;
        } else if (fs::exists(golden, ec)) {
            const std::string want = slurp_4913(golden);
            if (got != want) {
                std::size_t line = 1, k = 0;
                while (k < got.size() && k < want.size() && got[k] == want[k])
                    line += got[k++] == '\n';
                auto line_at = [](const std::string& s, std::size_t k) {
                    auto b = s.rfind('\n', k == 0 ? 0 : k - 1);
                    b = (b == std::string::npos || k == 0) ? 0 : b + 1;
                    auto e = s.find('\n', b);
                    return s.substr(b, e == std::string::npos ? std::string::npos : e - b);
                };
                why = std::format("stdout differs from {} at line {}:\n    want: {}\n    got:  {}",
                                  golden.string(), line, line_at(want, k), line_at(got, k));
            }
        }
        if (why.empty() && rc != 0)
            why = std::format("exit {}", rc);
        if (why.empty()) {
            std::println("ok   {} ({} ms)", t.string(), ms);
            continue;
        }
        ++failed;
        std::println("FAIL {} — {}", t.string(), why);
        std::istringstream err(slurp_4913(err_p));
        std::string l;
        for (int shown = 0; shown < 8 && std::getline(err, l); ++shown)
            std::println("    {}", l);
    }
    fs::remove_all(tmp, ec);
    std::println("{} passed, {} failed", tests.size() - static_cast<std::size_t>(failed), failed);
    return failed == 0 ? 0 : 1;
}

#ifndef AURA_BUILD_VERSION
#define AURA_BUILD_VERSION "dev"
#endif
#ifndef AURA_BUILD_GIT_SHA
#define AURA_BUILD_GIT_SHA "unknown"
#endif


// Issue #4935: CLI help. Usage text names the program `aura` (never the
// libexec path of the bundled binary); `--help` prints a short overview on
// stdout, `aura help <cmd>` / `aura <cmd> --help` the command's own usage,
// `aura help --all` the full reference.
struct SubHelp4935 {
    std::string_view name;
    std::string_view summary;
    std::string_view usage;
};
inline constexpr SubHelp4935 kSubHelp4935[] = {
    {"new", "create a project (aura.toml, tests, CI, README)",
     "usage: aura new [app|evolve|evolve-moo|agent] <dir>\n"
     "\n"
     "Scaffold a project in <dir> (must not exist or be empty).\n"
     "  app         modules + tests (default)\n"
     "  evolve      live AST: each KEEP / ROLLBACK printed in the terminal\n"
     "  evolve-moo  multi-objective + QD search (mock proposer, no LLM)\n"
     "  agent       orchestrated workflow\n"},
    {"demo", "watch a definition evolve in the terminal",
     "usage: aura demo [evolve|agent]\n"
     "\n"
     "Run the signature example from a temporary project (nothing to create):\n"
     "  (no arg)  four fixed candidates: KEEP, ROLLBACK, and the live source\n"
     "  agent     two roles propose and check a definition\n"},
    {"run", "run the project entry with its aura.toml preset",
     "usage: aura run [entry.aura] [options] [args…]\n"
     "\n"
     "Run the entry of the enclosing project (aura.toml `entry`, default main.aura).\n"
     "  --preset dev|ci|prod, --grant CAPS, --deny CAPS, --quiet, --keep-going\n"},
    {"test", "run tests/*.aura against tests/*.golden",
     "usage: aura test [--update] [tests/x.aura …]\n"
     "\n"
     "  --update  rewrite the .golden files from the current output\n"},
    {"repl", "interactive REPL with hot reload",
     "usage: aura repl [--preset P] [file.aura]\n"
     "\n"
     "Commands inside: :load FILE, :reload FILE, :ws, :worldline, :snapshot,\n"
     ":restore ID, :help\n"},
    {"evo", "inspect a project's evolution worldline",
     "usage: aura evo log [--json] [--all] [--run ID|last] [--front] "
     "[--lineage ID]\n"
     "       aura evo report --html DIR\n"
     "\n"
     "  log --front       only the non-dominated candidates (first two objectives)\n"
     "  log --lineage ID  ancestor chain of one record, root -> ID\n"
     "                    ID is a record id (<run>#<seq>), the #<seq> suffix,\n"
     "                    or a candidate name (v2 \"candidate\" or the target column).\n"
     "                    An unknown id is an error when the log has records.\n"
     "  report            score curve, Pareto scatter, HV curve, QD heat map,\n"
     "                    lineage tree, candidate folds, surrogate pre-screen\n"},
    {"lsp", "language server on stdio (see docs/editor.md)", "usage: aura lsp\n"},
    {"soft", "run a file with Restricted Mutate on its workspace",
     "usage: aura soft [--allow ffi,write] [--load] file.aura\n"
     "\n"
     "  --allow CAPS  explicit opt-in; never implied\n"},
    {"help", "show help for a command", "usage: aura help [command|--all]\n"},
};

static const SubHelp4935* find_sub_4935(std::string_view n) {
    for (const auto& h : kSubHelp4935)
        if (h.name == n)
            return &h;
    return nullptr;
}

static void print_top_help_4935() {
    std::println("aura {} — an AI-native Lisp", AURA_BUILD_VERSION);
    std::println("");
    std::println("usage: aura <command> [args]     project commands");
    std::println("       aura demo                  watch a definition evolve in the terminal");
    std::println("       aura [options] file.aura   run one file (or stdin, or -e EXPR)");
    std::println("");
    std::println("Commands:");
    for (const auto& h : kSubHelp4935)
        std::println("  {:<6} {}", h.name, h.summary);
    std::println("");
    std::println("Common options:");
    std::println("  -e, --eval EXPR    evaluate EXPR");
    std::println("  --preset dev|ci|prod, --grant CAPS, --deny CAPS   capabilities");
    std::println("  --fmt, --typecheck, --query Q, --inspect           tools");
    std::println("  -V, --version      print version");
    std::println("");
    std::println("'aura help <command>' for details; 'aura help --all' for every option.");
}

static std::size_t edit_distance_4935(std::string_view a, std::string_view b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j)
        row[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t prev = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t cur = row[j];
            row[j] =
                std::min({row[j] + 1, row[j - 1] + 1, prev + (a[i - 1] != b[j - 1] ? 1U : 0U)});
            prev = cur;
        }
    }
    return row[b.size()];
}

// Issue #4913: project front door. Returns an exit code for the commands it
// finishes itself (new, test); otherwise rewrites argv for the normal path
// (run → `aura [opts] <entry>`, soft/caps + --load → the file face, which the
// #4908 gate holds to the same semantics as --load).
static std::vector<std::string> g_front_args_4913;
static std::vector<char*> g_front_argv_4913;
// Issue #4911: `aura repl [FILE]` — REPL even on a pipe, FILE pre-:load'ed.
static bool g_repl_4911 = false;
static std::string g_repl_file_4911;
static std::optional<int> front_door_4913(int& argc, char**& argv) {
    if (argc < 2)
        return std::nullopt;
    auto rewrite = [&](std::vector<std::string> args) {
        g_front_args_4913 = std::move(args);
        g_front_argv_4913.clear();
        for (auto& s : g_front_args_4913)
            g_front_argv_4913.push_back(s.data());
        g_front_argv_4913.push_back(nullptr);
        argc = static_cast<int>(g_front_args_4913.size());
        argv = g_front_argv_4913.data();
    };
    const std::string_view sub(argv[1]);
    // Issue #4934: `aura --version` / `-V` → "aura <version> (<git sha>)".
    // AURA_BUILD_VERSION is `nightly`, `X.Y.Z` (release tag) or `dev`.
    if (sub == "--version" || sub == "-V") {
        std::println("aura {} ({})", AURA_BUILD_VERSION, AURA_BUILD_GIT_SHA);
        return 0;
    }
    // Issue #4935: help never falls through to a command's side effects.
    if (sub == "--help" || sub == "-h" || (sub == "help" && argc == 2)) {
        print_top_help_4935();
        return 0;
    }
    if (sub == "help") {
        const std::string_view what(argv[2]);
        if (what == "--all" || what == "-a") {
            rewrite({argv[0], "--help-all"});
            return std::nullopt;
        }
        if (what == "--help" || what == "-h") {
            std::print("{}", find_sub_4935("help")->usage);
            return 0;
        }
        if (const auto* h = find_sub_4935(what)) {
            std::print("{}", h->usage);
            return 0;
        }
        std::println(std::cerr, "error: unknown command '{}'", what);
        std::println(std::cerr, "hint: 'aura --help' lists the commands");
        return 1;
    }
    if (const auto* h = find_sub_4935(sub)) {
        for (int i = 2; i < argc; ++i) {
            const std::string_view a(argv[i]);
            if (a == "--")
                break;
            if (a == "--help" || a == "-h") {
                std::print("{}", h->usage);
                return 0;
            }
        }
    } else if (!sub.empty() && sub[0] != '-' && sub.find('/') == std::string_view::npos &&
               sub.find('.') == std::string_view::npos) {
        // A bare word that is no command and no file: say so instead of
        // "cannot open program file".
        std::error_code ec;
        if (!std::filesystem::exists(std::string(sub), ec)) {
            std::println(std::cerr, "error: unknown command '{}'", sub);
            const SubHelp4935* best = nullptr;
            std::size_t bd = 3;
            for (const auto& h : kSubHelp4935)
                if (const auto d = edit_distance_4935(sub, h.name); d < bd)
                    bd = d, best = &h;
            if (best)
                std::println(std::cerr, "hint: did you mean 'aura {}'?", best->name);
            std::println(std::cerr,
                         "hint: 'aura --help' lists the commands; 'aura {}.aura' runs a file", sub);
            return 1;
        }
    }
    if (sub == "new")
        return aura_new_4913(argc, argv);
    if (sub == "demo") {
        if (argc > 3) {
            std::println(std::cerr, "usage: aura demo [evolve|agent]");
            return 1;
        }
        const std::string_view tpl = argc == 3 ? argv[2] : "evolve";
        if (tpl != "evolve" && tpl != "agent") {
            std::println(std::cerr, "error: unknown demo '{}'", tpl);
            std::println(std::cerr,
                         "hint: evolve (KEEP / ROLLBACK worldline) or agent (two roles)");
            return 1;
        }
        const auto dir = make_demo_4913(tpl);
        if (!dir)
            return 1;
        std::error_code ec;
        std::filesystem::current_path(*dir, ec);
        if (ec) {
            std::println(std::cerr, "error: cannot enter {}: {}", dir->string(), ec.message());
            return 1;
        }
        // Same file face as `aura main.aura`, with the preset the template's
        // aura.toml names, so the record matches `aura new … && aura run`.
        rewrite({argv[0], "--preset", "dev", "main.aura"});
        return std::nullopt;
    }
    if (sub == "evo")
        return aura_evo_4910(argc, argv);
    if (sub == "lsp")
        return aura_lsp_4911();
    if (sub == "repl") {
        // `aura repl [--preset P|--grant C|--deny C|…] [FILE]`: options pass
        // through to the normal parser; FILE is :load'ed by the REPL itself.
        g_repl_4911 = true;
        std::vector<std::string> args{argv[0]};
        for (int i = 2; i < argc; ++i) {
            const std::string_view a(argv[i]);
            if (!a.starts_with("-")) {
                g_repl_file_4911 = argv[i];
                continue;
            }
            args.emplace_back(a);
            if ((a == "--preset" || a == "--grant" || a == "--deny") && i + 1 < argc)
                args.emplace_back(argv[++i]);
        }
        rewrite(std::move(args));
        return std::nullopt;
    }
    namespace fs = std::filesystem;
    if (sub == "run" || sub == "test") {
        std::vector<std::string> opts, pos;
        bool quiet = false, update = false;
        for (int i = 2; i < argc; ++i) {
            const std::string_view a(argv[i]);
            if (a == "--update" && sub == "test") {
                update = true;
            } else if (!a.empty() && a[0] == '-') {
                quiet = quiet || a == "--quiet";
                opts.emplace_back(a);
                if (caps_flag_with_value_4913(a) && i + 1 < argc)
                    opts.emplace_back(argv[++i]);
            } else {
                pos.emplace_back(a);
            }
        }
        std::error_code ec;
        const auto cwd = fs::current_path(ec);
        const fs::path start =
            (sub == "run" && !pos.empty()) ? fs::absolute(pos[0], ec).parent_path() : cwd;
        const auto root = find_project_root_4913(start);
        if (!root) {
            std::println(std::cerr, "error: no aura.toml in {} or any parent directory",
                         start.string());
            std::println(std::cerr,
                         "hint: '{} new app <dir>' creates a project; '{} file.aura' "
                         "runs a single file",
                         "aura", "aura");
            return 1;
        }
        std::vector<std::string> abs_pos;
        for (const auto& p : pos)
            abs_pos.push_back(fs::absolute(p, ec).string());
        fs::current_path(*root, ec);
        if (ec) {
            std::println(std::cerr, "error: cannot enter {}: {}", root->string(), ec.message());
            return 1;
        }
        // Module paths are project-root relative: (require "lib/x").
        std::string ap = root->string();
        if (const char* old = std::getenv("AURA_PATH"); old && *old)
            ap += std::string(":") + old;
        ::setenv("AURA_PATH", ap.c_str(), 1);
        const auto toml = *root / "aura.toml";
        if (sub == "test")
            return aura_test_4913(*root, opts, abs_pos, update);
        // Issue #4910: a project run keeps its worldline in .aura/evo/log.jsonl
        // (aura evo log / report read it). AURA_EVO_LOG= (empty) turns it off.
        if (!std::getenv("AURA_EVO_LOG"))
            ::setenv("AURA_EVO_LOG", (*root / ".aura" / "evo" / "log.jsonl").c_str(), 1);
        std::string entry = abs_pos.empty() ? read_toml_key_4913(toml, "entry") : abs_pos[0];
        if (entry.empty())
            entry = "main.aura";
        if (!abs_pos.empty())
            entry = fs::relative(entry, *root, ec).string();
        if (!fs::is_regular_file(*root / entry, ec)) {
            std::println(std::cerr, "error: entry '{}' not found in {}", entry, root->string());
            std::println(std::cerr, "hint: set entry = \"main.aura\" in aura.toml");
            return 1;
        }
        if (!quiet) {
            const auto out = read_toml_key_4913(toml, "out");
            std::println(std::cerr, "aura: project={} entry={}{}", root->string(), entry,
                         out.empty() ? std::string() : " out=" + (*root / out).string());
        }
        std::vector<std::string> args{argv[0]};
        args.insert(args.end(), opts.begin(), opts.end());
        args.push_back(entry);
        args.insert(args.end(), pos.begin() + (pos.empty() ? 0 : 1), pos.end());
        rewrite(std::move(args));
        return std::nullopt;
    }
    // soft / --preset / --grant … followed by --load f → the file face.
    std::vector<std::string> args{argv[0]};
    int i = 1;
    for (; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if ((a == "soft" && i == 1) || a == "--quiet" || a == "--keep-going" ||
            a.starts_with("--preset=") || a.starts_with("--grant=") || a.starts_with("--deny=") ||
            a.starts_with("--allow=")) {
            args.emplace_back(a);
            continue;
        }
        if (caps_flag_with_value_4913(a) && i + 1 < argc) {
            args.emplace_back(a);
            args.emplace_back(argv[++i]);
            continue;
        }
        break;
    }
    if (i > 1 && i < argc && std::string_view(argv[i]) == "--load") {
        for (++i; i < argc; ++i)
            args.emplace_back(argv[i]);
        rewrite(std::move(args));
        return std::nullopt;
    }
    // Issue #4908 / #4912: a plain `--load f` must honour the same preset as
    // `aura f` (AURA_PRESET, or preset = "..." in ./aura.toml or next to f).
    // The snapshot face never resolves presets, so the Restricted default
    // denied what the file face granted. Route it through the file face,
    // exactly as an explicit `--preset dev --load f` already is. Under
    // AURA_SANDBOX=off a preset grants nothing, so keep the snapshot face.
    if (i == 1 && argc > 2 && std::string_view(argv[1]) == "--load") {
        const char* sb = std::getenv("AURA_SANDBOX");
        const char* ev = std::getenv("AURA_PRESET");
        const bool off = sb && std::string_view(sb) == "off";
        if (!off && ((ev && *ev) || !read_toml_preset_4912(argv[2]).empty())) {
            for (int j = 2; j < argc; ++j)
                args.emplace_back(argv[j]);
            rewrite(std::move(args));
        }
    }
    return std::nullopt;
}

// Issue #4908: a producer location the evaluator really recorded. 1:1 of the
// root source is the "no location" stamp (an `error` call without a
// recorded producer), not the first column of the file.
template <class OptLoc> static bool recorded_loc_4908(const OptLoc& el) {
    if (!el || !el->valid())
        return false;
    return el->file_id != 0 || el->line != 1 || el->column != 1;
}

static std::vector<TopLevelForm4865> split_top_level_forms_4865(std::string_view src) {
    // Issue #4869: reader-aware top-level form split. The caller has already
    // parsed the whole source successfully, so this only has to find form
    // boundaries under the reader's lexical rules: strings with escapes,
    // #\ character literals (including #\ ( and #\"), |...| symbols,
    // ; comments, quote prefixes, and #( vectors. The old hand counter
    // treated #\ ( as a list open and '()' in a block comment as code.
    std::vector<TopLevelForm4865> out;
    const std::size_t n = src.size();
    std::size_t form_start = std::string_view::npos;
    std::uint32_t line = 1;
    std::size_t line_start = 0;
    std::uint32_t form_line = 1;
    std::uint32_t form_col = 1;
    int depth = 0;
    auto start_form = [&](std::size_t at) {
        if (form_start == std::string_view::npos) {
            form_start = at;
            form_line = line;
            form_col = static_cast<std::uint32_t>(at - line_start) + 1;
        }
    };
    auto flush_atom = [&](std::size_t end) {
        if (depth == 0 && form_start != std::string_view::npos) {
            out.push_back(
                {std::string(src.substr(form_start, end - form_start)), form_line, form_col});
            form_start = std::string_view::npos;
        }
    };
    for (std::size_t i = 0; i < n;) {
        char c = src[i];
        if (c == '\n') {
            ++i;
            ++line;
            line_start = i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == ';') {
            while (i < n && src[i] != '\n')
                ++i;
            continue;
        }
        if (c == '#' && i + 1 < n && src[i + 1] == '|') {
            // Issue #4875: R7RS block comment. A complete one is whitespace
            // between forms; an unterminated one stays in the current chunk
            // so the parser reports it and no later-looking form runs.
            const std::size_t comment_start = i;
            i += 2;
            int nest = 1; // R7RS: block comments nest
            bool closed = false;
            while (i < n) {
                if (src[i] == '#' && i + 1 < n && src[i + 1] == '|') {
                    i += 2;
                    ++nest;
                    continue;
                }
                if (src[i] == '|' && i + 1 < n && src[i + 1] == '#') {
                    i += 2;
                    if (--nest == 0) {
                        closed = true;
                        break;
                    }
                    continue;
                }
                if (src[i] == '\n') {
                    ++line;
                    line_start = i + 1;
                }
                ++i;
            }
            if (closed)
                continue;
            // Unterminated: keep the remainder as the current chunk.
            start_form(comment_start);
            continue;
        }
        if (c == '"') {
            start_form(i);
            ++i;
            while (i < n) {
                if (src[i] == '\\') {
                    i += (i + 1 < n) ? 2 : 1;
                    continue;
                }
                if (src[i] == '"') {
                    ++i;
                    break;
                }
                if (src[i] == '\n') {
                    ++line;
                    line_start = i + 1;
                }
                ++i;
            }
            flush_atom(i);
            continue;
        }
        if (c == '#' && i + 1 < n && src[i + 1] == '\\') {
            start_form(i);
            i += 2;
            if (i < n) {
                if (std::isalpha(static_cast<unsigned char>(src[i]))) {
                    while (i < n &&
                           (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '-'))
                        ++i;
                } else {
                    ++i; // the literal character itself, even ( ) " ; space
                }
            }
            flush_atom(i);
            continue;
        }
        if (c == '|') {
            start_form(i);
            ++i;
            while (i < n) {
                if (src[i] == '\\') {
                    i += (i + 1 < n) ? 2 : 1;
                    continue;
                }
                if (src[i] == '|') {
                    ++i;
                    break;
                }
                if (src[i] == '\n') {
                    ++line;
                    line_start = i + 1;
                }
                ++i;
            }
            flush_atom(i);
            continue;
        }
        if (c == '\'' || c == '`' || c == ',') {
            start_form(i);
            ++i;
            if (c == ',' && i < n && src[i] == '@')
                ++i;
            continue;
        }
        if (c == '(' || c == '[') {
            start_form(i);
            ++depth;
            ++i;
            continue;
        }
        if (c == '#' && i + 1 < n && src[i + 1] == '(') {
            start_form(i);
            ++depth;
            i += 2;
            continue;
        }
        if (c == ')' || c == ']') {
            if (depth > 0)
                --depth;
            ++i;
            if (depth == 0)
                flush_atom(i);
            continue;
        }
        // Atom (identifier, number, #t/#f, dotted tail, ...).
        start_form(i);
        ++i;
        while (i < n) {
            char d = src[i];
            if (d == ' ' || d == '\t' || d == '\r' || d == '\n' || d == '(' || d == ')' ||
                d == '[' || d == ']' || d == ';' || d == '"' || d == '\'' || d == '`' || d == ',' ||
                d == '|')
                break;
            if (d == '#' && i + 1 < n && (src[i + 1] == '\\' || src[i + 1] == '('))
                break;
            ++i;
        }
        flush_atom(i);
    }
    if (form_start != std::string_view::npos)
        out.push_back({std::string(src.substr(form_start)), form_line, form_col});
    return out;
}

// Sync --serve protocol, extracted from aura_main. The dispatcher only
// selects the mode. Session ownership, the workspace tree, and every JSON
// reply stay in these helpers.
struct SyncServeState {
    bool soft_shared_graph = false;
    void* shared_workspace_tree = nullptr;
    std::unordered_map<std::string, aura::compiler::CompilerService,
                       aura::core::TransparentStringHash, std::equal_to<>>
        sessions;
    std::unordered_set<std::string> session_names;
    std::string active_session = "default";
    aura::compiler::CompilerService& resolve() {
        if (soft_shared_graph)
            return sessions.at("default");
        return sessions[active_session];
    }
};

using ServeCmd = std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                                    std::equal_to<>>;

static void serve_json_session(SyncServeState& st, ServeCmd& cmd) {
    auto act_it = cmd.find("action");
    auto action = (act_it != cmd.end()) ? act_it->second : std::string{};

    if (action == "list") {
        std::println("{{\"status\":\"ok\",\"sessions\":[");
        bool first = true;
        for (const auto& sn : st.session_names) {
            if (!first)
                std::println(",");
            first = false;
            std::print("  \"{}\"", json_escape(sn));
        }
        std::println("],\"active\":\"{}\"}}", json_escape(st.active_session));
        return;
    }

    if (action == "delete") {
        auto name_it = cmd.find("name");
        if (name_it == cmd.end() || name_it->second == "default") {
            std::println("{{\"status\":\"error\",\"msg\":\"cannot delete default "
                         "session\"}}");
            return;
        }
        auto& sname = name_it->second;
        // Issue #955: drop process-global registry entry BEFORE
        // destroying the CompilerService (UAF otherwise).
        if (st.session_names.count(sname)) {
            aura::compiler::CompilerService::unregister_session(sname);
            // Issue #955 observability (via active session metrics if present)
            st.resolve().metrics().session_registry_unregisters_total.fetch_add(
                1, std::memory_order_relaxed);
            st.session_names.erase(sname);
            if (!st.soft_shared_graph)
                st.sessions.erase(sname);
            if (st.active_session == sname) {
                st.active_session = "default";
            }
            std::println("{{\"status\":\"deleted\",\"session\":\"{}\"}}", json_escape(sname));
        } else {
            std::println("{{\"status\":\"error\",\"msg\":\"session not found\"}}");
        }
        return;
    }

    // Default: activate / create session
    auto name_it = cmd.find("name");
    if (name_it == cmd.end()) {
        std::println("{{\"status\":\"error\",\"msg\":\"missing name field\"}}");
        return;
    }
    auto& sname = name_it->second;
    auto real_name = (sname.find("new:") == 0) ? sname.substr(4) : sname;
    bool created = st.session_names.insert(real_name).second;
    if (created) {
        if (st.soft_shared_graph) {
            // Soft (#4047 B): alias named session → shared default CS
            aura::compiler::CompilerService::register_session(real_name,
                                                              &st.sessions.at("default"));
        } else {
            auto [it, _] = st.sessions.try_emplace(real_name);
            it->second.set_session_id(real_name);
            it->second.set_workspace_tree(st.shared_workspace_tree);
            aura::compiler::CompilerService::register_session(real_name, &it->second);
        }
    }
    if (created)
        std::println("{{\"status\":\"created\",\"session\":\"{}\"}}", json_escape(real_name));
    else
        std::println("{{\"status\":\"ok\",\"session\":\"{}\"}}", json_escape(real_name));
    st.active_session = real_name;
    return;
}

static void serve_json_module(aura::compiler::CompilerService& cs, ServeCmd& cmd) {
    auto action_it = cmd.find("action");
    auto name_it = cmd.find("name");
    if (action_it == cmd.end()) {
        std::println("{{\"status\":\"error\",\"msg\":\"missing action field\"}}");
        return;
    }
    auto& action = action_it->second;

    if (action == "compile") {
        auto code_it = cmd.find("code");
        if (name_it == cmd.end() || code_it == cmd.end()) {
            std::println("{{\"status\":\"error\",\"msg\":\"missing name or code field\"}}");
            return;
        }
        auto result = cs.compile_module(name_it->second, code_it->second);
        if (result) {
            std::println("{{\"status\":\"ok\",\"module\":\"{}\"}}", json_escape(name_it->second));
        } else {
            std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                         json_escape(result.error().message));
        }
    } else if (action == "unload") {
        if (name_it == cmd.end()) {
            std::println("{{\"status\":\"error\",\"msg\":\"missing name field\"}}");
            return;
        }
        cs.unload_module(name_it->second);
        std::println("{{\"status\":\"ok\",\"unloaded\":\"{}\"}}", json_escape(name_it->second));
    } else if (action == "reload") {
        if (name_it == cmd.end()) {
            std::println("{{\"status\":\"error\",\"msg\":\"missing name field\"}}");
            return;
        }
        auto result = cs.reload_module(name_it->second);
        if (result) {
            std::println("{{\"status\":\"ok\",\"reloaded\":\"{}\"}}", json_escape(name_it->second));
        } else {
            std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                         json_escape(result.error().message));
        }
    } else if (action == "list") {
        auto modules = cs.loaded_modules();
        std::println("{{\"status\":\"ok\",\"modules\":[");
        bool first = true;
        for (auto& m : modules) {
            if (!first)
                std::println(",");
            first = false;
            std::print("  \"{}\"", json_escape(m));
        }
        std::println("]}}");
    } else if (action == "stats") {
        auto stats = cs.module_memory_stats();
        std::println("{{\"status\":\"ok\",\"arenas\":[");
        bool first = true;
        for (auto& [name, s] : stats) {
            if (!first)
                std::println(",");
            first = false;
            std::print("  {{\"name\":\"{}\",\"used\":{},\"capacity\":{}}}", json_escape(name),
                       s.used, s.capacity);
        }
        std::println("]}}");
    } else {
        std::println("{{\"status\":\"error\",\"msg\":\"unknown action: {}\"}}",
                     json_escape(action));
    }
    return;
}

// Commands that do not take a code field. False means the verb still needs
// the code-bearing dispatcher.
static bool serve_json_immediate(aura::compiler::CompilerService& cs, ServeCmd& cmd,
                                 const std::string& type) {
    if (type == "mutate") {
        // {"cmd": "mutate", "op": "record-patch", "node": 0, "op-name": "replace-type",
        // "summary": "change type"}
        auto op_it = cmd.find("op");      // mutation op name
        auto node_it = cmd.find("node");  // target node ID
        auto val_it = cmd.find("value");  // for replace-value
        auto on_it = cmd.find("op-name"); // for record-patch: recorded op name
        auto sum_it = cmd.find("summary");
        if (op_it == cmd.end() || node_it == cmd.end()) {
            std::println("{{\"status\":\"error\",\"msg\":\"missing op or node\"}}");
        } else {
            auto op_name = op_it->second;
            auto node = std::stoll(node_it->second);
            std::string sexpr;
            if (op_name == "mutate:record-patch") {
                // (mutate:record-patch node op-name summary)
                std::string on = on_it != cmd.end() ? on_it->second : "patch";
                std::string s = sum_it != cmd.end() ? sum_it->second : "";
                sexpr = std::format("(mutate:record-patch {} \"{}\" \"{}\")", node, on, s);
            } else if (op_name == "mutate:replace-value") {
                // (mutate:replace-value node new-value summary)
                std::string v = val_it != cmd.end() ? val_it->second : "0";
                std::string s = sum_it != cmd.end() ? sum_it->second : "";
                sexpr = std::format("(mutate:replace-value {} {} \"{}\")", node, v, s);
            } else if (op_name == "mutate:replace-type") {
                // (mutate:replace-type node type-string)
                std::string t = val_it != cmd.end() ? val_it->second : "Dyn";
                sexpr = std::format("(mutate:replace-type {} \"{}\")", node, t);
            } else {
                // Generic: pass through with all fields
                std::string v = val_it != cmd.end() ? val_it->second : "0";
                std::string s = sum_it != cmd.end() ? sum_it->second : "";
                sexpr = std::format("({} {} {} \"{}\")", op_name, node, v, s);
            }
            auto mut_result = cs.typed_mutate(sexpr);
            // Issue #147: typed-mutate response now
            // includes invariant_status + invariant_diagnostics.
            print_typed_mutate_response(mut_result);
        }
        return true;
    }
    if (type == "rollback") {
        // {"cmd": "rollback", "id": 1}
        auto id_it = cmd.find("id");
        if (id_it == cmd.end()) {
            std::println("{{\"status\":\"error\",\"msg\":\"missing id\"}}");
        } else {
            auto sexpr = std::format("(rollback {})", id_it->second);
            auto result = cs.eval_on_current(sexpr);
            std::println("{{\"status\":\"ok\",\"rolled_back\":{}}}",
                         result
                             ? (is_bool(*result) ? (as_bool(*result) ? "true" : "false") : "false")
                             : "false");
        }
        return true;
    }
    if (type == "mutation-log") {
        // {"cmd": "mutation-log", "node": 0}
        auto node_it = cmd.find("node");
        if (node_it == cmd.end()) {
            std::println("{{\"status\":\"error\",\"msg\":\"missing node\"}}");
        } else {
            auto node = static_cast<aura::ast::NodeId>(std::stoul(node_it->second));
            auto entries = cs.query_mutation_log(node);
            std::println("{{\"status\":\"ok\",\"log\":[");
            bool first = true;
            for (auto& e : entries) {
                if (!first)
                    std::println(",");
                first = false;
                std::print("  {{\"id\":{},\"ts\":{},\"node\":{},\"op\":\"{}\","
                           "\"old_type\":\"{}\",\"new_type\":\"{}\","
                           "\"summary\":\"{}\",\"status\":\"{}\","
                           "\"invariant_status\":\"{}\"}}",
                           e.mutation_id, e.timestamp_ms, e.target_node,
                           json_escape(e.operator_name), json_escape(e.old_type),
                           json_escape(e.new_type), json_escape(e.summary), json_escape(e.status),
                           json_escape(e.invariant_status));
            }
            std::println("]}}");
        }
        return true;
    }

    // Issue #157 Phase 5: jit:metrics --serve command.
    // Reports the runtime telemetry counters (bypass_count
    // / unchecked_fastpath_count / deopt_count). After
    // Phase 2, bypass_count stays at 0 in steady state
    // and acts as a regression detector — if a future
    // change adds a new bypass site, the counter will
    // tick and ops dashboards can flag it. Pass
    // {"reset": true} to read + reset in one call (the
    // response shows the pre-reset values).
    if (type == "jit:metrics") {
        auto reset_it = cmd.find("reset");
        bool do_reset =
            (reset_it != cmd.end() && (reset_it->second == "true" || reset_it->second == "1"));
        auto bypass = aura_bypass_count();
        auto fastpath = aura_unchecked_fastpath_count();
        auto deopt = aura_deopt_count();
        if (do_reset) {
            aura_counters_reset();
        }
        std::println("{{\"status\":\"ok\",\"metrics\":{{\"bypass_count\":{},"
                     "\"unchecked_fastpath_count\":{},\"deopt_count\":{}}},"
                     "\"reset\":{}}}",
                     bypass, fastpath, deopt, do_reset ? "true" : "false");
        return true;
    }

    // ── Configuration commands ─────────────────────────────
    if (type == "config") {
        auto key_it = cmd.find("key");
        auto val_it = cmd.find("value");
        if (key_it != cmd.end() && val_it != cmd.end()) {
            auto& key = key_it->second;
            if (key == "strict") {
                cs.set_strict_mode(val_it->second == "true" || val_it->second == "1");
                auto val_str = val_it->second == "true" ? "true" : "false";
                std::string out =
                    "{\"status\":\"ok\",\"config\":{\"" + json_escape(key) + "\":" + val_str + "}}";
                std::println("{}", out);
                return true;
            }
            std::println("{{\"status\":\"error\",\"msg\":\"unknown config key: {}\"}}",
                         json_escape(key));
        } else {
            std::println("{{\"status\":\"error\",\"msg\":\"missing key or value fields\"}}");
        }
        return true;
    }
    return false;
}

static void serve_json_code(aura::compiler::CompilerService& cs, ServeCmd& cmd,
                            const std::string& type) {
    auto code_it = cmd.find("code");
    if (code_it == cmd.end()) {
        std::println("{{\"status\":\"error\",\"msg\":\"missing code field\"}}");
        return;
    }
    auto& code = code_it->second;

    if (type == "defmacro") {
        auto result = cs.define_function(code);
        if (result) {
            auto name = cmd.count("name") ? cmd["name"] : "<macro>";
            std::println("{{\"status\":\"defined\",\"name\":\"{}\"}}", json_escape(name));
        } else {
            auto& d = result.error();
            std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}", json_escape(d.format()));
        }
    } else if (type == "define") {
        auto result = cs.define_function(code);
        if (result) {
            auto name = cmd.count("name") ? cmd["name"] : "<lambda>";
            std::println("{{\"status\":\"defined\",\"name\":\"{}\"}}", json_escape(name));
        } else {
            auto& d = result.error();
            std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}", json_escape(d.format()));
        }
    } else if (type == "exec") {
        // Issue #4702: split the chunk into its top-level
        // forms and evaluate them one at a time. A
        // multi-form chunk used to be evaluated as ONE
        // program: the compiler lowers a multi-form program
        // (Begin root) to IR, where the top-level defines
        // become IR-local cells that die with the IR module
        // — so the defines never reached the session env
        // and the NEXT exec failed with "unbound variable".
        // One form per eval makes every top-level define a
        // single-form program, which the compiler binds in
        // the session env. An unbalanced chunk is rejected
        // before anything runs (it used to be accepted
        // silently and to swallow the forms after it).
        auto split = aura::serve::detail::split_top_level_forms(code);
        if (!split.ok()) {
            std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}", json_escape(split.error));
            return;
        }
        // Timeout-based eval: run in async future, wait up to 30s
        int timeout_sec = 30;
        auto timeout_it = cmd.find("timeout");
        if (timeout_it != cmd.end()) {
            timeout_sec =
                aura::core::parse_leading_int<int>(timeout_it->second).value_or(timeout_sec);
        }
        auto future = std::async(std::launch::async, [&]() {
            return aura::serve::detail::eval_top_level_forms(
                split.forms, [&](std::string_view form) { return cs.exec_with_cache(form); });
        });
        auto status = future.wait_for(std::chrono::seconds(timeout_sec));
        if (status == std::future_status::timeout) {
            std::println("{{\"status\":\"timeout\",\"msg\":\"exec timed out ({}s)\"}}",
                         timeout_sec);
        } else {
            auto result = future.get();
            if (!result) {
                // The chunk held no top-level form.
                std::println("{{\"status\":\"error\",\"msg\":\"parse error: no "
                             "complete form in exec chunk\"}}");
            } else if (!*result) {
                auto& d = (*result).error();
                std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}", json_escape(d.format()));
            } else {
                try {
                    auto& v = **result;
                    if (aura::compiler::types::is_closure(v)) {
                        std::println("{{\"status\":\"closure\",\"value\":\"#<"
                                     "procedure>\"}}");
                    } else {
                        std::println("{{\"status\":\"ok\",\"value\":\"{}\"}}",
                                     json_escape(fmt_val(v, cs)));
                    }
                } catch (const std::bad_alloc&) {
                    std::println("{{\"status\":\"error\",\"msg\":\"out of memory\"}}");
                }
            }
        }
    } else if (type == "redefine") {
        auto name = cmd.count("name") ? cmd["name"] : "<lambda>";
        auto result = cs.define_function(code);
        if (result) {
            std::println("{{\"status\":\"redefined\",\"name\":\"{}\"}}", json_escape(name));
        } else {
            auto& d = result.error();
            std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}", json_escape(d.format()));
        }
    } else if (type == "unparse") {
        auto alloc = cs.arena().allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto ux_pr = aura::parser::parse_to_flat(code, flat, pool);
        if (!ux_pr.success) {
            std::println("{{\"status\":\"error\",\"msg\":\"parse error\"}}");
        } else {
            flat.root = ux_pr.root;
            auto src = aura::compiler::unparse_node(flat, pool, flat.root);
            std::println("{{\"status\":\"ok\",\"source\":\"{}\"}}", json_escape(src));
        }
    } else if (type == "write") {
        auto write_path = cmd.count("file") ? cmd["file"] : "";
        if (write_path.empty()) {
            std::println("{{\"status\":\"error\",\"msg\":\"missing file field\"}}");
        } else {
            auto w_alloc = cs.arena().allocator();
            aura::ast::StringPool w_pool(w_alloc);
            aura::ast::FlatAST w_flat(w_alloc);
            auto w_pr = aura::parser::parse_to_flat(code, w_flat, w_pool);
            if (!w_pr.success) {
                std::println("{{\"status\":\"error\",\"msg\":\"parse error\"}}");
            } else {
                w_flat.root = w_pr.root;
                auto w_src = aura::compiler::unparse_node(w_flat, w_pool, w_flat.root);
                std::ofstream w_f(write_path);
                if (w_f) {
                    w_f << w_src << "\n";
                    std::println("{{\"status\":\"ok\",\"file\":\"{}\"}}", json_escape(write_path));
                } else {
                    std::println("{{\"status\":\"error\",\"msg\":\"cannot write: {}\"}}",
                                 json_escape(write_path));
                }
            }
        }
    } else if (type == "query") {
        // Eval code and return result (like exec, but simpler)
        auto future = std::async(std::launch::async, [&]() { return cs.exec_with_cache(code); });
        auto status = future.wait_for(std::chrono::seconds(30));
        if (status == std::future_status::timeout) {
            std::println("{{\"status\":\"timeout\",\"msg\":\"query timed out (30s)\"}}");
        } else {
            auto result = future.get();
            if (result) {
                auto& v = *result;
                if (aura::compiler::types::is_closure(v)) {
                    std::println("{{\"status\":\"ok\",\"value\":\"#<procedure>\"}}");
                } else {
                    std::println("{{\"status\":\"ok\",\"value\":\"{}\"}}",
                                 json_escape(fmt_val(v, cs)));
                }
            } else {
                auto& d = result.error();
                std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}", json_escape(d.format()));
            }
        }
    } else if (type == "mutate") {
        // {"cmd": "mutate", "op": "replace-value", "node": 0, "value": 42,
        // "summary": "change x"}
        auto op_it = cmd.find("op");
        auto node_it = cmd.find("node");
        auto val_it = cmd.find("value");
        auto sum_it = cmd.find("summary");
        if (op_it == cmd.end() || node_it == cmd.end()) {
            std::println("{{\"status\":\"error\",\"msg\":\"missing op or node\"}}");
        } else {
            auto node = std::stoll(node_it->second);
            auto sexpr = std::format(
                "({} {} {}{}{})", op_it->second, node, val_it != cmd.end() ? val_it->second : "0",
                sum_it != cmd.end() ? " \"" + sum_it->second + "\"" : " \"\"", "");
            // Evaluate mutation against the persistent AST
            auto mut_result = cs.typed_mutate(sexpr);
            // Issue #147: typed-mutate response now
            // includes invariant_status + invariant_diagnostics.
            print_typed_mutate_response(mut_result);
        }
    } else if (type == "rollback") {
        // {"cmd": "rollback", "id": 1}
        auto id_it = cmd.find("id");
        if (id_it == cmd.end()) {
            std::println("{{\"status\":\"error\",\"msg\":\"missing id\"}}");
        } else {
            auto sexpr = std::format("(rollback {})", id_it->second);
            auto result = cs.eval_on_current(sexpr);
            std::println("{{\"status\":\"ok\",\"rolled_back\":{}}}",
                         result
                             ? (is_bool(*result) ? (as_bool(*result) ? "true" : "false") : "false")
                             : "false");
        }
    } else if (type == "mutation-log") {
        // {"cmd": "mutation-log", "node": 0}
        auto node_it = cmd.find("node");
        if (node_it == cmd.end()) {
            std::println("{{\"status\":\"error\",\"msg\":\"missing node\"}}");
        } else {
            auto node = static_cast<aura::ast::NodeId>(std::stoul(node_it->second));
            auto entries = cs.query_mutation_log(node);
            std::println("{{\"status\":\"ok\",\"log\":[");
            bool first = true;
            for (auto& e : entries) {
                if (!first)
                    std::println(",");
                first = false;
                std::print("  {{\"id\":{},\"ts\":{},\"node\":{},\"op\":\"{}\","
                           "\"old_type\":\"{}\",\"new_type\":\"{}\","
                           "\"summary\":\"{}\",\"status\":\"{}\","
                           "\"invariant_status\":\"{}\"}}",
                           e.mutation_id, e.timestamp_ms, e.target_node,
                           json_escape(e.operator_name), json_escape(e.old_type),
                           json_escape(e.new_type), json_escape(e.summary), json_escape(e.status),
                           json_escape(e.invariant_status));
            }
            std::println("]}}");
        }
    } else {
        std::println("{{\"status\":\"error\",\"msg\":\"unknown command: {}\"}}", json_escape(type));
    }
}

static int run_sync_serve() {
    SyncServeState st;
    st.soft_shared_graph = !aura::serve::production_abi_selfcheck_required();
    st.shared_workspace_tree = aura::compiler::Evaluator::create_workspace_tree();
    st.sessions.try_emplace(st.active_session);
    st.session_names.insert(st.active_session);
    {
        auto& default_cs = st.sessions[st.active_session];
        default_cs.set_session_id(st.active_session);
        default_cs.set_workspace_tree(st.shared_workspace_tree);
        aura::compiler::CompilerService::register_session(st.active_session, &default_cs);
    }
    if (st.soft_shared_graph) {
        static bool soft_serve_b_logged = false;
        if (!soft_serve_b_logged) {
            soft_serve_b_logged = true;
            std::println(std::cerr, "aura: Soft shared_workspace (#4047 B): sync --serve named "
                                    "sessions share one CompilerService + workspace_tree — "
                                    "orch→project bindings visible. NOT production isolation.");
        }
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;

        auto trimmed = line;
        auto first_non_space = trimmed.find_first_not_of(" \t");
        if (first_non_space != std::string::npos && trimmed[first_non_space] == '{') {
            // ── JSON command protocol ──────────────────────────
            auto json_input = trimmed.substr(first_non_space);
            auto cmd = parse_json_command(json_input);
            if (cmd.empty()) {
                std::println("{{\"status\":\"parse-error\",\"msg\":\"invalid JSON command\"}}");
                continue;
            }
            auto cmd_type = cmd.find("cmd");
            if (cmd_type == cmd.end()) {
                std::println("{{\"status\":\"error\",\"msg\":\"missing cmd field\"}}");
                continue;
            }
            auto& type = cmd_type->second;
            if (type == "session") {
                serve_json_session(st, cmd);
                continue;
            }
            if (type == "module") {
                serve_json_module(st.resolve(), cmd);
                continue;
            }
            auto& cs = st.resolve();
            aura::messaging::g_current_compiler_service = &cs;
            if (serve_json_immediate(cs, cmd, type))
                continue;
            serve_json_code(cs, cmd, type);
        } else {
            // ── Plain S-expression (backward compatible) ────────
            auto& cs = st.resolve();
            aura::messaging::g_current_compiler_service = &cs;
            auto r = cs.eval(line);
            if (r) {
                std::println("{{\"status\":\"ok\",\"value\":\"{}\"}}",
                             json_escape(fmt_val(*r, cs)));
            } else {
                auto& d = r.error();
                std::println("{{\"status\":\"error\",\"kind\":{},\"msg\":\"{}\",\"node_id\":{}}}",
                             static_cast<int>(d.kind), json_escape(d.format()), d.node_id);
                // Auto-fix path removed: it depended on the per-eval
                // `flat` / `pool` (now produced by cs.eval internally).
                // Without them, lower_to_ir / AutoFixEngine have no
                // FlatAST to operate on. Surfacing the error directly
                // is sufficient for the --serve JSON protocol.
            }
        }
    }
    aura::compiler::Evaluator::destroy_workspace_tree(st.shared_workspace_tree);
    return 0;
}

static void inline_emit_module_sources(std::string& input) {
    // ── Resolve import/require: prepend module source (recursive) ──
    // Replace (import "std/name") / (require "std/name") with the
    // module's source code so defines are processed inline by eval_ir.
    // Recursively inline any nested requires too, so the prepended
    // block is self-contained and doesn't re-execute requires at
    // runtime (which would re-load modules, leak module arenas, and
    // can segfault the IR pipeline on CI).
    {
        static const std::string std_root = aura::compiler::paths::resolve_stdlib_root();

        // Find the matching close paren starting from an open paren,
        // respecting string literals and nested parens.
        auto find_matching_close = [](const std::string& s, std::size_t open_paren) {
            std::size_t i = open_paren;
            int depth = 0;
            bool in_str = false;
            while (i < s.size()) {
                char c = s[i];
                if (in_str) {
                    if (c == '\\' && i + 1 < s.size()) {
                        i += 2;
                        continue;
                    }
                    if (c == '"')
                        in_str = false;
                } else {
                    if (c == '"')
                        in_str = true;
                    else if (c == '(')
                        depth++;
                    else if (c == ')') {
                        depth--;
                        if (depth == 0)
                            return i + 1;
                    }
                }
                i++;
            }
            return std::string::npos;
        };

        // Strip (export ...) — finds the first '(' matching its ')'.
        auto strip_export = [&](std::string& s) {
            auto p = s.find("(export");
            if (p == std::string::npos)
                return;
            auto end = find_matching_close(s, p);
            if (end == std::string::npos)
                return;
            s.erase(p, end - p);
        };

        // Resolve "std/name" or sym-like-name to a file path.
        // name is string_view — materialize before operator+ with literals.
        auto resolve_path = [&](std::string_view name) -> std::string {
            if (name.starts_with("std/"))
                return std_root + std::string(name.substr(4)) + ".aura";
            return std::string(name) + ".aura";
        };

        // Recursively inline a module file. Replaces nested
        // (require X) / (import "X") forms with the inlined source
        // of X (depth-first, cycle-safe via `visited`).
        std::set<std::string> visited;
        std::function<std::string(const std::string&)> inline_module;
        inline_module = [&](const std::string& path) -> std::string {
            if (visited.count(path))
                return "";
            visited.insert(path);
            std::ifstream mf(path);
            if (!mf) {
                std::println(std::cerr, "warning: cannot open module '{}'", path);
                return "";
            }
            std::string src((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
            strip_export(src);
            std::string out;
            std::size_t pos = 0;
            while (pos < src.size()) {
                // Skip whitespace and comments at the top level
                while (pos < src.size() &&
                       (src[pos] == ' ' || src[pos] == '\n' || src[pos] == '\t' ||
                        src[pos] == '\r' || src[pos] == ';')) {
                    if (src[pos] == ';') {
                        // line comment
                        while (pos < src.size() && src[pos] != '\n')
                            pos++;
                    } else
                        pos++;
                }
                if (pos >= src.size() || src[pos] != '(') {
                    if (pos < src.size())
                        out += src[pos++];
                    else
                        break;
                    continue;
                }
                std::size_t form_start = pos;
                std::size_t form_end = find_matching_close(src, pos);
                if (form_end == std::string::npos) {
                    out += src.substr(pos);
                    break;
                }
                std::string form = src.substr(form_start, form_end - form_start);
                // Inspect the form's head
                std::string head;
                std::size_t h = form_start + 1;
                while (h < form_end &&
                       (src[h] == ' ' || src[h] == '\n' || src[h] == '\t' || src[h] == '\r'))
                    h++;
                while (h < form_end && src[h] != ' ' && src[h] != '\n' && src[h] != '\t' &&
                       src[h] != '\r' && src[h] != ')')
                    head += src[h++];
                if (head == "require" || head == "import" || head == "use") {
                    // Extract first arg (a symbol or string)
                    while (h < form_end &&
                           (src[h] == ' ' || src[h] == '\n' || src[h] == '\t' || src[h] == '\r'))
                        h++;
                    std::string arg;
                    bool in_str = false;
                    while (h < form_end) {
                        char c = src[h];
                        if (in_str) {
                            arg += c;
                            if (c == '"')
                                in_str = false;
                        } else if (c == '"') {
                            arg += c;
                            in_str = true;
                        } else if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == ')') {
                            break;
                        } else {
                            arg += c;
                        }
                        h++;
                    }
                    // Strip surrounding quotes if present
                    if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"')
                        arg = arg.substr(1, arg.size() - 2);
                    if (!arg.empty()) {
                        std::string nested = resolve_path(arg);
                        out += inline_module(nested);
                        out += '\n';
                    }
                    // Skip the original form in src (don't copy it to out)
                } else {
                    out += form;
                    out += '\n';
                }
                pos = form_end;
            }
            return out;
        };

        // Process the user input: for each top-level (import "X") or
        // (require "X"), inline the module and replace the form.
        std::string resolved;
        std::size_t pos = 0;
        while (pos < input.size()) {
            auto imp = input.find("(import \"", pos);
            auto req = input.find("(require \"", pos);
            std::size_t found = std::string::npos;
            bool has_imp = (imp != std::string::npos);
            bool has_req = (req != std::string::npos);
            if (!has_imp && !has_req) {
                resolved += input.substr(pos);
                break;
            }
            if (has_imp && (!has_req || imp < req))
                found = imp;
            else
                found = req;
            resolved += input.substr(pos, found - pos);
            auto quote_start = input.find('"', found);
            auto quote_end = input.find('"', quote_start + 1);
            if (quote_start == std::string::npos || quote_end == std::string::npos) {
                resolved += input.substr(found);
                break;
            }
            auto module_name = input.substr(quote_start + 1, quote_end - quote_start - 1);
            auto close_paren = input.find(')', quote_end);
            if (close_paren == std::string::npos) {
                resolved += input.substr(found);
                break;
            }
            std::string module_path = resolve_path(module_name);
            visited.clear();
            resolved += inline_module(module_path);
            resolved += '\n';
            pos = close_paren + 1;
        }
        input = resolved;
    }
}

static void emit_register_primitive_dispatch(aura::compiler::CompilerService& cs) {
    // ── Step 3: Generate primitive dispatch C code ───────────
    // Build per-slot dispatch functions with the correct ABI:
    // each function has signature (int64_t* args, int32_t argc)
    // matching the PrimFn type used in aura_closure_call.
    //
    // The generated code creates one static function per primitive
    // slot and registers each at its correct evaluator slot number.
    // The slot numbers are matched at compile time.
    {
        auto& prims = cs.evaluator().primitives();
        std::string code = "// Auto-generated by aura --emit-binary\n";
        code += "#include <stdint.h>\n#include <stddef.h>\n#include <stdio.h>\n";
        // Issue #4628: the AOT member wrapper compares the elements
        // structurally (string content), so strcmp is needed.
        code += "#include <string.h>\n\n";
        // Issue #4502 F3: pairs are exactly low2 == 01. The old `x<0 ||`
        // test matched every negative fixnum and every boxed float/string
        // under #4482. Value ABI must match lib/runtime.c AURA_VALUE_ABI.
        // #4502 step 1: every constant comes from lib/aura_value_abi.h.
        code += "#define AURA_AOT_VALUE_ABI " AURA_ABI_STR(AURA_VALUE_ABI) "\n";
        static_assert(AURA_VALUE_ABI == 4, "#4502: update the AOT preamble with the ABI");
        code += "#define AURA_AOT_IS_PAIR(x) (((x)&3)==1)\n";
        code += "#define AURA_AOT_FALSE " AURA_ABI_STR(AURA_V_FALSE) "\n";
        code += "#define AURA_AOT_TRUE " AURA_ABI_STR(AURA_V_TRUE) "\n";
        code += "#define AURA_AOT_VOID " AURA_ABI_STR(AURA_V_VOID) "\n";
        code += "#define MAKE_STRING_SENTINEL(i) ((int64_t)(((uint64_t)(i) << " AURA_ABI_STR(
            AURA_IMM_PAYLOAD_SHIFT) ") | " AURA_ABI_STR(AURA_STRING_LOW8) "))\n";
        code += "int64_t aura_display_int(int64_t);\n";
        code += "int64_t aura_alloc_pair(int64_t, int64_t);\n";
        code += "int64_t aura_pair_car(int64_t);\n";
        code += "int64_t aura_pair_cdr(int64_t);\n";
        code += "int64_t aura_closure_call(int64_t, int64_t*, int64_t);\n";
        // Issue #4628: the AOT face of the evaluator's equal? — content
        // compare for strings, value compare for floats.
        code += "const char* aura_string_ref(int64_t);\n";
        code += "double aura_float_ref(int64_t);\n";
        code += "\n";
        code += "typedef int64_t (*PF)(int64_t*,int32_t);\n";
        // Issue #4628: the AOT member wrapper compared raw words, so even
        // two identical strings were identity-compared and a structurally
        // equal pair was never found. This helper is the AOT face of the
        // evaluator's equal? comparison (R7RS 6.4: member uses equal?).
        code += "#define AURA_AOT_IS_STRING(x) ((((int64_t)(x)) & 0xFF) == " AURA_ABI_STR(
            AURA_STRING_LOW8) ")\n";
        code += "#define AURA_AOT_IS_FLOAT(x) ((((int64_t)(x)) & 0xFF) == " AURA_ABI_STR(
            AURA_FLOAT_LOW8) ")\n";
        code += "static inline int aura_aot_equal(int64_t a, int64_t b, int depth){\n";
        code += "  if(a==b) return 1;\n";
        code += "  if(depth>64) return 1;\n";
        code += "  if(AURA_AOT_IS_PAIR(a)&&AURA_AOT_IS_PAIR(b)) return "
                "aura_aot_equal(aura_pair_car(a),aura_pair_car(b),depth+1) && "
                "aura_aot_equal(aura_pair_cdr(a),aura_pair_cdr(b),depth+1);\n";
        code += "  if(AURA_AOT_IS_STRING(a)&&AURA_AOT_IS_STRING(b)) return "
                "strcmp(aura_string_ref(a),aura_string_ref(b))==0;\n";
        code += "  if(AURA_AOT_IS_FLOAT(a)&&AURA_AOT_IS_FLOAT(b)) return "
                "aura_float_ref(a)==aura_float_ref(b);\n";
        code += "  return 0;\n";
        code += "}\n";
        code += "\n";
        code += "void aura_register_primitive_fn(int64_t, int64_t);\n";
        code += "\n";

        // Generate one wrapper function for each known primitive
        for (std::size_t s = 0; s < prims.slot_count(); ++s) {
            auto n = prims.name_for_slot(s);
            if (n.empty())
                continue;
            code += "static int64_t prim_" + std::to_string(s) + "(int64_t* a, int32_t c) {\n";
            if (n == "+")
                code += "    int64_t r=0; for(int i=0;i<c;i++) r+=a[i]; return r;\n";
            else if (n == "-")
                code += "    if(c<1)return 0; if(c==1)return -a[0]; int64_t r=a[0]; for(int "
                        "i=1;i<c;i++) r-=a[i]; return r;\n";
            else if (n == "*")
                code += "    int64_t r=1; for(int i=0;i<c;i++) r*=a[i]; return r;\n";
            else if (n == "/")
                code += "    if(c<1)return 1; int64_t r=a[0]; for(int "
                        "i=1;i<c;i++){if(a[i]==0){r=0;break;}r/=a[i];} return r;\n";
            else if (n == "=" || n == "eq?" || n == "eqv?")
                // Issue #4640: a boolean result must be the TAGGED
                // #t / #f (7 / 3), not a raw 1 / 0: the AOT display
                // reads the low bits, and a raw 1 looks like a pair
                // (low2 == 01) -- the image printed "(0)" / "(2)"
                // instead of #t. The evaluator's booleans are the
                // tagged specials (value_tags.h: kSpecialTrue 7,
                // kSpecialFalse 3).
                code += "    for(int i=0;i<c-1;i++) if(a[i]!=a[i+1]) return AURA_AOT_FALSE; "
                        "return AURA_AOT_TRUE;\n";
            else if (n == "equal?")
                // Issue #4640: equal? is the STRUCTURAL comparison
                // (R7RS 6.4): content compare for strings, value
                // compare for numbers, structural compare for pairs
                // -- the same walk member uses (#4628). The AOT face
                // compared raw words, so two structurally equal lists
                // were never equal and the std assoc (which compares
                // the key with equal?) answered differently from the
                // evaluator.
                code += "    for(int i=0;i<c-1;i++) if(!aura_aot_equal(a[i],a[i+1],0)) "
                        "return AURA_AOT_FALSE; return AURA_AOT_TRUE;\n";
            else if (n == "<")
                code += "    for(int i=0;i<c-1;i++) if(!(a[i]<a[i+1])) return AURA_AOT_FALSE; "
                        "return AURA_AOT_TRUE;\n";
            else if (n == ">")
                code += "    for(int i=0;i<c-1;i++) if(!(a[i]>a[i+1])) return AURA_AOT_FALSE; "
                        "return AURA_AOT_TRUE;\n";
            else if (n == "<=")
                code += "    for(int i=0;i<c-1;i++) if(!(a[i]<=a[i+1])) return AURA_AOT_FALSE; "
                        "return AURA_AOT_TRUE;\n";
            else if (n == ">=")
                code += "    for(int i=0;i<c-1;i++) if(!(a[i]>=a[i+1])) return AURA_AOT_FALSE; "
                        "return AURA_AOT_TRUE;\n";
            else if (n == "not")
                code += "    return "
                        "(c<1||a[0]==AURA_AOT_FALSE)?AURA_AOT_TRUE:AURA_AOT_FALSE;\n"; // #4491:
                                                                                       // only
                                                                                       // #f is
                                                                                       // false
            else if (n == "append") {
                code += "    if(c<1)return AURA_AOT_VOID; int64_t lst1=c>0?a[0]:0; int64_t "
                        "lst2=c>1?a[1]:0;\n";
                code += "    int64_t rev[1024]; int revc=0;\n";
                code += "    "
                        "while(AURA_AOT_IS_PAIR(lst1)&&revc<1024){rev[revc++]=aura_pair_car("
                        "lst1); lst1=aura_pair_cdr(lst1);}\n";
                code += "    int64_t r=lst2;\n";
                code += "    for(int i=revc-1;i>=0;i--) r=aura_alloc_pair(rev[i],r);\n";
                code += "    return r;\n";
            } else if (n == "member") {
                // Issue #4628: compare the elements structurally
                // (equal? semantics), not by raw word identity, and answer
                // #f when the element is not found — the evaluator's
                // contract (R7RS 6.4 / #4491).
                code += "    int64_t key=c>0?a[0]:0; int64_t lst=c>1?a[1]:0;\n";
                code += "    while(AURA_AOT_IS_PAIR(lst)){if(aura_aot_equal(aura_pair_car(lst),"
                        "key,0))return lst; lst=aura_pair_cdr(lst);}\n";
                code += "    return AURA_AOT_FALSE;\n";
            } else if (n == "map") {
                code += "    int64_t fn=c>0?a[0]:0; int64_t lst=c>1?a[1]:0;\n";
                code += "    int64_t rev[1024]; int revc=0;\n";
                code += "    int64_t args[8];\n";
                code += "    while(AURA_AOT_IS_PAIR(lst)&&revc<1024){args[0]=aura_pair_car(lst); "
                        "rev[revc++]=aura_closure_call(fn,args,1); lst=aura_pair_cdr(lst);}\n";
                code += "    int64_t r=AURA_AOT_VOID; for(int i=revc-1;i>=0;i--) "
                        "r=aura_alloc_pair(rev[i],r);\n";
                code += "    return r;\n";
            } else if (n == "null?")
                // Issue #4640: tagged #t / #f, not a raw 1 / 0 (see
                // the comparison wrappers above).
                // #4502: only '() is null -- not 0 or unspecified.
                code += "    return "
                        "(c>=1&&a[0]==AURA_AOT_VOID)?AURA_AOT_TRUE:AURA_AOT_FALSE;\n";
            else if (n == "pair?")
                // Issue #106 sub-task 4 (audit): the previous
                // expression was (c>0&&(((a[0]&3)==1))||a[0]<0)
                // which due to C precedence parses as
                //   (c>0 && ((a[0]&3)==1)) || (a[0]<0)
                // and reads a[0] when c==0 — a read of an
                // out-of-range arg slot that can segfault on
                // x86_64 emit-binary with certain call sites
                // (the old -9000... string sentinel is no
                // longer part of the pair tag; we just check
                // the low-bit tag like IS_PAIR in the runtime).
                // Issue #4640: tagged #t / #f, not a raw 1 / 0 (see
                // the comparison wrappers above).
                code += "    return (c>0 && ((a[0]&3)==1)) ? AURA_AOT_TRUE : AURA_AOT_FALSE;\n";
            else if (n == "cons")
                code += "    return aura_alloc_pair(c>0?a[0]:0,c>1?a[1]:0);\n";
            else if (n == "car")
                code += "    return (c>0)?aura_pair_car(a[0]):0;\n";
            else if (n == "cdr")
                code += "    return (c>0)?aura_pair_cdr(a[0]):0;\n";
            else if (n == "length") {
                code += "    int64_t v=c>0?a[0]:0; int64_t n=0;\n";
                code += "    while(AURA_AOT_IS_PAIR(v)&&n<9999){n++;v=aura_pair_cdr(v);}\n";
                code += "    return n;\n";
            } else if (n == "list-ref") {
                code += "    int64_t v=c>0?a[0]:0; int64_t i=c>1?a[1]:0;\n";
                code += "    while(i>0&&AURA_AOT_IS_PAIR(v)){v=aura_pair_cdr(v);i--;}\n";
                code += "    if(i==0&&AURA_AOT_IS_PAIR(v))return aura_pair_car(v); return 0;\n";
            } else if (n == "reverse") {
                code += "    int64_t v=c>0?a[0]:0; int64_t r=AURA_AOT_VOID;\n";
                code += "    "
                        "while(AURA_AOT_IS_PAIR(v)){r=aura_alloc_pair(aura_pair_car(v),r);v=aura_"
                        "pair_cdr(v);}\n";
                code += "    return r;\n";
            } else if (n == "list") {
                code += "    int64_t r=AURA_AOT_VOID;\n";
                code += "    for(int i=c-1;i>=0;i--) r=aura_alloc_pair(a[i],r);\n";
                code += "    return r;\n";
            } else if (n == "display") {
                code += "    if(c>0)aura_display_int(a[0]); fflush(stdout);\n";
                code += "    return (c>0)?a[0]:0;\n";
            } else if (n == "filter") {
                code += "    int64_t pred=c>0?a[0]:0; int64_t lst=c>1?a[1]:0;\n";
                code += "    int64_t rev[1024]; int revc=0;\n";
                code += "    int64_t args[8];\n";
                code += "    while(AURA_AOT_IS_PAIR(lst)&&revc<1024){int64_t v=aura_pair_car(lst); "
                        "args[0]=v; if(aura_closure_call(pred,args,1)!=0)rev[revc++]=v; "
                        "lst=aura_pair_cdr(lst);}\n";
                code += "    int64_t r=AURA_AOT_VOID; for(int i=revc-1;i>=0;i--) "
                        "r=aura_alloc_pair(rev[i],r);\n";
                code += "    return r;\n";
            } else if (n == "foldl") {
                code += "    int64_t fn=c>0?a[0]:0; int64_t acc=c>1?a[1]:0; int64_t "
                        "lst=c>2?a[2]:0;\n";
                code += "    int64_t args[8];\n";
                code += "    while(AURA_AOT_IS_PAIR(lst)){args[0]=acc; args[1]=aura_pair_car(lst); "
                        "acc=aura_closure_call(fn,args,2); lst=aura_pair_cdr(lst);}\n";
                code += "    return acc;\n";
            } else {
                // Unknown primitives (e.g. ADT constructors): build pair chain with tag string
                code += "    int64_t r=AURA_AOT_VOID; for(int i=c-1;i>=0;i--) "
                        "r=aura_alloc_pair(a[i],r);\n";
                code += "    r=aura_alloc_pair(MAKE_STRING_SENTINEL(" + std::to_string(s) +
                        "),r); return r;\n";
            }
            code += "}\n\n";
        }

        // Constructor: register each primitive function at its slot
        code += "__attribute__((constructor)) void reg(void) {\n";
        for (std::size_t s = 0; s < prims.slot_count(); ++s) {
            auto n = prims.name_for_slot(s);
            if (!n.empty()) {
                code += "  aura_register_primitive_fn(" + std::to_string(s) + ",(int64_t)prim_" +
                        std::to_string(s) + ");\n";
            }
        }
        code += "}\n";
        aura_set_prim_registration(code.c_str());
    }
}

static int emit_standalone_binary(int argc, char** argv) {
    aura::compiler::CompilerService cs;
    std::string input;
    std::string out_path;

    // ── Collect input: file args or stdin ─────────────────────
    // Usage:
    //   echo '(+ 1 2)' | ./aura --emit-binary myapp         (stdin)
    //   ./aura --emit-binary '(+ 1 2)' myapp    (inline: expr,output)
    //   ./aura --emit-binary file1 file2 out    (file: sources...,output)
    //
    if (argc > 3) {
        // Decide mode: if argv[2] starts with '(', it's inline expression.
        // Otherwise, all args from argv[2..argc-2] are files, argv[argc-1] is output.
        if (argv[2][0] == '(') {
            // Inline mode: ./aura --emit-binary '(+ 1 2)' myapp
            // Issue #960: do not mutate main's argv/argc (keeps
            // crash diagnostics and later argv reads correct).
            input = argv[2];
            out_path = argv[3];
        } else {
            // File mode: ./aura --emit-binary lib.aura main.aura out
            for (int i = 2; i < argc - 1; ++i) {
                std::ifstream f(argv[i]);
                if (!f) {
                    std::println(std::cerr, "error: cannot open '{}'", argv[i]);
                    return 1;
                }
                input += std::string((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                input += '\n';
            }
            out_path = argv[argc - 1];
        }
    } else {
        // Stdin mode: echo '(+ 1 2)' | ./aura --emit-binary myapp
        // Issue #4640: read the WHOLE stdin, not just its first
        // line. The eval path reads all of stdin, so a program
        // piped in with several lines was emitted with only its
        // first line -- the AOT binary then disagreed with the
        // evaluator by construction (every row after the first
        // line was missing from the image).
        {
            std::ostringstream buf;
            buf << std::cin.rdbuf();
            input = buf.str();
        }
        out_path = argv[2];
    }

    if (input.empty()) {
        std::println(std::cerr, "error: no input");
        return 1;
    }

    // Issue #4640: the emit must resolve the SAME names as the
    // evaluator. The oneshot eval path auto-loads the std modules
    // the program can reference (build_lazy_oneshot_std_prelude),
    // because assoc / foldr / zip / ... are std FUNCTIONS, not
    // primitives: without the std module the IR path cannot call
    // them and the emit died with "cannot call: assoc" while the
    // evaluator answered. Prepend the same lazy prelude so the
    // std module source is inlined by the import/require step
    // below and the AOT image carries the same definitions.
    {
        std::string prelude = build_lazy_oneshot_std_prelude(input);
        if (!prelude.empty())
            input = prelude + input;
    }

    inline_emit_module_sources(input);

    // ── Step 1: Parse → Lower → Passes ──
    // eval_ir now includes pre_exec_requires for module support.
    auto eval_result = cs.eval_ir(input);
    if (!eval_result) {
        std::println(std::cerr, "error: {}", eval_result.error().format());
        return 1;
    }
    auto& mod = cs.last_ir_module();
    if (!mod || mod->functions.empty()) {
        std::println(std::cerr, "no IR module generated");
        return 1;
    }

    // ── Step 2: Convert IRModule functions to FlatFunction ────
    // We need to keep all backing storage alive for the FlatFunction array.
    // Each FlatFunction has pointers into our owned vectors.
    //
    // Storage layout:
    //   instr_pool[batch_i][fn_i][block_i]  = vector of FlatInstruction
    //   block_pool[batch_i][fn_i]           = vector of FlatBlock (points into instr_pool)
    //   name_pool[batch_i]                  = string (flat_fn.name points here)
    //   flat_fn_array                       = vector of FlatFunction (final array)

    std::size_t num_fns = mod->functions.size();
    std::vector<std::vector<std::vector<aura::jit::FlatInstruction>>> instr_pool;
    std::vector<std::vector<aura::jit::FlatBlock>> block_pool;
    std::deque<std::string> name_pool; // deque: stable pointers on push_back
    std::vector<aura::jit::FlatFunction> flat_fn_array;
    flat_fn_array.reserve(num_fns);
    instr_pool.reserve(num_fns);
    block_pool.reserve(num_fns);

    for (auto& ir_fn : mod->functions) {
        instr_pool.emplace_back();
        block_pool.emplace_back();
        name_pool.push_back(ir_fn.name);

        auto& instrs = instr_pool.back();
        auto& blocks = block_pool.back();

        instrs.resize(ir_fn.blocks.size());
        blocks.resize(ir_fn.blocks.size());

        for (std::size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
            auto& block = ir_fn.blocks[bi];
            for (auto& inst : block.instructions) {
                instrs[bi].push_back(
                    {static_cast<std::uint32_t>(inst.opcode),
                     {inst.operands[0], inst.operands[1], inst.operands[2], inst.operands[3]}});
            }
            blocks[bi] = {block.id, instrs[bi].data(),
                          static_cast<std::uint32_t>(instrs[bi].size())};
        }

        flat_fn_array.push_back({name_pool.back().c_str(), ir_fn.entry_block, ir_fn.local_count,
                                 ir_fn.arg_count, blocks.data(),
                                 static_cast<std::uint32_t>(blocks.size()),
                                 nullptr, // func_id_map not used for AOT
                                 0});
    }

    emit_register_primitive_dispatch(cs);

    // Set string pool for OpConstString
    {
        auto& pool = mod->string_pool;
        std::vector<const char*> raw_pool(pool.size());
        for (std::size_t i = 0; i < pool.size(); ++i)
            raw_pool[i] = pool[i].c_str();
        aura_set_string_pool(raw_pool.data(), static_cast<unsigned>(raw_pool.size()));
    }

    // Issue #243 Phase 1 / #957: production defuse_version() for AOT
    // mangle suffix + registration record (one acquire load).
    aura_set_aot_defuse_version(cs.evaluator().defuse_version());

    // ── Step 4: Compile via LLVM AOT pipeline ────────────────
    bool compiled =
        aura_emit_native_file(input.c_str(), out_path.c_str(), (const void*)flat_fn_array.data(),
                              static_cast<unsigned int>(flat_fn_array.size()));
    // Issue #62 Iter 1: AOT counters
    if (compiled) {
        cs.metrics().aot_emits.fetch_add(1, std::memory_order_relaxed);
    } else {
        cs.metrics().aot_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Step 4: Failure handling ──────────────────────────────
    // Issue #237: distinguish two failure modes:
    //
    //   (A) flat_fn_array is non-empty AND AOT failed
    //       → real AOT compile/link error (e.g. x86_64 PIE
    //         mismatch, missing runtime.c, LLVM internal error).
    //       The caller asked for a real native binary; giving
    //       them a shell wrapper script that re-invokes aura
    //       is not a native binary and breaks downstream
    //       tooling (e.g. test_issue_237's is_elf() check).
    //       → Return rc=1 with a clear error message.
    //
    //   (B) flat_fn_array is empty (all functions were
    //       filtered out as Evolution-region) AND the C
    //       wrapper fallback also failed
    //       → no AOT was possible, C wrapper also failed.
    //       Shell-wrapper fallback is the last resort for
    //       this case so the user still gets a runnable
    //       script (rare — requires no cc + no Evolution-
    //       AOT to be possible).
    if (!compiled) {
        // Case A: real AOT failure with functions to AOT-compile.
        // Bail out loudly with the LLVM/bridge stderr above.
        if (!flat_fn_array.empty()) {
            std::println(std::cerr,
                         "AOT: failed to emit native binary for {} function(s). "
                         "See AOT: errors above. Common causes: x86_64 PIE/PIC "
                         "mismatch, missing lib/runtime.c, LLVM internal error. "
                         "Not falling back to shell wrapper (--emit-binary "
                         "contract is a real ELF executable).",
                         flat_fn_array.size());
            return 1;
        }

        // Case B: empty AOT batch + C wrapper failure.
        // Fall back to shell wrapper as a last-resort script.
        std::string self_path = std::string(argv[0]);
        // Issue #237: log every step of the shell-wrapper fallback
        // so CI can see exactly where the failure mode hits. The
        // previous version silently fell through to rc=1 when the
        // LLVM AOT + C-fallback both failed; this verbose trace
        // makes the failure mode visible.
        std::println(std::cerr, "[#237] AOT failed; entering shell wrapper fallback");
        std::println(std::cerr, "[#237]   out_path={}", out_path);
        std::println(std::cerr, "[#237]   self_path={}", self_path);
        {
            std::ofstream sf(out_path);
            if (!sf) {
                std::println(std::cerr, "[#237] ERROR: cannot open {} for write (rc=1)", out_path);
                return 1;
            }
            sf << "#!/bin/bash\n";
            sf << "# Aura compiled binary (shell wrapper fallback)\n";
            sf << "exec " << self_path << " --load " << out_path << ".tmp.aura \"$@\"\n";
            if (!sf) {
                std::println(std::cerr, "[#237] ERROR: write to {} failed (rc=1)", out_path);
                return 1;
            }
        }
        std::filesystem::permissions(
            out_path, std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                          std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                          std::filesystem::perms::others_exec);
        // Save source for --load
        {
            std::ofstream sf(out_path + ".tmp.aura");
            if (!sf) {
                std::println(std::cerr, "[#237] ERROR: cannot open {}.tmp.aura for write (rc=1)",
                             out_path);
                return 1;
            }
            sf << input;
        }
        if (std::filesystem::exists(out_path)) {
            std::println("emitted binary: {} (shell wrapper)", out_path);
            return 0;
        }
        std::println(std::cerr, "[#237] ERROR: out_path {} does not exist after write (rc=1)",
                     out_path);
        std::println(std::cerr, "error: cannot create binary");
        return 1;
    }

    std::println("emitted binary: {}", out_path);
    return 0;
}

static void print_denseness_usage(const char*, std::ostream& out = std::cerr) {
    const char* prog = "aura"; // #4935: never the libexec path
    std::println(out, "usage: {} [options] [file.aura …]", prog);
    std::println(out, "       {} -e '(+ 1 2)'", prog);
    std::println(out, "       {} soft [--allow ffi,write] [--load] [file.aura …]", prog);
    std::println(out, "       echo '(+ 1 2)' | {}", prog);
    std::println(out, "       {} < file.aura", prog);
    std::println(out, "");
    std::println(out, "Projects (#4913):");
    std::println(out,
                 "  {} new [app|evolve|evolve-moo|agent] <dir>  scaffold aura.toml, "
                 "tests/, CI, README",
                 prog);
    std::println(out,
                 "  {} run [entry.aura] [options]    run the project entry with its "
                 "aura.toml preset",
                 prog);
    std::println(out,
                 "  {} test [--update] [tests/x.aura] run tests/*.aura against "
                 "tests/*.golden",
                 prog);
    std::println(out,
                 "  {} demo [evolve|agent]            run the signature example in a "
                 "temporary project",
                 prog);
    std::println(out, "  (require \"lib/x\") in a project resolves from the project root.");
    std::println(out, "Evolution (#4910):");
    std::println(out,
                 "  {} evo log [--json] [--front] [--lineage ID]  worldline: KEEP/ROLLBACK, "
                 "reason, evidence, diff",
                 prog);
    std::println(out, "  {} evo report --html DIR  Pareto/HV/QD/lineage SVG + score curve + diffs",
                 prog);
    std::println(out, "REPL (#4911):");
    std::println(out,
                 "  {} repl [file.aura]   REPL; :load/:reload FILE, :ws, :worldline, "
                 ":snapshot, :restore ID, :help",
                 prog);
    std::println(out,
                 "  {} lsp                stdio language server (diagnostics, "
                 "goto-definition); see docs/editor.md",
                 prog);
    std::println(out, "Options:");
    std::println(out, "  -e, --eval EXPR     evaluate EXPR (repeatable)");
    std::println(out, "  --load FILE         run FILE (same semantics as '{} FILE')", prog);
    std::println(out, "  --keep-going        keep running forms after a top-level error");
    std::println(out, "  --preset P, --grant CAPS, --deny CAPS, --quiet   capabilities "
                      "(below)");
    std::println(out, "  --no-arena, --worker-threads N, --health-port N");
    std::println(out, "Tools (read code from the argument or stdin):");
    std::println(out, "  --fmt [--check] | --unparse   format; --check exits 1 when not "
                      "formatted");
    std::println(out, "  --typecheck | --auto-fix      type diagnostics / suggested fixes");
    std::println(out, "  --query Q | --query-and-fix   structural queries over the code");
    std::println(out, "  --evo-explain                 explain an evolution step (AURA_OBS=1)");
    std::println(out, "  --inspect [ir|…], --ir, --jit, --jit-session   compiler views");
    std::println(out, "  --env, --env-json             runtime environment report");
    std::println(out, "  --freeze, --emit-binary, --cache, --cache-open, --write   "
                      "artifacts");
    std::println(out, "  --serve, --serve-async, --health-server, --strategy, --hot-swap  "
                      "services");
    std::println(out, "");
    std::println(out, "Programs are read from stdin when no file/-e is given.");
    std::println(out, "A program file stops at its first top-level error (rc=1);");
    std::println(out, "  --keep-going (or AURA_KEEP_GOING=1) runs the remaining forms too.");
    std::println(out, "Capability presets (#4912):");
    std::println(out,
                 "  {} --preset dev file.aura   mutate + read + write (self-evolving "
                 "programs)",
                 prog);
    std::println(out,
                 "  {} --preset ci  file.aura   same as dev (write not yet "
                 "path-scoped)",
                 prog);
    std::println(out, "  {} --preset prod file.aura  the Restricted default", prog);
    std::println(out, "  --grant mutate,write,ffi,exec,network / --deny … adjust it; "
                      "--quiet drops the caps line.");
    std::println(out, "  Also AURA_PRESET=dev or preset = \"dev\" in aura.toml.");
    std::println(out, "Soft experiments (#4575):");
    std::println(out, "  {} soft file.aura              Restricted Mutate on this workspace", prog);
    std::println(out, "  {} soft --allow ffi,write      explicit opt-in; never implied", prog);
    std::println(out, "Denseness / span host env (soft multi-module probes):");
    std::println(out, "  export AURA_BIN=…/build/aura   absolute path (#2772 multi-process)");
    std::println(out, "  export AURA_PATH=…/lib[:extra] module search path");
    std::println(out, "  export AURA_PIPELINE_STRICT=0  allow tree-walker fallback (#2213)");
    std::println(out, "Multi-process: shell children inherit only exported vars.");
    std::println(out, "  Prefer: (shell (string-append (getenv \"AURA_BIN\") \" …\"))");
    std::println(out, "  Or:     (shell (string-append (aura-executable-path) \" …\"))");
    std::println(out, "  (aura seeds AURA_BIN from self when unset — still export in runners)");
    std::println(out, "Special modes: --serve, --serve-async, --ir, --jit, --help");
}

// force-soa oneshot face. -1 means this process is not in that mode and the
// caller should keep the single cs.eval path.
static int eval_oneshot_force_soa(aura::compiler::CompilerService& cs, const std::string& all_input,
                                  bool stop_first_4909) {
    {
        const char* strict_env2 = std::getenv("AURA_PIPELINE_STRICT");
        const std::string_view strict_v2 =
            strict_env2 ? std::string_view(strict_env2) : std::string_view{};
        if (strict_v2 == "force-soa" || strict_v2 == "force_soa" || strict_v2 == "soa") {
            // Issue #4865: same face as --load force-soa. A value define the
            // IR path cannot lower (fiber:spawn / agent forms) must still run
            // with correct semantics; block-forcing it would silently drop
            // the body instead of executing it.
            // Issue #4892 (string_ref_walk_4320 bench regression): Allow only for value defines
            // (set per form below); everything else keeps ForceSoa so loops
            // stay on the IR instead of the tree-walker.
            aura::ast::ASTArena parse_arena;
            auto parse_alloc = parse_arena.allocator();
            aura::ast::StringPool parse_pool(parse_alloc);
            aura::ast::FlatAST parse_flat(parse_alloc);
            auto pr = aura::parser::parse_to_flat(all_input, parse_flat, parse_pool);
            if (!pr.success || pr.root == aura::ast::NULL_NODE) {
                std::println(std::cerr, "error: {}", pr.error.empty() ? "parse error" : pr.error);
                return 1;
            }
            std::string last_out;
            bool last_closure_4908 = false; // Issue #4908: see below
            bool any_error = false;
            std::vector<std::string> user_macros_4893;
            for (auto& tf : split_top_level_forms_4865(all_input)) {
                const std::string& form = tf.text;
                bool uses_user_macro = false;
                for (const auto& m : user_macros_4893) {
                    if (form_mentions_symbol_4893(form, m)) {
                        uses_user_macro = true;
                        break;
                    }
                }
                if (auto mname = user_macro_def_name_4893(form); !mname.empty())
                    user_macros_4893.push_back(std::move(mname));
                const bool value_define = is_value_define_form_4892(form) || uses_user_macro;
                // Issue #4893: a macro / record / syntax definition registers in
                // the evaluator's expander tables, which the IR does not lower.
                // Under ForceSoa the walker escape is blocked and the
                // registration was silently dropped (later forms: `cannot
                // call` / `unbound variable`). Give every definitional form
                // other than a plain procedure define the #4865 Allow face.
                const bool special_define =
                    form_head_is_definitional_4893(form) && !form.starts_with("(define ") &&
                    !form.starts_with("(define\n") && !form.starts_with("(define\t") &&
                    !form.starts_with("(begin");
                aura::compiler::set_tree_walker_fallback_policy(
                    value_define || special_define
                        ? aura::compiler::TreeWalkerFallbackPolicy::Allow
                        : aura::compiler::TreeWalkerFallbackPolicy::ForceSoa);
                // Issue #4892 (string_ref_walk_4320 bench regression): a lone top-level form whose
                // root is a primitive call — `(display ...)`, `(while ...)` —
                // takes the #2213 registered-prim-root walker shortcut on
                // every face, so `(while (< i n) ...)` ran in the tree-walker
                // (the old whole-program eval had a Begin root and never hit
                // it). Evaluate non-define list forms as a one-child Begin so
                // they get the same sequential IR routing as before #4865.
                // Forms that build closures or touch fibers / agents keep the
                // exact #4865 face (their bodies must see live top-level
                // cells; the walker shortcut is what gives them that).
                const bool wrap_begin = !value_define && !form.empty() && form[0] == '(' &&
                                        !form_head_is_definitional_4893(form) &&
                                        !form_builds_closure_or_task_4892(form);
                constexpr std::string_view kBeginPrefix4892 = "(begin ";
                auto fr = cs.eval(positioned_form_4908(
                    tf.line, tf.column, wrap_begin ? kBeginPrefix4892 : std::string_view{}, form,
                    wrap_begin ? std::string_view{")"} : std::string_view{}));
                bool loc_absolute = true; // Issue #4908: positioned eval
                // Issue #4896: a Restricted dispatch deny that came back as
                // the form's error VALUE (top-level `(set-car! q 5)`) was
                // dropped here as an ordinary result: no diagnostic, rc=0.
                // The whole-program face raises it; do the same.
                // Issue #4899 follow-up: the same holds for EVERY error value
                // a top-level form returns. The whole-program face turns a
                // statement's error value into a located diagnostic (walker
                // #4465, IR entry #4899); the per-form face only did so for
                // effect-denied, so `(define x (car 5))` (the IR value-define
                // bind returns the cell value) printed nothing and exited 0.
                if (fr && aura::compiler::types::is_error(*fr) &&
                    !aura::compiler::types::is_string(*fr)) {
                    auto msg = cs.evaluator().soft_error_message(*fr);
                    aura::diag::Diagnostic d{cs.evaluator().error_value_kind(*fr), std::move(msg)};
                    // Issue #4909: a refusal without a recorded producer
                    // still gets the form's location (it printed none).
                    // Issue #4916: a producer inside a module keeps its file.
                    // Issue #4908: the positioned eval made a recorded
                    // producer location file-absolute (an earlier define's
                    // body included), so keep it on every face.
                    const auto el = cs.evaluator().error_location(*fr);
                    if (recorded_loc_4908(el))
                        d.location = *el;
                    else {
                        d.location = per_form_error_value_location_4899(form);
                        loc_absolute = false;
                    }
                    fr = std::unexpected(std::move(d));
                }
                if (!fr && wrap_begin) {
                    // Map the location back onto the unwrapped form text.
                    auto& loc = fr.error().location;
                    const std::uint32_t first_col = tf.column + kBeginPrefix4892.size();
                    if (loc_absolute && loc.file_id == 0 && loc.line == tf.line &&
                        loc.column >= first_col)
                        loc.column -= static_cast<std::uint32_t>(kBeginPrefix4892.size());
                }
                if (!fr) {
                    // Issue #4460/#4467: a failing top-level form is a located
                    // diagnostic, not the end of the file — the whole-program
                    // face prints it once, keeps running the forms after it,
                    // and exits 1. Keep that contract on the form-by-form
                    // force-soa face too (otherwise "after" is never printed
                    // and later forms' arity diagnostics are lost).
                    // Issue #4875: map the form-relative position back onto
                    // the real file coordinates so the diagnostic points at
                    // the actual line/column (e.g. after a block comment).
                    auto& loc = fr.error().location;
                    if (!loc_absolute && loc.valid() && loc.file_id == 0) {
                        if (loc.line == 1 && loc.column > 0 && tf.column > 1)
                            loc.column += tf.column - 1;
                        loc.line += tf.line - 1;
                    }
                    std::string ctx(static_cast<std::size_t>(tf.line - 1), '\n');
                    ctx.append(static_cast<std::size_t>(tf.column - 1), ' ');
                    ctx += form;
                    std::println(std::cerr, "error: {}", fr.error().format_with_source(ctx));
                    print_module_backtrace_4916(loc, tf.line, tf.column, form); // #4916
                    any_error = true;
                    // Issue #4876: a refused user macro is fatal, as on the
                    // whole-program face. Running on printed a cascade of
                    // `unbound variable: <macro>` / `cannot call` for every
                    // later use of the macro the deny never registered.
                    if (fr.error().message.find("macro-self-evo not granted") != std::string::npos)
                        break;
                    if (stop_first_4909) { // Issue #4909: first error stops a file
                        // The value of the form before the error is not the
                        // program's value: print nothing (the default face).
                        last_out.clear();
                        last_closure_4908 = false;
                        break;
                    }
                    continue;
                }
                // Mirror the whole-program face: only the LAST top-level
                // form's value is printed, and a void last form prints
                // nothing (an earlier non-void form must not leak out).
                // Issue #4908: format_value warns "program returned an
                // uncalled function" as it formats a closure, so formatting
                // every form's value warned for an intermediate
                // (eval-current) / lambda value the whole-program face never
                // prints. Defer a top-level closure to the end.
                last_closure_4908 = !is_void(*fr) && aura::compiler::types::is_closure(*fr);
                last_out = is_void(*fr) || last_closure_4908 ? std::string{} : fmt_val(*fr, cs);
                if (value_define || special_define)
                    cs.sync_soft_export_cells_for_ir();
            }
            if (last_closure_4908) {
                std::println(std::cerr, "⚠ program returned an uncalled function");
                last_out = "#<procedure>";
            }
            if (!last_out.empty())
                std::println("{}", last_out);
            return (any_error || aura_displayed_error_count() > 0) ? 1 : 0;
        }
    }
    return -1;
}

struct OneshotLaunch {
    int status = -1;
    std::string all_input;
    bool source_from_argv = false;
    bool stop_first_4909 = false;
    bool soft_launch = false;
    bool soft_explicit_4912 = false;
    std::uint16_t soft_extra = 0;
    std::string preset_4912;
    bool quiet_4912 = false;
    bool grant_seen_4912 = false;
    std::uint16_t deny_bits_4912 = 0;
    std::string soft_allow_list;
};

static int fold_oneshot_capabilities(
    OneshotLaunch& out, std::string& all_input, bool source_from_argv, bool from_file_4909,
    bool keep_going_4909, bool& soft_launch, std::uint16_t& soft_extra, std::string& preset_4912,
    bool quiet_4912, bool grant_seen_4912, std::uint16_t deny_bits_4912, bool deny_mutate_4912,
    bool preset_bad_4912, bool soft_allow_bad, const std::string& soft_allow_bad_tok,
    std::vector<std::string>& write_dirs_4912, bool& write_unscoped_4912,
    std::string& entry_path_4912, std::string& soft_allow_list) {
    const bool stop_first_4909 = from_file_4909 && !keep_going_4909;
    if (preset_bad_4912) {
        std::println(std::cerr, "error: unknown --preset '{}'", preset_4912);
        std::println(std::cerr, "hint: --preset dev | ci | prod");
        out.status = 1;
        return 1;
    }
    if (soft_allow_bad) {
        std::println(std::cerr, "error: unknown capability token '{}'", soft_allow_bad_tok);
        if (soft_allow_bad_tok == "macro-self-evo" || soft_allow_bad_tok == "tenant-admin" ||
            soft_allow_bad_tok == "syscall")
            std::println(std::cerr,
                         "hint: {} is never granted from the command line (it stays a "
                         "host-side grant; #4912)",
                         soft_allow_bad_tok);
        else
            std::println(std::cerr, "hint: mutate, read, write, ffi, exec|shell, network|net");
        std::println(std::cerr, "hint: 'aura --help' for usage");
        out.status = 1;
        return 1;
    }
    // Issue #4912: resolve the preset (flag > AURA_PRESET > ./aura.toml or
    // the entry file's aura.toml) and fold it into the soft session.
    // Issue #4911: `aura repl FILE` reads FILE's aura.toml preset; the REPL is
    // a development face, so it defaults to dev (:reload needs mutate).
    if (g_repl_4911 && entry_path_4912.empty())
        entry_path_4912 = g_repl_file_4911;
    if (preset_4912.empty()) {
        if (const char* ev = std::getenv("AURA_PRESET"); ev && *ev)
            preset_4912 = ev;
        else
            preset_4912 = read_toml_preset_4912(entry_path_4912);
        if (preset_4912.empty() && g_repl_4911)
            preset_4912 = "dev";
        if (!preset_4912.empty() && preset_4912 != "dev" && preset_4912 != "ci" &&
            preset_4912 != "prod") {
            std::println(std::cerr, "error: unknown preset '{}' (AURA_PRESET / aura.toml)",
                         preset_4912);
            std::println(std::cerr, "hint: dev | ci | prod");
            out.status = 1;
            return 1;
        }
    }
    if (preset_4912 == "prod" && grant_seen_4912) {
        std::println(std::cerr, "error: --preset prod takes no --grant (it is the Restricted "
                                "default)");
        out.status = 1;
        return 1;
    }
    const bool soft_explicit_4912 = soft_launch;
    if (preset_4912 == "dev" || preset_4912 == "ci") {
        soft_launch = true;
        soft_extra = static_cast<std::uint16_t>(
            soft_extra | static_cast<std::uint16_t>(aura::core::capability::Effect::Write));
    }
    // Issue #4912: ci = host paths read-only, Write only under the project's
    // out directory (aura.toml `out`, default out/; `aura run` runs from the
    // project root). dev / a bare `--grant write` keep Write on every path.
    if (preset_4912 == "ci") {
        auto out = read_toml_key_4913("aura.toml", "out");
        write_dirs_4912.push_back(out.empty() ? std::string("out") : out);
    }
    if (preset_4912 == "dev")
        write_unscoped_4912 = true;
    if (!write_unscoped_4912 && !write_dirs_4912.empty())
        aura::compiler::security::set_write_scope(write_dirs_4912);
    if (grant_seen_4912 && preset_4912 != "prod")
        soft_launch = true;
    soft_extra = static_cast<std::uint16_t>(soft_extra & ~deny_bits_4912);
    if (deny_mutate_4912)
        soft_launch = false;
    if (!soft_launch && !preset_4912.empty() && !quiet_4912)
        std::println(std::cerr, "aura: preset={} sandbox=restricted caps=read (default)",
                     preset_4912);

    out.all_input = std::move(all_input);
    out.source_from_argv = source_from_argv;
    out.stop_first_4909 = stop_first_4909;
    out.soft_launch = soft_launch;
    out.soft_explicit_4912 = soft_explicit_4912;
    out.soft_extra = soft_extra;
    out.preset_4912 = std::move(preset_4912);
    out.quiet_4912 = quiet_4912;
    out.grant_seen_4912 = grant_seen_4912;
    out.deny_bits_4912 = deny_bits_4912;
    out.soft_allow_list = std::move(soft_allow_list);
    return 0;
}

static OneshotLaunch parse_oneshot_launch(int argc, char** argv) {
    OneshotLaunch out;
    // Issue #2767: collect program source from -e / --eval / file args
    // before falling through to REPL or pipe mode. Leave special flags
    // (--serve, --ir, …) to earlier branches (they return).
    std::string all_input;
    bool source_from_argv = false;
    // Issue #4909: a program file stops at its first uncaught top-level
    // error (one diagnostic, rc=1) instead of running the rest of the file
    // into a cascade of follow-on errors. --keep-going / AURA_KEEP_GOING=1
    // restores the run-every-form contract (#4460/#4467). -e / stdin keep it.
    bool from_file_4909 = false;
    bool keep_going_4909 = [] {
        const char* v = std::getenv("AURA_KEEP_GOING");
        if (!v)
            return false;
        const std::string_view s(v);
        return s == "1" || s == "true" || s == "yes";
    }();
    // Issue #4575: `aura soft` is the launcher. It is not an env var.
    // `--allow` is recognized only after `soft`. Plain `aura --allow`
    // stays an unknown option.
    bool soft_launch = false;
    std::uint16_t soft_extra = 0;
    std::string soft_allow_list;
    bool soft_allow_bad = false;
    std::string soft_allow_bad_tok;
    // Issue #4912: `write:<dir>/` grants Write under <dir> only; a bare
    // `write` (or the dev preset) is every path and wins over the scope.
    std::vector<std::string> write_dirs_4912;
    bool write_unscoped_4912 = false;
    auto take_allow = [&](std::string_view list) {
        using aura::core::capability::Effect;
        std::size_t p = 0;
        while (p < list.size()) {
            while (p < list.size() && (list[p] == ' ' || list[p] == ','))
                ++p;
            if (p >= list.size())
                break;
            auto j = p;
            while (j < list.size() && list[j] != ',')
                ++j;
            auto tok = list.substr(p, j - p);
            while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
                tok.remove_suffix(1);
            p = j < list.size() ? j + 1 : j;
            if (tok.empty())
                continue;
            if (tok == "mutate") // Issue #4912: always part of a soft/preset session
                continue;
            std::uint16_t bit = 0;
            if (tok == "ffi")
                bit = static_cast<std::uint16_t>(Effect::Ffi);
            else if (tok == "write") {
                bit = static_cast<std::uint16_t>(Effect::Write);
                write_unscoped_4912 = true;
            } else if (tok.starts_with("write:") && tok.size() > 6) {
                bit = static_cast<std::uint16_t>(Effect::Write);
                write_dirs_4912.emplace_back(tok.substr(6));
            } else if (tok == "exec" || tok == "shell")
                bit = static_cast<std::uint16_t>(Effect::Exec);
            else if (tok == "network" || tok == "net")
                bit = static_cast<std::uint16_t>(Effect::Network);
            else if (tok == "read")
                bit = static_cast<std::uint16_t>(Effect::Read);
            else {
                // mutate is the default, not an opt-in. tenant-admin,
                // syscall, macro-self-evo, and unknown tokens fail closed.
                soft_allow_bad = true;
                soft_allow_bad_tok = std::string(tok);
                return;
            }
            soft_extra = static_cast<std::uint16_t>(soft_extra | bit);
            if (!soft_allow_list.empty())
                soft_allow_list.push_back(',');
            soft_allow_list.append(tok);
        }
    };
    // Issue #4912: capability presets. `--preset dev|ci|prod` (or
    // AURA_PRESET, or `preset = "..."` in ./aura.toml) picks the session;
    // `--grant` / `--deny` adjust it; `--quiet` drops the caps banner.
    std::string preset_4912;
    bool preset_bad_4912 = false;
    bool quiet_4912 = false;
    bool grant_seen_4912 = false;
    bool deny_mutate_4912 = false;
    std::uint16_t deny_bits_4912 = 0;
    std::string entry_path_4912;
    auto take_deny_4912 = [&](std::string_view list) {
        using aura::core::capability::Effect;
        std::size_t p = 0;
        while (p < list.size()) {
            auto j = list.find(',', p);
            if (j == std::string_view::npos)
                j = list.size();
            auto tok = list.substr(p, j - p);
            p = j + 1;
            while (!tok.empty() && tok.front() == ' ')
                tok.remove_prefix(1);
            while (!tok.empty() && tok.back() == ' ')
                tok.remove_suffix(1);
            if (tok.empty())
                continue;
            if (tok == "mutate")
                deny_mutate_4912 = true;
            else if (tok == "ffi")
                deny_bits_4912 |= static_cast<std::uint16_t>(Effect::Ffi);
            else if (tok == "write")
                deny_bits_4912 |= static_cast<std::uint16_t>(Effect::Write);
            else if (tok == "exec" || tok == "shell")
                deny_bits_4912 |= static_cast<std::uint16_t>(Effect::Exec);
            else if (tok == "network" || tok == "net")
                deny_bits_4912 |= static_cast<std::uint16_t>(Effect::Network);
            else {
                soft_allow_bad = true;
                soft_allow_bad_tok = std::string(tok);
                return;
            }
        }
    };
    auto flag_value_4912 = [&](int& i, std::string_view a, std::string_view name,
                               std::string_view& out) -> bool {
        if (a == name) {
            if (i + 1 >= argc)
                return false;
            out = argv[++i];
            return true;
        }
        out = a.substr(name.size() + 1);
        return true;
    };
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::string_view a(argv[i]);
            if (a == "--quiet") { // Issue #4912
                quiet_4912 = true;
                continue;
            }
            if (a == "--preset" || a.starts_with("--preset=")) { // Issue #4912
                std::string_view v;
                if (!flag_value_4912(i, a, "--preset", v) ||
                    (v != "dev" && v != "ci" && v != "prod")) {
                    preset_bad_4912 = true;
                    preset_4912 = std::string(v);
                    break;
                }
                preset_4912 = std::string(v);
                continue;
            }
            if (a == "--grant" || a.starts_with("--grant=")) { // Issue #4912
                std::string_view v;
                if (!flag_value_4912(i, a, "--grant", v)) {
                    soft_allow_bad = true;
                    soft_allow_bad_tok = "--grant";
                    break;
                }
                grant_seen_4912 = true;
                take_allow(v);
                if (soft_allow_bad)
                    break;
                continue;
            }
            if (a == "--deny" || a.starts_with("--deny=")) { // Issue #4912
                std::string_view v;
                if (!flag_value_4912(i, a, "--deny", v)) {
                    soft_allow_bad = true;
                    soft_allow_bad_tok = "--deny";
                    break;
                }
                take_deny_4912(v);
                if (soft_allow_bad)
                    break;
                continue;
            }
            if (a == "-h" || a == "--help" || a == "--help-all") {
                print_denseness_usage(argv[0], std::cout);
                out.status = 0;
                return out;
            }
            if (a == "-e" || a == "--eval") {
                if (i + 1 >= argc) {
                    std::println(std::cerr, "error: {} requires an expression argument", a);
                    std::println(std::cerr, "hint: 'aura --help' for usage");
                    out.status = 1;
                    return out;
                }
                if (!all_input.empty())
                    all_input += '\n';
                all_input += argv[++i];
                source_from_argv = true;
                continue;
            }
            // Already consumed in the pre-parse loop (sets g_use_arena=false).
            if (a == "--no-arena")
                continue;
            if (a == "--keep-going") { // Issue #4909
                keep_going_4909 = true;
                continue;
            }
            // Health / worker flags may appear before denseness source args.
            if (a == "--worker-threads") {
                if (i + 1 < argc)
                    ++i; // skip value
                continue;
            }
            if (a.starts_with("--worker-threads="))
                continue;
            // #5063: --load is a normal option. The dedicated path handles
            // `aura --load FILE` (argv[1]). Flags before it used to make the
            // oneshot parser call --load unknown.
            if (a == "--load") {
                if (i + 1 >= argc) {
                    std::println(std::cerr, "error: --load requires a file argument");
                    std::println(std::cerr, "hint: 'aura --help' for usage");
                    out.status = 1;
                    return out;
                }
                ++i;
                a = argv[i];
                // Fall through to the bare-path reader below.
            } else if (a == "--health-port") {
                if (i + 1 < argc)
                    ++i;
                continue;
            }
            if (a.starts_with("--health-port="))
                continue;
            // Issue #4575: first bare arg `soft` is the subcommand.
            // `aura ./soft` remains a file. `--allow` is only valid here.
            if (a == "soft" && i == 1 && !soft_launch) {
                soft_launch = true;
                continue;
            }
            if (soft_launch && (a == "--allow" || a.starts_with("--allow="))) {
                std::string_view list;
                if (a == "--allow") {
                    if (i + 1 >= argc) {
                        soft_allow_bad = true;
                        soft_allow_bad_tok = "--allow";
                        break;
                    }
                    list = argv[++i];
                } else {
                    list = a.substr(std::string_view("--allow=").size());
                }
                take_allow(list);
                if (soft_allow_bad)
                    break;
                continue;
            }
            if (!a.empty() && a[0] == '-') {
                std::println(std::cerr, "error: unknown option '{}'", a);
                std::println(std::cerr, "hint: 'aura --help' for usage");
                out.status = 1;
                return out;
            }
            // Bare path: read file contents (denseness sugar for `./aura < file`).
            std::ifstream f(argv[i]);
            if (!f) {
                std::println(std::cerr, "error: cannot open program file '{}'", argv[i]);
                std::println(std::cerr, "hint: programs default to stdin — try: aura < {}",
                             argv[i]);
                std::println(std::cerr, "hint: 'aura --help' for usage");
                out.status = 1;
                return out;
            }
            if (!all_input.empty())
                all_input += '\n';
            std::string file_text((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
            // Issue #4916: exactly one program file is the entry file.
            if (!source_from_argv) {
                register_entry_file_4916(argv[i], file_text);
                entry_path_4912 = argv[i];
            } else
                aura::diag::set_entry_source_file(0);
            all_input += file_text;
            source_from_argv = true;
            from_file_4909 = true;
        }
    }
    if (fold_oneshot_capabilities(out, all_input, source_from_argv, from_file_4909, keep_going_4909,
                                  soft_launch, soft_extra, preset_4912, quiet_4912, grant_seen_4912,
                                  deny_bits_4912, deny_mutate_4912, preset_bad_4912, soft_allow_bad,
                                  soft_allow_bad_tok, write_dirs_4912, write_unscoped_4912,
                                  entry_path_4912, soft_allow_list) != 0)
        return out;
    return out;
}

static std::optional<std::vector<std::string>> split_oneshot_forms(const std::string& all_input) {
    // Split into balanced S-expressions by tracking paren balance
    // Also track string literals to avoid counting parens inside strings
    std::vector<std::string> exprs;
    std::string current;
    int depth = 0;
    bool in_string = false;

    for (std::size_t i = 0; i < all_input.size(); ++i) {
        auto c = all_input[i];

        if (in_string) {
            current += c;
            if (c == '\\' && i + 1 < all_input.size()) {
                // Skip escaped character
                ++i;
                current += all_input[i];
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        // Issue #4622: a #\x character literal is one atomic token, so a
        // delimiter inside it (#\( #\) #\" #\; #\[ #\]) is not a delimiter
        // here either -- skip the whole literal instead of counting it.
        if (c == '#') {
            const std::size_t lit = character_literal_length(all_input, i);
            if (lit > 0) {
                current.append(all_input, i, lit);
                i += lit - 1;
                continue;
            }
        }

        // Issue #4875: a (nested) #| block comment |# is not code: a ( or "
        // inside it must not move the paren depth. Keep it verbatim inside
        // a form (the reader skips it there); between forms it is just a
        // separator. An unterminated one stays in the chunk so the reader
        // reports it at its own position and nothing after it runs.
        if (c == '#' && i + 1 < all_input.size() && all_input[i + 1] == '|') {
            std::size_t j = i + 2;
            int nest = 1;
            while (j < all_input.size() && nest > 0) {
                if (all_input[j] == '#' && j + 1 < all_input.size() && all_input[j + 1] == '|') {
                    j += 2;
                    ++nest;
                } else if (all_input[j] == '|' && j + 1 < all_input.size() &&
                           all_input[j + 1] == '#') {
                    j += 2;
                    --nest;
                } else {
                    ++j;
                }
            }
            if (nest > 0 && depth == 0) {
                // Between forms: the reader reports it at its own position
                // after the complete prefix ran (#4875 direct contract).
                current.append(all_input, i, std::string::npos);
                break;
            }
            if (nest > 0) {
                // Inside a form: same contract as an unclosed list (#4866),
                // but name the real cause instead of a missing ')'.
                std::size_t line = 1, col = 1;
                for (std::size_t k = 0; k < i; ++k) {
                    if (all_input[k] == '\n') {
                        ++line;
                        col = 1;
                    } else {
                        ++col;
                    }
                }
                std::println(std::cerr,
                             "error: {}:{}: parse error: unterminated block comment '#|'", line,
                             col);
                return std::nullopt;
            }
            if (depth > 0)
                current.append(all_input, i, j - i);
            else
                current += ' ';
            i = j - 1;
            continue;
        }

        if (c == ';') {
            // Line comment: skip to end of line
            if (depth > 0)
                current += ';';
            while (i + 1 < all_input.size() && all_input[i + 1] != '\n') {
                ++i;
            }
            continue;
        }

        if (c == '"') {
            current += c;
            in_string = true;
            continue;
        }

        if (c == '(' || c == '[') {
            if (depth == 0 && !current.empty()) {
                // Prefix characters (' ` ,) before ( form a single expression
                // e.g. '(+ 1 2), `(1 ,x 3), ,(+ 1 2)
                bool is_prefix = false;
                for (auto pc : current) {
                    if (pc == '\'' || pc == '`' || pc == ',') {
                        is_prefix = true;
                        break;
                    }
                    if (!std::isspace(static_cast<unsigned char>(pc))) {
                        is_prefix = false;
                        break;
                    }
                }
                if (!is_prefix) {
                    // Start of a new expression while we had non-whitespace leftover?
                    // Push current as complete
                    auto trimmed = current;
                    auto pos = trimmed.find_first_not_of(" \t\r\n");
                    if (pos != std::string::npos) {
                        trimmed = trimmed.substr(pos);
                        auto end = trimmed.find_last_not_of(" \t\r\n");
                        if (end != std::string::npos)
                            trimmed = trimmed.substr(0, end + 1);
                        exprs.push_back(trimmed);
                    }
                    current.clear();
                }
            }
            current += c;
            ++depth;
            continue;
        }

        if ((c == ')' || c == ']') && depth == 0) {
            // Issue #3917: extra close at file/form top level is a hard
            // read error. Do not evaluate a already-split prefix — a
            // truncated named-let would run with rewritten control flow
            // (n becomes (1)/(), infinite back-off).
            std::size_t line = 1, col = 1;
            for (std::size_t k = 0; k < i; ++k) {
                if (all_input[k] == '\n') {
                    ++line;
                    col = 1;
                } else {
                    ++col;
                }
            }
            std::println(std::cerr, "error: {}:{}: parse error: unexpected '{}'", line, col, c);
            return std::nullopt;
        }
        if ((c == ')' || c == ']') && depth > 0) {
            current += c;
            --depth;
            if (depth == 0) {
                // Complete expression found
                auto trimmed = current;
                auto pos = trimmed.find_first_not_of(" \t\r\n");
                if (pos != std::string::npos) {
                    trimmed = trimmed.substr(pos);
                    auto end = trimmed.find_last_not_of(" \t\r\n");
                    if (end != std::string::npos)
                        trimmed = trimmed.substr(0, end + 1);
                    exprs.push_back(trimmed);
                }
                current.clear();
            }
            continue;
        }

        current += c;
    }

    // If there's leftover text, try it as a complete expression
    if (!current.empty()) {
        auto trimmed = current;
        auto pos = trimmed.find_first_not_of(" \t\r\n");
        if (pos != std::string::npos) {
            trimmed = trimmed.substr(pos);
            auto end = trimmed.find_last_not_of(" \t\r\n");
            if (end != std::string::npos)
                trimmed = trimmed.substr(0, end + 1);
            if (!trimmed.empty() && depth == 0)
                exprs.push_back(trimmed);
            else if (!trimmed.empty()) {
                // Issue #4866: the direct-run path used to warn and then
                // evaluate the complete prefix, exiting 0. Match --load:
                // a missing ')' is a located parse error and no part of
                // the file runs.
                std::size_t line = 1, col = 1;
                for (char ch : all_input) {
                    if (ch == '\n') {
                        ++line;
                        col = 1;
                    } else {
                        ++col;
                    }
                }
                std::println(
                    std::cerr,
                    "error: {}:{}: parse error: unclosed list: missing ')' before end of file",
                    line, col);
                return std::nullopt;
            }
        }
    }

    if (exprs.empty()) {
        // Issue #2767: no balanced S-expression after split.
        std::println(std::cerr, "error: no complete S-expression in program source");
        std::println(std::cerr, "hint: 'aura --help' for usage");
        return std::nullopt;
    }
    return exprs;
}

static void recover_oneshot_eval_error(aura::compiler::CompilerService& cs,
                                       const std::string& all_input,
                                       std::vector<std::string>& exprs, bool stop_first_4909,
                                       bool& err, const aura::diag::Diagnostic& eval_error) {
    // The single pass already ran every form before the failing one
    // (display, set!, set-code). Re-walking those forms repeats side
    // effects and, after set-code, rebuilds closures so later calls
    // throw invalid closure (Issue #4355).
    //
    // Issue #4471: a runtime error inside a procedure is located at
    // that body, which may be an earlier define. Resume after the
    // top-level form that was executing (file_resume_form), not after
    // the form that contains the diagnostic. Print the first diagnostic
    // once against the whole program. Later forms are evaluated alone,
    // so their locations are shifted back onto all_input. A parse
    // failure happens before any form runs, so the historical
    // per-expression walk still covers it (diag:parse-error-*).
    struct FormAnchor {
        std::size_t offset = std::string::npos;
        std::uint32_t line = 1;
        std::uint32_t col = 1;
    };
    std::vector<FormAnchor> anchors(exprs.size());
    {
        std::size_t search = 0;
        for (std::size_t i = 0; i < exprs.size(); ++i) {
            const auto pos = all_input.find(exprs[i], search);
            if (pos == std::string::npos)
                break;
            std::uint32_t ln = 1;
            std::uint32_t cl = 1;
            for (std::size_t k = 0; k < pos; ++k) {
                if (all_input[k] == '\n') {
                    ++ln;
                    cl = 1;
                } else {
                    ++cl;
                }
            }
            anchors[i] = {pos, ln, cl};
            search = pos + exprs[i].size();
        }
    }
    auto offset_of = [&](aura::diag::SourceLocation loc) -> std::size_t {
        if (!loc.valid())
            return 0;
        std::size_t off = 0;
        std::uint32_t line = 1;
        for (std::size_t k = 0; k < all_input.size() && line < loc.line; ++k) {
            if (all_input[k] == '\n')
                ++line;
            off = k + 1;
        }
        if (loc.column > 0)
            off += static_cast<std::size_t>(loc.column - 1);
        if (off > all_input.size())
            off = all_input.size();
        return off;
    };
    const auto& diag = eval_error;
    // Issue #4876: a capability deny during macro expansion is fatal for
    // the program. Print it once and do not resume form-by-form: the
    // unexpanded call would otherwise report a misleading unbound name.
    const bool macro_deny = aura_hygiene_expand_deny_blocks_eval() != 0 ||
                            diag.message.find("macro-self-evo") != std::string::npos;
    if (macro_deny) {
        std::println(std::cerr, "error: {}", diag.format_with_source(all_input));
        err = true;
    }
    const bool parse_fail = diag.kind == aura::diag::ErrorKind::ParseError ||
                            diag.kind == aura::diag::ErrorKind::UnexpectedToken ||
                            diag.kind == aura::diag::ErrorKind::UnterminatedSExpr;
    std::size_t resume_at = 0;
    bool file_resume = false;
    // Issue #4909: a runtime diagnostic without a location used to
    // fall through to the recovery walk from form 0, so every form
    // before the failure ran TWICE (output printed twice) and the
    // error came out unlocated. The executing top-level form is
    // stamped; when even that is missing, report once and stop.
    // A location inside a required module (file_id != 0) says nothing
    // about which entry form was running, so it cannot pick the resume
    // point either.
    const auto stamped_form = cs.evaluator().file_resume_form();
    const bool entry_loc = diag.location.valid() && diag.location.file_id == 0;
    // Issue #4909: the whole-file eval may run on the tree-walker (a
    // sibling define is not known at gate time), whose C-stack guard
    // stops non-tail recursion at 512 frames. That is a limit of the
    // walker, not of the program: the per-form path runs the same code
    // on the IR (heap frames, AURA_IR_MAX_CALL_DEPTH). This used to be
    // recovered by silently re-running every form (output twice). Now:
    // re-bind the earlier procedure defines (no side effects) on the
    // per-form path and resume AT the failing form, reporting nothing.
    bool walker_depth_resume = false;
    if (!parse_fail && !macro_deny && diag.kind == aura::diag::ErrorKind::InternalError &&
        diag.message.starts_with("recursion depth exceeded") &&
        diag.message.find("fiber=") != std::string::npos && (entry_loc || stamped_form.valid())) {
        const std::size_t err_off = offset_of(stamped_form.valid() ? stamped_form : diag.location);
        std::size_t failing = exprs.size();
        for (std::size_t i = 0; i < exprs.size(); ++i) {
            if (anchors[i].offset == std::string::npos)
                break;
            if (anchors[i].offset <= err_off)
                failing = i;
            else
                break;
        }
        if (failing < exprs.size()) {
            const auto is_proc_define = [](std::string_view f) {
                const auto s = f.find_first_not_of(" \t\r\n");
                if (s == std::string_view::npos)
                    return false;
                f.remove_prefix(s);
                if (f.starts_with("(define ("))
                    return true;
                if (!f.starts_with("(define "))
                    return false;
                f.remove_prefix(8);
                const auto e = f.find_first_of(" \t\r\n()");
                if (e == std::string_view::npos)
                    return false;
                f.remove_prefix(e);
                const auto b = f.find_first_not_of(" \t\r\n");
                return b != std::string_view::npos && f.substr(b).starts_with("(lambda ");
            };
            for (std::size_t i = 0; i < failing; ++i) {
                if (is_proc_define(exprs[i]))
                    (void)cs.eval(exprs[i]);
            }
            resume_at = failing;
            walker_depth_resume = true;
        }
    }
    if (!walker_depth_resume && !parse_fail && !macro_deny && !entry_loc && !stamped_form.valid()) {
        std::println(std::cerr, "error: {}", diag.format_with_source(all_input));
        err = true;
        resume_at = exprs.size();
    }
    if (!walker_depth_resume && !parse_fail && !macro_deny && (entry_loc || stamped_form.valid())) {
        auto resume_loc = diag.location;
        const auto stamped = stamped_form;
        if (stamped.valid())
            resume_loc = stamped;
        const std::size_t err_off = offset_of(resume_loc);
        for (std::size_t i = 0; i < exprs.size(); ++i) {
            if (anchors[i].offset == std::string::npos)
                break;
            if (anchors[i].offset <= err_off)
                resume_at = i + 1;
            else
                break;
        }
        auto shown = diag;
        if (!shown.location.valid())
            shown.location = stamped_form; // Issue #4909: never unlocated
        std::println(std::cerr, "error: {}", shown.format_with_source(all_input));
        if (resume_at > 0 && resume_at <= exprs.size() &&
            anchors[resume_at - 1].offset != std::string::npos) {
            const auto& a = anchors[resume_at - 1];
            print_module_backtrace_4916(
                shown.location, a.line, a.col,
                std::string_view(all_input).substr(a.offset)); // Issue #4916
        }
        err = true;
        file_resume = true;
        if (stop_first_4909) // Issue #4909: first error stops a file
            resume_at = exprs.size();
    }
    for (std::size_t i = resume_at; !macro_deny && i < exprs.size(); ++i) {
        auto& e = exprs[i];
        auto s = e.find_first_not_of(" \t\r\n");
        if (s == std::string::npos)
            continue;
        e = e.substr(s);
        auto r2 = cs.eval(e);
        if (!r2) {
            // Issue #4875: shift a form-relative location onto all_input
            // for parse errors too (the recovery walk runs after a parse
            // failure), not only for file_resume runtime errors.
            if (anchors[i].offset != std::string::npos && r2.error().location.valid()) {
                auto d = r2.error();
                // Line 1 of a trimmed form is shifted by the form's
                // start column. Later lines keep their own columns.
                // Issue #4916: a module location is already in that
                // file's coordinates.
                if (d.location.file_id == 0) {
                    if (d.location.line == 1 && d.location.column > 0 && anchors[i].col > 0)
                        d.location.column += anchors[i].col - 1;
                    d.location.line += anchors[i].line - 1;
                }
                std::println(std::cerr, "error: {}", d.format_with_source(all_input));
                print_module_backtrace_4916(d.location, anchors[i].line, anchors[i].col, e);
            } else {
                std::println(std::cerr, "error: {}", r2.error().format_with_source(e));
            }
            err = true;
            if (stop_first_4909) // Issue #4909: first error stops a file
                break;
            // Issue #4875: a reader/parse error means the source at this
            // point could not be read, so later-looking forms are not a
            // trustworthy suffix (a bare '#' must stop the file). Runtime
            // errors keep the #4460/#4467 resume contract.
            const auto kind = r2.error().kind;
            if (kind == aura::diag::ErrorKind::ParseError ||
                kind == aura::diag::ErrorKind::UnexpectedToken ||
                kind == aura::diag::ErrorKind::UnterminatedSExpr)
                break;
        } else if (i + 1 == exprs.size() && !is_void(*r2))
            std::println("{}", fmt_val(*r2, cs));
    }
}

static int run_oneshot_or_file(int argc, char** argv) {
    // ── Normal REPL / pipe / file / -e mode ─────────────────────
    // Issue #2767: denseness span runners (Aether/Hephaestus/Hermes)
    // hit two footguns that look like product bugs:
    //   1. argv / -e printed bare "usage: echo … | ./aura" and exited
    //   2. AURA_PIPELINE_STRICT production default needs =0 for soft denseness
    // Issue #2772: multi-process child via (shell …) needs *exported*
    // AURA_BIN (or rely on process-seed + (aura-executable-path)).
    // Accept file paths and -e EXPR as sugar; keep stdin pipe default.
    // Issue #4131: an ancestor that ignored SIGTERM (daemonized holder,
    // nohup-style batch runner) leaks SIG_IGN into this image — POSIX
    // carries ignored dispositions across execve, so a hung oneshot
    // (pathological list recursion / while spin) then survives the
    // host's timeout SIGTERM and only killpg(SIGKILL) reclaims it.
    // Oneshot faces reset TERM to default here so `aura file.aura`
    // always honors SIGTERM promptly. --serve/--serve-async return from
    // earlier branches and manage their own lifecycle — untouched.
    ::signal(SIGTERM, SIG_DFL);


    // Issue #5049: a bare `aura` on a TTY is the same interactive face as
    // `aura repl` — one soft session, one std prelude, one preset default.
    // Non-TTY stays the oneshot pipe (#2322), so gate on the terminal.
    if (argc == 1 && ::isatty(STDIN_FILENO))
        g_repl_4911 = true;

    const OneshotLaunch launch = parse_oneshot_launch(argc, argv);
    if (launch.status >= 0)
        return launch.status;
    std::string all_input = std::move(launch.all_input);
    const bool source_from_argv = launch.source_from_argv;
    const bool stop_first_4909 = launch.stop_first_4909;
    bool soft_launch = launch.soft_launch;
    const bool soft_explicit_4912 = launch.soft_explicit_4912;
    const std::uint16_t soft_extra = launch.soft_extra;
    const std::string preset_4912 = std::move(launch.preset_4912);
    const bool quiet_4912 = launch.quiet_4912;
    const bool grant_seen_4912 = launch.grant_seen_4912;
    const std::uint16_t deny_bits_4912 = launch.deny_bits_4912;
    const std::string soft_allow_list = std::move(launch.soft_allow_list);
    aura::compiler::CompilerService cs;
    // Issue #4864: the oneshot file / expression face is the SAME script face
    // as --load (#4848), not the production serve pipeline. The pack default
    // (Forbidden) vetoed ordinary forms the IR path cannot express
    // (let-bound closures, named let, set!, do, try, internal defines,
    // hygienic macros), so `aura file.aura` rejected a program that
    // `aura --load file.aura` ran. Take the same permissive walker face
    // --load takes, exactly as --serve-async-bench does (#4844). Operators
    // can still pin a face explicitly via AURA_PIPELINE_STRICT.
    if (const char* strict = std::getenv("AURA_PIPELINE_STRICT"); !(strict && *strict))
        aura::compiler::set_tree_walker_fallback_policy(
            aura::compiler::TreeWalkerFallbackPolicy::Allow);
    // Issue #4575: AURA_SANDBOX=off stays the dev hatch and says so once.
    // --serve / --ir / --jit returned above. `aura soft` prints its own
    // refuse line instead of this one.
    if (!soft_launch) {
        if (const char* sb = std::getenv("AURA_SANDBOX")) {
            const std::string_view sv(sb);
            if ((sv == "off" || sv == "OFF" || sv == "Off") &&
                // Issue #4575 follow-up (asan-verify): the hatch note is for
                // the human at a REPL. Piped runs (CI, case harnesses) merge
                // stderr into the compared output — the banner on cerr fails
                // every text comparison in tests/run-tests.sh (281 cases).
                // Interactive faces keep the note; pipes stay clean.
                // AURA_FORCE_HATCH=1 opts a piped harness back in (the plain-off
                // "warning once" AC pins the once-semantics; default under
                // pipes stays silent for the text-compare contracts).
                (::isatty(::fileno(stderr)) || std::getenv("AURA_FORCE_HATCH") != nullptr))
                std::println(std::cerr, "aura: sandbox=off (dev escape hatch)");
        }
    }
    cs.set_session_id("default");
    aura::compiler::CompilerService::register_session("default", &cs);
    // Issue (lyapunov-fact demo 2026-07-28): use Repl for pipe mode too.
    // The previous batch fallback (`buf << std::cin.rdbuf()`) blocked
    // until stdin EOF, which never happens for the demo runner (it
    // sends forms one at a time over a long-running subprocess PIPE
    // and reads stdout incrementally). Repl::run() already uses C
    // stdio fprintf + explicit fflush, so stdout flushes per line in
    // pipe mode (line buffering via setvbuf at main() top). Banner is
    // suppressed in non-TTY mode by Repl itself.
    //
    // Issue #2322: `bool interactive = (argc == 1)` was true for any
    // no-arg invocation, including piped stdin from CI harnesses.
    // Repl's pipe path reads stdin one line at a time via
    // `std::getline`, which silently breaks multi-line expressions
    // like `(define (loop n acc) ...body on next line...)` — only the
    // first line is fed to the parser, the rest are parsed as
    // separate (incomplete) S-expressions, and the failure cascades
    // into "unbound variable" errors on subsequent lines + REPL
    // swallowing eval failures (returns 0 instead of 1).
    //
    // Fix: only enter Repl when stdin is actually a TTY. When stdin
    // is piped, fall through to the pipe-mode handler below, which
    // reads the entire input, splits balanced S-expressions via paren
    // depth tracking, evaluates each, and returns 1 on the first eval
    // failure (line 2817). This restores correct behavior for
    //   - issue_135_define_loop_n_acc (expected '5150')
    //   - issue_135_define_sum_0_define (expected '210')
    //   - err_unbound × 4 (expected exit 1)
    // while preserving interactive TTY behavior unchanged.
    // Issue #2767: never enter interactive REPL when -e / file supplied.
    bool interactive = (argc == 1) && ::isatty(STDIN_FILENO) && !source_from_argv;
    if (interactive || g_repl_4911) {
        // Issue #4911: `aura repl` honours the preset / grants like a file run.
        if (g_repl_4911 && soft_launch && cs.evaluator().effect_sandbox_mode() != 0) {
            const auto st = cs.evaluator().enter_soft_session(soft_extra);
            if (!quiet_4912)
                std::println(std::cerr, "aura: repl preset={} soft auto-grant: {}",
                             preset_4912.empty() ? "custom" : preset_4912,
                             st.active ? "mutate,read" : "refused");
        }
        aura::Repl repl(cs);
        if (g_repl_4911) {
            // Interactive input can name any std binding (assq, string-join,
            // …), so load the full prelude a file run would load lazily.
            if (auto pre = cs.eval("(require \"std/list\" all:)(require \"std/string\" all:)"
                                   "(require \"std/hash\" all:)(require \"std/math\" all:)");
                !pre)
                std::println(std::cerr, "aura: repl std prelude failed: {}",
                             pre.error().format_with_source("prelude"));
        }
        if (!g_repl_file_4911.empty())
            repl.load(g_repl_file_4911);
        repl.run();
        return 0;
    }

    // ── Pipe / file / -e mode: read all input, split into S-expressions ──
    // Join all lines to support multi-line expressions. File/-e already
    // filled all_input above (#2767).
    if (!source_from_argv) {
        std::ostringstream buf;
        buf << std::cin.rdbuf();
        all_input = buf.str();
    }
    if (all_input.empty()) {
        // Issue #2767: explicit stdin-only hint (was bare usage only).
        std::println(std::cerr, "error: no program source (stdin empty and no file/-e given)");
        std::println(std::cerr, "hint: 'aura --help' for usage");
        return 1;
    }

    // Issue #4575: mint only once we have a program. Refuse exits before
    // any form runs. The banner is the one stderr line for this launch.
    // Issue #4912: a preset under AURA_SANDBOX=off has nothing to grant.
    if (soft_launch && !soft_explicit_4912 && cs.evaluator().effect_sandbox_mode() == 0) {
        soft_launch = false;
        if (!quiet_4912)
            std::println(std::cerr, "aura: preset={} sandbox=off (every capability)",
                         preset_4912.empty() ? "custom" : preset_4912);
    }
    if (soft_launch) {
        const auto st = cs.evaluator().enter_soft_session(soft_extra);
        if (!st.active) {
            const char* word = "restricted";
            if (cs.evaluator().effect_sandbox_mode() == 2)
                word = "strict";
            else if (cs.evaluator().effect_sandbox_mode() == 0)
                word = "off (dev escape hatch)";
            else if (const char* sb = std::getenv("AURA_SANDBOX")) {
                const std::string_view sv(sb);
                if (sv == "off" || sv == "OFF" || sv == "Off")
                    word = "off (dev escape hatch)";
                else if (sv == "strict" || sv == "STRICT" || sv == "Strict")
                    word = "strict";
            }
            std::println(std::cerr, "aura: sandbox={}, soft auto-grant: refused", word);
            return 1;
        }
        std::string line =
            std::format("aura: sandbox=restricted, soft auto-grant: mutate(tenant={}, ws={}) "
                        "token=soft-auto-grant",
                        st.tenant, st.workspace_id);
        if (!preset_4912.empty() || grant_seen_4912 || deny_bits_4912 != 0) {
            // Issue #4912: one line naming the effective capability set.
            using aura::core::capability::Effect;
            std::string caps = "mutate,read";
            const std::pair<Effect, const char*> names[] = {{Effect::Write, "write"},
                                                            {Effect::Ffi, "ffi"},
                                                            {Effect::Exec, "exec"},
                                                            {Effect::Network, "network"}};
            for (const auto& [e, n] : names)
                if ((st.effects & static_cast<std::uint16_t>(e)) != 0) {
                    caps += ',';
                    caps += n;
                    // Issue #4912: name the scope (write:out/).
                    if (e == Effect::Write && aura::compiler::security::write_scope_active()) {
                        caps += ':';
                        caps += aura::compiler::security::write_scope_label();
                    }
                }
            line = std::format("aura: preset={} sandbox=restricted caps={} (tenant={}, ws={})",
                               preset_4912.empty() ? "custom" : preset_4912, caps, st.tenant,
                               st.workspace_id);
        } else if (!soft_allow_list.empty()) {
            line.push_back(' ');
            line += "allow=";
            line += soft_allow_list;
        }
        if (!quiet_4912)
            std::println(std::cerr, "{}", line);
    }

    auto oneshot_forms = split_oneshot_forms(all_input);
    if (!oneshot_forms)
        return 1;
    std::vector<std::string> exprs = std::move(*oneshot_forms);

    bool err = false;
    // Issue #4356: hold the thread-fiber body mutex for this oneshot eval.
    // read-line, read-byte, and fiber:join drop it while they block, then
    // take it back before touching the Evaluator. Destroyed before ~cs
    // drains still-running workers (they need the mutex to finish).
    aura::messaging::CliMainEvalBodyLock cli_body_lock;
    // Soft oneshot std prelude (#4178–#4219): auto-load Soft std list /
    // string / hash / math so bare oneshot (`aura -e` / file / pipe) binds
    // foldr, any/all, string-take/drop/pad/trim/split/replace/case,
    // starts-with?/ends-with?/contains?, make-list, list-tail, last,
    // hash-for-each/fold/empty?/->list, make-hash, even? — matching Soft
    // std semantics. Prefer Soft std auto-load over aura-build soft_*.aura
    // host fills (three-layer: Soft runtime/std, not gold host workarounds).
    {
        std::string prelude = build_lazy_oneshot_std_prelude(all_input);
        if (!prelude.empty()) {
            if (auto pre = cs.eval(prelude); !pre) {
                std::println(std::cerr, "error: Soft oneshot std prelude failed: {}",
                             pre.error().format_with_source(prelude));
                return 1;
            }
            // Issue #4228: Soft TW std/math preds must resolve via TopCellLoad
            // for native filter/map (IR MakeClosure stubs return empty).
            cs.sync_soft_export_cells_for_ir();
        }
    }
    // #3918 follow-up (CI cheap/medium red): evaluate the whole program in
    // ONE cs.eval pass, matching the --load entry. Per-expression eval
    // split pipeline state across calls and broke fiber:spawn parameter
    // capture inside defines — issue_135_define_loop_n_acc printed 0 from
    // stdin while the same source via --load printed 5150.
    // Issue #4460: failed require is a located diagnostic. This loop then
    // prints it once, runs forms after that location, and exits 1.
    cs.set_require_failure_is_diagnostic(true);
    // Issue #4865: force-soa lowers every top-level form before any of them
    // runs, so a fiber/agent body captures a stale copy of a top-level value
    // cell (set! / hash-set! / channel:send lost). Evaluate form by form under
    // force-soa, exactly as --load does, so each define publishes its live
    // cell before a later form's closure is lowered.
    if (const int forced = eval_oneshot_force_soa(cs, all_input, stop_first_4909); forced >= 0)
        return forced;
    if (auto r = cs.eval(all_input)) {
        if (!is_void(*r))
            std::println("{}", fmt_val(*r, cs));
    } else {
        recover_oneshot_eval_error(cs, all_input, exprs, stop_first_4909, err, r.error());
    }
    // Clear messaging bridge to prevent dangling pointer access during
    // static destruction (the CompilerService captures 'this' in lambdas
    // and is about to go out of scope).
    aura::messaging::g_current_compiler_service = nullptr;
    aura::messaging::g_messaging_bridge.send = nullptr;
    aura::messaging::g_messaging_bridge.recv = nullptr;
    aura::messaging::g_messaging_bridge.my_id = nullptr;
    aura::messaging::g_mailbox_read = nullptr;
    aura::messaging::g_mailbox_last_sender = nullptr;
    aura::messaging::g_mailbox_count = nullptr;
    aura::messaging::g_session_id = nullptr;
    aura::messaging::g_session_exists = nullptr;
    aura::messaging::g_reset_arena = nullptr;
    aura::messaging::g_http_post_async = nullptr;
    aura::messaging::g_fiber_spawn = {}; // std::function
    aura::messaging::g_fiber_yield = nullptr;
    aura::messaging::g_fiber_block = nullptr;
    aura::messaging::g_session_create = nullptr;

    // Issue #4497: an error consumed by display/write also fails the run.
    return (err || aura_displayed_error_count() > 0) ? 1 : 0;
}

static void install_fatal_signal_handlers() {
    // ── Crash handler: print backtrace on fatal signal ────────────
    // ── Crash handler: print backtrace on fatal signal ────────────
    // CI's --emit-binary path has been hitting 2 segfaults that don't
    // reproduce locally. This handler flushes a backtrace to stderr
    // before re-raising, so run-tests.sh's stderr capture (3ddf924)
    // will surface the failing instruction/frame. Skipped on macOS
    // (no execinfo.h); on Linux we use backtrace(3) from <execinfo.h>.
    //
    // CRITICAL: install an alternate signal stack (sigaltstack) so the
    // handler itself doesn't crash when the main stack is exhausted
    // (synthesize:optimize infinite recursion → stack overflow → SIGSEGV).
    // Without this, the handler would run on the main stack and re-fault
    // before backtrace() can capture frames, yielding only the header
    // line and "Backtrace:" with zero frame data.
#if defined(__linux__) && !defined(__APPLE__)
    static char alt_stack[65536]; // 64K alt stack (SIGSTKSZ is system-dependent)
    // Note: alt_stack must outlive the process; static-storage is fine.
    {
        stack_t ss{};
        ss.ss_sp = alt_stack;
        ss.ss_size = sizeof(alt_stack);
        ss.ss_flags = 0;
        if (::sigaltstack(&ss, nullptr) != 0) {
            (void)!::write(2, "aura: sigaltstack failed\n", 24);
        }
    }

    // Issue #4931: a SIGSEGV whose si_addr sits on (or just below) the
    // faulting thread's own stack is an overflow, not a wild pointer. The
    // old handler only consulted dladdr(), so a 512K-stack exhaustion in
    // FlatAST::assemble_nodeview reported "si_addr ... is outside any loaded
    // module (use-after-free or uninit ptr?)" and sent triage down the wrong
    // path. Read /proc/self/maps with raw syscalls (async-signal-safe, runs
    // on the alternate stack) and report the used depth instead.
    static auto report_stack_exhaustion = +[](const void* bad, const void* sp) -> bool {
        if (bad == nullptr || sp == nullptr)
            return false;
        static char maps_buf[262144];
        const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        long total = 0;
        while (total < static_cast<long>(sizeof(maps_buf)) - 1) {
            const long got =
                static_cast<long>(::read(fd, maps_buf + total, sizeof(maps_buf) - 1 - total));
            if (got <= 0)
                break;
            total += got;
        }
        ::close(fd);
        if (total <= 0)
            return false;
        maps_buf[total] = '\0';

        const auto sp_u = reinterpret_cast<unsigned long long>(sp);
        const auto bad_u = reinterpret_cast<unsigned long long>(bad);
        unsigned long long lo = 0, hi = 0;   // mapping that holds sp
        unsigned long long slo = 0, shi = 0; // the [stack] mapping
        long i = 0;
        while (i < total) {
            long j = i;
            while (j < total && maps_buf[j] != '\n')
                ++j;
            // "start-end perms ... [stack]"
            unsigned long long a = 0, b = 0;
            long k = i;
            auto hexval = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            while (k < j) {
                const int v = hexval(maps_buf[k]);
                if (v < 0)
                    break;
                a = (a << 4) | static_cast<unsigned long long>(v);
                ++k;
            }
            bool ok = k < j && maps_buf[k] == '-';
            if (ok)
                ++k;
            while (ok && k < j) {
                const int v = hexval(maps_buf[k]);
                if (v < 0)
                    break;
                b = (b << 4) | static_cast<unsigned long long>(v);
                ++k;
            }
            if (ok) {
                bool is_stack = false;
                for (long t = i; t + 6 <= j; ++t) {
                    if (maps_buf[t] == '[' && maps_buf[t + 1] == 's' && maps_buf[t + 2] == 't' &&
                        maps_buf[t + 3] == 'a' && maps_buf[t + 4] == 'c' &&
                        maps_buf[t + 5] == 'k') {
                        is_stack = true;
                        break;
                    }
                }
                if (is_stack && (slo == 0 || (sp_u >= a && sp_u < b))) {
                    slo = a;
                    shi = b;
                }
                if (sp_u >= a && sp_u < b) {
                    lo = a;
                    hi = b;
                }
            }
            i = j + 1;
        }
        // The faulting sp normally sits inside the thread's stack mapping.
        // An oversized frame (`sub sp, #N` past the guard) lands just BELOW
        // the mapping and the first store faults at sp+off, so accept an sp
        // in the guard window too.
        constexpr unsigned long long kGuardWindow = 1024ULL * 1024ULL;
        if (lo == 0 && slo != 0) {
            const bool sp_inside = sp_u >= slo && sp_u < shi;
            const bool sp_in_guard = sp_u < slo && (slo - sp_u) <= kGuardWindow;
            if (sp_inside || sp_in_guard) {
                lo = slo;
                hi = shi;
            }
        }
        if (lo == 0 || hi <= lo)
            return false;
        const unsigned long long stack_sz = hi - lo;
        const bool sp_below = sp_u < lo;
        // Used depth: bytes consumed from the stack top, including an
        // oversized frame that already spilled into the guard page.
        const unsigned long long used = sp_below ? (stack_sz + (lo - sp_u)) : (hi - sp_u);
        const bool in_stack = bad_u >= lo && bad_u < hi;
        const bool below_stack = bad_u < lo && (lo - bad_u) <= kGuardWindow;
        const unsigned long long dist = bad_u >= sp_u ? bad_u - sp_u : sp_u - bad_u;
        if (!((in_stack || below_stack) && dist <= (256ULL * 1024ULL)))
            return false;
        char line[512];
        const int n =
            std::snprintf(line, sizeof(line),
                          "si_addr %p is stack exhaustion: used %llu of %llu bytes of stack "
                          "[%p,%p) (sp %p, fault %lld bytes from sp)%s\n",
                          bad, used, stack_sz, reinterpret_cast<const void*>(lo),
                          reinterpret_cast<const void*>(hi), sp,
                          static_cast<long long>(bad_u) - static_cast<long long>(sp_u),
                          sp_below ? " [sp past the stack bottom: oversized frame]" : "");
        if (n > 0)
            (void)!::write(2, line, n);
        return true;
    };

    static auto crash_handler = +[](int sig, siginfo_t* info, void* ucontext) {
        // Use raw write(2) — stdio may be in undefined state.
        const char* name = sig == SIGSEGV   ? "SIGSEGV"
                           : sig == SIGABRT ? "SIGABRT"
                           : sig == SIGBUS  ? "SIGBUS"
                           : sig == SIGFPE  ? "SIGFPE"
                                            : "SIGNAL";
        // 1) Read the faulting instruction pointer directly from the kernel-
        //    provided ucontext. This is the most useful single piece of
        //    info: `addr2line -e aura -f -C <IP>` gives function + line.
        //    We deliberately do NOT call backtrace(3) here because it walks
        //    the (corrupt) frame pointer chain on the main stack, which can
        //    re-fault inside the handler and prevent any output. (We've seen
        //    this exact failure mode: handler writes header + "Backtrace:"
        //    then dies with no frames visible.)
        void* ip = nullptr;
        void* sp = nullptr;
        if (ucontext) {
#if defined(__x86_64__)
            // Linux x86_64: gregs[REG_RIP] / gregs[REG_RSP] (sys/ucontext.h).
            auto* mctx = &static_cast<ucontext_t*>(ucontext)->uc_mcontext;
            ip = reinterpret_cast<void*>(mctx->gregs[REG_RIP]);
            sp = reinterpret_cast<void*>(mctx->gregs[REG_RSP]);
#elif defined(__aarch64__)
            // Linux aarch64: mcontext_t has pc / sp (sys/ucontext.h).
            auto* mctx = &static_cast<ucontext_t*>(ucontext)->uc_mcontext;
            ip = reinterpret_cast<void*>(mctx->pc);
            sp = reinterpret_cast<void*>(mctx->sp);
#endif
        }

        // 2) Header: name, signal#, si_addr (the data address being accessed),
        //    si_code (SEGV_MAPERR=1, etc.), plus IP and SP from ucontext.
        char hdr[512];
        int n = 0;
        if (info) {
            n = std::snprintf(
                hdr, sizeof(hdr),
                "\n=== AURA CRASH: %s (signal %d) si_addr=%p si_code=%d ip=%p sp=%p ===\n", name,
                sig, info->si_addr, info->si_code, ip, sp);
        } else {
            n = std::snprintf(hdr, sizeof(hdr),
                              "\n=== AURA CRASH: %s (signal %d) ip=%p sp=%p ===\n", name, sig, ip,
                              sp);
        }
        if (n > 0)
            (void)!::write(2, hdr, n);

        // 3) Symbol-resolve the faulting IP (and si_addr) with dladdr.
        //    dladdr is async-signal-safe (POSIX) and works on the alt stack.
        //    Requires -rdynamic to see internal symbols; otherwise we still
        //    get dli_fname (the module that contains the address).
        if (ip) {
            Dl_info dlinfo;
            char line[512];
            int ln = 0;
            if (::dladdr(ip, &dlinfo) && dlinfo.dli_sname) {
                std::ptrdiff_t off =
                    static_cast<char*>(ip) - static_cast<const char*>(dlinfo.dli_saddr);
                ln =
                    std::snprintf(line, sizeof(line), "Faulting IP: %s+%td in %s\n",
                                  dlinfo.dli_sname, off, dlinfo.dli_fname ? dlinfo.dli_fname : "?");
            } else {
                ln = std::snprintf(line, sizeof(line),
                                   "Faulting IP: %p (no symbol; -rdynamic missing or stripped)\n",
                                   ip);
            }
            if (ln > 0)
                (void)!::write(2, line, ln);
        }
        // Issue #4931: classify a stack-overflow fault before falling back
        // to dladdr(), which cannot resolve a stack address and used to
        // report it as a use-after-free/wild pointer.
        // Only address-bearing faults carry a meaningful si_addr (for
        // SIGABRT/SIGFPE the union holds the signal's own payload).
        const bool stack_overflow = (sig == SIGSEGV || sig == SIGBUS) && info && info->si_addr &&
                                    report_stack_exhaustion(info->si_addr, sp);
        if (!stack_overflow && info && info->si_addr) {
            Dl_info dlinfo;
            char line[512];
            int ln = 0;
            void* bad = info->si_addr;
            if (::dladdr(bad, &dlinfo) && dlinfo.dli_sname) {
                std::ptrdiff_t off =
                    static_cast<char*>(bad) - static_cast<const char*>(dlinfo.dli_saddr);
                ln =
                    std::snprintf(line, sizeof(line), "si_addr in: %s+%td in %s\n",
                                  dlinfo.dli_sname, off, dlinfo.dli_fname ? dlinfo.dli_fname : "?");
            } else {
                ln = std::snprintf(
                    line, sizeof(line),
                    "si_addr %p is outside any loaded module (use-after-free or uninit ptr?)\n",
                    bad);
            }
            if (ln > 0)
                (void)!::write(2, line, ln);
        }
        (void)!::write(2, "Resolve with: addr2line -e aura -f -C <ip> <si_addr>\n", 54);
        (void)!::write(2, "=== END CRASH ===\n", 18);
        // Re-raise with default handler so the shell sees the signal
        // and core-dump machinery (ulimit -c) still kicks in.
        struct sigaction dfl{};
        dfl.sa_handler = SIG_DFL;
        ::sigaction(sig, &dfl, nullptr);
        ::raise(sig);
    };

    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK; // SA_ONSTACK → use alt stack
    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGABRT, &sa, nullptr);
    ::sigaction(SIGBUS, &sa, nullptr);
    ::sigaction(SIGFPE, &sa, nullptr);
#endif
}

static int run_cli_health_server(int argc, char** argv, int health_port) {
    const int port = (argc > 2) ? std::atoi(argv[2]) : (health_port > 0 ? health_port : 8080);
    if (port <= 0) {
        std::println(std::cerr, "usage: aura --health-server [PORT]");
        return 1;
    }
    if (health_port <= 0) {
        static std::atomic<bool> health_ready{true};
        aura::serve::http::HealthConfig cfg;
        cfg.ready = &health_ready;
        cfg.runtime_resolved = [] { return aura::serve::http::runtime_c_resolved(); };
        aura::serve::http::start_health_server(port, cfg);
    }
    std::println(std::cout, "aura health server listening on :{}", port);
    for (;;)
        ::pause();
}

static int run_cli_serve_async(int num_workers) {
#if AURA_HAVE_EPOLL
    // Issue #4048: Soft Ready auto (num_workers==0) defaults to 1 worker.
    // Soft shared Evaluator denseness is unsafe across workers even with
    // affinity pins (parent may still run on the pre-pin worker until the
    // first join yield). Explicit --worker-threads=N keeps multi-worker;
    // denseness then relies on affinity inherit + body mutex (#4048).
    if (num_workers == 0 && !aura::serve::production_abi_selfcheck_required())
        num_workers = 1;
    aura::serve::run_serve_async(num_workers);
#else
    std::println(std::cerr, "aura: --serve-async not supported on macOS (requires epoll)");
#endif
    return 0;
}

static int run_cli_concurrent_metrics(int num_workers) {
#if AURA_HAVE_EPOLL
    aura::serve::Scheduler sched(num_workers);
    constexpr int N = 200;
    std::atomic<int> completed{0};

    // Pattern 1: burst of CPU-bound fibers
    for (int i = 0; i < N; ++i) {
        sched.spawn([&completed]() {
            volatile int sum = 0;
            for (int j = 0; j < 50000; ++j)
                sum += j;
            completed.fetch_add(1);
        });
    }

    // Pattern 2: fibers that yield (simulate IO-bound)
    for (int i = 0; i < N; ++i) {
        sched.spawn([&completed]() {
            for (int k = 0; k < 3; ++k) {
                volatile int sum = 0;
                for (int j = 0; j < 10000; ++j)
                    sum += j;
                aura::serve::Fiber::yield();
            }
            completed.fetch_add(1);
        });
    }

    sched.run();

    std::println("Completed: {}/{}", completed.load(), N * 2);
    sched.metrics().dump();
#else
    std::println(std::cerr, "aura: --concurrent-metrics not supported on macOS (requires epoll)");
#endif
    return 0;
}

static int run_cli_serve_async_bench(int argc, char** argv, int num_workers) {
#if AURA_HAVE_EPOLL
    // Issue #4877: bench must honor the same Soft auto rule as
    // --serve-async (#4048): one worker for the shared Evaluator. It
    // passed num_workers=0 straight to Scheduler, which auto-sized to
    // hardware_concurrency(), while the Soft Ready banner announced
    // "Auto workers=1" and the shared-Evaluator safety rationale.
    // Explicit --worker-threads=N still opts into multi-worker.
    if (num_workers == 0 && !aura::serve::production_abi_selfcheck_required())
        num_workers = 1;
    std::string file_path = (argc > 2) ? argv[2] : "tests/bench.aura";
    // Issue #4916: same "file:line:col" face as --load (#4856 parity).
    if (std::ifstream bf(file_path); bf)
        register_entry_file_4916(
            file_path.c_str(),
            std::string((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>()));
    aura::serve::run_serve_async_bench(file_path, num_workers);
    return aura::serve::serve_async_bench_failed() ? 1 : 0;
#else
    std::println(std::cerr, "aura: --serve-async-bench not supported on macOS (requires epoll)");
#endif
    return 0;
}

static int run_cli_ir(int argc, char** argv) {
    aura::compiler::CompilerService cs;
    if (argc > 2) {
        auto source = argv[2];
        auto result = cs.eval_ir(source);
        if (!result) {
            std::println(std::cerr, "error: {}", result.error().format_with_source(source));
            return 1;
        }
        if (!is_void(*result))
            std::println("{}", fmt_val(*result, cs));
    } else {
        std::ostringstream buf;
        buf << std::cin.rdbuf();
        auto source = buf.str();
        auto result = cs.eval_ir(source);
        if (!result) {
            std::println(std::cerr, "error: {}", result.error().format_with_source(source));
            return 1;
        }
        if (!is_void(*result))
            std::println("{}", fmt_val(*result, cs));
    }
    return 0;
}

static int run_cli_inspect_state(int argc, char** argv) {
    aura::compiler::CompilerService cs;
    std::string mode = (argc > 2) ? argv[2] : "ir";

    if (mode == "cache-open") {
        // Load from cache file instead of evaluating stdin
        if (argc < 4) {
            std::println(std::cerr, "usage: {} --inspect cache-open <file.abc>", argv[0]);
            return 1;
        }
        auto mc = aura::compiler::cache::open_cache(argv[3]);
        if (!mc.valid()) {
            std::println(std::cerr, "error: cannot open cache file {}", argv[3]);
            return 1;
        }
        std::println("cache: {} nodes, root={}", mc.size(), mc.root());
        if (mc.has_ir()) {
            aura::ir::IRModule cached_mod;
            for (auto& fn : mc.ir_functions())
                cached_mod.add_function(fn);
            cached_mod.set_entry(mc.ir_entry());
            cached_mod.string_pool.assign(mc.ir_strings().begin(), mc.ir_strings().end());

            aura::compiler::ComputeKindWrap ck;
            aura::compiler::ArityWrap ar;
            aura::compiler::ConstantFoldingWrap cf;
            std::println(std::cerr, "PM: running {}->{}->{}", ck.name(), ar.name(), cf.name());
            ck.run(cached_mod);
            ar.run(cached_mod);
            cf.run(cached_mod);
            // Debug print removed (#63723): was polluting
            // test framework stream redirect for tests like
            // edsl-ir-cache:cascade-after-mutate. The folded
            // count is already in metrics_ via cf_pass metrics.
            (void)cf.folded_count();

            std::size_t json_size = 0;
            const aura::ir::IrBufferPtr json_data{aura_inspect_ir_json(&cached_mod, &json_size)};
            std::string_view json_str(json_data.get(), json_size);
            std::println("{}", prettify_json(std::string(json_str)));
        } else {
            std::println("no IR cache in {}", argv[3]);
        }
        return 0;
    }

    // Evaluate the input first (stdin)
    {
        std::ostringstream buf;
        buf << std::cin.rdbuf();
        auto input = buf.str();
        if (!input.empty()) {
            auto result = cs.eval_ir(input);
            if (!result) {
                std::println(std::cerr, "error: {}", result.error().format());
                return 1;
            }
        }
    }

    // Dump inspection output
    if (mode == "ir" || mode == "pretty" || mode == "all") {
        auto& last_mod = cs.last_ir_module();
        if (last_mod) {
            std::size_t json_size = 0;
            const aura::ir::IrBufferPtr json_data{aura_inspect_ir_json(&*last_mod, &json_size)};
            std::string_view json_str(json_data.get(), json_size);
            if (mode == "pretty") {
                std::println("{}", prettify_json(std::string(json_str)));
            } else {
                std::println("{}", json_str);
            }
        } else {
            std::println(std::cerr, "inspect: no IR module available");
        }
    }

    if (mode == "closures" || mode == "all") {
        auto closures = cs.last_closures();
        std::println("closures: {}", closures.size());
        for (auto& c : closures) {
            std::println("  [{}] func[{}] '{}': env[{}]", c.id, c.func_id, c.func_name,
                         c.env.size());
        }
    }

    if (mode == "cache" || mode == "all") {
        std::println("cached functions: {}", cs.cached_function_count());
    }

    if (mode == "evaluator" || mode == "all") {
        std::print("{}", cs.inspect_env());
    }

    if (mode == "typecheck" || mode == "all") {
        aura::core::TypeRegistry treg;
        aura::compiler::TypeChecker tc(treg);
        aura::diag::DiagnosticCollector diag;
        auto& flat = const_cast<aura::ast::FlatAST&>(cs.last_flat());
        auto& pool = const_cast<aura::ast::StringPool&>(cs.last_pool());
        if (flat.root < flat.size()) {
            auto result = tc.infer_flat(flat, pool, flat.root, diag);
            std::println("typecheck result: {}", treg.format_type(result));
            std::println("nodes: {}", flat.size());
            // Dump type for each node that has one
            for (aura::ast::NodeId nid = 0; nid < flat.size(); ++nid) {
                auto tid = flat.type_id(nid);
                if (tid > 0 && tid < treg.size()) {
                    auto ttype = aura::core::TypeId{tid, 1};
                    std::println("  node[{}]: {} (type_id={})", nid, treg.format_type(ttype), tid);
                }
            }
            if (!diag.diagnostics().empty()) {
                std::println("diagnostics:");
                for (auto& d : diag.diagnostics())
                    std::println("  {}", d.format());
            }
        } else {
            std::println(std::cerr, "inspect: no AST available");
        }
    }

    return 0;
}

static int run_cli_query(int argc, char** argv) {
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    std::string input;
    if (argc > 3) {
        input = argv[3];
    } else {
        std::getline(std::cin, input);
    }
    auto pr = aura::parser::parse_to_flat(input, flat, pool);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        std::println(std::cerr, "parse error{}", pr.error.empty() ? "" : ": " + pr.error);
        return 1;
    }
    flat.root = pr.root;
    aura::core::TypeRegistry tr;
    flat.resolve_type_ids(tr, pool);
    aura::compiler::QueryEngine engine(flat, pool);
    // Issue #62 Iter 4: parse the query first, check if it's
    // a global observability kind, and route to
    // execute_global() if so. Otherwise fall through to the
    // per-node execute() path.
    auto qexpr = engine.parse(argv[2]);
    if (qexpr.kind == aura::compiler::QueryExpr::Kind::DeoptCount ||
        qexpr.kind == aura::compiler::QueryExpr::Kind::ArenaUsage ||
        qexpr.kind == aura::compiler::QueryExpr::Kind::SpecializationCount) {
        // Live metrics from the singleton CompilerService.
        // The provider is non-owning; safe because the CLI exits
        // immediately after.
        struct LiveProvider : aura::compiler::QueryEngine::MetricsProvider {
            aura::compiler::CompilerService* cs;
            std::uint64_t deopt_count() const {
                return cs->metrics().deopt_count.load(std::memory_order_relaxed);
            }
            std::uint64_t arena_bytes_used() const {
                return cs->metrics().arena_bytes_used.load(std::memory_order_relaxed);
            }
            std::uint64_t jit_compilations() const {
                return cs->metrics().jit_compilations.load(std::memory_order_relaxed);
            }
        };
        aura::compiler::CompilerService cs;
        LiveProvider lp = LiveProvider{{}, &cs};
        engine.set_metrics_provider(&lp);
        std::uint64_t v = engine.execute_global(qexpr);
        std::println("{}: {}", argv[2], v);
    } else {
        auto results = engine.execute(qexpr);
        std::println("query: {} matches", results.size());
        for (auto id : results)
            std::println("  node[{}]: tag={}", id, static_cast<int>(flat.get(id).tag));
    }
    return 0;
}

static int run_cli_query_and_fix(int argc, char** argv) {
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    std::string input;
    if (argc > 4) {
        input = argv[4];
    } else {
        std::getline(std::cin, input);
    }
    auto pr = aura::parser::parse_to_flat(input, flat, pool);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        std::println(std::cerr, "parse error{}", pr.error.empty() ? "" : ": " + pr.error);
        return 1;
    }
    flat.root = pr.root;
    aura::core::TypeRegistry tr;
    flat.resolve_type_ids(tr, pool);
    aura::compiler::QueryEngine engine(flat, pool);
    aura::compiler::TransformEngine xform(flat, pool);
    auto result = xform.query_and_fix(engine, argv[2], argv[3]);
    std::println("transform: {} matches, {} patches, applied={}", result.match_count,
                 result.patch_count, result.applied);
    return result.applied ? 0 : 1;
}

static int run_cli_typecheck(int argc, char** argv) {
    aura::compiler::CompilerService cs;
    std::string input;
    if (argc > 2) {
        input = argv[2];
    } else {
        // Issue #4911: the whole program, not just its first line.
        input.assign(std::istreambuf_iterator<char>(std::cin), {});
    }
    // Issue #79: use the structured result so we can return a proper
    // exit code. The old code parsed the output for "diagnostics:"
    // which the formatter never produces (always returned 0).
    auto result = cs.typecheck_full(input);
    std::print("{}", result.output);
    return result.has_errors ? 1 : 0;
}

static int run_cli_auto_fix(int argc, char** argv) {
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    std::string input;
    if (argc > 2)
        input = argv[2];
    else
        std::getline(std::cin, input);
    auto pr = aura::parser::parse_to_flat(input, flat, pool);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        std::println(std::cerr, "parse error{}", pr.error.empty() ? "" : ": " + pr.error);
        return 1;
    }
    flat.root = pr.root;
    aura::core::TypeRegistry tr;
    flat.resolve_type_ids(tr, pool);
    aura::compiler::AutoFixEngine fixer(flat, pool);
    fixer.add_default_rules();
    auto patches = fixer.run_all();
    std::println("auto-fix: {} patches applied", patches);
    auto mod = aura::compiler::lower_to_ir(flat, pool, arena);
    aura::compiler::Primitives prims;
    aura::compiler::IRContext ctx(prims);
    aura::compiler::IRInterpreter interp(mod, ctx);
    auto eval = interp.execute();
    if (eval)
        std::println("result: {}", aura::compiler::types::format_value(*eval));
    return 0;
}

static int run_cli_fmt(int argc, char** argv) {
    bool in_place = false;
    bool check_only = false;
    std::string in_path, input;

    for (int i = 2; i < argc; ++i) {
        if (std::string_view(argv[i]) == "-i")
            in_place = true;
        else if (std::string_view(argv[i]) == "--check")
            check_only = true;
        else if (argv[i][0] != '-') {
            in_path = argv[i];
        }
    }

    if (!in_path.empty()) {
        // Read from file
        std::ifstream f(in_path);
        if (!f) {
            std::println(std::cerr, "error: cannot read {}", in_path);
            return 1;
        }
        input = std::string((std::istreambuf_iterator<char>(f)), {});
    } else {
        // Read from stdin
        std::getline(std::cin, input);
    }

    if (input.empty())
        return 1;

    aura::compiler::CompilerService cs;
    auto alloc = cs.arena().allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto pr = aura::parser::parse_to_flat(input, flat, pool);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        std::println(std::cerr, "parse error{}", pr.error.empty() ? "" : ": " + pr.error);
        return 1;
    }
    flat.root = pr.root;
    auto source = aura::compiler::unparse_node(flat, pool, flat.root);
    source += "\n";

    if (check_only) {
        if (source == input) {
            std::println(std::cerr, "formatted OK");
            return 0;
        }
        std::println(std::cerr, "needs formatting");
        return 1;
    }

    if (in_place && !in_path.empty()) {
        std::ofstream f(in_path);
        if (!f) {
            std::println(std::cerr, "error: cannot write {}", in_path);
            return 1;
        }
        f << source;
        std::println(std::cerr, "formatted {}", in_path);
    } else if (!in_path.empty()) {
        std::ofstream f(in_path + ".fmt");
        if (!f) {
            std::println(std::cerr, "error: cannot write {}.fmt", in_path);
            return 1;
        }
        f << source;
        std::println(std::cerr, "written to {}.fmt", in_path);
    } else {
        std::print("{}", source);
    }
    return 0;
}

static int run_cli_write(char** argv) {
    std::string input;
    std::getline(std::cin, input);
    std::ofstream f(argv[2]);
    if (!f) {
        std::println(std::cerr, "error: cannot write {}", argv[2]);
        return 1;
    }
    f << input << "\n";
    std::println(std::cerr, "written to {}", argv[2]);
    return 0;
}

static int run_cli_cache(int argc, char** argv) {
    std::string input;
    if (argc > 3) {
        input = argv[3];
    } else {
        std::getline(std::cin, input);
    }

    if (input.empty()) {
        std::println(std::cerr, "error: empty input");
        return 1;
    }

    aura::ast::ASTArena arena;
    aura::ast::StringPool pool(arena.allocator());
    aura::ast::FlatAST flat(arena.allocator());
    auto pr = aura::parser::parse_to_flat(input, flat, pool);
    if (pr.error.size()) {
        std::println(std::cerr, "parse error: {}", pr.error);
        return 1;
    }
    flat.root = pr.root;

    // Lower to IR and run passes
    auto ir_mod = aura::compiler::lower_to_ir(flat, pool, arena);
    aura::compiler::ComputeKindWrap ck;
    aura::compiler::ArityWrap ar;
    aura::compiler::ConstantFoldingWrap cf;
    ck.run(ir_mod);
    ar.run(ir_mod);
    cf.run(ir_mod);

    // Write cache with IR
    if (!aura::compiler::cache::write_cache(argv[2], flat, pool, pr.root, 0, &ir_mod, nullptr)) {
        std::println(std::cerr, "error: cannot write cache file {}", argv[2]);
        return 1;
    }

    std::println(std::cerr, "cache written to {} ({} ir functions, {} ir strings)", argv[2],
                 ir_mod.functions.size(), ir_mod.string_pool.size());

    // Evaluate normally
    aura::compiler::CompilerService cs;
    auto result = cs.eval_ir(input);
    if (!result) {
        std::println(std::cerr, "eval error: {}", result.error().format());
        return 1;
    }
    if (!is_void(*result))
        if (!is_void(*result))
            std::println("{}", fmt_val(*result, cs));
    return 0;
}

static int run_cli_cache_open(char** argv) {
    auto mc = aura::compiler::cache::open_cache(argv[2]);
    if (!mc.valid()) {
        std::println(std::cerr, "error: cannot open cache file {}", argv[2]);
        return 1;
    }
    std::println("cache: {} nodes, root={}, version=3 (O(1) resolve)", mc.size(), mc.root());
    // Show first few nodes with sym resolution
    auto show_n = std::min<std::size_t>(mc.size(), 6);
    for (std::uint32_t i = 0; i < show_n; ++i) {
        auto nv = mc.get(i);
        auto tag_int = static_cast<int>(nv.tag);
        std::string sym;
        if (nv.sym_id == 0xFFFFFFFFu) {
            sym = "(none)";
        } else {
            auto sv = mc.resolve(nv.sym_id);
            sym = sv.empty() ? "(unresolved)" : std::string(sv);
        }
        std::println("  [{}] tag={} sym_id={} sym='{}' int={}", i, tag_int, nv.sym_id, sym,
                     nv.int_value);
    }
    // Show embedded type signatures
    if (mc.has_sig()) {
        auto sig = mc.sig_data();
        std::println("Type signatures ({} bytes):", sig.size());
        // Print each line with indentation
        std::size_t start = 0;
        while (start < sig.size()) {
            auto end = sig.find('\n', start);
            if (end == std::string_view::npos)
                end = sig.size();
            std::println("  {}", sig.substr(start, end - start));
            start = end + 1;
        }
    }

    // Show IR cache info
    if (mc.has_ir()) {
        std::println("IR cache: {} functions, {} strings", mc.ir_functions().size(),
                     mc.ir_strings().size());
        for (auto& fn : mc.ir_functions()) {
            std::println("  func[{}] '{}': {} blocks, {} params, {} locals, {} args", fn.id,
                         fn.name, fn.blocks.size(), fn.params.size(), fn.local_count, fn.arg_count);
        }
        // Build IRModule from cached functions
        aura::ir::IRModule cached_mod;
        for (auto& fn : mc.ir_functions())
            cached_mod.add_function(fn);
        cached_mod.set_entry(mc.ir_entry());
        cached_mod.string_pool.assign(mc.ir_strings().begin(), mc.ir_strings().end());

        // Run passes
        aura::compiler::ComputeKindWrap ck;
        aura::compiler::ArityWrap ar;
        aura::compiler::ConstantFoldingWrap cf;
        std::println(std::cerr, "PM: running {}->{}->{}", ck.name(), ar.name(), cf.name());
        ck.run(cached_mod);
        ar.run(cached_mod);
        cf.run(cached_mod);

        if (ar.has_error()) {
            std::println(std::cerr, "arity check failed from cache");
            return 1;
        }

        if (false) {
            // Debug print removed (#63723): was polluting
            // test framework stream redirect.
            (void)cf.folded_count();
        }

        // Execute
        aura::compiler::CompilerService cs_tmp;
        aura::compiler::IRContext ctx(cs_tmp.evaluator().primitives());
        aura::compiler::IRInterpreter interp(cached_mod, ctx);
        auto result = interp.execute();
        if (!result) {
            std::println(std::cerr, "error: {}", result.error().format());
            return 1;
        }
        std::println("{}", fmt_val(*result, cs_tmp));
        return 0;
    } else {
        std::println("no IR cache available \u2014 use --cache instead");
        return 1;
    }
}

static int run_cli_strategy(int argc, char** argv) {
    // Note: strategy parsing is simple placeholder
    aura::compiler::CompilerService cs;
    if (argc > 2) {
        aura::compiler::EvalStrategy s;
        if (std::string_view(argv[2]) == "no-inline")
            s.enable_inlining = false;
        if (std::string_view(argv[2]) == "specialize")
            s.enable_specialization = true;
        cs.set_strategy(s);
    }
    // Forward remaining args would re-dispatch to --ir
    // (not yet implemented — full strategy CLI / config passthrough pending)
    // Re-dispatch to --ir with the modified CompilerService
    // For now, fall through to --ir
    // This is a stub — full strategy CLI will come with proper arg parsing
    std::println(std::cerr, "strategy set, re-run with --ir or --inspect");
    return 0;
}

static int run_cli_hot_swap(int argc, char** argv) {
    aura::compiler::CompilerService cs;
    std::string input;
    if (argc > 2) {
        input = argv[2];
    } else {
        std::getline(std::cin, input);
    }
    auto result = cs.hot_swap(input);
    if (!result) {
        std::println(std::cerr, "error: {}", result.error().format_with_source(input));
        return 1;
    }
    std::println("{}", fmt_val(*result, cs));
    return 0;
}

static int run_cli_jit_session() {
#ifdef AURA_HAVE_LLVM
    aura::compiler::CompilerService cs;
    std::string line;
    int rc = 0;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        if (line.starts_with(";;invalidate ")) {
            cs.public_invalidate_function(line.substr(13));
            continue;
        }
        if (line.starts_with(";;storm ")) {
            // Issue #4600 §4: drive the deopt-storm policy directly —
            // note NAME N times (default: the threshold) so the next
            // compile probe of NAME drops shape speculation.
            const auto rest = std::string(line.substr(8));
            std::string name = rest;
            std::uint32_t n = aura::compiler::jit_assume::DeoptStorms::threshold();
            if (const auto sp = rest.find(' '); sp != std::string::npos) {
                name = rest.substr(0, sp);
                const long v = std::strtol(rest.c_str() + sp + 1, nullptr, 10);
                n = v > 0 ? static_cast<std::uint32_t>(v) : n;
            }
            for (std::uint32_t i = 0; i < n; ++i)
                aura::compiler::jit_assume::DeoptStorms::instance().note(name);
            std::println("stormed={} count={}",
                         aura::compiler::jit_assume::DeoptStorms::instance().stormed(name),
                         aura::compiler::jit_assume::DeoptStorms::instance().count(name));
            continue;
        }
        if (line == ";;stats") {
            std::println("compiles={} {}", cs.snapshot().jit_compilations,
                         aura::compiler::jit_assume::format_stats());
            continue;
        }
        if (line == ";;floatpool") {
            std::println("floatpool={}", aura_jit_float_pool_size());
            continue;
        }
        if (line.starts_with(";;ir ")) {
            std::println("{}", cs.public_jit_function_ir(line.substr(5)));
            continue;
        }
        if (line == ";;events") {
            std::println("{}", aura::compiler::jit_assume::format_events());
            continue;
        }
        if (line.starts_with(";;"))
            continue;
        auto result = cs.exec_jit(line);
        if (!result) {
            std::println("error: {}", result.error().format());
            rc = 1;
            continue;
        }
        std::println("{}", is_void(*result) ? std::string("#<void>") : fmt_val(*result, cs));
    }
    return rc;
#else
    std::println(std::cerr, "JIT not available — rebuild with LLVM");
    return 1;
#endif
}

static int run_cli_jit(int argc, char** argv) {
    aura::compiler::CompilerService cs;
    std::string input;
    if (argc > 2) {
        input = argv[2];
    } else {
        std::getline(std::cin, input);
    }
    // Issue #4636: the --jit oneshot face must resolve the SAME std
    // bindings as the eval face (-e / file / pipe / --load): the lazy
    // oneshot std prelude auto-loads the std modules the program
    // references. make-hash / foldr / assoc / ... are std FUNCTIONS,
    // not primitives, so without the prelude every std binding reads
    // unbound under --jit while eval answers it: the JIT refuses the
    // program (#4607's unresolved-name refuse) and the tree walker
    // raises "unbound variable: make-hash" where eval prints 1.
    // Load the same prelude the other oneshot entries load. The
    // export-cell sync (sync_soft_export_cells_for_ir) is
    // deliberately NOT done here: it would make the JIT resolve a std
    // function to a tree-walker closure word, which the JIT's raw
    // closure-id calling convention cannot dispatch. The #4607
    // refuse sends the program to the tree walker, which resolves the
    // std binding the same way eval does.
    {
        std::string prelude = build_lazy_oneshot_std_prelude(input);
        if (!prelude.empty()) {
            if (auto pre = cs.eval(prelude); !pre) {
                std::println(std::cerr, "error: Soft oneshot std prelude failed: {}",
                             pre.error().format_with_source(prelude));
                return 1;
            }
        }
    }

// Quick test: compile empty function and call it
#ifdef AURA_HAVE_LLVM
    auto result = cs.exec_jit(input);
    if (!result) {
        std::println(std::cerr, "error: {}", result.error().format());
        return 1;
    }
    if (!is_void(*result))
        std::println("{}", fmt_val(*result, cs));
    // Issue #4603: an error consumed by JIT-inlined display/write fails
    // the run, like the eval path's #4497 rule. `(car 5)` after the
    // non-pair deopt lands on aura_display_value's error arm, which
    // bumps the same counter the tree walker uses.
    return aura_displayed_error_count() > 0 ? 1 : 0;
#else
    std::println(std::cerr, "JIT not available — rebuild with LLVM");
    return 1;
#endif
}

static int run_cli_freeze(char** argv) {
    aura::compiler::CompilerService cs;
    std::ostringstream buf;
    buf << std::cin.rdbuf();
    auto input = buf.str();
    if (input.empty()) {
        std::println(std::cerr, "error: no input");
        return 1;
    }
    // Evaluate first (populates workspace env)
    auto result = cs.eval(input);
    if (!result) {
        std::println(std::cerr, "error: {}", result.error().format());
        return 1;
    }
    // Freeze: write source to snapshot file
    if (auto sf = aura::core::open_file(argv[2], "w")) {
        // #4936: a failed write or final flush used to report success.
        const bool wrote = std::fputs(input.c_str(), sf.get()) >= 0;
        if (!aura::core::close_file(sf) || !wrote) {
            std::println(std::cerr, "error: cannot write to {}", argv[2]);
            return 1;
        }
        std::println("frozen to {}", argv[2]);
    } else {
        std::println(std::cerr, "error: cannot write to {}", argv[2]);
        return 1;
    }
    return 0;
}

static int run_cli_load(char** argv) {
    std::ifstream f(argv[2]);
    if (!f) {
        std::println(std::cerr, "error: cannot open {}", argv[2]);
        return 1;
    }
    std::string content((std::istreambuf_iterator<char>(f)), {});
    register_entry_file_4916(argv[2], content);
    aura::compiler::CompilerService cs;
    // Issue #4848: --load is the script/denseness face, not the
    // production serve pipeline. The pack default (Forbidden) vetoed
    // ordinary forms the IR path cannot express (let-bound closures,
    // fiber:spawn lambdas, std bindings loaded after the gate) with a
    // generic internal error; the same scripts run under
    // AURA_PIPELINE_STRICT=0. Take that permissive walker face here,
    // exactly as --serve-async-bench does (#4844). Operators can still
    // pin a face explicitly via AURA_PIPELINE_STRICT.
    if (const char* strict = std::getenv("AURA_PIPELINE_STRICT"); !(strict && *strict))
        aura::compiler::set_tree_walker_fallback_policy(
            aura::compiler::TreeWalkerFallbackPolicy::Allow);
    // Soft oneshot std prelude (#4178–#4219) — same auto-load as -e/file,
    // now demand-loaded: only the std modules this program references are
    // evaluated (see build_lazy_oneshot_std_prelude).
    {
        std::string prelude = build_lazy_oneshot_std_prelude(content);
        if (!prelude.empty()) {
            if (auto pre = cs.eval(prelude); !pre) {
                std::println(std::cerr, "error: Soft oneshot std prelude failed: {}",
                             pre.error().format_with_source(prelude));
                return 1;
            }
            // Issue #4228: Soft TW std/math preds must resolve via TopCellLoad
            // for native filter/map (IR MakeClosure stubs return empty).
            cs.sync_soft_export_cells_for_ir();
        }
    }
    // Issue #4750: --load runs the same one-pass eval as the file entry
    // (aura program.aura). An error that escapes a top-level user call is
    // one located diagnostic with a non-zero exit status here too - the
    // old path printed the value formatter's <error> on stdout and exited
    // 0, so every harness that spawns Aura read a failed run as success.
    // #4465's file-mode gate is armed for the same root eval.
    cs.set_require_failure_is_diagnostic(true);
    const char* strict_env = std::getenv("AURA_PIPELINE_STRICT");
    const std::string_view strict_v =
        strict_env ? std::string_view(strict_env) : std::string_view{};
    if (strict_v == "force-soa" || strict_v == "force_soa" || strict_v == "soa") {
        // Issue #4869: never split invalid source. Parse the whole file
        // first so unclosed lists, block comments, and malformed literals
        // fail with the same located diagnostic (and non-zero exit) as
        // the non-force-soa path instead of silently running a prefix.
        {
            aura::ast::ASTArena parse_arena;
            auto parse_alloc = parse_arena.allocator();
            aura::ast::StringPool parse_pool(parse_alloc);
            aura::ast::FlatAST parse_flat(parse_alloc);
            auto pr = aura::parser::parse_to_flat(content, parse_flat, parse_pool);
            if (!pr.success || pr.root == aura::ast::NULL_NODE) {
                std::println(std::cerr, "error: {}", pr.error.empty() ? "parse error" : pr.error);
                return 1;
            }
        }
        // Issue #4865: per-form eval so each define registers its value
        // cell before later forms lower. ForceSoa's value-define gate
        // would reject fiber:spawn/agent defines on the per-form path;
        // allow the walker for those forms (correct semantics, SoA only
        // for what the IR path can express).
        aura::compiler::set_tree_walker_fallback_policy(
            aura::compiler::TreeWalkerFallbackPolicy::Allow);
        for (auto& tf : split_top_level_forms_4865(content)) {
            auto r = cs.eval(positioned_form_4908(tf.line, tf.column, {}, tf.text, {}));
            bool loc_absolute = true; // Issue #4908: positioned eval
            // Issue #4899 follow-up: a top-level form's error value is a
            // located diagnostic, as on the whole-program --load face
            // (`(define x (car 5))`, `(f)` whose body fails). It used to
            // be dropped: no output, rc=0.
            if (r && aura::compiler::types::is_error(*r) && !aura::compiler::types::is_string(*r)) {
                aura::diag::Diagnostic d{cs.evaluator().error_value_kind(*r),
                                         cs.evaluator().soft_error_message(*r)};
                if (auto el = cs.evaluator().error_location(*r); recorded_loc_4908(el))
                    d.location = *el; // #4916 module file / #4908 absolute
                else {
                    d.location = per_form_error_value_location_4899(tf.text);
                    loc_absolute = false;
                }
                r = std::unexpected(std::move(d));
            }
            if (!r) {
                // Issue #4875: form-relative -> file coordinates (only the
                // per-form fallback location is form-relative now, #4908).
                // Issue #4916: a location in another file is already
                // in that file's coordinates.
                auto d = r.error();
                if (!loc_absolute && d.location.valid() && d.location.file_id == 0) {
                    if (d.location.line == 1 && d.location.column > 0 && tf.column > 1)
                        d.location.column += tf.column - 1;
                    d.location.line += tf.line - 1;
                }
                std::string ctx(static_cast<std::size_t>(tf.line - 1), '\n');
                ctx.append(static_cast<std::size_t>(tf.column - 1), ' ');
                ctx += tf.text;
                std::println(std::cerr, "error: {}", d.format_with_source(ctx));
                print_module_backtrace_4916(d.location, tf.line, tf.column, tf.text);
                return 1;
            }
        }
        // Issue #4869: force-soa --load does not print the last form's
        // value; the other --load faces do not either.
        return aura_displayed_error_count() > 0 ? 1 : 0;
    }
    auto result = cs.eval(content);
    if (!result) {
        std::println(std::cerr, "error: {}", result.error().format_with_source(content));
        print_module_backtrace_at_4916(result.error().location, cs.evaluator().file_resume_form(),
                                       content);
        return 1;
    }
    if (!is_void(*result))
        std::println("{}", fmt_val(*result, cs));
    // Issue #4497: an error value consumed by display/write outside try
    // already names itself on stderr and fails the run.
    return aura_displayed_error_count() > 0 ? 1 : 0;
}

static int run_cli_inspect_reflect(int argc, char** argv) {
    aura::compiler::CompilerService cs;
    std::string input;
    if (argc > 2) {
        input = argv[2];
    } else {
        std::getline(std::cin, input);
    }

    cs.set_strategy({.enable_inlining = true,
                     .enable_specialization = false,
                     .max_unroll = 3,
                     .verbose_inspect = true});

    auto result = cs.eval_ir(input);

    if (!result) {
        std::println(std::cerr, "error: {}", result.error().format_with_source(input));
    } else {
        std::println("result: {}", fmt_val(*result, cs));
    }

    // ── Environment dump ─────────────────────────────────────
    auto closures = cs.last_closures();
    auto cells = cs.last_cells();

    std::println("┌─ closures ({})", closures.size());
    for (auto& c : closures) {
        std::println("├ [{}] func[{}] '{}'", c.id, c.func_id, c.func_name);
        // Free variables (captured from enclosing scope)
        if (!c.func_free_vars.empty()) {
            std::println("│   free-vars:");
            for (std::size_t i = 0; i < c.func_free_vars.size() && i < c.env.size(); ++i) {
                std::println("│     {} : {}", c.func_free_vars[i], fmt_val(c.env[i], cs));
            }
        }
        // Parameters
        if (!c.func_params.empty()) {
            std::println("│   params:");
            for (auto& p : c.func_params) {
                std::println("│     {}", p);
            }
        }
        // Remaining env (non-free-var values)
        if (c.env.size() > c.func_free_vars.size()) {
            std::println("│   extra env slots:");
            for (std::size_t i = c.func_free_vars.size(); i < c.env.size(); ++i) {
                std::println("│     [{}] = {}", i, fmt_val(c.env[i], cs));
            }
        }
    }

    std::println("┌─ cells ({})", cells.size());
    for (auto& c : cells) {
        std::println("├ [{}] = {}", c.id, fmt_val(c.value, cs));
    }

    return result ? 0 : 1;
}

static int run_cli_env(int argc, char** argv) {
    aura::compiler::CompilerService cs;
    std::string input;
    if (argc > 2) {
        input = argv[2];
    } else {
        std::getline(std::cin, input);
    }

    auto result = cs.eval_ir(input);
    if (result) {
        std::println("result: {}", fmt_val(*result, cs));
    } else {
        std::println(std::cerr, "error: {}", result.error().format_with_source(input));
    }

    auto closures = cs.last_closures();
    auto cells = cs.last_cells();

    for (auto& c : closures) {
        std::println("closure [{}] = func[{}] '{}'", c.id, c.func_id, c.func_name);
        for (std::size_t i = 0; i < c.env.size(); ++i) {
            auto label = i < c.func_free_vars.size() ? c.func_free_vars[i] : std::format("[{}]", i);
            std::println("  {} = {}", label, fmt_val(c.env[i], cs));
        }
    }

    for (auto& c : cells) {
        std::println("cell [{}] = {}", c.id, fmt_val(c.value, cs));
    }

    return result ? 0 : 1;
}

static int run_cli_env_json(int argc, char** argv) {
    aura::compiler::CompilerService cs;
    std::string input;
    if (argc > 2) {
        input = argv[2];
    } else {
        std::getline(std::cin, input);
    }

    auto result = cs.eval_ir(input);
    auto closures = cs.last_closures();
    auto cells = cs.last_cells();

    std::println("{{\"status\":\"{}\",\"result\":{},\"closures\":[", result ? "ok" : "error",
                 result ? fmt_val(*result, cs) : std::string("null"));

    for (std::size_t i = 0; i < closures.size(); ++i) {
        auto& c = closures[i];
        std::println("  {{\"id\":{},\"func_id\":{},\"func\":\"{}\",", c.id, c.func_id, c.func_name);
        std::println("   \"env\":[");
        for (std::size_t j = 0; j < c.env.size(); ++j) {
            auto label = j < c.func_free_vars.size() ? c.func_free_vars[j] : std::format("[{}]", j);
            std::println("    {{\"name\":\"{}\",\"value\":{}}}{}", label, fmt_val(c.env[j], cs),
                         j + 1 < c.env.size() ? "," : "");
        }
        std::println("   ]}}{}", i + 1 < closures.size() ? "," : "");
    }

    std::println("],\"cells\":[");
    for (std::size_t i = 0; i < cells.size(); ++i) {
        std::println("  {{\"id\":{},\"value\":{}}}{}", cells[i].id, fmt_val(cells[i].value, cs),
                     i + 1 < cells.size() ? "," : "");
    }
    std::println("]}}");
    return result ? 0 : 1;
}

static int run_cli_evo_explain(int argc, char** argv) {
    static const bool enabled = []() {
        const char* e = std::getenv("AURA_OBS");
        return e && (e[0] == '1' || e[0] == 't' || e[0] == 'T');
    }();
    if (!enabled) {
        std::println(std::cerr, "evo-explain: disabled (set AURA_OBS=1 to enable)");
        return 0;
    }
    aura::compiler::CompilerService cs;
    cs.set_session_id("evo-explain");
    std::string input;
    if (argc > 2) {
        input = argv[2];
    } else {
        std::getline(std::cin, input);
    }
    // Run the program so the metrics are populated.
    auto _ = cs.eval_ir(input);
    auto snap = cs.snapshot();
    // Issue #62 Iter 3: dump the snapshot via the helper in
    // observability_json.cpp → to_json (aura-reflect / Wave A1).
    std::println("{}", snapshot_to_json(snap));
    return 0;
}
struct CliPrep {
    int status = -1;
    int num_workers = 0;
};

static CliPrep prepare_cli_runtime(int& argc, char** argv) {
    CliPrep prep;
    // Issue #2076 / #2053: production security defaults before any runtime:
    //   AURA_SANDBOX → Restricted (default) | off | strict
    //   AURA_MULTI_TENANT=1 → Strict
    //   TypedMutationAudit Full (AURA_TYPED_AUDIT override)
    //   AURA_MUTATION_AUDIT_WAL / AURA_PERSIST_DIR → enable WAL
    // Dev/test: AURA_SANDBOX=off restores Off + Sampled audit.
    aura::compiler::security::apply_production_security_defaults();

    // Issue #2955: production startup strong-symbol ABI self-check for
    // steal-complete / fiber evaluator_id / mutation held / depth-from-ptr.
    // Soft / AURA_SANDBOX=off / unit light-link: no-op. Production defaults
    // with missing strong hooks → abort (never multi-worker with weak no-ops).
    aura_runtime_require_production_abi_c();

    // Issue #3098: production multi-worker Ready must refuse Soft fall-through.
    // AND-s strong ABI + production_defaults_active. Only for actual
    // multi-worker entries (--serve-async / --worker-threads /
    // --concurrent-metrics). CLI eval / P0 / Soft sandbox stay on the
    // single-worker ABI check above (returns true under Soft without abort).
    //
    // Issue #4047 Soft Ready: when Soft / !production_defaults_active
    // (production_abi_selfcheck_required() == false), do NOT call
    // aura_runtime_require_production_multi_worker — that would FATAL with
    // fail_bits=0x10 only. Enter serve-async under an honest Soft Ready
    // profile (fiber/epoll unchanged). Production path unchanged: when
    // selfcheck is required, multi-worker Ready still aborts on Soft.
    // Do NOT latch g_production_multi_worker_latched on the Soft path.
    bool multi_worker_entry = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--serve-async" || a == "--concurrent-metrics" || a == "--serve-async-bench" ||
            a == "--worker-threads" || a.starts_with("--worker-threads=")) {
            multi_worker_entry = true;
            break;
        }
    }
    if (multi_worker_entry) {
        if (aura::serve::production_abi_selfcheck_required()) {
            aura_runtime_require_production_multi_worker_c();
        } else {
            // Soft Ready banner once. Wording avoids aura-build Soft Ready
            // refuse fingerprints ("production multi-worker Ready self-check
            // failed", exact "Soft (AURA_SANDBOX=off)", "fail_bits=") so
            // consumers can stamp serve_mode=async from measurement only.
            static bool soft_ready_logged = false;
            if (!soft_ready_logged) {
                soft_ready_logged = true;
                // Keep profile tag (#4047/#4048) for CI AC1 needle; mention #4053/#4054 in body.
                std::fprintf(
                    stderr,
                    "aura: Soft Ready profile (#4047/#4048): Soft serve-async denseness "
                    "(fiber:spawn/join + async http-post #4053/#4054) under Soft contracts — NOT "
                    "production multi-worker Ready. Auto workers=1 for shared-Evaluator "
                    "safety; --worker-threads=N opts into multi-worker. Do not stamp "
                    "production Ready / production_defaults from this path.\n");
                std::fflush(stderr);
            }
            // Non-aborting ABI observability already ran via
            // aura_runtime_require_production_abi_c() above.
        }
    }

    install_fatal_signal_handlers();

    // ── Parse common flags ───────────────────────────────
    int num_workers = 0;
    int arg_start = 1;

    // Check for --no-arena flag
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--no-arena") {
            g_use_arena = false;
            break;
        }
    }

    // Initialize TL arena (Issue #900: destroy at process exit)
    if (g_use_arena) {
        tl_arena_init(&g_tl_arena);
        std::atexit([] { tl_arena_destroy(&g_tl_arena); });
    }

    // Check for --worker-threads flag before the mode argument
    auto parse_worker_threads = [&]() {
        for (int i = 1; i < argc; ++i) {
            std::string_view a(argv[i]);
            if (a == "--worker-threads" && i + 1 < argc) {
                num_workers = std::atoi(argv[i + 1]);
                return i; // return index of --worker-threads
            }
            if (a.starts_with("--worker-threads=")) {
                num_workers = std::atoi(a.data() + 17);
                return i;
            }
        }
        return -1;
    };
    // Issue #677: HTTP health/metrics (AURA_HTTP_PORT or --health-port).
    auto parse_health_port = [&]() -> int {
        int port = aura::serve::http::port_from_env();
        for (int i = 1; i < argc; ++i) {
            const std::string_view a(argv[i]);
            if (a == "--health-port" && i + 1 < argc) {
                port = std::atoi(argv[i + 1]);
                break;
            }
            if (a.starts_with("--health-port=")) {
                port = std::atoi(a.data() + 13);
                break;
            }
        }
        return port;
    };
    const int health_port = parse_health_port();
    if (health_port > 0) {
        static std::atomic<bool> health_ready{true};
        aura::serve::http::HealthConfig cfg;
        cfg.ready = &health_ready;
        cfg.metrics_prometheus = [] { return aura::serve::prometheus_scheduler_metrics(); };
        cfg.runtime_resolved = [] { return aura::serve::http::runtime_c_resolved(); };
        aura::serve::http::start_health_server(health_port, cfg);
        static int health_atexit =
            (std::atexit(+[]() { aura::serve::http::stop_health_server(); }), 0);
        (void)health_atexit;
    }

    if (argc > 1 && std::string_view(argv[1]) == "--health-server") {
        prep.status = run_cli_health_server(argc, argv, health_port);
        return prep;
    }

    // #5063: this return used to be unconditional, so --worker-threads was
    // never stripped and num_workers stayed 0. `aura --worker-threads N
    // --load file` then fell through to the oneshot parser, which skipped
    // the flag and rejected --load as unknown. serve-async / bench never
    // saw an explicit worker count either.
    int wt_idx = parse_worker_threads();
    if (wt_idx >= 0) {
        // Shift remaining args to remove --worker-threads. Compact from the
        // flag's own slot (not from argv[1]): the old loop left argv[1]
        // untouched, so `aura --serve-async-bench file --worker-threads=2`
        // kept the flag as argv[1] and the following mode argument was
        // rewritten over --serve-async-bench ("cannot open
        // '--serve-async-bench'"). Both placements now dispatch normally,
        // which also makes the bench banner's --worker-threads=N opt-in
        // (#4877 / #4048) real.
        const int remove_count = (std::string_view(argv[wt_idx]) == "--worker-threads") ? 2 : 1;
        int dst = wt_idx;
        for (int i = wt_idx + remove_count; i < argc; ++i)
            argv[dst++] = argv[i];
        argv[dst] = nullptr;
        argc = dst;
    }

    prep.num_workers = num_workers;
    return prep;
}

static int aura_main(int argc, char* argv[]) {
    // Issue #137: register an atexit handler that frees the
    // runtime's hash tables. The `hash` primitive in the
    // evaluator creates these via FlatHashTable::create; they
    // are normally only freed by (gc-heap). Test scripts that
    // create a hash and exit without calling gc-heap (e.g. all
    // 33 hash tests in tests/run-tests.sh) would otherwise
    // leak them, which LeakSanitizer flags as test failures.
    // aura_reset_runtime also clears the string/float pools
    // and JIT caches, so it's the canonical cleanup point.
    static int dummy = (std::atexit(+[]() { aura_reset_runtime(); }), 0);
    (void)dummy;

    // Issue (lyapunov-fact demo 2026-07-28): force line-buffered stdout
    // so the REPL runner (which reads stdout one line at a time via
    // readline + select with a 30s timeout) sees each response
    // immediately when stdin is a long-running pipe. Default block
    // buffering (when stdout is not a TTY) waits for buffer full or
    // stdin EOF before flushing, so the runner times out.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // Issue #2772: denseness multi-process — child (shell …) only inherits
    // *exported* env. Span runners often set AURA_BIN as a non-exported
    // shell variable; seed process environ from self path so getenv and
    // child shells see the absolute binary (avoids "aura: not found" / 127).
    aura::compiler::paths::ensure_aura_bin_environ(argc > 0 ? argv[0] : nullptr);

    // Issue #4913: aura new / run / test / help and `soft --load`.
    if (auto rc = front_door_4913(argc, argv))
        return *rc;

    const CliPrep prep = prepare_cli_runtime(argc, argv);
    if (prep.status >= 0)
        return prep.status;
    const int num_workers = prep.num_workers;
    // #5066: --load fiber bodies are OS threads. A positive --worker-threads
    // caps how many of those bodies run at once. 0 stays unlimited.
    aura_set_cli_fiber_body_limit(num_workers);

    // ── --serve-async: fiber-based async JSON-line protocol ─
    // Uses ucontext fibers + epoll for non-blocking multi-session support.
    // Same JSON-line protocol as --serve, but with non-blocking I/O.
    if (argc > 1 && std::string_view(argv[1]) == "--serve-async")
        return run_cli_serve_async(num_workers);

    // ── --concurrent-metrics: run concurrency benchmark and dump metrics ─
    // Spawns fibers under various load patterns and prints scheduler metrics.
    // Usage: ./build/aura --concurrent-metrics [--worker-threads=N]
    if (argc > 1 && std::string_view(argv[1]) == "--concurrent-metrics")
        return run_cli_concurrent_metrics(num_workers);

    // ── --serve-async-bench: run bench.aura in serve-async mode ─
    // Loads and evaluates an Aura file directly in the serve-async runtime,
    // enabling fiber:spawn parallelism without going through the JSON protocol.
    // Usage: ./build/aura --serve-async-bench tests/bench.aura
    if (argc > 1 && std::string_view(argv[1]) == "--serve-async-bench")
        return run_cli_serve_async_bench(argc, argv, num_workers);

    // ── --serve: persistent JSON-line compile-fix loop ─────────
    // Each line of output is JSON. Agent reads with JSON.parse(line).
    // Messages: ok, error, fix, fixed, fix-fail
    if (argc > 1 && std::string_view(argv[1]) == "--serve")
        return run_sync_serve();


    // ── --ir: lower to IR and execute ─────────────────────────────
    if (argc > 1 && std::string_view(argv[1]) == "--ir")
        return run_cli_ir(argc, argv);

    // ── --inspect: evaluate and dump compiler state as JSON ───────
    // Usage: ./aura --inspect [mode] [cache-file]
    //   echo '(+ 1 2)' | ./aura --inspect
    //   ./aura --inspect cache-open out.abc
    if (argc > 1 && std::string_view(argv[1]) == "--inspect")
        return run_cli_inspect_state(argc, argv);

    // ── --query: run AuraQuery on parsed AST ─────────────────────
    if (argc > 2 && std::string_view(argv[1]) == "--query")
        return run_cli_query(argc, argv);

    // ── --query-and-fix: query + transform on parsed AST ─────────
    // Issue #3352: offline throwaway FlatAST — no Evaluator is attached
    // (query_and_fix ev=nullptr). Do not hand this mutated tree to a live
    // Evaluator without full re-bind / re-load; in-process hosts pass &ev.
    if (argc > 3 && std::string_view(argv[1]) == "--query-and-fix")
        return run_cli_query_and_fix(argc, argv);

    // ── --typecheck: run compile-time type checking ────────────
    if (argc > 1 && std::string_view(argv[1]) == "--typecheck")
        return run_cli_typecheck(argc, argv);

    // ── --auto-fix: run built-in optimization fixes ──────────────
    // Issue #3352: offline throwaway FlatAST — run_all(ev=nullptr).
    // Subsequent lower_to_ir is a fresh IR build, not an Evaluator handoff.
    // Do not pass this mutated FlatAST into a live Evaluator without re-load.
    if (argc > 1 && std::string_view(argv[1]) == "--auto-fix")
        return run_cli_auto_fix(argc, argv);

    // ── --fmt / --unparse: parse, format (reindent), output ─────
    // Usage: ./aura --fmt file.aura    → stdout
    //        ./aura --fmt -i file.aura  → in-place rewrite
    //        ./aura --fmt --check file.aura → CI mode (exit 1 if unformatted)
    if (argc > 1 &&
        (std::string_view(argv[1]) == "--fmt" || std::string_view(argv[1]) == "--unparse"))
        return run_cli_fmt(argc, argv);

    // ── --write: write stdin content to file ──────────────────────
    // Usage: ./aura --write <file>
    if (argc > 2 && std::string_view(argv[1]) == "--write")
        return run_cli_write(argv);

    // ── --cache: parse stdin and write FlatAST+StringPool+IR cache file ─
    // Usage: echo '(+ 1 2)' | ./aura --cache out.abc
    // Parses stdin to FlatAST, lowers to IR, writes cache (with IR), then evaluates.
    if (argc > 2 && std::string_view(argv[1]) == "--cache")
        return run_cli_cache(argc, argv);

    // ── --cache-open: load and inspect a cache file ───────────────
    // Usage: ./aura --cache-open out.abc
    // Prints cache stats (AST + IR if available).
    if (argc > 2 && std::string_view(argv[1]) == "--cache-open")
        return run_cli_cache_open(argv);

    // ── --strategy: set eval strategy before executing ─────────────
    // Usage: echo '(let ...)' | ./aura --strategy 'max_unroll=5' --ir
    if (argc > 1 && std::string_view(argv[1]) == "--strategy")
        return run_cli_strategy(argc, argv);

    // ── --hot-swap: replace function in cached IR module with new code ─
    // First call seeds the cache like --ir; subsequent calls hot-swap.
    if (argc > 1 && std::string_view(argv[1]) == "--hot-swap")
        return run_cli_hot_swap(argc, argv);

    // ── --jit-session: many exec_jit calls in ONE CompilerService ──
    // Issue #4569: the JIT cache only pays off across calls, and
    // `--jit` runs one. Each stdin line is one program (exec_jit);
    // lines starting with ";;" are directives:
    //   ;;invalidate NAME  — rebind cascade entry (invalidate_function)
    //   ;;storm NAME [N]   — deopt-storm policy notes, N defaults to the
    //                        threshold (#4600; next compile drops speculation)
    //   ;;stats            — "compiles=N <jit-assume stats>"
    //   ;;events           — assumption invalidation events
    //   ;;ir NAME          — optimized LLVM IR of NAME (#4570)
    //   ;;floatpool        — JIT float-pool size (#4570)
    // Output: one line per program (value or "error: ...").
    if (argc > 1 && std::string_view(argv[1]) == "--jit-session")
        return run_cli_jit_session();

    // ── --jit: compile and execute via LLVM ORC JIT ─────────────
    if (argc > 1 && std::string_view(argv[1]) == "--jit")
        return run_cli_jit(argc, argv);

    // ── --freeze: freeze workspace state to a snapshot file ──────
    // Usage: echo '(define (f x) (+ x 1))(display (f 41))' | ./aura --freeze out.abc
    // Then: echo '(current-source)' | ./aura --cache-open out.abc
    if (argc > 2 && std::string_view(argv[1]) == "--freeze")
        return run_cli_freeze(argv);

    // ── --load: load and run a frozen snapshot ───────────────────
    // Usage: ./aura --load frozen.aura
    if (argc > 2 && std::string_view(argv[1]) == "--load")
        return run_cli_load(argv);

    // ── --emit-binary: compile to standalone native binary ───────
    // Usage: echo '(+ 1 2)' | ./aura --emit-binary myapp
    //
    // Pipeline:
    //   1. Parse source → Lower to IR → Run pass manager
    //   2. Convert IRModule functions to FlatFunction array
    //   3. LLVM IR → .ll → llc -filetype=obj → .o for each function
    //   4. Link .o files with runtime.c → standalone binary
    //
    // Falls back to shell wrapper when LLVM is unavailable.
    if (argc > 2 && std::string_view(argv[1]) == "--emit-binary")
        return emit_standalone_binary(argc, argv);

    // ── --inspect: eval with full runtime reflection dump ────────
    if (argc > 1 && std::string_view(argv[1]) == "--inspect")
        return run_cli_inspect_reflect(argc, argv);

    // ── --env: compact cell/closure state dump ───────────────────
    if (argc > 1 && std::string_view(argv[1]) == "--env")
        return run_cli_env(argc, argv);

    // ── --env-json: machine-readable JSON env dump ────────────────
    if (argc > 1 && std::string_view(argv[1]) == "--env-json")
        return run_cli_env_json(argc, argv);

    // ── --evo-explain: emit a JSON snapshot of the observability
    // state (Issue #62 Iter 3). Runs the program (argv[2] or
    // stdin), then dumps CompilerSnapshot via the reflect
    // framework's auto_to_json. Off by default; set AURA_OBS=1
    // to enable.
    if (argc > 1 && std::string_view(argv[1]) == "--evo-explain")
        return run_cli_evo_explain(argc, argv);

    return run_oneshot_or_file(argc, argv);
}
// Issue #4936 (Core Guidelines E.12 / bugprone-exception-escape): an exception
// that escapes main calls std::terminate with no message. Report it and exit
// with EX_SOFTWARE (70) instead.
int main(int argc, char* argv[]) {
    try {
        return aura_main(argc, argv);
    } catch (const std::exception& e) {
        ::aura::core::eprintln_nothrow("aura: fatal: uncaught exception: {}", e.what());
    } catch (...) {
        ::aura::core::eprintln_nothrow("aura: fatal: uncaught non-standard exception");
    }
    return 70;
}
