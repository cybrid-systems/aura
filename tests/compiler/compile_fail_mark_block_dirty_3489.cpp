// Compile-fail fixture for Issue #3489. Not in CMake.
// Under AURA_PRODUCTION_PACK (the `aura` binary) IRFunctionSoA::
// mark_block_dirty is `= delete`. This TU documents the expected
// compile failure; Soft/unit keep the symbol via AURA_ALLOW_IR_SOA_SINGLE_MARK.
#if defined(AURA_PRODUCTION_PACK)
import aura.compiler.ir_soa;
void aura_3489_should_not_compile(aura::compiler::IRFunctionSoA& fn) {
    fn.mark_block_dirty(0);
}
#endif
