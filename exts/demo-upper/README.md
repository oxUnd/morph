# demo-upper

The CLI loads this demo as the native `upper` tool. Its `ext_run` entry point
accepts a JSON object with a `text` string. For example, `{"text":"hello"}`
returns `{"result":"HELLO"}`.

## Build and install

From the Morph repository root after configuring the default build directory:

```sh
mkdir -p ~/.morph/exts/demo-upper
cc -shared -fPIC -Ibuild/_deps/nbase-src/include exts/demo-upper/upper.c -o ~/.morph/exts/demo-upper/upper.so
cp exts/demo-upper/manifest.toml ~/.morph/exts/demo-upper/
```

When using a local nbase checkout, point `-I` at its `include` directory.
Restart Morph to discover the tool. Native extensions execute inside the host
process. The desktop runtime refuses a native tool call under restricted access
because it cannot impose a child-process sandbox on the call.
[Extension manifest](manifest.toml) · [Permission policy](../../docs/permissions-design.md)
