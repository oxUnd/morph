#!/usr/bin/env bash

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOS=(
  "$DIR/../"
  "$DIR/../fronts/ios"
  "$DIR/../fronts/android"
  "$DIR/../fronts/morph-markdown"
  "$DIR/../vendor/mathjax-c"
)

for repo in "${REPOS[@]}"; do
  echo "==> Pulling $repo"
  git -C "$repo" pull
  echo ""
done
