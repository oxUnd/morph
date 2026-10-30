# Role-first setup

Run `morph setup` to create or reconfigure an installation. Both global
`morph -c PATH setup` and `morph setup -c PATH` select another configuration.
First interactive startup with a missing configuration opens the same wizard.
Explicit setup also supports line-oriented stdin for scripted configuration;
ordinary noninteractive agent invocations never open setup automatically.

Setup always saves and exits. It does not open the runtime, connect MCP servers,
start a model request, or create a conversation database. Run Morph again with
the same configuration path to load the result. There is no hot reload or
supervisor process.

## Flow

1. **Role:** choose the primary role, then choose whether to keep existing custom
   instructions or generate a new role prompt when reconfiguring.
2. **Account:** configure the text model and credentials.
3. **Preferences:** configure the remaining required models and optionally add
   other capabilities. All required models must have their model/endpoint and
   API base configured; required capability pages have no skip action.
4. **Save:** review and edit any model or return to role selection. Selecting a
   different role rechecks its requirements. Only final confirmation writes files.

| Role | Required model sections |
| --- | --- |
| General assistant | text |
| Software developer | text |
| Photographer | text, vision |
| Photo editor | text, vision, image |
| Writer | text |
| Designer | text, vision, image |
| Video creator | text, vision, image, video |
| Researcher | text |

The role catalog and model requirements are shared by terminal and plain-text
flows. Vision is image understanding, distinct from image generation. Each model
can use a different provider and key. Provider presets and token validation retain
the existing setup behavior. The wizard does not perform network capability or
credential tests: a configured key means present, not provider-verified.

Missing credentials can be completed later. Setup still collects every required
model and saves its settings, prints the needed environment-variable exports,
then exits. Re-run `morph setup` to continue editing, or export the keys before
starting the agent. Nonrequired models can be skipped.

## Prompt generation and skills

Shipped `prompts/scenarios/*.txt` templates share `prompts/common/scenario.txt`.
The developer template also includes the full existing `senior-engineer.txt`.
Role workflows, public source references and behavioral review cases are documented
in [role prompts](role-prompts.md).
Setup renders the selected template through the general `morph-prompt` module,
binding `role.name`, and creates a private Markdown snapshot in the configuration's
own prompt directory, for example `config.toml.prompts/developer.md`. Unchanged
snapshots are reused. If that named file was customized, a new
`developer-XXXXXX.md` snapshot is created and the customized file is preserved. `[prompt]` references its absolute path in `replace` mode. The
runtime continues to load plain text, retaining its built-in core requirements.
An unreadable or invalid template aborts saving; no fallback is silently selected.

Existing prompt file and directory settings are kept by default. Choosing role
instructions creates a new snapshot and clears the configured fragment directory
without changing any original prompt file. Reconfiguration and application
upgrades do not overwrite customized snapshots. A role can be selected while
keeping existing instructions; `[setup].role` records the last selected setup
preset, and `[prompt]` remains the authority for actual runtime instructions.

Bundled skills are already installed with Morph and remain discoverable on the
next start. External skill installation continues through `morph skills add`.
Curated external repositories, default activation policies, permission suggestions,
and generated key files require a separate preset-manifest design. This change
does not auto-download unspecified third-party skills or change skill activation.
Newly installed skills require restarting Morph.

## Reconfiguration and saving

Setup reads current model values without displaying stored API keys. Keeping the
same provider retains existing endpoint/model, token settings, credentials and
extra request parameters. Unedited configuration values are merged using the
TOML parser, including model timeout options, MCP definitions, hooks, permissions,
memory, and exec settings. The editor replaces only changed value tokens and
inserts missing fields or sections. Unchanged values, comments, spacing, line
endings and array-of-table layouts are preserved verbatim. The resulting document
is parsed again and checked against the intended merged values before saving.
Inline tables, dotted keys, quoted keys and multiline strings are supported.

The final save obtains the same `.lock` used by other configuration writers,
checks that the original file has not changed, writes a unique private backup,
and atomically publishes the merged configuration. A concurrent edit produces a
stale-file error rather than overwriting it. Symlink or nonregular configuration
targets are rejected. New configurations are published without replacing an
existing target. Configuration, backup and generated prompt files use mode 0600.

Prompt snapshots are published before their referencing configuration. A failed
configuration save removes the new snapshot unless the configuration was already
published before a directory-sync error. A crash between the two publications
may leave an unused snapshot; there is no multi-file atomic transaction. Existing
snapshots and backups are retained for recovery. Cancellation before confirmation
creates no setup output files.

The terminal wizard stays inline and restores terminal mode after completion,
cancellation and signals. PTY coverage includes 120, 80 and 60-column video
setup, review-time role changes, explicit reconfiguration, concurrent edits,
and production setup exiting even when credentials are already available.
