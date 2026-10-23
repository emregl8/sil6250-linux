import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from runtime_fixtures import LIBRARIES, build_libraries, build_library


REPO = Path(__file__).resolve().parents[1]


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.built = self.root / "build"
        self.stage = self.root / "stage"
        self.commands = self.root / "commands"
        self.commands.mkdir()
        self.calls = self.root / "calls"
        guard = '#!/bin/sh\nprintf "%s\\n" "$0 $*" >> "$INSTALL_TEST_CALLS"\nexit 99\n'
        for name in ["sudo", "systemctl", "udevadm", "modprobe", "dkms"]:
            command = self.commands / name
            command.write_text(guard)
            command.chmod(0o755)
        self.env = os.environ.copy()
        self.env.update(
            BUILD_ROOT=str(self.built),
            DESTDIR=str(self.stage),
            PREFIX="/usr/local",
            INSTALL_TEST_CALLS=str(self.calls),
            PATH=str(self.commands) + os.pathsep + self.env["PATH"],
        )

    def run_install(self, *args, **env):
        return subprocess.run(
            ["bash", str(REPO / "install.sh"), *args],
            env=self.env | env,
            capture_output=True,
            text=True,
        )

    def make_libraries(self):
        build_libraries(self.built / "prefix/lib")

    def assert_no_system_commands(self):
        self.assertFalse(self.calls.exists())

    def test_stage_requires_built_libraries(self):
        result = self.run_install("stage")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.stage.exists())
        self.assert_no_system_commands()

    def test_native_stage_does_not_activate_services(self):
        self.make_libraries()
        result = self.run_install("stage")
        self.assertEqual(result.returncode, 0, result.stderr)
        lib = self.stage / "usr/local/lib/sil6250"
        self.assertEqual((lib / "libfprint-2.so.2").resolve(), lib / "libfprint-2.so.2.0.0")
        for name in LIBRARIES:
            self.assertEqual((lib / name).read_bytes(),
                             (self.built / "prefix/lib" / name).read_bytes())
        override = self.stage / "etc/systemd/system/fprintd.service.d/60-sil6250.conf"
        self.assertEqual(override.stat().st_mode & 0o777, 0o644)
        self.assertIn("LD_LIBRARY_PATH=/usr/local/lib/sil6250", override.read_text())
        self.assertIn("Environment=SIL6250_PSK_MODE=provision", override.read_text())
        self.assertIn("DeviceAllow=/dev/sil6250 rw", override.read_text())
        self.assertIn("UMask=0077", override.read_text())
        self.assertIn("LimitCORE=0", override.read_text())
        self.assertIn("CoredumpFilter=0x0", override.read_text())
        self.assertNotIn("ExecStart", override.read_text())
        self.assert_no_system_commands()

    def test_invalid_runtime_is_rejected_without_partial_stage(self):
        self.make_libraries()
        for name in LIBRARIES:
            for defect in ['soname', 'runpath', 'rpath', 'asan', 'sanitizer-symbol', 'non-elf',
                           'elf32', 'machine', 'byte-order', 'executable', 'pie', 'truncated']:
                with self.subTest(name=name, defect=defect):
                    self.stage = self.root / f'stage-{name}-{defect}'
                    build_library(self.built / "prefix/lib", name, defect)
                    result = self.run_install("stage", DESTDIR=str(self.stage))
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertFalse(self.stage.exists())
                    self.assertEqual(list(self.root.glob(".sil6250-stage.*")), [])
                    self.assert_no_system_commands()
                    build_library(self.built / "prefix/lib", name)

    def test_copied_runtime_is_validated(self):
        self.make_libraries()
        bad = self.root / "bad"
        name = 'libsil6250_ffi.so.0'
        build_library(bad, name, 'sanitizer-symbol')
        install = self.commands / "install"
        install.write_text('#!/bin/bash\nset -eu\n/usr/bin/install "$@"\n'
                           'if [[ ${!#} == */lib/sil6250/ ]]; then\n'
                           '    /usr/bin/cp -- "$BAD_LIBRARY" "${!#}/libsil6250_ffi.so.0"\n'
                           'fi\n')
        install.chmod(0o755)
        result = self.run_install("stage", BAD_LIBRARY=str(bad / name))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('Sanitizer symbols', result.stderr)
        self.assertFalse(self.stage.exists())
        self.assertEqual(list(self.root.glob(".sil6250-stage.*")), [])
        self.assert_no_system_commands()

    def test_existing_stage_and_redirected_files_are_preserved(self):
        self.make_libraries()
        canary = self.root / "keep"
        canary.write_bytes(b"must not change")
        target = self.stage / "etc/systemd/system/fprintd.service.d"
        target.mkdir(parents=True)
        (target / "60-sil6250.conf").symlink_to(canary)
        result = self.run_install("stage")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(canary.read_bytes(), b"must not change")
        self.assertTrue((target / "60-sil6250.conf").is_symlink())
        self.assert_no_system_commands()

    def test_symlinked_stage_is_rejected(self):
        self.make_libraries()
        target = self.root / "elsewhere"
        target.mkdir()
        self.stage.symlink_to(target, target_is_directory=True)
        result = self.run_install("stage")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(target.iterdir()), [])
        self.assert_no_system_commands()

    def test_failed_copy_leaves_no_partial_stage(self):
        self.make_libraries()
        install = self.commands / "install"
        install.write_text("#!/bin/sh\nexit 33\n")
        install.chmod(0o755)
        result = self.run_install("stage")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.stage.exists())
        self.assertEqual(list(self.root.glob(".sil6250-stage.*")), [])
        self.assert_no_system_commands()

    def test_invalid_stage_is_rejected_before_kernel_installation(self):
        result = self.run_install("kernel", "invalid")
        self.assertNotEqual(result.returncode, 0)
        self.assert_no_system_commands()

    def test_staging_to_filesystem_root_is_rejected(self):
        self.make_libraries()
        result = self.run_install("stage", DESTDIR="/")
        self.assertNotEqual(result.returncode, 0)
        self.assert_no_system_commands()

    def test_unsafe_prefix_is_rejected(self):
        self.make_libraries()
        for prefix in ["relative", "/usr/local\nEnvironment=OTHER=1", "/usr/../etc"]:
            with self.subTest(prefix=prefix):
                result = self.run_install("stage", PREFIX=prefix)
                self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.stage.exists())
        self.assert_no_system_commands()


if __name__ == "__main__":
    unittest.main()
