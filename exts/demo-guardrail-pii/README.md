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
- Returns `0` on success, negative on error (error defaults to PASS)

## Detected patterns

| Pattern | Example |
|---|---|
| SSN (XXX-XX-XXXX) | `123-45-6789` |
| Credit card (16 digits) | `4111 1111 1111 1111` |
| Email address | `user@example.com` |
| Phone number (10+ digits) | `+1 (555) 123-4567` |
| Credentials | `password:`, `secret_key`, `api_key` |

## Install

```bash
cp -r exts/demo-guardrail-pii ~/.morph/exts/
cc -shared -fPIC -o ~/.morph/exts/demo-guardrail-pii/pii_check.so \
   ~/.morph/exts/demo-guardrail-pii/pii_check.c
```

## Config

Just enable guardrails — auto-discovered from `~/.morph/exts/`:

```toml
guardrail_enabled = true
```

Or manually (for scripts outside `~/.morph/exts/`):

```toml
guardrail_enabled = true

[[react.guardrail_ext_rules]]
name = "pii_check"
hook = "input"
ext_type = "so"
ext_entry = "/path/to/pii_check.so"
action_text = "Do not include personal identifiable information."
```

## Permission

`permissions = 0` — no network, no filesystem, no exec. Runs fully constrained in-process.

## Manual test

```c
/* Compile and test standalone:
   cc -DTEST_MAIN -o pii_test pii_check.c && ./pii_test
*/
#ifdef TEST_MAIN
#include <stdio.h>
int main(void) {
    char *result = NULL;
    guardrail_check("My SSN is 123-45-6789", "pii", "PII check", &result);
    printf("%s\n", result);  // {"pass":false,"reason":"..."}
    free(result);
    guardrail_check("Hello world", "pii", "PII check", &result);
    printf("%s\n", result);  // {"pass":true,"reason":""}
    free(result);
    return 0;
}
#endif
```
