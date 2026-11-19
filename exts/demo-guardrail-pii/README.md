# demo-guardrail-pii

A `.so` guardrail ext that detects personally identifiable information (PII) in text.

## Type

`so` — loaded via `dlopen`, runs in-process. Exported symbol: `guardrail_check`.

## Auto-discovery

This ext has `purpose = "guardrail"` in its manifest, so it is **automatically
discovered and registered** when placed in `~/.morph/exts/`. No config needed
beyond `guardrail_enabled = true`.

The manifest includes `hook = "input"` and `action_text`, which are read
automatically during discovery.

## Entry point

```c
int guardrail_check(const char *text, const char *rule,
                    const char *description, char **result_json);
```

- `text` — the content being checked (user input, tool output, or proposed answer)
- `rule` — the rule name from config
- `description` — the rule description from config
- `result_json` — must be set to a `malloc`'d JSON string: `{"pass":bool,"reason":"..."}`
- Returns `0` on success, a negative status on error. The configured guardrail error policy determines how the runtime handles checker failure

## Detected patterns

| Pattern | Example |
|---|---|
| SSN (XXX-XX-XXXX) | `123-45-6789` |
| Credit card (16 digits) | `4111 1111 1111 1111` |
| Email address | `user@example.com` |
| Phone number (10+ digits) | `+1 (555) 123-4567` |
| Credentials | `password:`, `secret_key`, `api_key` |

## Install

Run from the repository root after configuring the default build directory.
For a local nbase checkout, point `-I` at its `include` directory.

```bash
mkdir -p ~/.morph/exts/demo-guardrail-pii
cp exts/demo-guardrail-pii/manifest.toml ~/.morph/exts/demo-guardrail-pii/
cc -shared -fPIC -Ibuild/_deps/nbase-src/include -o ~/.morph/exts/demo-guardrail-pii/pii_check.so \
   exts/demo-guardrail-pii/pii_check.c
```

## Config

Just enable guardrails — auto-discovered from `~/.morph/exts/`:

```toml
[react]
guardrail_enabled = true
```

Or manually (for scripts outside `~/.morph/exts/`):

```toml
[react]
guardrail_enabled = true

[[react.guardrail_ext_rules]]
name = "pii_check"
hook = "input"
ext_type = "so"
ext_entry = "/path/to/pii_check.so"
action_text = "Do not include personal identifiable information."
```

## Execution

The runtime loads the checker with `dlopen`. It executes with the permissions
of the host process. Its manifest declares no subprocess sandbox capabilities.
[Guardrail behavior and error policy](../../docs/guardrails.md)
