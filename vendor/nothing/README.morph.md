# nbase vendored snapshot

- Upstream: https://codeberg.org/oxund/nothing
- Library/API: nbase / `nb_`
- Pinned commit: `ef0ddba471bd82857f1cc74b9db795446b15e74d`
  (upstream `feat/morph-nbase-migration`)
- Version: `0.1.0`

Morph and its embedded nothing-tui link the same `nbase::nbase` target built
from this directory. The pinned revision includes the native file, thread,
Unicode and process APIs used by the migration.

## Local patch

`src/platform/posix/path.c` uses the procfs executable-path implementation
on Cygwin as well as Linux. The executable-path regression in
`tests/test_fs_extra.c` checks the resulting absolute path.

## License

The pinned upstream commit does not contain a license file for the library
itself. `tests/toml-test/LICENSE` covers only the TOML test corpus and does
not extend to nbase. Distribution as a morph dependency requires an explicit
license from upstream; see the migration assessment,
[section 9](https://github.com/oxUnd/morph/blob/feat/nothing-migration/docs/nothing-migration-assessment.zh-CN.md#9-当前待解决事项).
