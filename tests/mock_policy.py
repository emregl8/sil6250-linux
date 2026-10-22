# SPDX-License-Identifier: LGPL-2.1-or-later

import os
import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

address = os.environ.get('DBUS_SESSION_BUS_ADDRESS')
if os.geteuid() == 0 or not address or address != os.environ.get('DBUS_SYSTEM_BUS_ADDRESS'):
    raise SystemExit('Run only as a regular user on the isolated test bus')
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(address)
name = dbus.service.BusName('org.freedesktop.PolicyKit1', bus=bus, do_not_queue=True)


class Authority(dbus.service.Object):
    allowed = frozenset()

    @dbus.service.method('org.freedesktop.PolicyKit1.Authority',
                         in_signature='(sa{sv})sa{ss}us', out_signature='(bba{ss})')
    def CheckAuthorization(self, subject, action, details, flags, cancellation):
        return action in self.allowed, False, dbus.Dictionary({}, signature='ss')

    @dbus.service.method('org.sil6250.TestPolicy', in_signature='as', out_signature='')
    def SetAllowed(self, actions):
        self.allowed = frozenset(actions)


authority = Authority(bus, '/org/freedesktop/PolicyKit1/Authority')
GLib.MainLoop().run()
