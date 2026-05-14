#!/bin/sh
# Demo translate ext — JSON-RPC 2.0 over stdin/stdout
# Reads a JSON-RPC request line from stdin, writes response to stdout.

set -e
read -r line || true
if [ -z "$line" ]; then
	echo '{"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"parse error: empty input"}}'
	exit 0
fi

# Extract id (default 1)
req_id=$(echo "$line" | sed -n 's/.*"id"[[:space:]]*:[[:space:]]*\([0-9]\+\).*/\1/p')
[ -z "$req_id" ] && req_id=1

# Extract text param — try "text" key
text=$(echo "$line" | sed -n 's/.*"text"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
if [ -z "$text" ]; then
	echo "{\"jsonrpc\":\"2.0\",\"id\":$req_id,\"error\":{\"code\":-32602,\"message\":\"missing text param\"}}"
	exit 0
fi

# Extract target language — accept "target_lang" or "target_language"
target=$(echo "$line" | sed -n 's/.*"target_lang"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
if [ -z "$target" ]; then
	target=$(echo "$line" | sed -n 's/.*"target_language"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
fi
if [ -z "$target" ]; then
	echo "{\"jsonrpc\":\"2.0\",\"id\":$req_id,\"error\":{\"code\":-32602,\"message\":\"missing target_lang or target_language param\"}}"
	exit 0
fi

case "$target" in
	"zh"|"chinese"|"Chinese"|"中文")
		case "$text" in
			"hello world"|"Hello world"|"Hello World")
				result="你好，世界"
				;;
			*)
				result="【翻译结果】 $text"
				;;
		esac
		;;
	"en"|"english"|"英文")
		result="[Translated] $text"
		;;
	"ja"|"japanese"|"日文")
		result="【翻訳結果】 $text"
		;;
	*)
		result="$text (translated to $target)"
		;;
esac

echo "{\"jsonrpc\":\"2.0\",\"id\":$req_id,\"result\":{\"translated\":\"$result\"}}"
