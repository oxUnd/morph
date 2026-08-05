---
name: morph-usage
description: Answer questions about how to use, configure, operate, or troubleshoot Morph itself. Activate when the user asks about Morph commands, config.toml, model providers, API keys, tools, skills, MCP, extensions, memory, credits, shell permissions, or asks Morph to help edit its own configuration.
metadata:
  short-description: Morph usage and configuration manual
---

# Morph Usage Skill

Use this skill whenever the user asks how Morph works or how to configure it.
Before answering, ground the answer in the Vim-style manual at
`docs/morph.txt`. If that relative path is not readable, use the activated
skill `dir` value and try `../../morph.txt` for an installed tree, then
`../../docs/morph.txt` for a source tree. Read only the relevant tagged
sections unless the user asks for a broad overview.

## Routing

- Quick start: read `*morph-quickstart*`.
- Configuration: read `*morph-config*` and, when model-related, `*morph-models*`.
- CLI commands: read `*morph-cli*`.
- Built-in tools or permissions: read `*morph-tools*`.
- Skills: read `*morph-skills*`.
- MCP: read `*morph-mcp*`.
- Extensions: read `*morph-exts*`.
- Memory, context, or credits: read `*morph-memory*`.
- Config editing: read `*morph-config-edit*`.
- Errors and failures: read `*morph-troubleshooting*`.

## Answering Rules

- Answer in the user's language.
- Prefer concrete commands and TOML snippets over broad explanations.
- Mention the config path `~/.morph/config.toml` unless the user is running
  with `-c <path>` or a different active config is known.
- Do not invent provider-specific model IDs. If the user has not specified a
  provider/model, show a minimal template and say what they need to fill in.
- Do not include raw API key values in examples. Use `api_key_env`.

## Editing Config

When the user asks Morph to change its own configuration:

1. Inspect the current config if available.
2. State the exact path and setting changes.
3. Ask for approval before writing.
4. Use `config_edit` with a minimal patch when available.
5. Validate that the resulting TOML parses.
6. Tell the user whether Morph must be restarted.

Never use raw `apply_patch` or shell commands on the active config unless no
restricted config-editing tool exists and the user explicitly approves that
fallback. A successful edit requires a normal Morph restart; do not claim the
running process hot-reloaded startup state.
