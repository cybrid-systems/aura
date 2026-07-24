export module aura.core;
export import aura.core.arena;
export import aura.core.ast;
export import aura.core.concepts;
// Issue #1885: layering DAG + cross-layer contracts (ModuleLayer, AllowedDependency).
export import aura.core.module_boundary;
export import aura.core.error;
export import aura.core.envframe_lifetime;
export import aura.core.mutators;
export import aura.core.panic_checkpoint_raii;