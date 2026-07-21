// evaluator_primitives_eda.cpp — Issue #499: Foundational EDA primitives
// module (parse/query/mutate/waveform/hardware feedback) for Agent-driven
// verification + hardware co-design workflows.
//
// Issue #1968: eda:* is a commercial EDA vertical (DOMAIN_STATUS deferred).
// Registration gated by AURA_ENABLE_EDA (CMake option, default ON).
// Slim/core builds: -DAURA_ENABLE_EDA=OFF → register_eda_primitives no-op.
// See docs/eda.md + scripts/check_primitive_surface.py COMMERCIAL_DOMAIN_BUDGETS.

module;

#include <sys/stat.h>

#include <cctype>
#include <fstream>
#include <string>
#include <string_view>

#include "observability_metrics.h"
#include "runtime_shared.h"
#include "security_capabilities.h"
#include "hash_meta.h" // FNV constants (#901)

// Default ON when the TU is compiled outside the CMake graph (tools/IDE).
#ifndef AURA_ENABLE_EDA
#define AURA_ENABLE_EDA 1
#endif

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;
import aura.compiler.hardware_backend;

namespace aura::compiler::primitives_detail::eda_detail {

using EvalValue = types::EvalValue;
// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

inline std::optional<aura::ast::NodeTag> tag_from_name(std::string_view name) {
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    auto eq_ci = [&](std::string_view lit) {
        if (name.size() != lit.size())
            return false;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (lower(name[i]) != lower(lit[i]))
                return false;
        }
        return true;
    };
    if (eq_ci("interface"))
        return aura::ast::NodeTag::Interface;
    if (eq_ci("modport"))
        return aura::ast::NodeTag::Modport;
    if (eq_ci("property"))
        return aura::ast::NodeTag::Property;
    if (eq_ci("sequence"))
        return aura::ast::NodeTag::Sequence;
    if (eq_ci("assert"))
        return aura::ast::NodeTag::Assert;
    if (eq_ci("covergroup"))
        return aura::ast::NodeTag::Covergroup;
    if (eq_ci("coverpoint"))
        return aura::ast::NodeTag::Coverpoint;
    if (eq_ci("constraint"))
        return aura::ast::NodeTag::Constraint;
    if (eq_ci("class"))
        return aura::ast::NodeTag::Class;
    return std::nullopt;
}

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

inline std::vector<std::string_view> split_colon(std::string_view line) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= line.size()) {
        auto pos = line.find(':', start);
        if (pos == std::string_view::npos) {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

inline std::vector<aura::ast::SymId> split_ports(aura::ast::StringPool& pool,
                                                 std::string_view ports_csv) {
    std::vector<aura::ast::SymId> out;
    std::size_t start = 0;
    while (start <= ports_csv.size()) {
        auto pos = ports_csv.find(',', start);
        const auto slice = trim(ports_csv.substr(
            start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
        if (!slice.empty())
            out.push_back(pool.intern(std::string(slice)));
        if (pos == std::string_view::npos)
            break;
        start = pos + 1;
    }
    return out;
}

} // namespace aura::compiler::primitives_detail::eda_detail

namespace aura::compiler::primitives_detail {

void register_eda_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev) {
#if !AURA_ENABLE_EDA
    // Issue #1968: commercial EDA vertical disabled for this build.
    (void)add;
    (void)ev;
    return;
#else
#endif // AURA_ENABLE_EDA
}

} // namespace aura::compiler::primitives_detail