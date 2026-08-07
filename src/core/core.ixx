export module aura.core;
export import aura.core.arena;
export import aura.core.ast;
// FlatAST decomp step 2: mutation fold pipeline (not re-exported from ast —
// would cycle). Pull via umbrella for service/type_checker convenience.
export import aura.core.ast_mutation_pipeline;
export import aura.core.concepts;
// Issue #1885: layering DAG + cross-layer contracts (ModuleLayer, AllowedDependency).
export import aura.core.module_boundary;
export import aura.core.error;
export import aura.core.envframe_lifetime;
export import aura.core.mutators;
export import aura.core.panic_checkpoint_raii;
// Issue #2555: unified TransactionGuard (MutationBoundary + PanicCheckpoint).
export import aura.core.transaction_guard;