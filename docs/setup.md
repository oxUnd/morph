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

1. **Role:** choose the primary role. Setup prepares presets for every required
   model; optional capabilities stay off unless previously configured.
2. **Overview:** see actual text, vision, image and video model names, key status,
   optional capabilities, role MCP defaults and approval/host-execution settings.
   Accept the defaults or open only a setting that needs changing.

The role picker uses one numbered row per role. Only the highlighted role's
description, required models and MCP recommendations are shown below the list.
Arrow keys, Tab and number keys move the highlight; Enter applies it. Returning
from the overview uses the title **Change your role**, marks the current preset,
and leaves it unchanged when cancelled. Confirming the same role again does not
add another history receipt. Smaller terminals scroll the list while retaining
the selected role's model requirements.

With usable credentials already configured or exported, selecting the role and
confirming the overview completes setup. There is no mandatory provider tour,
thinking confirmation or sequence of optional-capability questions. If credentials
are missing, **Connect accounts and save** requests the missing accounts. A key
entered for a default provider is shared with other models using that same missing
credential source; ready accounts, different providers and explicitly different
environment variables are preserved. **Save for later** saves a draft directly.
Export instructions are printed once per missing variable, not once per model.

Every model edit keeps a visible context header identifying **Text model**,
**Vision model**, **Image model** or **Video model**, together with its provider
and current model. Provider, model, account and advanced settings are editable
independently. Token limits and API URLs remain in advanced settings; DeepSeek
thinking is an optional model setting. Required models cannot be disabled, and
incomplete model metadata must be completed before saving. A missing key can be
deferred, but an absent model/endpoint or invalid token range cannot.

Existing prompt sources are kept automatically when reconfiguring. The overview's
Instructions item can explicitly select a new role prompt. Returning to role
selection fills newly required defaults without discarding existing model choices.
Only final confirmation writes files. Esc in a field cancels that draft; Esc from
its settings menu returns to the overview. Cancelling setup writes nothing.

The line-oriented fallback also defaults to role then overview/save. Its explicit
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

Setup writes these defaults, including during explicit reconfiguration:

```toml
[react]
hitl_auto_approve_readonly = true
hitl_auto_approve_media = true

[exec]
allow_host_execution = true
```

Read-only tools, including MCP tools marked read-only, bypass tool-call approval.
Built-in image/video generation, editing, conversion, annotation and analysis also
bypass that approval when the media setting is enabled. These exemptions take
precedence over `hitl_tools`. Media operations keep their mutation metadata and
filesystem checks. Other tool approvals, pre-tool hooks and exec's per-program
approval continue to apply. Host execution permits explicit `exec` calls with
`host=true`; it does not make every command a host command or auto-approve it.
Existing `hitl_enabled`, network settings, grants and permission profiles remain
unchanged; a new installation retains setup's `exec.network=true` default.

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

The terminal wizard stays inline and restores terminal mode after completion,
cancellation and signals. PTY coverage includes 120, 80 and 60-column video
setup, review-time role changes, explicit reconfiguration, concurrent edits,
and production setup exiting even when credentials are already available.
