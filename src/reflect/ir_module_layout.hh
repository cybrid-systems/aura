// ir_module_layout.hh — pinned wire layout of the IR module mirror (#5159).
//
// src/compiler/ir_reflect_serialize.cpp cannot import aura.compiler.ir
// (import std and <meta> cannot share a TU). It keeps a mirror and
// reinterprets the real IRModule through it. These offsets are the real
// type's layout on this ABI. Both the mirror and the real type
// static_assert against them, so a field added in ir.ixx fails the build
// instead of faulting inside the serializer.
#ifndef AURA_REFLECT_IR_MODULE_LAYOUT_HH
#define AURA_REFLECT_IR_MODULE_LAYOUT_HH

// No standard headers: this file is included from a module implementation
// unit that already imported std (#5159).

namespace aura::ir_layout {

inline constexpr unsigned kInstrSize = 52;
inline constexpr unsigned kInstrOpcode = 0;
inline constexpr unsigned kInstrOperands = 4;
inline constexpr unsigned kInstrSource = 20;
inline constexpr unsigned kInstrTypeId = 24;
inline constexpr unsigned kInstrShapeId = 28;
inline constexpr unsigned kInstrLinear = 32;
inline constexpr unsigned kInstrAdt = 36;
inline constexpr unsigned kInstrNarrow = 40;
inline constexpr unsigned kInstrMarker = 44;
inline constexpr unsigned kInstrProvenance = 48;

inline constexpr unsigned kBlockSize = 56;
inline constexpr unsigned kBlockId = 0;
inline constexpr unsigned kBlockInstrs = 8;
inline constexpr unsigned kBlockSuccs = 32;

inline constexpr unsigned kFuncSize = 168;
inline constexpr unsigned kFuncId = 0;
inline constexpr unsigned kFuncName = 8;
inline constexpr unsigned kFuncEntry = 40;
inline constexpr unsigned kFuncBlocks = 48;
inline constexpr unsigned kFuncParams = 72;
inline constexpr unsigned kFuncFreeVars = 96;
inline constexpr unsigned kFuncLocals = 120;
inline constexpr unsigned kFuncArgs = 124;
inline constexpr unsigned kFuncVariadic = 128;
inline constexpr unsigned kFuncRegion = 129;
inline constexpr unsigned kFuncMarker = 130;
inline constexpr unsigned kFuncSpecialized = 132;
inline constexpr unsigned kFuncGeneric = 136;
inline constexpr unsigned kFuncEscape = 144;

// ClosureBridgeData. flat/pool are 16-byte views (shared_ptr on the real
// type, two pointers on the mirror). They are not persisted.
inline constexpr unsigned kBridgeSize = 112;
inline constexpr unsigned kBridgeFlat = 0;
inline constexpr unsigned kBridgePool = 16;
inline constexpr unsigned kBridgeBodyId = 32;
inline constexpr unsigned kBridgeSource = 40;
inline constexpr unsigned kBridgeEpoch = 72;
inline constexpr unsigned kBridgeMarker = 80;
inline constexpr unsigned kBridgeProvenance = 84;
inline constexpr unsigned kBridgeNodeLocs = 88;
inline constexpr unsigned kNodeLocSize = 12;

// IRModule prefix. string_map_ follows string_pool on the real type and is
// not part of the mirror (it is rebuilt by add_string).
inline constexpr unsigned kModFunctions = 0;
inline constexpr unsigned kModBridges = 24;
inline constexpr unsigned kModEntry = 48;
inline constexpr unsigned kModStrings = 56;

} // namespace aura::ir_layout

// Checks whichever aura::ir the including TU can see (the mirror, or the
// real module type). offsetof on these aggregates is the point of the check.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

static_assert(sizeof(aura::ir::IRInstruction) == aura::ir_layout::kInstrSize);
static_assert(__builtin_offsetof(aura::ir::IRInstruction, opcode) == aura::ir_layout::kInstrOpcode);
static_assert(__builtin_offsetof(aura::ir::IRInstruction, operands) ==
              aura::ir_layout::kInstrOperands);
static_assert(__builtin_offsetof(aura::ir::IRInstruction, source_ast_node_id) ==
              aura::ir_layout::kInstrSource);
static_assert(__builtin_offsetof(aura::ir::IRInstruction, type_id) ==
              aura::ir_layout::kInstrTypeId);
static_assert(__builtin_offsetof(aura::ir::IRInstruction, shape_id) ==
              aura::ir_layout::kInstrShapeId);
static_assert(__builtin_offsetof(aura::ir::IRInstruction, linear_ownership_state) ==
              aura::ir_layout::kInstrLinear);
static_assert(__builtin_offsetof(aura::ir::IRInstruction, adt_variant_id) ==
              aura::ir_layout::kInstrAdt);
static_assert(__builtin_offsetof(aura::ir::IRInstruction, narrow_evidence) ==
              aura::ir_layout::kInstrNarrow);
