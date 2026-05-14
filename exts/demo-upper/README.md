# demo-upper

A `.so` ext that converts text to uppercase.

## Type

`so` — loaded via `dlopen`, runs in-process.

## Install

Copy to `~/.morph/exts/`:

```bash
cp -r exts/demo-upper ~/.morph/exts/
# Build the .so (if not already compiled):
cc -shared -fPIC -o ~/.morph/exts/demo-upper/upper.so ~/.morph/exts/demo-upper/upper.c
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

Exports `int ext_run(const char *args_json, char **result_json)` from `upper.so`.
