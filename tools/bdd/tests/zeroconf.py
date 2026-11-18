#!/usr/bin/env python3
"""Verify Avahi publication and reconnection on a private D-Bus test bus.

No system services are changed and no multicast packets are sent.
"""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

try:
    import dbus
    import dbus.service
    import dbus.mainloop.glib
    from gi.repository import GLib
except ImportError:
    sys.exit(77)

SERVER = 'org.freedesktop.Avahi.Server'
GROUP = 'org.freedesktop.Avahi.EntryGroup'
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
records = []
commits = []
groups = []


class Group(dbus.service.Object):
    def __init__(self, bus, path, sender):
        super().__init__(bus, path)
        self.empty = True
        self.sender = sender
        self.path = path

    @dbus.service.method(GROUP, in_signature='iiussssqaay')
    def AddService(self, interface, protocol, flags, name, kind, domain, host, port, txt):
        records.append((str(name), str(kind), int(port), {bytes(item).decode() for item in txt}))
        self.empty = False

    @dbus.service.method(GROUP, out_signature='i')
    def GetState(self):
        return 0

    @dbus.service.method(GROUP, out_signature='b')
    def IsEmpty(self):
        return self.empty

    @dbus.service.method(GROUP)
    def Commit(self):
        commits.append(True)

    @dbus.service.method(GROUP)
    def Reset(self):
        self.empty = True

    @dbus.service.method(GROUP)
    def Free(self):
        self.remove_from_connection()

    def StateChanged(self, state, error):
        # Avahi entry-group notifications are unicast to their owning client.
        message = dbus.lowlevel.SignalMessage(self.path, GROUP, 'StateChanged')
        message.set_destination(self.sender)
        message.append(state, error, signature='is')
        self.connection.send_message(message)


class Server(dbus.service.Object):
    @dbus.service.method(SERVER, out_signature='s')
    def GetVersionString(self):
        return 'avahi 0.8'

    @dbus.service.method(SERVER, out_signature='u')
    def GetAPIVersion(self):
        return 0x0202

    @dbus.service.method(SERVER, out_signature='i')
    def GetState(self):
        return 2  # AVAHI_SERVER_RUNNING

    @dbus.service.method(SERVER, out_signature='o', sender_keyword='sender')
    def EntryGroupNew(self, sender):
        path = f'/group{len(groups)}'
        groups.append(Group(self.connection, path, sender))
        return path


def wait_for(condition, process, description):
    for _ in range(800):
        if condition():
            return
        if process.poll() is not None:
            raise AssertionError(process.stderr.read().decode())
        time.sleep(0.01)
    raise AssertionError(description)


with tempfile.TemporaryDirectory(prefix='bdd-zeroconf-') as work:
    bus_process = subprocess.Popen(['dbus-daemon', '--session', '--nofork', '--print-address=1'], stdout=subprocess.PIPE)
    address = bus_process.stdout.readline().decode().strip()
    connection = dbus.bus.BusConnection(address)
    name = dbus.service.BusName('org.freedesktop.Avahi', bus=connection)
    server = Server(connection, '/')
    loop = GLib.MainLoop()
    thread = threading.Thread(target=loop.run)
    thread.start()
    process = None
    try:
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        config = Path(work) / 'bdd.conf'
        config.write_text(f'[bdd]\nListenAddress=127.0.0.1\nPort={port}\nZeroconf=yes\n')
        process = subprocess.Popen([sys.argv[1], '--config', str(config)],
                                   env=dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=address), stderr=subprocess.PIPE)
        wait_for(lambda: len(commits) >= 1, process, 'API was not published')
        first = records[0]
        assert first[1:] == ('_http._tcp', port, {'path=/api/v1/baptism-data', 'openapi=/openapi.yaml', 'api=baptism-data'})
        GLib.idle_add(lambda: (groups[-1].StateChanged(3, 'collision'), False)[1])
        wait_for(lambda: len(commits) >= 2, process, 'Collision was not resolved')
        assert records[1][0] != first[0]
        # Drop the owner and acquire it again to simulate an Avahi restart.
        connection.release_name('org.freedesktop.Avahi')
        time.sleep(0.1)
        connection.request_name('org.freedesktop.Avahi')
        wait_for(lambda: len(commits) >= 3, process, 'API was not republished after reconnect')
        print('Zeroconf service type, port, TXT records, collision and reconnect passed')
    finally:
        if process:
            process.terminate()
            _, errors = process.communicate(timeout=5)
            if errors: print(errors.decode(), file=sys.stderr)
            assert process.returncode == 0, errors.decode()
        loop.quit()
        thread.join(timeout=5)
        connection.close()
        bus_process.terminate()
        bus_process.wait(timeout=5)
