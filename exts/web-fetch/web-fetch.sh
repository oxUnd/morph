#!/bin/sh
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
if [ ! -d "$DIR/node_modules" ]; then
	echo "web-fetch dependencies are missing; run the extension build step first" >&2
	exit 1
fi
exec node --experimental-strip-types "$DIR/web-fetch.ts"
