# demo-upper

A `.so` skill that converts text to uppercase.

## Type

`so` — loaded via `dlopen`, runs in-process.

## Install

Copy to `~/.morph/skills/`:

```bash
cp -r skills/demo-upper ~/.morph/skills/
# Build the .so (if not already compiled):
cc -shared -fPIC -o ~/.morph/skills/demo-upper/upper.so ~/.morph/skills/demo-upper/upper.c
```

## Usage

The ReAct loop calls it automatically:

```
> convert "hello world" to uppercase
[Action] upper({"text":"hello world"})
[Obs]   {"result":"HELLO WORLD"}
```

## Manual test

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"run","params":{"text":"hello"}}' | \
  upper.so  # not applicable — loaded via dlopen, not standalone
```

## Permission

`permissions = 0` — no network, no filesystem, no exec. Runs fully constrained.

## Entry point

Exports `int skill_run(const char *args_json, char **result_json)` from `upper.so`.