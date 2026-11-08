#!/usr/bin/env python3
"""Keep project-owned C-family code free of unstructured jumps."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("src", "tests", "exts", "examples")
SOURCE_SUFFIXES = frozenset((".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".m", ".mm", ".inc"))
TOKENS = re.compile(
    r'(?:(?:u8|[uUL])?R"(?P<delimiter>[^\s()\\]{0,16})\(.*?\)(?P=delimiter)")'
    r'|/\*.*?\*/'
    r'|//[^\n]*'
    r'|"(?:\\.|[^"\\])*"'
    r"|'(?:\\.|[^'\\])*'"
    r"|\b[0-9][a-zA-Z0-9_.']*"
    r'|(?P<jump>\bgoto\b)',
    re.DOTALL,
)


def jump_lines(source):
    """Scan all branches and macros, after C line splicing and literal masking."""
    splices = list(re.finditer(r"\\\r?\n", source))
    logical = re.sub(r"\\\r?\n", "", source)
    offsets = [match.start() - sum(len(item.group()) for item in splices[:index])
               for index, match in enumerate(splices)]
    result = []
    for token in TOKENS.finditer(logical):
        if token.lastgroup != "jump":
            continue
        offset = token.start()
        original = offset + sum(len(match.group()) for start, match in zip(offsets, splices)
                                if start <= offset)
        result.append(source.count("\n", 0, original) + 1)
    return result


class SourceControlFlowTest(unittest.TestCase):
    def test_ignores_comments_strings_characters_and_raw_literals(self):
        source = '''/* goto failure; */
// goto failure;
const char *message = "goto failure;";
char quote = '\\'';
auto raw = R"tag(before " goto failure; after)tag";
auto count = 1'000;
'''
        self.assertEqual(jump_lines(source), [])
        self.assertEqual(jump_lines("// continued \\\ngoto hidden;\n"), [])

    def test_finds_jumps_in_functions_macros_and_inactive_branches(self):
        source = ("void run(void) { goto cleanup; }\n"
                  "#define FAIL() goto cleanup\n"
                  "#if 0\ngoto *dispatch;\n#endif\n")
        self.assertEqual(jump_lines(source), [1, 2, 4])
        self.assertEqual(jump_lines("go\\\nto cleanup;\n"), [1])

    def test_all_project_sources_use_structured_control_flow(self):
        violations = []
        for directory in SOURCE_ROOTS:
            for path in sorted((ROOT / directory).rglob("*")):
                if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                    continue
                for line in jump_lines(path.read_text(encoding="utf-8")):
                    violations.append(f"{path.relative_to(ROOT)}:{line}")
        self.assertEqual(violations, [], "Unstructured jumps found:\n" + "\n".join(violations))


if __name__ == "__main__":
    unittest.main()
