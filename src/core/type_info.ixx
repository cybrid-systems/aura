export module aura.core.type_info;

import aura.core.type;

namespace aura::core {

// TypeInfo 的 P2996 验证（将来扩展）
// template<typename T>
// consteval bool validate_type_info() {
//     // Check alignment: resolved_type at offset 0
//     // Check size: exactly 3 * sizeof(uint64_t)
//     // ...
// }

} // namespace aura::core
