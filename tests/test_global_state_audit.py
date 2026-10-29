"""Exercise the audit with definitions hidden by macros and type aliases."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / 'scripts/audit_global_state.py'
SPEC = importlib.util.spec_from_file_location('state_audit', MODULE)
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class GlobalStateAuditTest(unittest.TestCase):
    def audit(self, source, headers=None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / 'src').mkdir()
            (root / 'vendor').mkdir()
            for name, contents in (headers or {}).items():
                (root / name).write_text(contents)
            file = root / 'src/fixture.cpp'
            file.write_text(source)
            entry = {'file': str(file), 'arguments': [shutil.which('clang++'),
                     '-std=c++17', '-I' + str(root), '-c', str(file)]}
            return {hit[2] for hit in AUDIT.Clang().audit(entry, root)}

    def test_mutable_storage_in_every_scope(self):
        self.assertEqual(self.audit('''
            int global;
            static int file_static;
            thread_local int tls;
            extern int initialized = 1;
            int f() { static int local_static; int automatic; return ++local_static; }
        '''), {'global', 'file_static', 'tls', 'initialized', 'local_static'})

    def test_const_applies_to_the_object(self):
        self.assertEqual(self.audit('''
            typedef const char *Text;
            Text writable_pointer = "text";
            static const char *writable_elements[] = {"text"};
            static const char *const immutable_elements[] = {"text"};
            static const int immutable_array[] = {1, 2};
            static constexpr int constant = 3;
            extern int external_declaration;
        '''), {'writable_pointer', 'writable_elements'})

    def test_const_records_with_mutable_members(self):
        self.assertEqual(self.audit('''
            struct Mutable { mutable int value; };
            struct Nested { Mutable member; };
            struct Derived : Mutable {};
            static const Mutable direct{};
            static const Nested nested{};
            static const Derived inherited{};
            static const Mutable array[2]{};
            static const Mutable *const pointer = nullptr;
        '''), {'direct', 'nested', 'inherited', 'array'})

    def test_headers_macros_and_vendor_boundary(self):
        self.assertEqual(self.audit('''
            #include "src/fixture.h"
            #include "vendor/fixture.h"
            #define STATE(name) static int name
            STATE(macro_global);
            int f() { STATE(macro_local); return ++macro_local; }
        ''', {'src/fixture.h': 'static int owned_header;\n',
              'vendor/fixture.h': 'static int vendor_state;\n'}),
                         {'owned_header', 'macro_global', 'macro_local'})


if __name__ == '__main__':
    unittest.main()
