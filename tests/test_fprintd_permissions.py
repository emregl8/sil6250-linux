# SPDX-License-Identifier: LGPL-2.1-or-later

import argparse
import os
from pathlib import Path
import pwd
import subprocess
import tempfile
import time
import unittest

import dbus

NAME = 'net.reactivated.Fprint'
ACTION = 'net.reactivated.fprint.device.'
options = None



class Permissions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.geteuid() == 0:
            raise RuntimeError('Run permission tests as a regular user')
        cls.directory = tempfile.TemporaryDirectory(prefix='sil6250-permissions-')
        directory = Path(cls.directory.name)
        cls.bus_process = subprocess.Popen(['dbus-daemon', '--session', '--nofork',
                                             '--print-address=1'], stdout=subprocess.PIPE,
                                            stderr=subprocess.DEVNULL, text=True)
        cls.addClassCleanup(cls.stop, cls.bus_process)
        cls.addClassCleanup(cls.directory.cleanup)
        cls.address = cls.bus_process.stdout.readline().strip()
        if not cls.address.startswith('unix:'):
            raise RuntimeError('Private bus did not start')
        cls.client = dbus.bus.BusConnection(cls.address)
        cls.addClassCleanup(cls.client.close)
        env = os.environ.copy()
        env.update(DBUS_SYSTEM_BUS_ADDRESS=cls.address, DBUS_SESSION_BUS_ADDRESS=cls.address,
                   STATE_DIRECTORY=str(directory / 'storage'),
                   LD_LIBRARY_PATH=str(Path(options.libdir).resolve()),
                   FP_DRIVERS_ALLOWLIST='virtual_device',
                   FP_VIRTUAL_DEVICE=str(directory / 'reader.sock'))
        (directory / 'storage').mkdir(mode=0o700)
        cls.log = open(directory / 'daemon.log', 'w+')
        cls.addClassCleanup(cls.log.close)
        cls.policy = subprocess.Popen(['/usr/bin/python3', str(Path(__file__).with_name('mock_policy.py'))],
                                      env=env, stdout=cls.log, stderr=cls.log)
        cls.addClassCleanup(cls.stop, cls.policy)
        bus = dbus.Interface(cls.client.get_object('org.freedesktop.DBus', '/org/freedesktop/DBus'),
                             'org.freedesktop.DBus')
        for _ in range(100):
            if bus.NameHasOwner('org.freedesktop.PolicyKit1'):
                break
            time.sleep(.02)
        else:
            raise RuntimeError('Mock policy did not start')
        cls.authority = dbus.Interface(cls.client.get_object('org.freedesktop.PolicyKit1',
            '/org/freedesktop/PolicyKit1/Authority'), 'org.sil6250.TestPolicy')
        cls.daemon = subprocess.Popen([str(Path(options.fprintd).resolve()), '--no-timeout'],
                                      env=env, stdout=cls.log, stderr=cls.log)
        cls.addClassCleanup(cls.stop, cls.daemon)
        for _ in range(100):
            try:
                manager = dbus.Interface(cls.client.get_object(NAME, '/net/reactivated/Fprint/Manager'),
                                         NAME + '.Manager')
                devices = manager.GetDevices(timeout=1)
                if devices:
                    cls.device_path = devices[0]
                    return
            except dbus.DBusException:
                pass
            if cls.daemon.poll() is not None:
                break
            time.sleep(.05)
        cls.log.seek(0)
        text = cls.log.read()
        raise RuntimeError('Virtual reader did not appear: ' + text)

    @staticmethod
    def stop(child):
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        if child.stdout is not None:
            child.stdout.close()

    def setUp(self):
        self.authority.SetAllowed([])
        self.device = dbus.Interface(self.client.get_object(NAME, self.device_path), NAME + '.Device')

    def tearDown(self):
        self.authority.SetAllowed([ACTION + 'verify', ACTION + 'enroll'])
        try:
            self.device.Release(timeout=3)
        except dbus.DBusException:
            pass

    def denied(self, operation, *args, error='PermissionDenied'):
        with self.assertRaises(dbus.DBusException) as caught:
            operation(*args, timeout=3)
        self.assertEqual(caught.exception.get_dbus_name(), NAME + '.Error.' + error)

    def test_unprivileged_claim_is_denied(self):
        self.denied(self.device.Claim, '')

    def test_other_user_requires_separate_authorization(self):
        self.authority.SetAllowed([ACTION + 'verify', ACTION + 'enroll'])
        self.denied(self.device.Claim, 'root')
        self.denied(self.device.ListEnrolledFingers, 'root')
        self.denied(self.device.DeleteEnrolledFingers, 'root')
        self.device.Claim('', timeout=3)

    def test_verification_cannot_enroll_or_delete(self):
        self.authority.SetAllowed([ACTION + 'verify'])
        self.device.Claim('', timeout=3)
        self.denied(self.device.EnrollStart, 'right-middle-finger')
        self.denied(self.device.DeleteEnrolledFingers2)
        self.denied(self.device.DeleteEnrolledFinger, 'right-middle-finger')

    def test_revoked_permission_cannot_start_verification(self):
        self.authority.SetAllowed([ACTION + 'verify'])
        self.device.Claim('', timeout=3)
        self.authority.SetAllowed([])
        self.denied(self.device.VerifyStart, 'right-middle-finger')

    def test_disconnect_cancels_enrollment_and_releases_claim(self):
        self.authority.SetAllowed([ACTION + 'verify', ACTION + 'enroll'])
        peer = dbus.bus.BusConnection(self.address)
        try:
            device = dbus.Interface(peer.get_object(NAME, self.device_path), NAME + '.Device')
            device.Claim('', timeout=3)
            device.EnrollStart('right-middle-finger', timeout=3)
        finally:
            peer.close()
        deadline = time.monotonic() + 3
        while True:
            try:
                self.device.Claim('', timeout=1)
                break
            except dbus.DBusException as error:
                if time.monotonic() >= deadline or error.get_dbus_name() != NAME + '.Error.AlreadyInUse':
                    raise
                time.sleep(.02)

    def test_claim_is_bound_to_client_connection(self):
        self.authority.SetAllowed([ACTION + 'verify', ACTION + 'enroll'])
        self.device.Claim('', timeout=3)
        peer = dbus.bus.BusConnection(self.address)
        try:
            device = dbus.Interface(peer.get_object(NAME, self.device_path), NAME + '.Device')
            self.denied(device.Claim, '', error='AlreadyInUse')
            self.denied(device.Release, error='AlreadyInUse')
            self.denied(device.VerifyStart, 'right-middle-finger', error='AlreadyInUse')
            self.denied(device.EnrollStart, 'right-middle-finger', error='AlreadyInUse')
            self.device.Release(timeout=3)
            device.Claim('', timeout=3)
            device.Release(timeout=3)
        finally:
            peer.close()

    def test_enrollment_authorization_does_not_grant_other_user_access(self):
        self.authority.SetAllowed([ACTION + 'enroll'])
        self.denied(self.device.Claim, 'root')
        self.device.Claim(pwd.getpwuid(os.getuid()).pw_name, timeout=3)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--fprintd', required=True)
    parser.add_argument('--libdir', required=True)
    options, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
