"""Black-box contract checks through the independently linked extension ABI."""
import ctypes
import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest


class ReadLinesExtensionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.probe = sys.argv[1] if '--probe' in sys.argv else None
        if cls.probe:
            return
        cls.library = ctypes.CDLL(sys.argv[1])
        cls.library.ext_run.argtypes = (ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p))
        cls.library.ext_run.restype = ctypes.c_int
        cls.libc = ctypes.CDLL(None)
        cls.libc.free.argtypes = (ctypes.c_void_p,)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='morph-read-lines-')
        self.root = Path(self.temporary.name).resolve()
        self.environment = os.environ.copy()
        self.previous_directory = Path.cwd()
        self.allowed = self.root / 'allowed'
        self.allowed.mkdir()
        self.outside = self.root / 'allowed-other'
        self.outside.mkdir()
        os.environ['MORPH_READ_LINES_ROOTS'] = str(self.allowed)

    def tearDown(self):
        os.chdir(self.previous_directory)
        os.environ.clear()
        os.environ.update(self.environment)
        self.temporary.cleanup()

    def call(self, path, **options):
        arguments = dict(path=str(path), start_line=1, line_count=10)
        arguments.update(options)
        payload = json.dumps(arguments, ensure_ascii=False)
        if self.probe:
            process = subprocess.run([self.probe, payload], capture_output=True,
                                     text=True, check=True)
            status, result = process.stdout.split('\n', 1)
            return int(status), json.loads(result)
        output = ctypes.c_void_p()
        status = self.library.ext_run(payload.encode(),
                                      ctypes.byref(output))
        self.assertTrue(output.value)
        try:
            result = json.loads(ctypes.string_at(output.value).decode())
        finally:
            self.libc.free(output)
        return status, result

    def test_complete_lines_and_unterminated_final_line(self):
        source = self.allowed / 'lines.txt'
        source.write_bytes(b'first\nsecond\nlast')
        status, result = self.call(source, start_line=2)
        self.assertEqual(status, 0)
        self.assertEqual(result, dict(path=str(source), start_line=2, line_count=10,
                                     end_line=3, next_line=None, eof=True,
                                     truncated=False, content='second\nlast'))
        status, bounded = self.call(source, line_count=1)
        self.assertEqual(status, 0)
        self.assertEqual(bounded['content'], 'first\n')
        self.assertEqual(bounded['next_line'], 2)
        self.assertFalse(bounded['eof'])

    def test_limit_stops_before_complete_line_and_counts_number_prefix(self):
        source = self.allowed / 'cap.txt'
        source.write_bytes(b'a\nbbbb\nlast\n')
        for limit, numbered, expected, next_line in (
                (6, False, 'a\n', 2), (1, False, '', 1),
                (5, True, '1: a\n', 2), (4, True, '', 1)):
            with self.subTest(limit=limit, numbered=numbered):
                status, result = self.call(source, max_bytes=limit,
                                           include_line_numbers=numbered)
                self.assertEqual(status, 0)
                self.assertEqual(result['content'], expected)
                self.assertEqual(result['next_line'], next_line)
                self.assertTrue(result['truncated'])
                self.assertFalse(result['eof'])

    def test_root_boundaries_and_symlink_escape(self):
        outside = self.outside / 'secret.txt'
        outside.write_text('secret\n')
        link = self.allowed / 'escape.txt'
        link.symlink_to(outside)
        for source in (outside, link):
            status, result = self.call(source)
            self.assertLess(status, 0)
            self.assertIn('allowed root', result['error'])
        os.environ['MORPH_READ_LINES_ROOTS'] = ':' + str(self.allowed) + '::' + str(self.outside)
        self.assertEqual(self.call(outside)[0], 0)
        os.environ['MORPH_READ_LINES_ROOTS'] = '/'
        self.assertEqual(self.call(outside)[0], 0)

    def test_default_roots_and_override(self):
        os.environ.pop('MORPH_READ_LINES_ROOTS')
        os.environ['HOME'] = str(self.root / 'home')
        os.chdir(self.allowed)
        source = self.allowed / 'local.txt'
        source.write_text('local\n')
        self.assertEqual(self.call(source)[0], 0)
        for name in ('.morph', '.agents'):
            root = Path(os.environ['HOME']) / name
            root.mkdir(parents=True)
            data = root / 'data.txt'
            data.write_text('home\n')
            self.assertEqual(self.call(data)[0], 0)
        os.environ['MORPH_READ_LINES_ROOTS'] = str(self.outside)
        self.assertLess(self.call(source)[0], 0)

    def test_unicode_and_json_controls_round_trip(self):
        source = self.allowed / '中文🙂.txt'
        content = '中文🙂 "quoted" \\ slash\t\r\x01\n'
        source.write_bytes(content.encode())
        status, result = self.call(source)
        self.assertEqual(status, 0)
        self.assertEqual(result['path'], str(source))
        self.assertEqual(result['content'], content)

    def test_empty_past_eof_binary_directory_and_argument_rejections(self):
        source = self.allowed / 'empty.txt'
        source.write_bytes(b'')
        status, result = self.call(source, start_line=100)
        self.assertEqual(status, 0)
        self.assertEqual(result['content'], '')
        self.assertTrue(result['eof'])
        self.assertIsNone(result['next_line'])
        source.write_bytes(b'first\n\0binary\n')
        for start in (1, 3):
            status, result = self.call(source, start_line=start)
            self.assertLess(status, 0)
            self.assertIn('binary', result['error'])
        for path, options in ((self.allowed, {}), ('relative.txt', {}),
                              (source, {'line_count': 1001}),
                              (source, {'max_bytes': 262145}),
                              (source, {'max_bytes': 0}),
                              (source, {'start_line': 0})):
            self.assertLess(self.call(path, **options)[0], 0)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
