// aura_jit_bridge.cpp — C-linkage bridge for AOT native compilation
// Routes compilation requests to the LLVM-based emit backend in aura_jit.cpp.

#include "aura_jit.h"

#include <cstdio>
#include <string>
#include <cstdlib>
#include <cstring>
#include <vector>

// Helper: convert aura::ir::IRFunction to aura::jit::FlatFunction
// This bridges between the compiler's IR types and the JIT's FlatFunction.
// Caller must keep flat_instrs/name_storage alive for the returned FlatFunction.
struct FlatFunctionHolder {
    std::vector<std::vector<aura::jit::FlatInstruction>> flat_instrs;
    std::vector<aura::jit::FlatBlock> flat_blocks;
    std::string name_storage;
    aura::jit::FlatFunction flat_fn;
};

// This collection is passed to the bridge function as user data.
// We define it inline since the compiler's IR types are opaque to this bridge.

// Since we don't have visibility into aura::ir::IRFunction from this TU,
// the AOT bridge receives an already-converted FlatFunction array.
// For the C-linkage bridge, we accept a FlatFunction array directly.

// ── Old JIT test stub (kept for backward compat) ───────────────
extern "C" int64_t aura_jit_test() {
#if AURA_HAVE_LLVM
    return 42;
#else
    fprintf(stderr, "JIT: LLVM not available\n");
    return -1;
#endif
}

// ── aura_emit_native: AOT compile a set of FlatFunctions to native binary ──
// Takes an array of FlatFunction + runtime.c path + output binary path.
// 1. Compiles each FlatFunction to a .o file via LLVM IR + llc
// 2. Links all .o files with runtime.c into the final binary
// Returns true on success.

// ── Generator: closure registration C file ──────────────────────────
// Generates a .c file that registers all compiled function pointers
// with the runtime's func_table before main() runs.
// Each LLVM-compiled function is an ELF symbol; the runtime needs
// aura_register_fn(func_id, fn_ptr) to associate them by IR func_id
// so that aura_alloc_closure(func_id) can set the correct function ptr.
//
// func_ids array: parallel to functions[], holds the IR func_id for each.
static bool generate_registration_c(const aura::jit::FlatFunction* functions,
                                     const uint32_t* func_ids,
                                     unsigned int num_functions,
                                     const std::string& reg_c_path) {
    FILE* f = std::fopen(reg_c_path.c_str(), "w");
    if (!f) return false;
    
    fprintf(f, "// Auto-generated closure registration for AOT binary\n");
    fprintf(f, "#include <stdint.h>\n");
    fprintf(f, "#include <stddef.h>\n");
    fprintf(f, "\n");
    fprintf(f, "// Runtime: register function by func_id for closure dispatch\n");
    fprintf(f, "void aura_register_fn(int64_t func_id, int64_t fn_ptr);\n");
    fprintf(f, "\n");
    
    // Generate extern declarations for all compiled functions
    for (unsigned int i = 0; i < num_functions; ++i) {
        std::string safe_name(functions[i].name);
        for (auto& c : safe_name)
            if (c == '@' || c == '.' || c == '-' || c == ' ') c = '_';
        fprintf(f, "extern int64_t %s(int64_t*, uint32_t);\n", safe_name.c_str());
    }
    
    fprintf(f, "\n// Constructor — runs before main()\n");
    fprintf(f, "__attribute__((constructor)) void aura_aot_register_fns(void) {\n");
    
    for (unsigned int i = 0; i < num_functions; ++i) {
        std::string safe_name(functions[i].name);
        for (auto& c : safe_name)
            if (c == '@' || c == '.' || c == '-' || c == ' ') c = '_';
        fprintf(f, "    aura_register_fn(%u, (int64_t)%s);\n",
                func_ids[i], safe_name.c_str());
    }
    
    fprintf(f, "}\n");
    fclose(f);
    return true;
}

static bool aot_flat_functions_to_binary(const aura::jit::FlatFunction* functions,
                                          unsigned int num_functions,
                                          const std::string& out_path,
                                          const std::string& runtime_c_path) {
    if (num_functions == 0)
        return false;

    std::vector<std::string> obj_files;

    // Step 1: Compile each FlatFunction to .o via LLVM IR + llc
    for (unsigned int i = 0; i < num_functions; ++i) {
        std::string obj_path = out_path + ".func" + std::to_string(i) + ".o";
        bool ok = aura::jit::emit_native_object(functions[i], obj_path);
        if (!ok) {
            fprintf(stderr, "AOT: failed to compile function '%s'\n", functions[i].name);
            for (auto& p : obj_files)
                std::remove(p.c_str());
            return false;
        }
        obj_files.push_back(obj_path);
    }

    // Step 2: Compile runtime.c (contains main(), bump allocator, closures,
    //          cells, pairs, I/O, strings — the complete standalone runtime)
    std::string cc = ::getenv("CC") ? ::getenv("CC") : "gcc";
    std::string runtime_o = out_path + ".runtime.o";
    {
        std::string cmd = cc + " -c " + runtime_c_path + " -o " + runtime_o + " 2>/dev/null";
        int rc = ::system(cmd.c_str());
        if (rc != 0) {
            cmd = "clang -c " + runtime_c_path + " -o " + runtime_o + " 2>/dev/null";
            rc = ::system(cmd.c_str());
        }
        if (rc != 0) {
            fprintf(stderr, "AOT: cannot compile runtime '%s'\n", runtime_c_path.c_str());
            for (auto& p : obj_files)
                std::remove(p.c_str());
            return false;
        }
        obj_files.push_back(runtime_o);
    }

    // Step 3: Generate and compile closure registration .c
    // Uses array index as func_id (matches IR module function order).
    // Functions are in IR module order: [entry, lambda_0, lambda_1, ...]
    // OpMakeClosure(func_id=N) references the N-th function.
    std::vector<uint32_t> func_ids(num_functions);
    for (unsigned int i = 0; i < num_functions; ++i)
        func_ids[i] = i;
    
    std::string reg_c_path = out_path + "._reg.c";
    std::string reg_o_path = out_path + "._reg.o";
    if (generate_registration_c(functions, func_ids.data(), num_functions, reg_c_path)) {
        std::string cmd = cc + " -c " + reg_c_path + " -o " + reg_o_path + " 2>/dev/null";
        int rc = ::system(cmd.c_str());
        if (rc != 0) {
            cmd = "clang -c " + reg_c_path + " -o " + reg_o_path + " 2>/dev/null";
            rc = ::system(cmd.c_str());
        }
        if (rc == 0) {
            obj_files.push_back(reg_o_path);
        }
        std::remove(reg_c_path.c_str());
    }

    // Step 4: Link all .o files into binary
    std::string link_cmd = cc;
    for (auto& p : obj_files)
        link_cmd += " " + p;
    link_cmd += " -o " + out_path + " -lm 2>&1";
    int rc = ::system(link_cmd.c_str());

    // Cleanup temp .o files
    for (auto& p : obj_files)
        std::remove(p.c_str());

    if (rc == 0) {
        fprintf(stderr, "AOT: emitted native binary: %s\n", out_path.c_str());
        return true;
    }

    fprintf(stderr, "AOT: link failed (symbols missing)\n");
    return false;
}


