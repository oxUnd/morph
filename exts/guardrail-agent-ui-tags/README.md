# guardrail-agent-ui-tags

A `.so` guardrail ext that validates Agent UI card tags in assistant output.

The rule is intended for mobile frontends that render Agent UI cards. It is not
a core builtin rule because not every frontend understands or needs these tags.

## Behavior

Allowed tags:

- `m:speak`
- `m:vocab`
- `m:sentence`
- `m:button`
- `m:navigate`
- `m:highlight`
- `m:copy`

Any other `<m:...>` tag fails the guardrail. In particular, `ask_user` is a
tool call and must not be rendered as `<m:ask_user>`, `<m:question>`, or
`<m:choice>`.

## Build and enable

For a host build, run this from the Morph repository root after CMake has
fetched nbase. A custom checkout can supply its own include path.

```sh
cc -shared -fPIC -Ibuild/_deps/nbase-src/include exts/guardrail-agent-ui-tags/agent_ui_tags.c -o agent_ui_tags.so
```

For deployment, build for the target architecture and package the library beside
`manifest.toml` in the frontend's extensions directory. The manifest selects
Android and iOS. Enable guardrails in that frontend's configuration.

```toml
[react]
guardrail_enabled = true
```

## Entry Point

```c
int guardrail_check(const char *text, const char *rule,
                    const char *description, char **result_json);
```

The result is a `malloc`'d JSON object:

```json
{"pass":true,"reason":""}
```
