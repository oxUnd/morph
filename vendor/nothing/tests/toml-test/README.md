# toml-test corpus (TOML 1.1)

Vendored test set selected by the upstream
`tests/files-toml-1.1.0` manifest from the official
[toml-lang/toml-test][upstream] suite.

| Field | Value |
| --- | --- |
| Source | https://github.com/toml-lang/toml-test |
| Commit | `96fd088448ec0f4d9f061c730ed7854399c46faf` |
| Collected | 2026-11-11 |
| License | MIT, see `LICENSE` |
| Manifest | `files-toml-1.1.0` |

## Contents

- `valid/**/*.toml` — documents that must parse successfully (218 files).
- `invalid/**/*.toml` — documents that must be rejected (502 files).
- `files-toml-1.1.0` — the upstream manifest used to select these files.

Relative directory layout matches `tests/` in the upstream repository.

## Exclusions

- TOML 1.0-only cases not listed in the 1.1 manifest (for example the old
  inline-table linebreak/trailing-comma, missing-seconds and `\x` rejection
  tests) are not vendored.
- All non-`.toml` upstream files (JSON expectation files, generator scripts,
  `*.json` companions) are not vendored.

## Use

`tests/test_toml_corpus.c` walks this tree recursively. The corpus root can be
passed as `argv[1]`, otherwise it falls back to the compile-time
`NB_TOML_TEST_DIR` macro when defined, then to `tests/toml-test` relative to
the working directory.

[upstream]: https://github.com/toml-lang/toml-test
