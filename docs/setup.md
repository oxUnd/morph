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

## Form

Setup is a native TUI page built from card, form, label, text field, drop-down,
tab, check box and button controls. Text, Vision, Images and Video have separate
tabs. The current model scrolls independently while the role, account status
and save actions remain visible.

The **Role** drop-down selects the primary preset. It fills defaults for newly
required models without discarding existing choices. Required models are always
enabled; vision, image and video can be enabled or disabled with their standard
check boxes.

Each tab shows its provider, model name and account source. **Advanced settings**
reveals the API base and, for text and vision, the context window, max output and
DeepSeek thinking option. Choosing a custom provider opens advanced settings
automatically. **Account** is a drop-down: use an environment variable, save a
masked key in the private config, or set up later. Only the selected source is
written.
The text model's advanced section also summarizes the role's integrations and
permission defaults.

The **Instructions** drop-down keeps existing or built-in instructions, or
selects the role prompt. Changes stay in memory until **Save and exit** or
**Save for later**. Esc closes a drop-down; Esc from the form and Ctrl+C cancel
without writing. **Save for later** also defers an unfilled direct API key.
Invalid URLs and token ranges stay on the field with an inline
form error. A missing key is a warning, not a blocked save: setup saves the draft
and prints the required export command.

With credentials already configured or exported, the form starts at **Ready to
save** and one Save completes setup. There is no provider tour, thinking
confirmation or sequence of optional-capability questions. Export instructions
are printed once per missing variable, not once per model.

Existing prompt sources are kept automatically when reconfiguring. The
Instructions drop-down can explicitly select a new role prompt. Selecting a role
fills newly required defaults without discarding existing model choices. Only
the final save writes files.

Click fields to edit, click menus and tabs to change settings, and click buttons
to save or cancel. The wheel and native scrollbars scroll the current model.
Tab/Shift+Tab move focus; Enter moves to the next text field or opens a menu.
F10 or Ctrl+Enter saves. Arrow keys navigate an open menu or the model tabs, Space
toggles a check box, and Esc closes a menu before cancelling the page. Drafts
and cursor positions survive resizing. The layout supports 120, 80, 60 and 40
columns; narrow terminals stack model labels above their controls.

The line-oriented fallback keeps its role then overview/save flow. Its explicit
Customize action opens the full model walkthrough for scripted advanced editing.

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
can use a different provider and key. Existing provider presets and token
validation apply to the generated defaults. The wizard does not perform network capability or
credential tests: a configured key means present, not provider-verified.

Missing credentials can be completed later. Setup still collects every required
model and saves its settings, prints the needed environment-variable exports,
then exits. Re-run `morph setup` to continue editing, or export the keys before
starting the agent. Nonrequired models can be skipped.

For each model, setup saves exactly one nonempty credential source: `api_key`
for a locally stored key, or `api_key_env` for an environment variable. The other
field is absent, including when reconfiguring an older file that contained both.
Keeping an existing direct key retains its runtime precedence and removes the
environment-variable field. Unconfigured models with no credentials have neither
field. Credential cleanup preserves unrelated settings and original backups.

## MCP and approval defaults

Setup adds Exa search and webpage reading for every role. Developer and researcher
presets add Playwright browser automation. On macOS, developer, photographer,
photo editor, designer and video creator presets add Computer Use. Designers also
receive a Figma desktop MCP entry that starts disconnected until Figma's server
is enabled. See [MCP preset sources and prerequisites](setup-mcp.md).

Existing server definitions, credentials and connection preferences are preserved.
Names, matching endpoints and recognized package aliases prevent duplicate
defaults. Switching roles adds missing recommendations; it does not delete
previously configured integrations. Existing array-of-table formatting is
preserved. Inline arrays remain inline and retain their original entries.

Setup does not launch MCP processes or download dependencies. Exa uses its
official hosted endpoint. Local MCP presets auto-connect on the next start only
when their launcher is present during setup. Missing launchers produce a saved,
disconnected entry and setup prints the prerequisites. The pinned packages may
be downloaded by their launchers on the first connection. After installing a
missing launcher, use `/mcp connect NAME` or enable that server's `auto_connect`.
Configured does not mean authenticated, OS-authorized or successfully connected.

The setup writer still emits these legacy fields, including during explicit reconfiguration:

```toml
[react]
hitl_auto_approve_readonly = true
hitl_auto_approve_media = true

[exec]
allow_host_execution = true
```

A new configuration also receives `exec.network = true`. On load, the runtime
maps legacy permission settings to Ask (workspace access, on-request approval,
human reviewer). An explicit `[permissions]` policy takes precedence. Tool
metadata, the requested operation and existing grants determine which calls need
review. For example, `host=true` requests host execution and a media operation can
require permission to write its output. Inspect the effective policy with
`/permissions status`. [Permission policy](permissions-design.md) · [Exec modes](sandbox.md)

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
memory, and exec settings other than setup's explicit policy defaults above.
The editor replaces only changed value tokens and
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

The native Application owns input decoding, mouse capture, focus, popups and
rendering. It restores the previous terminal screen and modes after completion,
cancellation and signals. PTY coverage includes mouse and keyboard editing,
menus, model tabs, scrolling, draft preservation during resize, password masking,
explicit reconfiguration, concurrent edits and production setup.
