// agent_name_table_fwd.h — Issue #2078: forward declaration shim for
// AgentNameTable, kept in the global module fragment (not the purview)
// so it matches the definition's module-attachment.
//
// Why this header exists:
// evaluator.ixx holds the storage as `std::unique_ptr<AgentNameTable>`
// but the full definition (agent_name_table.h) pulls in orch headers
// (orch/orch.h → orch/agent_spawn.h → serve/fiber.h) which would conflict
// with service.ixx's direct include of serve/fiber.h via thread_local
// weak-symbol generation if brought into evaluator.ixx's global fragment.
// So evaluator.ixx does NOT include agent_name_table.h — it includes
// only this fwd shim (in its global fragment, alongside other #includes),
// then forward-declares AgentNameTable via the shim's contents.
//
// C++20 modules caveat: a `struct X;` declared directly in the module
// purview (e.g. `namespace aura::compiler { struct AgentNameTable; }`
// in evaluator.ixx's purview) is "attached to" the module, while a
// `struct X { ... }` defined in a header included from a TU's global
// fragment is "module-orphaned" (or attached differently). They are
// treated as DIFFERENT types by the module system, which fails
// `std::unique_ptr<AgentNameTable>` destruction / std::make_unique
// (sizeof incomplete / different module errors). Keeping the forward
// declaration in the global fragment via #include makes both the forward
// decl and the definition "module-orphaned" — same type.
//
// The actual methods (put/find/drain_for_cleanup/size) live in
// agent_name_table.h and require the full definition; they are invoked
// from evaluator_ctor.cpp / evaluator_primitives_agent.cpp / the test
// file which include the full header.

#ifndef AURA_COMPILER_AGENT_NAME_TABLE_FWD_H
#define AURA_COMPILER_AGENT_NAME_TABLE_FWD_H

namespace aura::compiler {
// Forward declaration only — definition lives in agent_name_table.h.
// Both this declaration and the definition live in the global module
// fragment (not the purview), so C++20 modules treats them as the same
// type and std::unique_ptr<AgentNameTable> works end-to-end.
struct AgentNameTable;
} // namespace aura::compiler

#endif // AURA_COMPILER_AGENT_NAME_TABLE_FWD_H