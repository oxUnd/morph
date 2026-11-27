---
name: morph-usage
description: Help users configure, use, or troubleshoot Morph itself, including first-run setup, model adapters, thinking, CLI commands, recaps, tools, skills, MCP, scheduled tasks, and config changes. Use for Morph product questions, not ordinary tasks performed with Morph.
metadata:
  short-description: Morph usage and configuration manual
---

# Morph Usage

Ground answers in the manual shipped with the skill's version. Using the
activated skill `dir`, resolve `../../morph.txt` for a build/install tree or
`../../docs/morph.txt` for a source checkout. These paths are relative to that
skill directory, not the process working directory. For example,
`<prefix>/share/morph/skills/morph-usage` reads `<prefix>/share/morph/morph.txt`,
while `<checkout>/skills/morph-usage` reads `<checkout>/docs/morph.txt`.
Fall back to `docs/morph.txt` in the working directory only when the skill-relative
manual is unavailable. Read the relevant tagged sections, not the entire
manual. If none is readable, state that limitation rather than inventing
commands or settings.

For setup, model configuration, thinking, recap settings, process waits, or host automation, read
[references/configuration.md](references/configuration.md). It explains the
current configuration distinctions and gives a complete DeepSeek example.
When working in the source checkout, resolve discrepancies against
`config.toml.example`, `src/config/schema.c`, and the corresponding implementation.
Do not assume an older installed binary supports unreleased source changes.

## Routing

- First run and onboarding: `*morph-quickstart*`.
- Config paths, database storage, sections, keys, and models: `*morph-config*`,
  `*morph-models*`.
- Commands, session switching, and interactive details: `*morph-cli*`.
- Tools and filesystem or shell permissions: `*morph-tools*`.
- Process wait ranges and `exec.max_wait_ms`: `*morph-tools*` and
  [Process wait budgets](references/configuration.md#process-wait-budgets).
- AppleScript/System Events errors or explicit host execution: `*morph-tools*`
  and [Host execution](references/configuration.md#explicit-host-execution).
- Skills, MCP, or extensions: `*morph-skills*`, `*morph-mcp*`, `*morph-exts*`.
- Checkpoints and rewind: `*morph-checkpoints*`.
- Lifecycle hook configuration and trust: `*morph-hooks*`.
- Scheduled work and delivered results: `*morph-tasks*`.
- Context, compression, recaps, memory, or credits: `*morph-memory*`.
- Config validation and active-config changes: `*morph-config-validate*`.
- Errors: `*morph-troubleshooting*` and the relevant model/tool section.

## Answering

- Answer in the user's language with the smallest useful command or TOML edit.
- Identify the active config. Default: `~/.morph/config.toml`; `-c <path>`
  overrides it. Do not edit the default when a different active path is known.
- Prefer the first-run wizard for a missing config. It uses append mode, keeps
  completed answers in scrollback, and puts technical fields in Advanced settings.
  Use `morph setup` to reconfigure an existing file. The final save backs it up and merges the changes.
- Distinguish text, vision understanding, image generation, and video generation.
  Configuring one does not automatically configure the others.
- Treat `adapter` as the wire-protocol selector, not another model/provider name.
  Keep provider-specific fields scoped to the selected adapter.
- Do not invent model IDs, capability support, token limits, or prices. Preserve
  a user's deployment/endpoint ID; setup defaults and hand-written defaults can differ.
- Use `api_key_env` in examples and explain that it must be exported before launch.
  Setup also permits a hidden key saved in the local config. Never echo real keys.
- Separate `/recap` (a user-facing catch-up) from `/compress` (context reduction).
  Neither thinking previews nor recaps require inventing a separate model setting.
- Explain the actual HTTP status and provider detail when available. A 400
  protocol/config error is not evidence of a network outage or a reason to keep retrying.

## History navigation

Back to bottom occupies a separate normal-background row above the composer.
Live and restored sub-agent mail is grouped with a successful mailbox wait when
available: Finished waiting lists returned agent paths and elbow-line rows show
short report previews. Ordinary messages remain updates, not completed tasks.
Show details and Hide details toggle full envelopes. Explicit human input stays
a user message.

## Empty conversations

Empty chat views show Start a new task with `/help`, `/resume`, `/skills`, and
`/agents`. Clicking an action fills the composer without running it. The card
hides after the first message and returns for a new empty session.

## Multi-agent CLI

`/share` exports the latest answered parent turn. Only human messages appear as
YOU; received sub-agent mail and hook feedback are labeled entries in the
collapsed execution process. Child transcripts and forked history are not appended.
New records have explicit provenance; old untagged steering is treated as agent
mail only when it matches the complete mailbox transport header. An explicit
user source always remains a human question.

Session restoration context, compaction checkpoints and system-origin prompts
are not human questions and are omitted from the export. A user-authored request
to continue after restoring remains part of YOU. Loading a session with `/resume`
does not itself create a user message.

The `Working` label has a repeating gray-to-white sweep driven by TUI animation.
No-color output remains steady.

Live sub-agent activity and waiting status appear inside the CLI composer.
Multiple agents use flat gray `└` rows aligned with the `Working` text, without
circle markers before their paths.
Click an agent path to open its feed;
`/subagents /root/name` opens the same feed by command.

## Editing the Active Config

1. Inspect the current config and prepare a minimal edit to the requested settings.
2. Explain the target path and change. Honor authorization already given for that
   exact change; do not add a redundant conversational approval step.
3. Use an authorized file-editing tool to make the change, preserving unrelated
   sections and comments. Honor shared write permissions and any denial.
4. Use `config_validate` with no arguments to check the active file after editing,
   or pass `path` for another config file. Complete TOML can be checked with
   `content` before writing. The validator is read-only and reports legality;
   it does not create backups, apply settings, or test network connections.
5. Report validation errors and warnings. A changed config requires a normal Morph
   restart; a successful check does not mean startup state was hot-reloaded.

When changing `extra_body_json`, merge the requested fields with the existing object.
