import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]


class KernelInstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / 'repo'
        self.repo.mkdir()
        shutil.copyfile(REPO / 'install.sh', self.repo / 'install.sh')
        shutil.copytree(REPO / 'kernel', self.repo / 'kernel',
                        ignore=shutil.ignore_patterns('*.o', '*.ko', '*.cmd', '*.mod*'))
        self.commands = self.root / 'commands'
        self.commands.mkdir()
        self.calls = self.root / 'calls'
        self.helper = self.commands / 'dispatch'
        self.helper.write_text('''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import sys
command = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ['KERNEL_TEST_CALLS'], 'a') as log:
    log.write(json.dumps([command, *args]) + '\\n')
if command == 'uname':
    print('6.12-test')
elif command == 'grep':
    if args[-1] == '/proc/modules':
        sys.exit(0 if os.environ.get('MODULE_LOADED') == '1' else 1)
    sys.exit(subprocess.call(['/usr/bin/grep', *args]))
elif command == 'make':
    sys.exit(int(os.environ.get('BUILD_FAILURE', '0')))
elif command == 'sudo':
    if args[0] == 'bash':
        source, version, kernel = args[-3:]
        assert (Path(source) / 'sil6250.c').is_file()
        assert 'PACKAGE_VERSION="' + version + '"' in (Path(source) / 'dkms.conf').read_text()
        digest = __import__('hashlib').sha256((Path(source) / 'sil6250.c').read_bytes()).hexdigest()
        with open(os.environ['KERNEL_TEST_SNAPSHOTS'], 'a') as log:
            log.write(digest + '\\n')
        sys.exit(int(os.environ.get('DKMS_FAILURE', '0')))
    if args[0] == 'modprobe' and os.environ.get('MODULE_FAILURE') == '1':
        print('Key was rejected by service', file=sys.stderr)
        sys.exit(1)
    assert args[0] in {'install', 'udevadm', 'modprobe'}, args
elif command == 'mokutil':
    print('SecureBoot disabled')
elif command == 'dkms':
    raise AssertionError('DKMS must run through the privileged helper')
else:
    raise AssertionError(command)
''')
        self.helper.chmod(0o755)
        for name in ['make', 'sudo', 'dkms', 'uname', 'grep', 'mokutil']:
            (self.commands / name).symlink_to(self.helper)
        self.env = os.environ | {'PATH':str(self.commands) + os.pathsep + os.environ['PATH'],
                                  'KERNEL_TEST_CALLS':str(self.calls),
                                  'KERNEL_TEST_SNAPSHOTS':str(self.root / 'snapshots')}

    def install(self, **env):
        return subprocess.run(['bash', str(self.repo / 'install.sh'), 'kernel'],
                              env=self.env | env, capture_output=True, text=True)

    def operations(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()] if self.calls.exists() else []

    def versions(self):
        return [call[-2] for call in self.operations() if call[:2] == ['sudo', 'bash']]

    def test_identical_sources_reuse_version(self):
        for _ in range(2):
            result = self.install()
            self.assertEqual(result.returncode, 0, result.stderr)
        versions = self.versions()
        self.assertEqual(len(versions), 2)
        self.assertEqual(versions[0], versions[1])
        self.assertEqual(versions[0], '0.1.0')
        hashes = (self.root / 'snapshots').read_text().splitlines()
        self.assertEqual(hashes[0], hashes[1])

    def test_changed_source_gets_separate_version(self):
        self.assertEqual(self.install().returncode, 0)
        source = self.repo / 'kernel/sil6250.c'
        source.write_text(source.read_text().replace('SIL6250_WAIT_DEFAULT_MS\t500',
                                                    'SIL6250_WAIT_DEFAULT_MS\t501'))
        self.assertEqual(self.install().returncode, 0)
        hashes = (self.root / 'snapshots').read_text().splitlines()
        self.assertNotEqual(hashes[0], hashes[1])

    def test_build_failure_does_not_request_privilege(self):
        result = self.install(BUILD_FAILURE='21')
        self.assertEqual(result.returncode, 21)
        self.assertFalse(any(call[0] == 'sudo' for call in self.operations()))

    def test_dkms_failure_does_not_activate(self):
        result = self.install(DKMS_FAILURE='22')
        self.assertEqual(result.returncode, 22)
        calls = [call for call in self.operations() if call[0] == 'sudo']
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][1], 'bash')

    def test_module_failure_is_reported(self):
        result = self.install(MODULE_FAILURE='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('signature was rejected', result.stderr)
        self.assertFalse(any(call[1:3] == ['udevadm', 'trigger'] for call in self.operations()))

    def test_loaded_module_is_not_unloaded_or_reloaded(self):
        result = self.install(MODULE_LOADED='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('user-initiated reboot', result.stdout)
        self.assertFalse(any('modprobe' in call or 'rmmod' in call for call in self.operations()))

    def test_other_kernel_is_not_loaded(self):
        result = self.install(KERNEL_VERSION='6.13-test')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('running kernel was not changed', result.stdout)
        self.assertFalse(any('modprobe' in call for call in self.operations()))

    def test_invalid_kernel_is_rejected(self):
        result = self.install(KERNEL_VERSION='../invalid')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.operations(), [])


class DkmsHelperTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sources = self.root / 'sources'
        self.sources.mkdir()
        self.locks = self.root / 'locks'
        self.locks.mkdir()
        self.commands = self.root / 'commands'
        self.commands.mkdir()
        self.state = self.root / 'state.json'
        self.calls = self.root / 'calls.jsonl'
        helper = (REPO / 'integrations/kernel/install-dkms.sh').read_text()
        helper = helper.replace('/usr/src', str(self.sources)).replace('/run/lock', str(self.locks))
        self.helper = self.root / 'install-dkms.sh'
        self.helper.write_text(helper)
        (self.commands / 'id').write_text('#!/bin/sh\n/bin/echo 0\n')
        (self.commands / 'id').chmod(0o755)
        dkms = self.commands / 'dkms'
        dkms.write_text('''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys
args = sys.argv[1:]
state_path = Path(os.environ['HELPER_TEST_STATE'])
state = json.loads(state_path.read_text())
with open(os.environ['HELPER_TEST_CALLS'], 'a') as log:
    log.write(json.dumps(args) + '\\n')
version = args[args.index('-v') + 1] if '-v' in args else state.get('version', 'old')
if args[0] == 'status':
    state['version'] = version
    if state['status']:
        print('sil6250/' + version + ', 6.12-test, x86_64: ' + state['status'])
elif args[0] == 'install':
    state['status'] = state.get('after_install', 'installed')
elif args[0] == 'build':
    state['build_calls'] = state.get('build_calls', 0) + 1
elif args[0] not in {'add', 'build', 'remove'}:
    raise AssertionError(args)
state_path.write_text(json.dumps(state))
if args[0] == 'build' and state['build_calls'] == 1:
    sys.exit(state.get('build_error_once', 0))
sys.exit(state.get('install_error', 0) if args[0] == 'install' else 0)
''')
        dkms.chmod(0o755)
        self.env = os.environ | {'PATH': str(self.commands) + os.pathsep + os.environ['PATH'],
                                  'HELPER_TEST_STATE': str(self.state),
                                  'HELPER_TEST_CALLS': str(self.calls)}

    def install(self, status, **state):
        self.state.write_text(json.dumps({'status': status, **state}))
        return subprocess.run(['bash', str(self.helper), str(REPO / 'kernel'), '0.1.0', '6.12-test'],
                              env=self.env, capture_output=True, text=True)

    def operations(self):
        return [json.loads(line)[0] for line in self.calls.read_text().splitlines()]

    def test_healthy_install_is_reused(self):
        for status in ['installed', 'installed (Original modules exist)', 'installed (original_module exists)']:
            with self.subTest(status=status):
                self.calls.unlink(missing_ok=True)
                result = self.install(status)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.operations(), ['status'])

    def test_damaged_or_unknown_status_is_repaired(self):
        for status in ['installed (Differences between built and installed modules)',
                       'installed (Built modules are missing in the kernel modules folder)',
                       'installed (Original modules exist) (Differences between built and installed modules)',
                       'installed (WARNING! Diff between built and installed module!)',
                       'installed (WARNING! Missing some built modules!)',
                       'installed (unexpected warning)', 'built', '']:
            with self.subTest(status=status):
                self.calls.unlink(missing_ok=True)
                result = self.install(status)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('build', self.operations())
                self.assertIn('install', self.operations())
                self.assertEqual(self.operations()[-1], 'status')

    def test_multiline_status_is_not_reused(self):
        result = self.install('installed\nother/module: installed')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('install', self.operations())

    def test_missing_cached_module_forces_rebuild(self):
        for status in ['installed (Built modules are missing in the kernel modules folder)',
                       'installed (WARNING! Missing some built modules!)']:
            with self.subTest(status=status):
                self.calls.unlink(missing_ok=True)
                result = self.install(status)
                self.assertEqual(result.returncode, 0, result.stderr)
                calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
                build = next(call for call in calls if call[0] == 'build')
                self.assertIn('--force', build)

    def test_incomplete_registration_gets_fresh_build_generation(self):
        self.assertEqual(self.install('added').returncode, 0)
        result = self.install('added', build_error_once=3)
        self.assertEqual(result.returncode, 0, result.stderr)
        sources = sorted(self.sources.glob('sil6250-*'))
        self.assertEqual(len(sources), 2)
        self.assertTrue(any('+local.2.src.' in path.name for path in sources))

    def test_added_registration_with_successful_build_reuses_generation(self):
        for _ in range(2):
            result = self.install('added')
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(list(self.sources.glob('sil6250-*'))), 1)

    def test_install_success_with_bad_files_is_failure(self):
        result = self.install('built', after_install='installed (Differences between built and installed modules)')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('could not be verified', result.stderr)

    def test_empty_post_install_status_is_failure(self):
        result = self.install('built', after_install='')
        self.assertNotEqual(result.returncode, 0)

    def test_failed_repair_attempts_same_version_restore(self):
        result = self.install('installed (Differences between built and installed modules)', install_error=42)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.operations().count('install'), 2)
        self.assertIn('Restoring previously installed', result.stderr)


if __name__ == '__main__':
    unittest.main()
