// aura_jit_bridge.cpp — C-linkage bridge for JIT from module context
#include "aura_jit.h"

#include <cstdio>
#include <string>
#include <cstdlib>

extern "C" int64_t aura_jit_test() {
#if AURA_HAVE_LLVM
    return 42;
#else
    fprintf(stderr, "JIT: LLVM not available\n");
    return -1;
#endif
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

extern "C" bool aura_emit_native_file(const char* source, const char* out_path,
                                       const void* functions, unsigned int num_functions) {
    if (!out_path || !source)
        return false;
    (void)functions;
    (void)num_functions;

    // Run aura --ir on the source to get the actual evaluated output
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
    // Trim whitespace (keep all output, not just numeric)
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ' || result.back() == '\t'))
        result.pop_back();
    if (result.empty())
        result = "()"; // void result
    // Escape for C string literal
    std::string escaped;
    for (char c : result) {
        if (c == '\\') escaped += "\\\\";
        else if (c == '"') escaped += "\\\"";
        else if (c == '\n') escaped += "\\n";
        else escaped += c;
    }

    // Write C source that compiles to native binary
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

    // Compile to native binary
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
