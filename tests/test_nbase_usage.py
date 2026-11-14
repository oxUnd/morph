"""Guard the native mechanisms completed by the nbase migration."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
LITERALS = re.compile(
    r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.DOTALL
)
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
REPLACED = re.compile(
    r"realloc|asprintf|vasprintf|realpath|rand|srand|pthread_\w+|thrd_\w+|"
    r"fork|vfork|posix_spawn|posix_spawnp|waitpid|waitid|execv|execve|execvp|"
    r"clock_gettime|gettimeofday|timespec_get|nb_mem_set_allocator|nb_mem_set_oom_handler"
)
DIRECTORY_EXCEPTIONS = {
    "src/agent/tools/exec_tool.c": "fd-relative no-follow scratch cleanup",
    "src/agent/tools/file_list.c": "dirfd/fstatat metadata and checked directory iteration",
}


class NbaseUsageTest(unittest.TestCase):
    def test_production_uses_native_mechanisms(self):
        violations = []
        for directory in ("src", "exts"):
            for path in sorted((ROOT / directory).rglob("*")):
                if path.suffix not in {".c", ".h", ".m", ".mm"}:
                    continue
                source = path.read_text()
                code = LITERALS.sub(
                    lambda match: "".join("\n" if c == "\n" else " "
                                          for c in match.group()), source
                )
                relative = str(path.relative_to(ROOT))
                for match in CALL.finditer(code):
                    symbol = match.group(1)
                    directory_call = symbol in {"opendir", "readdir", "closedir"}
                    if (REPLACED.fullmatch(symbol) or
                            (directory_call and relative not in DIRECTORY_EXCEPTIONS)):
                        line = code.count("\n", 0, match.start()) + 1
                        violations.append(f"{relative}:{line}: {symbol}")
                for match in re.finditer(r"\bstrerror\s*\(\s*-", code):
                    line = code.count("\n", 0, match.start()) + 1
                    violations.append(f"{relative}:{line}: native status as errno")
        self.assertEqual(violations, [], "\n".join(violations))

    def test_directory_exceptions_keep_descriptor_constraints(self):
        for relative in DIRECTORY_EXCEPTIONS:
            source = (ROOT / relative).read_text()
            self.assertIn("fstatat(", source)
            self.assertIn("dirfd(", source)


if __name__ == "__main__":
    unittest.main()
