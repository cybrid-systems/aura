// ir_buffer_abi.h — Issue #4936: ownership of the buffers the IR
// serialize / --inspect C ABI (ir_reflect_serialize.cpp) hands out.
//
// The ABI stays extern "C" (that TU cannot import the module graph, #2290),
// but every buffer it returns is now released by the TU that allocated it:
// hold it in IrBufferPtr, never delete[] it by hand.
#pragma once

#include <cstddef>
#include <memory>

extern "C" {
void aura_ir_serialize(const void* mod, const char** out_data, std::size_t* out_size);
char* aura_inspect_ir_json(const void* mod, std::size_t* out_size);
void aura_ir_buffer_free(const char* data) noexcept;
}

namespace aura::ir {

struct IrBufferFree {
    void operator()(const char* data) const noexcept { aura_ir_buffer_free(data); }
};
using IrBufferPtr = std::unique_ptr<const char[], IrBufferFree>;

} // namespace aura::ir
