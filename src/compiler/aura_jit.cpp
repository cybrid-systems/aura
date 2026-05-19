// aura_jit.cpp — LLVM ORC JIT backend for Aura IR
// Compiles Aura IR functions to native code via LLVM JIT.
// Guarded by AURA_HAVE_LLVM compile definition.

#include "aura_jit.h"

#if AURA_HAVE_LLVM

#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdio>
#include <format>
#include <unordered_map>
#include <memory>

namespace aura::jit {

// ── AuraJIT implementation ────────────────────────────────────

struct AuraJIT::Impl {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    llvm::orc::JITDylib* main_dylib = nullptr;
    llvm::LLVMContext ctx;
    bool initialized = false;

    bool init() {
        if (initialized) return true;

        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();

        auto jit_or_err = llvm::orc::LLJITBuilder().create();
        if (!jit_or_err) {
            fprintf(stderr, "JIT: failed to create LLJIT: %s\n",
                         llvm::toString(jit_or_err.takeError()).c_str());
            return false;
        }
        jit = std::move(*jit_or_err);
        main_dylib = &jit->getMainJITDylib();
        initialized = true;
        return true;
    }

    JitFunction compile_empty() {
        if (!init()) return nullptr;

        // Create a simple function: i64 @empty_func() { ret i64 42 }
        auto mod = std::make_unique<llvm::Module>("empty", ctx);
        auto fn_type = llvm::FunctionType::get(
            llvm::Type::getInt64Ty(ctx), false);
        auto fn = llvm::Function::Create(
            fn_type, llvm::Function::ExternalLinkage, "empty_func", mod.get());

        llvm::IRBuilder<> builder(
            llvm::BasicBlock::Create(ctx, "entry", fn));
        builder.CreateRet(llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx), 42));

        // Verify module
        if (llvm::verifyModule(*mod, &llvm::errs())) {
            fprintf(stderr, "JIT: module verification failed\n");
            return nullptr;
        }

        // Add to JIT
        auto tsm = llvm::orc::ThreadSafeModule(std::move(mod),
            std::make_unique<llvm::LLVMContext>());
        auto err = jit->addIRModule(std::move(tsm));
        if (err) {
            fprintf(stderr, "JIT: failed to add module: %s\n",
                         llvm::toString(std::move(err)).c_str());
            return nullptr;
        }

        // Look up function
        auto sym_or_err = jit->lookup("empty_func");
        if (!sym_or_err) {
            fprintf(stderr, "JIT: symbol lookup failed: %s\n",
                         llvm::toString(sym_or_err.takeError()).c_str());
            return nullptr;
        }

        int64_t (*fn_ptr)() = sym_or_err->toPtr<int64_t(*)()>();
        return fn_ptr;
    }
};

// ── Public API ────────────────────────────────────────────────

AuraJIT::AuraJIT() : impl_(std::make_unique<Impl>()) {}
AuraJIT::~AuraJIT() = default;

bool AuraJIT::available() const {
    return impl_->initialized;
}

JitFunction AuraJIT::compile_empty() {
    return impl_->compile_empty();
}

} // namespace aura::jit

#else // !AURA_HAVE_LLVM

namespace aura::jit {

AuraJIT::AuraJIT() : impl_(nullptr) {}
AuraJIT::~AuraJIT() = default;
bool AuraJIT::available() const { return false; }
JitFunction AuraJIT::compile_empty() { return nullptr; }

} // namespace aura::jit

#endif
