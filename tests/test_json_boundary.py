#!/usr/bin/env python3
"""Keep the vendored JSON dependency private to its owning module."""
import re
import sys
from pathlib import Path


def main():
    root = Path(sys.argv[1])
    backend = {"src/util/json.c", "src/util/json_parse.c", "src/util/json_internal.h"}
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\bcJSON\w*\b', re.S)
    failures = []
    for path in sorted((root / "src").rglob("*")):
        if path.suffix not in (".c", ".h"):
            continue
        name = path.relative_to(root).as_posix()
        if name in backend:
            continue
        source = path.read_text()
        if re.search(r'#\s*include\s*["<](?:[^">]*/)?(?:cJSON|json_internal)\.h[">]', source):
            failures.append(f"{name}: backend header must remain private")
        for token in tokens.finditer(source):
            if token.group().startswith("cJSON"):
                line = source.count("\n", 0, token.start()) + 1
                failures.append(f"{name}:{line}: backend symbol {token.group()}")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("JSON backend is private to src/util/json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
