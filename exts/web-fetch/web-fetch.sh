#!/bin/sh
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
if [ ! -d "$DIR/node_modules" ]; then
	npm install --production --silent --prefix "$DIR" 2>/dev/null
fi
exec node --experimental-strip-types "$DIR/web-fetch.ts"
