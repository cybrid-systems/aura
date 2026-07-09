// evaluator_primitives_file.cpp — P0 step 25: read / read-file / write-file / file-* / shell /
// directory-list aura.compiler.evaluator module partition; registered via
// evaluator_primitives_registry.cpp.

module;

#include <dirent.h>
#include <sys/stat.h>
#include "runtime_shared.h"
#include "security_capabilities.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

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

void register_file_primitives(PrimRegistrar add, Evaluator& ev) {

    const auto deny_io = [&ev](std::string_view cap, std::string_view msg) -> EvalValue {
        if (!ev.sandbox_mode() || ev.has_capability(cap) ||
            ev.has_capability(aura::compiler::security::kCapIo) ||
            ev.has_capability(aura::compiler::security::kCapWildcard))
            return make_void();
        ev.bump_capability_denial();
        return make_primitive_error(ev.string_heap_, ev.error_values_, msg,
                                    ev.primitive_error_counter_ptr());
    };
    const auto deny_exec = [&ev](std::string_view msg) -> EvalValue {
        if (!ev.sandbox_mode() || ev.has_capability(aura::compiler::security::kCapExec) ||
            ev.has_capability(aura::compiler::security::kCapWildcard))
            return make_void();
        ev.bump_capability_denial();
        return make_primitive_error(ev.string_heap_, ev.error_values_, msg,
                                    ev.primitive_error_counter_ptr());
    };

    add("read", [&ev](const auto&) {
        std::string line;
        std::getline(std::cin, line);
        if (line.empty())
            return make_void();
        auto id = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(line));
        return make_string(id);
    });

    // Helper: check path is a regular file (skip directories)
    auto is_regular = [](const std::string& path) -> bool {
        struct stat st;
        return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    };

    add("read-file", [&ev, is_regular, deny_io](const auto& a) {
        if (auto denied = deny_io(aura::compiler::security::kCapIoRead,
                                  "capability denied: io-read required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        auto& path = ev.string_heap_[idx];
        if (!is_regular(path))
            return make_void();
        std::ifstream f(path);
        if (!f)
            return make_void();
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto id = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(content));
        return make_string(id);
    });

    add("write-file", [&ev, deny_io](std::span<const EvalValue> a) {
        if (auto denied = deny_io(aura::compiler::security::kCapIoWrite,
                                  "capability denied: io-write required");
            is_error(denied))
            return denied;
        if (a.size() < 2 || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        auto& path = ev.string_heap_[idx];
        std::string content;
        if (is_string(a[1])) {
            auto cidx = as_string_idx(a[1]);
            if (cidx < ev.string_heap_.size())
                content = ev.string_heap_[cidx];
        } else if (is_int(a[1])) {
            content = std::to_string(as_int(a[1]));
        } else {
            return make_void();
        }
        std::ofstream f(path);
        if (!f)
            return make_void();
        f << content;
        return make_int(1);
    });

    add("command-line", [&ev](const auto&) -> EvalValue {
        // Returns list of command-line argument strings, NOT including argv[0].
        // Parsed from /proc/self/cmdline on Linux.
        std::ifstream f("/proc/self/cmdline");
        if (!f)
            return make_void();
        std::string raw;
        std::getline(f, raw, '\0'); // skip argv[0]
        std::vector<std::string> items;
        while (std::getline(f, raw, '\0')) {
            if (!raw.empty())
                items.push_back(raw);
        }
        EvalValue result = make_void();
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(*it);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    add("file-exists?", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        auto& path = ev.string_heap_[idx];
        struct stat st;
        return make_int(::stat(path.c_str(), &st) == 0 ? 1 : 0);
    });

    add("file-copy", [&ev, is_regular, deny_io](const auto& a) {
        if (auto denied = deny_io(aura::compiler::security::kCapIoWrite,
                                  "capability denied: io-write required");
            is_error(denied))
            return denied;
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void();
        auto sidx = as_string_idx(a[0]), didx = as_string_idx(a[1]);
        if (sidx >= ev.string_heap_.size() || didx >= ev.string_heap_.size())
            return make_void();
        if (!is_regular(ev.string_heap_[sidx]))
            return make_void();
        std::ifstream src(ev.string_heap_[sidx], std::ios::binary);
        if (!src)
            return make_void();
        std::ofstream dst(ev.string_heap_[didx], std::ios::binary);
        if (!dst)
            return make_void();
        dst << src.rdbuf();
        return make_int(1);
    });

    add("file-delete", [&ev, deny_io](std::span<const EvalValue> a) {
        if (auto denied = deny_io(aura::compiler::security::kCapIoWrite,
                                  "capability denied: io-write required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        return make_int(std::remove(ev.string_heap_[idx].c_str()) == 0 ? 1 : 0);
    });

    add("file-size", [&ev, is_regular](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size() || !is_regular(ev.string_heap_[idx]))
            return make_int(0);
        std::ifstream f(ev.string_heap_[idx], std::ios::ate | std::ios::binary);
        if (!f)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(f.tellg()));
    });

    add("shell", [&ev, deny_exec](std::span<const EvalValue> a) -> EvalValue {
        if (auto denied = deny_exec("capability denied: exec required"); is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_int(-1);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(-1);
        return make_int(::system(ev.string_heap_[idx].c_str()));
    });

    add("command-output", [&ev, deny_exec](std::span<const EvalValue> a) -> EvalValue {
        if (auto denied = deny_exec("capability denied: exec required"); is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        auto& cmd = ev.string_heap_[idx];
        std::array<char, 4096> buf;
        std::string result;
        auto* fp = ::popen(cmd.c_str(), "r");
        if (!fp)
            return make_void();
        while (::fgets(buf.data(), buf.size(), fp) != nullptr)
            result += buf.data();
        ::pclose(fp);
        if (!result.empty() && result.back() == '\n')
            result.pop_back();
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(result));
        return make_string(sid);
    });

    add("directory-list", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        auto& dir_path = ev.string_heap_[idx];
        EvalValue result = make_void();
        auto dir = opendir(dir_path.c_str());
        if (!dir)
            return make_void();
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name == "." || name == "..")
                continue;
            auto sid = ev.string_heap_.size();
            ev.string_heap_.push_back(name);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sid), result});
            result = make_pair(pid);
        }
        closedir(dir);
        return result;
    });
}

} // namespace aura::compiler::primitives_detail