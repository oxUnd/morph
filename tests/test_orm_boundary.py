"""Keep persistent models and database mechanisms inside the ORM directory."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
ORM = ROOT / "src/persistence/orm"
SUFFIXES = {".c", ".h", ".cpp", ".hpp", ".m", ".mm"}
TOKENS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.DOTALL)
DRIVER = re.compile(r"\bsqlite3(?:_\w+)?\b|\bSQLITE_\w+\b|sqlite3\.h")
ORM_SYMBOL = re.compile(r"\bnorm_\w+\b|\bNORM_\w+\b|\borm_(?:scope\w*|query|execute|script)\b")
SQL = re.compile(r"\b(?:SELECT\s+.+?\s+FROM|INSERT\s+(?:OR\s+\w+\s+)?INTO|"
                 r"UPDATE\s+\w+\s+SET|DELETE\s+FROM|CREATE\s+(?:TEMP\s+)?TABLE|"
                 r"ALTER\s+TABLE|DROP\s+TABLE|PRAGMA\s+\w+)\b", re.DOTALL)


class OrmBoundaryTest(unittest.TestCase):
    def test_native_database_handles_stay_in_dependency(self):
        violations = []
        for directory in ("src", "tests", "exts", "examples"):
            for path in sorted((ROOT / directory).rglob("*")):
                if path.suffix not in SUFFIXES:
                    continue
                source = path.read_text(encoding="utf-8")
                code = TOKENS.sub(lambda match: "\n" * match.group().count("\n"), source)
                for match in DRIVER.finditer(code):
                    violations.append(f"{path.relative_to(ROOT)}: {match.group()}")
                if re.search(r'#\s*include\s*[<"]sqlite3\.h[>"]', source):
                    violations.append(f"{path.relative_to(ROOT)}: native driver header")
        self.assertEqual(violations, [], "\n".join(violations))

    def test_maintenance_tools_use_model_services(self):
        violations = []
        for directory in ("scripts", "exts", "eval", "misc", "web/src"):
            for path in sorted((ROOT / directory).rglob("*")):
                if path.suffix not in {".py", ".js", ".ts", ".tsx", ".sh"}:
                    continue
                source = path.read_text(encoding="utf-8")
                if re.search(r"\b(?:from|import)\s+sqlite3\b|\bsqlite3\.\w+|better-sqlite3", source):
                    violations.append(f"{path.relative_to(ROOT)}: native database access")
        self.assertEqual(violations, [], "\n".join(violations))

    def test_models_use_structured_database_operations(self):
        violations = []
        trigger_metadata = re.compile(
            r'NORM_TRIGGER_(?:SQLITE|MYSQL|PG)_BODY\s*\(\s*(?:"(?:\\.|[^"\\])*"\s*)+\)',
            re.DOTALL,
        )
        statements = re.compile(
            r"\b(?:SELECT\b|INSERT\s+(?:OR\s+\w+\s+)?INTO|UPDATE\s+\w+\s+SET|"
            r"DELETE\s+FROM|CREATE\s+(?:UNIQUE\s+|TEMP\s+)?(?:TABLE|INDEX|TRIGGER)|"
            r"DROP\s+(?:TABLE|INDEX|TRIGGER)|ALTER\s+TABLE)\b"
        )
        raw_calls = re.compile(r"\b(?:orm_(?:query|execute|script)|norm_sql_\w+|norm_stmt_\w+|norm_sqlite_exec_script)\s*\(")
        for path in sorted(ORM.rglob("*")):
            if path.suffix not in SUFFIXES:
                continue
            if path.name == "test_fixture.c":
                continue
            source = path.read_text(encoding="utf-8")
            code = TOKENS.sub(lambda match: "\n" * match.group().count("\n"), source)
            if raw_calls.search(code):
                violations.append(f"{path.relative_to(ROOT)}: raw database entry point")
            # Trigger bodies are dialect metadata explicitly required by the ORM API.
            checked = trigger_metadata.sub("", source)
            for token in TOKENS.finditer(checked):
                if token.group().startswith('"') and statements.search(token.group()[1:-1]):
                    violations.append(f"{path.relative_to(ROOT)}: handwritten SQL statement")
                    break
        self.assertEqual(violations, [], "\n".join(violations))

    def test_production_database_access_uses_models(self):
        violations = []
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in SUFFIXES or ORM in path.parents:
                continue
            source = path.read_text(encoding="utf-8")
            code = TOKENS.sub(lambda match: "\n" * match.group().count("\n"), source)
            if ORM_SYMBOL.search(code):
                violations.append(f"{path.relative_to(ROOT)}: ORM mechanism outside models")
            if re.search(r'#\s*include\s*[<"]norm/', source):
                violations.append(f"{path.relative_to(ROOT)}: ORM dependency header")
            for header in ("internal", "records", "history_models", "fcgi_models",
                           "sync_manifest_models", "test_fixture", "schema_models", "memory_models",
                           "session_models", "error_migration_models", "sync_display_models",
                           "fcgi_quota_internal"):
                if f'"persistence/orm/{header}.h"' in source:
                    violations.append(f"{path.relative_to(ROOT)}: internal model header")
            strings = " ".join(token.group()[1:-1] for token in TOKENS.finditer(source)
                               if token.group().startswith('"'))
            if SQL.search(strings):
                violations.append(f"{path.relative_to(ROOT)}: SQL outside models")
        self.assertEqual(violations, [], "\n".join(violations))


if __name__ == "__main__":
    unittest.main()
