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

## Install

```bash
cp -r exts/guardrail-agent-ui-tags ~/.morph/exts/
make -C ~/.morph/exts/guardrail-agent-ui-tags
```

Enable guardrails in the frontend that should use this card protocol:

```toml
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
