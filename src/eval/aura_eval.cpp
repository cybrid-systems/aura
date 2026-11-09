// aura_eval.cpp — Issue #4946: in-process evaluation ABI.
//
// One session owns one CompilerService, i.e. the same parser / typechecker /
// evaluator / module loader the REPL and the file-load face drive. The C
// entry points wrap it in a stable, exception-free surface:
//   - errors come back as a JSON record plus AURA_EVAL_ERROR, never abort;
//   - calls on a session are serialized by a mutex, so a host may share it
//     across threads (one evaluation at a time);
//   - the session registry key is unique per session, so a host that
//     deliberately creates several sessions does not clobber the REPL's
//     "default" slot.
//
// Process-wide boundary: CompilerService installs process-global hooks (the
// active-service pointer, the type registry, query-evaluator TLS) at
// construction, so only one live session per process is supported. See
// aura_eval.h.

#include "eval/aura_eval.h"

#include "compiler/pipeline_policy.hh"
#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.diag;

namespace {

using aura::compiler::CompilerService;

// The opaque C handle. Not movable: the service is address-stable and the
// session registry keeps a raw pointer to it.
struct Session {
    std::mutex mu;
    std::string label;
    std::string registry_key;
    CompilerService service;
    bool registered = false;
    bool soft_session = false;
};

std::atomic<std::uint64_t> g_session_seq{0};
std::once_flag g_sandbox_once;

// Issue #4946: an embedder's session is a development face, like the CLI's
// file and REPL faces: mutate + read inside the session, plus the effects
// AURA_EVAL_GRANT lists (write, exec|shell, network|net, ffi). TenantAdmin,
// Syscall and MacroSelfEvo stay host-only and are never granted here.
[[nodiscard]] std::uint16_t extra_effect_grants() {
    namespace sec = aura::compiler::security;
    const char* env = std::getenv("AURA_EVAL_GRANT");
    if (env == nullptr || *env == '\0')
        return 0;
    std::uint16_t bits = 0;
    std::string_view rest(env);
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const auto tok = rest.substr(0, comma);
        if (tok == "write")
            bits = static_cast<std::uint16_t>(bits | sec::kEffectWrite);
        else if (tok == "exec" || tok == "shell")
            bits = static_cast<std::uint16_t>(bits | sec::kEffectExec);
        else if (tok == "network" || tok == "net")
            bits = static_cast<std::uint16_t>(bits | sec::kEffectNetwork);
        else if (tok == "ffi")
            bits = static_cast<std::uint16_t>(bits | sec::kEffectFfi);
        if (comma == std::string_view::npos)
            break;
        rest.remove_prefix(comma + 1);
    }
    return bits;
}

void append_json_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

[[nodiscard]] const char* value_kind_name(const aura::compiler::types::EvalValue& v) noexcept {
    using namespace aura::compiler::types;
    if (is_void(v))
        return "void";
    if (is_unspecified(v))
        return "unspecified";
    if (is_bool(v))
        return "bool";
    if (is_int(v))
        return "int";
    if (is_float(v))
        return "float";
    if (is_string(v))
        return "string";
    if (is_pair(v))
        return "pair";
    if (is_vector(v))
        return "vector";
    if (is_hash(v) || is_imm_hash(v))
        return "hash";
    if (is_closure(v))
        return "closure";
    if (is_keyword(v))
        return "keyword";
    if (is_error(v))
        return "error";
    if (is_module(v))
        return "module";
    if (is_primitive(v))
        return "primitive";
    if (is_cell(v))
        return "cell";
    if (is_linear(v))
        return "linear";
    if (is_opaque(v))
        return "opaque";
    return "unknown";
}

[[nodiscard]] std::string format_eval_value(CompilerService& cs,
                                            const aura::compiler::types::EvalValue& v) {
    auto& ev = cs.evaluator();
    return aura::compiler::format_value(v, ev.primitives().string_heap(), ev.pairs(), 0,
                                        &ev.primitives(), ev.keyword_table());
}

[[nodiscard]] std::string error_json(std::string_view message, std::string_view kind,
                                     std::uint32_t line, std::uint32_t column) {
    std::string out = "{\"status\":\"error\",\"message\":";
    append_json_string(out, message);
    if (!kind.empty()) {
        out += ",\"kind\":";
        append_json_string(out, kind);
    }
    if (line != 0) {
        out += ",\"line\":";
        out += std::to_string(line);
        if (column != 0) {
            out += ",\"column\":";
            out += std::to_string(column);
        }
    }
    out.push_back('}');
    return out;
}

[[nodiscard]] char* heap_copy(std::string s) {
    auto* buf = static_cast<char*>(std::malloc(s.size() + 1));
    if (!buf)
        return nullptr;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    return buf;
}

[[nodiscard]] Session* impl(aura_eval_session* h) noexcept {
    return reinterpret_cast<Session*>(h);
}

// Evaluate one source string on an already-locked session.
[[nodiscard]] int eval_locked(Session& s, const std::string& source, char** result_json) {
    try {
        auto r = s.service.eval(std::string_view(source));
        if (!r) {
            const auto& d = r.error();
            *result_json = heap_copy(error_json(d.message, aura::diag::kind_name(d.kind),
                                                d.location.line, d.location.column));
            return AURA_EVAL_ERROR;
        }
        const auto& v = *r;
        const char* kind = value_kind_name(v);
        if (aura::compiler::types::is_error(v) && !aura::compiler::types::is_string(v)) {
            // A refused form (a bad car, a blocked mutate) is a value, not a
            // throw; report it as an error with the evaluator's cause text.
            *result_json = heap_copy(
                error_json(s.service.evaluator().soft_error_message(v), "error-value", 0, 0));
            return AURA_EVAL_ERROR;
        }
        std::string out = "{\"status\":\"ok\",\"value\":";
        append_json_string(out, format_eval_value(s.service, v));
        out += ",\"type\":";
        append_json_string(out, kind);
        out.push_back('}');
        *result_json = heap_copy(std::move(out));
        return AURA_EVAL_OK;
    } catch (const std::exception& e) {
        *result_json = heap_copy(error_json(e.what(), "internal-error", 0, 0));
        return AURA_EVAL_INTERNAL;
    } catch (...) {
        *result_json =
            heap_copy(error_json("unknown exception during eval", "internal-error", 0, 0));
        return AURA_EVAL_INTERNAL;
    }
}

} // namespace

