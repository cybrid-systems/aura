// C++26 contract violation handler for GCC 16.1
// Required by the compiler runtime for contract violations.
// Note: <contracts> is included via -include compiler flag (CMakeLists.txt)
// to avoid header-unit resolution issues with -fmodules-ts.
#include <cstdlib>
#include <iostream>

// GCC 16.1 requires this exact signature at global scope.
// The contract_violation type is in std::contracts.
void handle_contract_violation(const std::contracts::contract_violation&) {
    std::cerr << "contract violation\n";
    std::abort();
}
