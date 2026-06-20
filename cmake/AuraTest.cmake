# Shared compile options for Aura C++ test targets.
#
# Usage:
#   aura_test_compile_options(my_test_target)
#
# Wired into aura_add_issue_test() and the main test binaries
# (test_ir, test_concurrent, aura_test_objects, aura).

function(aura_test_compile_options TARGET)
    target_compile_options(${TARGET} PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-missing-field-initializers
        -Wno-unused-variable -Wno-unused-function
        -Wno-unused-but-set-variable -Wno-unused-parameter
        -Wno-misleading-indentation -Wno-parentheses
    )
endfunction()

function(aura_test_compile_options_reflect TARGET)
    target_compile_options(${TARGET} PRIVATE -freflection)
    aura_test_compile_options(${TARGET})
endfunction()