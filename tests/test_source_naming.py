#!/usr/bin/env python3
"""Prevent numbered migration placeholders in project-owned C-family sources."""

from bisect import bisect_right
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("src", "include", "tests", "exts", "examples")
SOURCE_SUFFIXES = frozenset((".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".m", ".mm", ".inc"))
TOKENS = re.compile(
    r'(?:(?:u8|[uUL])?R"(?P<delimiter>[^\s()\\]{0,16})\(.*?\)(?P=delimiter)")'
    r'|/\*.*?\*/'
    r'|//[^\n]*'
    r'|"(?:\\.|[^"\\])*"'
    r"|'(?:\\.|[^'\\])*'"
    r"|\b[0-9][a-zA-Z0-9_.']*"
    r'|(?P<placeholder>\b(?:file_value|file_copy|file_status|directory_status|'
    r'expand_status|read_status|file_write_options)_[0-9]+\b)',
    re.DOTALL,
)


def numbered_placeholders(source):
    """Scan macros and inactive branches, ignoring literals and comments."""
    offsets = []
    removed = [0]
    for splice in re.finditer(r"\\\r?\n", source):
        offsets.append(splice.start() - removed[-1])
        removed.append(removed[-1] + len(splice.group()))
    logical = re.sub(r"\\\r?\n", "", source)
    result = []
    for token in TOKENS.finditer(logical):
        if token.lastgroup != "placeholder":
            continue
        original = token.start() + removed[bisect_right(offsets, token.start())]
        result.append((token.group(), source.count("\n", 0, original) + 1))
    return result


class SourceNamingTest(unittest.TestCase):
    def test_finds_all_migration_placeholder_families(self):
        families = ("file_value", "file_copy", "file_status", "directory_status",
                    "expand_status", "read_status", "file_write_options")
        source = "\n".join(f"int {family}_123 = 0;" for family in families)
        self.assertEqual(numbered_placeholders(source),
                         [(f"{family}_123", line) for line, family in enumerate(families, 1)])

    def test_ignores_comments_strings_characters_and_raw_literals(self):
        source = '''/* file_value_1 */
// file_copy_2
const char *message = "file_status_3";
char quote = '\\'';
auto raw = u8R"tag(before " directory_status_4; after)tag";
auto count = 1'000;
'''
        self.assertEqual(numbered_placeholders(source), [])
        self.assertEqual(numbered_placeholders("// continued \\\nfile_value_1;\n"), [])

    def test_checks_macros_inactive_branches_and_spliced_identifiers(self):
        source = ("#define READ() file_value_1\n"
                  "#if 0\nint file_status_2;\n#endif\n"
                  "int file_val\\\nue_3;\nint read_status_4;\n")
        self.assertEqual(numbered_placeholders(source),
                         [("file_value_1", 1), ("file_status_2", 3),
                          ("file_value_3", 5), ("read_status_4", 7)])
        self.assertEqual(numbered_placeholders("int file_val\\\r\nue_5;\n"),
                         [("file_value_5", 1)])

    def test_preserves_semantic_numbers_and_identifier_boundaries(self):
        source = ("reply_404(); sha256_update();\n"
                  "const int memory_notes_fk_0_fields[] = {0};\n"
                  "const int memory_notes_check_0_values[] = {0};\n"
                  "int CLI_OUTPUT_FIELD_DECIMAL_3; int __x86_64__;\n"
                  "int file_value_1_size; int source_file_value_1;\n"
                  "int source_path_buffer; int destination_path_status;\n")
        self.assertEqual(numbered_placeholders(source), [])

    def test_all_project_sources_use_meaningful_migration_names(self):
        violations = []
        for directory in SOURCE_ROOTS:
            for path in sorted((ROOT / directory).rglob("*")):
                if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                    continue
                for name, line in numbered_placeholders(path.read_text(encoding="utf-8")):
                    violations.append(f"{path.relative_to(ROOT)}:{line}: {name}")
        self.assertEqual(violations, [], "Name placeholders by their role:\n" +
                         "\n".join(violations))


if __name__ == "__main__":
    unittest.main()
