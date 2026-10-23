from contextlib import redirect_stdout
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from runtime_fixtures import LIBRARIES, build_libraries, build_library


REPO = Path(__file__).resolve().parents[1]


@unittest.skipUnless(all(shutil.which(tool) for tool in
                         ['dpkg-architecture', 'dpkg-shlibdeps', 'dpkg-deb']),
                     'Debian packaging tools are required')
class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'
        self.output = self.root / 'packages'
        self.lib = self.build / 'prefix/lib'
        build_libraries(self.lib)

    def package(self):
        return subprocess.run(['python3', *self.arguments()],
                              capture_output=True, text=True)

    def arguments(self):
        return [str(REPO / 'packaging/debian/build.py'),
                '--build-root', str(self.build), '--output', str(self.output),
                '--maintainer', 'Test <test@example.invalid>']

    def test_clean_package_contains_validated_libraries(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        archives = list(self.output.glob('*.deb'))
        self.assertEqual(len(archives), 1)
        unpacked = self.root / 'unpacked'
        subprocess.run(['dpkg-deb', '-x', str(archives[0]), str(unpacked)], check=True)
        for name in LIBRARIES:
            paths = list(unpacked.rglob(name))
            self.assertEqual(len(paths), 1)
            self.assertEqual(paths[0].read_bytes(), (self.lib / name).read_bytes())
        dropin = unpacked / 'usr/lib/systemd/system/fprintd.service.d/60-sil6250.conf'
        self.assertIn('LimitCORE=0', dropin.read_text())
        self.assertIn('CoredumpFilter=0x0', dropin.read_text())
        self.assertEqual(list(self.output.glob('.sil6250-package-*')), [])

    def test_invalid_runtime_is_rejected_without_archive(self):
        for name in LIBRARIES:
            for defect in ['soname', 'runpath', 'rpath', 'asan', 'sanitizer-symbol', 'non-elf',
                           'elf32', 'machine', 'byte-order', 'executable', 'pie', 'truncated']:
                with self.subTest(name=name, defect=defect):
                    self.output = self.root / f'packages-{name}-{defect}'
                    build_library(self.lib, name, defect)
                    result = self.package()
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertEqual(list(self.output.iterdir()), [])
                    build_library(self.lib, name)

    def test_copied_runtime_is_validated(self):
        name = 'libsil6250_ffi.so.0'
        bad = self.root / 'bad'
        build_library(bad, name, 'sanitizer-symbol')
        spec = importlib.util.spec_from_file_location('package_builder', self.arguments()[0])
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)
        copyfile = shutil.copyfile

        def copy(source, destination):
            result = copyfile(source, destination)
            if Path(destination).name == name:
                copyfile(bad / name, destination)
            return result

        with patch('sys.argv', self.arguments()), \
             patch.object(builder.shutil, 'copyfile', side_effect=copy), \
             redirect_stdout(io.StringIO()):
            with self.assertRaises(subprocess.CalledProcessError) as rejected:
                builder.main()
        self.assertEqual(rejected.exception.cmd[1], str(REPO / 'tools/check_runtime.py'))
        self.assertEqual(list(self.output.iterdir()), [])

    def test_missing_runtime_is_rejected_without_archive(self):
        for name in LIBRARIES:
            with self.subTest(name=name):
                (self.lib / name).unlink()
                result = self.package()
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(list(self.output.iterdir()), [])
                build_library(self.lib, name)

    def test_cross_architecture_metadata_is_rejected(self):
        result = subprocess.run(['python3', *self.arguments()],
                                env=os.environ | {'DEB_HOST_ARCH':'i386'},
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Cross packaging', result.stderr)
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