static_assert(__builtin_offsetof(aura::ir::IRInstruction, source_marker) ==
              aura::ir_layout::kInstrMarker);
static_assert(__builtin_offsetof(aura::ir::IRInstruction, provenance) ==
              aura::ir_layout::kInstrProvenance);

static_assert(sizeof(aura::ir::BasicBlock) == aura::ir_layout::kBlockSize);
static_assert(__builtin_offsetof(aura::ir::BasicBlock, id) == aura::ir_layout::kBlockId);
static_assert(__builtin_offsetof(aura::ir::BasicBlock, instructions) ==
              aura::ir_layout::kBlockInstrs);
static_assert(__builtin_offsetof(aura::ir::BasicBlock, successors) == aura::ir_layout::kBlockSuccs);

static_assert(sizeof(aura::ir::IRFunction) == aura::ir_layout::kFuncSize);
static_assert(__builtin_offsetof(aura::ir::IRFunction, id) == aura::ir_layout::kFuncId);
static_assert(__builtin_offsetof(aura::ir::IRFunction, name) == aura::ir_layout::kFuncName);
static_assert(__builtin_offsetof(aura::ir::IRFunction, entry_block) == aura::ir_layout::kFuncEntry);
static_assert(__builtin_offsetof(aura::ir::IRFunction, blocks) == aura::ir_layout::kFuncBlocks);
static_assert(__builtin_offsetof(aura::ir::IRFunction, params) == aura::ir_layout::kFuncParams);
static_assert(__builtin_offsetof(aura::ir::IRFunction, free_vars) ==
              aura::ir_layout::kFuncFreeVars);
static_assert(__builtin_offsetof(aura::ir::IRFunction, local_count) ==
              aura::ir_layout::kFuncLocals);
static_assert(__builtin_offsetof(aura::ir::IRFunction, arg_count) == aura::ir_layout::kFuncArgs);
static_assert(__builtin_offsetof(aura::ir::IRFunction, variadic) == aura::ir_layout::kFuncVariadic);
static_assert(__builtin_offsetof(aura::ir::IRFunction, region) == aura::ir_layout::kFuncRegion);
static_assert(__builtin_offsetof(aura::ir::IRFunction, marker) == aura::ir_layout::kFuncMarker);
static_assert(__builtin_offsetof(aura::ir::IRFunction, specialized_for) ==
              aura::ir_layout::kFuncSpecialized);
static_assert(__builtin_offsetof(aura::ir::IRFunction, generic_id) ==
              aura::ir_layout::kFuncGeneric);
static_assert(__builtin_offsetof(aura::ir::IRFunction, escape_map) == aura::ir_layout::kFuncEscape);

static_assert(sizeof(aura::ir::ClosureBridgeData) == aura::ir_layout::kBridgeSize);
static_assert(__builtin_offsetof(aura::ir::ClosureBridgeData, flat) ==
              aura::ir_layout::kBridgeFlat);
static_assert(__builtin_offsetof(aura::ir::ClosureBridgeData, pool) ==
              aura::ir_layout::kBridgePool);
static_assert(__builtin_offsetof(aura::ir::ClosureBridgeData, pool) -
                  __builtin_offsetof(aura::ir::ClosureBridgeData, flat) ==
              16);
static_assert(__builtin_offsetof(aura::ir::ClosureBridgeData, body_id) ==
              aura::ir_layout::kBridgeBodyId);
static_assert(__builtin_offsetof(aura::ir::ClosureBridgeData, body_source) ==
              aura::ir_layout::kBridgeSource);
static_assert(__builtin_offsetof(aura::ir::ClosureBridgeData, bridge_epoch) ==
              aura::ir_layout::kBridgeEpoch);
static_assert(__builtin_offsetof(aura::ir::ClosureBridgeData, source_marker) ==
              aura::ir_layout::kBridgeMarker);
static_assert(__builtin_offsetof(aura::ir::ClosureBridgeData, provenance) ==
              aura::ir_layout::kBridgeProvenance);
static_assert(__builtin_offsetof(aura::ir::ClosureBridgeData, node_locs) ==
              aura::ir_layout::kBridgeNodeLocs);
static_assert(sizeof(aura::ir::ClosureBridgeData::NodeLoc) == aura::ir_layout::kNodeLocSize);

static_assert(__builtin_offsetof(aura::ir::IRModule, functions) == aura::ir_layout::kModFunctions);
static_assert(__builtin_offsetof(aura::ir::IRModule, closure_bridge) ==
              aura::ir_layout::kModBridges);
static_assert(__builtin_offsetof(aura::ir::IRModule, entry_function_id) ==
              aura::ir_layout::kModEntry);
static_assert(__builtin_offsetof(aura::ir::IRModule, string_pool) == aura::ir_layout::kModStrings);

#pragma GCC diagnostic pop

#endif
