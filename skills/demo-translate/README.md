# demo-translate

An `exec` skill that translates text between languages (hardcoded demo responses).

## Type

`exec` — runs as a subprocess communicating over stdin/stdout via JSON-RPC 2.0.

## Install

Copy to `~/.morph/skills/`:

```bash
cp -r skills/demo-translate ~/.morph/skills/
chmod +x ~/.morph/skills/demo-translate/translate.sh
```

## Usage

The ReAct loop calls it automatically:

```
> translate hello world to Chinese
[Action] translate({"text":"hello world","target_lang":"zh"})
[Obs]   {"result":"你好，世界"}
```

## Manual test

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"run","params":{"text":"hello world","target_lang":"zh"}}' | \
  ~/.morph/skills/demo-translate/translate.sh
```

Output:

```json
{"jsonrpc":"2.0","id":1,"result":{"translated":"你好，世界"}}
```

## Permission

`permissions = 4` (`SKILL_PERM_EXEC`) — required because the shell script needs to fork child processes (`sed`, `echo`).

## Supported languages

Hardcoded demo: `zh`/`en`/`ja`. Unknown languages return a fallback response.