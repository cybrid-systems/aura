// Issue #2294 / #2267: RootRemapPass implementation lives in the module
// interface unit (root_remap_pass.ixx). This TU is intentionally empty —
// kept so path references / inventory scripts that look for the .cpp
// continue to resolve. Do not add non-module definitions here.
