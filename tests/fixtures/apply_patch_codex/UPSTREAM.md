# Codex apply_patch compatibility fixtures

Imported verbatim from OpenAI Codex commit
`757c151a0e920c6238801866a3d13e010dfeddb8`:

`codex-rs/apply-patch/tests/fixtures/scenarios`

The imported material is distributed under the Apache License 2.0; see
`LICENSE.openai-codex` in this directory.

The upstream README defines these as portable end-to-end specification tests.
Morph intentionally keeps its workspace security boundary: absolute paths,
parent traversal, and symbolic-link targets remain rejected even though the
standalone Codex crate accepts absolute paths.

Upstream test mapping:

- `tests/suite/scenarios.rs`: ported by `CodexApplyPatchScenarios`.
- `src/parser.rs`, `src/seek_sequence.rs`, and applicator tests in `src/lib.rs`:
  ported as focused tests in `test_apply_patch_codex.cpp`.
- `tests/suite/cli.rs`, `tests/suite/tool.rs`: filesystem behavior is covered;
  Rust CLI stdout/stderr plumbing is not applicable to Morph's custom tool API.
- `src/invocation.rs` and Codex core handler/runtime tests: not applicable;
  these test shell interception, sandbox routing, and Codex protocol objects,
  none of which are part of Morph's raw-text `apply_patch` tool.
