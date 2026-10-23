---
name: morph-usage
description: Help users configure, use, or troubleshoot Morph itself, including first-run setup, model adapters, thinking, CLI commands, recaps, tools, skills, MCP, scheduled tasks, and config changes. Use for Morph product questions, not ordinary tasks performed with Morph.
metadata:
  short-description: Morph usage and configuration manual
---

# Morph Usage

Ground answers in the manual shipped with the skill's version. Using the
activated skill `dir`, try `../../morph.txt` for an installed tree or
`../../docs/morph.txt` for a source checkout. Fall back to `docs/morph.txt`
in the working directory. Read the relevant tagged sections, not the entire
manual. If none is readable, state that limitation rather than inventing
commands or settings.

For setup, model configuration, thinking, or recap settings, also read
[references/configuration.md](references/configuration.md). It explains the
current configuration distinctions and gives a complete DeepSeek example.
When working in the source checkout, resolve discrepancies against
`config.toml.example`, `src/config/schema.c`, and the corresponding implementation.
Do not assume an older installed binary supports unreleased source changes.

## Routing

- First run and onboarding: `*morph-quickstart*`.
- Config paths, sections, keys, and models: `*morph-config*`, `*morph-models*`.
- Commands, session switching, and interactive details: `*morph-cli*`.
- Tools and filesystem or shell permissions: `*morph-tools*`.
- Skills, MCP, or extensions: `*morph-skills*`, `*morph-mcp*`, `*morph-exts*`.
- Scheduled work and delivered results: `*morph-tasks*`.
- Context, compression, recaps, memory, or credits: `*morph-memory*`.
- Active-config changes: `*morph-config-edit*`.
- Errors: `*morph-troubleshooting*` and the relevant model/tool section.

## Answering

- Answer in the user's language with the smallest useful command or TOML edit.
- Identify the active config. Default: `~/.morph/config.toml`; `-c <path>`
  overrides it. Do not edit the default when a different active path is known.
- Prefer the first-run wizard for a missing config. It uses append mode, keeps
  completed answers in scrollback, and puts technical fields in Advanced settings.
  Existing configs are not overwritten or automatically reconfigured by setup.
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

## Editing the Active Config

1. Inspect the current config and prepare a minimal edit to the requested settings.
2. Explain the target path and change. Honor authorization already given for that
   exact change; do not add a redundant conversational approval step.
3. Use `config_edit` when available. It enforces the active-file operation approval,
   validates the full config, creates a backup, and writes atomically. Do not bypass
   its approval or denial by switching to another write tool.
4. Report the tool's actual result. A changed config requires a normal Morph restart;
   a successful write does not mean startup state was hot-reloaded.

Use a raw file-editing fallback only when `config_edit` is unavailable and the
user authorizes that fallback. Preserve unrelated sections and comments. When
changing `extra_body_json`, merge the requested fields with the existing object.
