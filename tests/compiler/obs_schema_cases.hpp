// tests/compiler/obs_schema_cases.hpp — observability schema case table.
//
// History: this case table was previously expected at
// tests/domain/cases/obs_schema_cases.hpp but never landed as a file —
// only references in docs / scripts / legacy inventory pointed at it.
// During the R1 tests/ reorg (#1977 / #1957 follow-up), production_sweep_cases.hpp
// was the only case table actually shipped; obs_schema_cases.hpp is a
// planned-but-not-populated table. test_obs_schema_matrix.cpp and
// test_open_issues_phase1_batch.cpp include it as a placeholder so the
// schema matrix can grow incrementally when new stats surfaces ship.
//
// Populate this file when adding a new query:*-stats / engine:* schema
// gate (see tests/README.md "Decision flow → new stats surface").
//
// Currently empty: production flag surfaces are the only ones actually
// folded into test_obs_schema_matrix via production_sweep_cases.hpp.
#pragma once