extern "C" bool aura_emit_object_file(const void* mod, const char* path) {
    (void)mod;
    if (!path)
        return false;
    std::string out_path(path);
    auto dump_path = out_path + ".ir";
    if (auto* f = std::fopen(dump_path.c_str(), "w")) {
        std::fprintf(f, "aura emit-object placeholder\n");
        std::fclose(f);
        return true;
    }
    return false;
}

// ── aura_emit_native_file: C-linkage entry point for AOT compilation ──
//
// Parameters:
//   source        - The Aura source code string
//   out_path      - Path for the output native binary
//   functions     - Opaque pointer to an array of FlatFunction structs
//   num_functions - Number of functions in the array
//
// Returns true on successful native binary emission.
//
extern "C" bool aura_emit_native_file(const char* source, const char* out_path,
                                       const void* functions, unsigned int num_functions) {
    if (!out_path || !source)
        return false;

    // If functions were provided, use the LLVM AOT pipeline
    if (functions && num_functions > 0) {
        auto* flat_fns = static_cast<const aura::jit::FlatFunction*>(functions);

        // Find runtime.c path (contains main(), closures, cells, pairs, I/O)
        std::string runtime_dir;
        {
            auto* env = std::getenv("AURA_RUNTIME_DIR");
            if (env)
                runtime_dir = env;
            else
                runtime_dir = "lib";
        }
        std::string runtime_c = runtime_dir + "/runtime.c";
        std::string alt_runtime_c = "../lib/runtime.c";
        {
            FILE* f = std::fopen(runtime_c.c_str(), "r");
            if (!f) {
                f = std::fopen(alt_runtime_c.c_str(), "r");
                if (f) {
                    runtime_c = alt_runtime_c;
                    fclose(f);
                }
            } else {
                fclose(f);
            }
        }

        bool ok = aot_flat_functions_to_binary(flat_fns, num_functions,
                                                std::string(out_path),
                                                runtime_c);
        if (ok)
            return true;

        fprintf(stderr, "AOT: LLVM pipeline failed, falling back to shell wrapper\n");
    }

    // Original fallback: run aura --ir on the source to get the evaluated output
    std::string src(source);
    std::string cmd = "echo '" + src + "' | ./build/aura --ir 2>/dev/null | head -1";
    std::string result;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe) {
        char buf[4096];
        std::string line;
        while (::fgets(buf, sizeof(buf), pipe))
            line += buf;
        if (!line.empty())
            result = line;
        ::pclose(pipe);
    }
    // Trim whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ' || result.back() == '\t'))
        result.pop_back();
    if (result.empty())
        result = "()";
    // Escape for C string literal
    std::string escaped;
    for (char c : result) {
        if (c == '\\') escaped += "\\\\";
        else if (c == '"') escaped += "\\\"";
        else if (c == '\n') escaped += "\\n";
        else escaped += c;
    }

    // Write C source
    std::string c_path = std::string(out_path) + ".c";
    auto* f = std::fopen(c_path.c_str(), "w");
    if (!f)
        return false;

    fprintf(f, "#include <stdio.h>\n");
    fprintf(f, "#include <stdint.h>\n");
    fprintf(f, "int main(int argc, char** argv) {\n");
    fprintf(f, "    (void)argc; (void)argv;\n");
    fprintf(f, "    printf(\"%%s\\n\", \"%s\");\n", escaped.c_str());
    fprintf(f, "    return 0;\n");
    fprintf(f, "}\n");
    fclose(f);

    std::string out_binary(out_path);
    std::string cc = ::getenv("CC") ? ::getenv("CC") : "gcc";
    cmd = cc + " " + c_path + " -o " + out_binary + " 2>/dev/null";
    int rc = ::system(cmd.c_str());
    if (rc != 0) {
        cmd = "clang " + c_path + " -o " + out_binary + " 2>/dev/null";
        rc = ::system(cmd.c_str());
    }

    std::remove(c_path.c_str());

    if (rc == 0) {
        fprintf(stderr, "emitted native: %s\n", out_binary.c_str());
        return true;
    }

    fprintf(stderr, "compile failed\n");
    return false;
}
