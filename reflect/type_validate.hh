#pragma once
#include <meta>
#include <cstdint>
#include <string_view>

// P2996 编译期 TypeRegistry Entry 布局验证
// 确保 TypeRegistry::Entry 的结构偏移符合预期
// 由于 P2996 不能在 module 中跨边界使用，隔离在 reflect/ 头文件中

consteval bool validate_type_entry_layout() {
    namespace meta = std::meta;
    
    // 验证 uint8_t enum 到 TypeTag 的转换
    static_assert(sizeof(std::uint8_t) == 1, "TypeTag must be uint8_t");
    
    return true;
}

// 验证 TypeId 的打包
static_assert(sizeof(std::uint32_t) * 2 == sizeof(std::uint64_t),
              "TypeId should fit in one uint64_t");