extern "C" {

const char* aura_eval_abi_version(void) {
    return "1";
}

aura_eval_session* aura_eval_session_create(const char* label) {
    // CompilerService's constructor may throw (it allocates arenas and
    // installs hooks); a failed create is a NULL handle, not a crash.
    Session* s = nullptr;
    try {
        s = new Session();
    } catch (...) {
        return nullptr;
    }
    s->label = (label && *label != '\0') ? label : "embed";
    // The CLI's oneshot/load faces take the permissive walker face unless the
    // operator pins AURA_PIPELINE_STRICT; an embedder's ordinary forms must
    // not be vetoed by the production pack default.
    const char* strict = std::getenv("AURA_PIPELINE_STRICT");
    if (!(strict && *strict != '\0')) {
        aura::compiler::set_tree_walker_fallback_policy(
            aura::compiler::TreeWalkerFallbackPolicy::Allow);
    }
    s->registry_key = "embed:" + s->label + ":" + std::to_string(g_session_seq.fetch_add(1) + 1);
    s->service.set_session_id(s->registry_key);
    // Issue #2076/#4946: the CLI resolves AURA_SANDBOX (default Restricted)
    // through the process-wide mode authority before any evaluator runs. An
    // embedder has no main() doing that, so the first session does it once:
    // without it the capability registry, the strict link and the provenance
    // policy disagree and every mutating primitive is refused.
    std::call_once(g_sandbox_once, [] { aura::compiler::security::apply_aura_sandbox_env(); });
    // Issue #4575: the Soft session grant the CLI's dev faces mint. Without
    // it every mutating primitive (hash-set!, cell-set!) answers
    // "effect-denied: mutate not granted" — a loaded stdlib like std/moead
    // builds its state with hash-set!.
    //
    // AURA_SANDBOX=off is a CLI dev hatch: the CLI then skips the soft mint
    // because Off bypasses effect checks. That bypass is not present on the
    // embedded capability path, so an embedder that inherits AURA_SANDBOX=off
    // (the test/CMake env) would get a refused soft session (mode must be
    // Restricted) and every std/moead hash-set! fails. An embedder has no CLI
    // preset, so pin Restricted here; the Soft session then grants mutate+read.
    if (s->service.evaluator().effect_sandbox_mode() == 0)
        s->service.evaluator().set_effect_sandbox_mode(1);
    const auto soft = s->service.evaluator().enter_soft_session(extra_effect_grants());
    s->soft_session = soft.active;
    if (const char* dbg = std::getenv("AURA_EVAL_DEBUG"); dbg && *dbg != '\0') {
        std::fprintf(stderr,
                     "aura_eval: sandbox=%u single_tenant=%d soft=%d refused=%d effects=0x%x\n",
                     static_cast<unsigned>(s->service.evaluator().effect_sandbox_mode()),
                     static_cast<int>(s->service.evaluator().kernel_self_single_tenant()),
                     static_cast<int>(soft.active), static_cast<int>(soft.refused),
                     static_cast<unsigned>(soft.effects));
    }
    CompilerService::register_session(s->registry_key, &s->service);
    s->registered = true;
    return reinterpret_cast<aura_eval_session*>(s);
}

void aura_eval_session_destroy(aura_eval_session* session) {
    if (!session)
        return;
    auto* s = impl(session);
    if (s->registered) {
        CompilerService::unregister_session(s->registry_key);
        s->registered = false;
    }
    delete s;
}

const char* aura_eval_session_label(const aura_eval_session* session) {
    if (!session)
        return nullptr;
    return reinterpret_cast<const Session*>(session)->label.c_str();
}

int aura_eval_source(aura_eval_session* session, const char* source, size_t len,
                     char** result_json) {
    if (!result_json)
        return AURA_EVAL_INVALID_ARGUMENT;
    *result_json = nullptr;
    if (!session || !source)
        return AURA_EVAL_INVALID_ARGUMENT;
    auto* s = impl(session);
    // Copy: the evaluator keeps string_views into its input across the parse,
    // and a caller's buffer need not be NUL-terminated (len may be exact).
    std::string code(source, (len == static_cast<size_t>(-1)) ? std::strlen(source) : len);
    std::lock_guard<std::mutex> lock(s->mu);
    return eval_locked(*s, code, result_json);
}

int aura_eval_file(aura_eval_session* session, const char* path, char** result_json) {
    if (!result_json)
        return AURA_EVAL_INVALID_ARGUMENT;
    *result_json = nullptr;
    if (!session || !path)
        return AURA_EVAL_INVALID_ARGUMENT;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        *result_json =
            heap_copy(error_json(std::string("cannot read file: ") + path, "io-error", 0, 0));
        return AURA_EVAL_IO_ERROR;
    }
    std::string code;
    char buf[8192];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
        code.append(buf, n);
    std::fclose(f);
    auto* s = impl(session);
    std::lock_guard<std::mutex> lock(s->mu);
    return eval_locked(*s, code, result_json);
}

void aura_eval_string_free(char* text) {
    std::free(text);
}

} // extern "C"
