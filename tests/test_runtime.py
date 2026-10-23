from pathlib import Path
import subprocess
import tempfile
import unittest

from runtime_fixtures import build_library


CHECK = Path(__file__).resolve().parents[1] / 'tools/check_runtime.py'


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'fixture.c'
        self.source.write_text('int fixture(void) { return 0; }\n')
        self.build('libfprint-2.so.2.0.0', 'libfprint-2.so.2')
        self.build('libsil6250_ffi.so.0', 'libsil6250_ffi.so.0')

    def build(self, name, soname, *flags):
        subprocess.run(['cc', '-shared', '-fPIC', str(self.source),
                        '-Wl,-soname,' + soname, *flags, '-o', str(self.root / name)],
                       check=True, capture_output=True)

    def check(self):
        return subprocess.run(['python3', str(CHECK), str(self.root)],
                              capture_output=True, text=True)

    def test_production_libraries_pass(self):
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_wrong_abi_is_rejected(self):
        self.build('libsil6250_ffi.so.0', 'libsil6250_ffi.so.99')
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Unexpected SONAME', result.stderr)

    def test_embedded_search_path_is_rejected(self):
        self.build('libsil6250_ffi.so.0', 'libsil6250_ffi.so.0', '-Wl,-rpath,/tmp/untrusted')
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Embedded runtime search path', result.stderr)

    def test_sanitizer_runtime_is_rejected(self):
        self.build('libsil6250_ffi.so.0', 'libsil6250_ffi.so.0', '-fsanitize=address')
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Sanitizer', result.stderr)

    def test_sanitizer_symbols_without_dependency_are_rejected(self):
        name = 'libsil6250_ffi.so.0'
        build_library(self.root, name, 'sanitizer-symbol')
        dynamic = subprocess.check_output(['readelf', '-d', str(self.root / name)], text=True)
        self.assertNotIn('libubsan', dynamic)
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Sanitizer symbols', result.stderr)

    def test_wrong_architecture_is_rejected(self):
        for defect in ['machine', 'byte-order']:
            with self.subTest(defect=defect):
                build_library(self.root, 'libsil6250_ffi.so.0', defect)
                result = self.check()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('architecture mismatch', result.stderr)

    def test_actual_32bit_library_is_rejected(self):
        build_library(self.root, 'libsil6250_ffi.so.0', 'elf32')
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('architecture mismatch', result.stderr)

    def test_executables_and_truncated_headers_are_rejected(self):
        for defect in ['executable', 'pie', 'truncated']:
            with self.subTest(defect=defect):
                build_library(self.root, 'libsil6250_ffi.so.0', defect)
                self.assertNotEqual(self.check().returncode, 0)

    def test_foreign_pair_requires_explicit_reference(self):
        for name in ['libfprint-2.so.2.0.0', 'libsil6250_ffi.so.0']:
            build_library(self.root, name, 'elf32')
        self.assertNotEqual(self.check().returncode, 0)
        result = subprocess.run(['python3', str(CHECK), str(self.root), '--reference',
                                 str(self.root / 'libfprint-2.so.2.0.0')],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
