"""Exercise real GIO copies with an in-memory D-Bus receiver, never a desktop daemon."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

argparse.ArgumentParser(description=__doc__).parse_args()
project = Path(__file__).resolve().parent

# GTK still needs a display connection, but no windows or notification surfaces
# are created. Keep incidental accessibility/portal sockets away from the host.
runtime = tempfile.TemporaryDirectory(prefix='nemo-progress-test-runtime-')
old_runtime = os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')
display = os.environ.get('WAYLAND_DISPLAY', 'wayland-1')
environment = os.environ | {
    'WAYLAND_DISPLAY': os.path.join(old_runtime, display),
    'XDG_RUNTIME_DIR': runtime.name,
    'NO_AT_BRIDGE': '1',
    'GTK_USE_PORTAL': '0',
    'GIO_USE_VFS': 'local',
    'G_DEBUG': 'fatal-criticals',
    'GTK_MODULES': str(project / 'build/libnemo-progress.so'),
    'LD_PRELOAD': str(project / 'build/libnemo-progress.so'),
    'NEMO_PROGRESS_TEST_DURABLE': '1',
}

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
owner = bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
    'org.freedesktop.DBus', 'NameHasOwner',
    GLib.Variant('(s)', ('org.freedesktop.Notifications',)),
    GLib.VariantType.new('(b)'), Gio.DBusCallFlags.NONE, 1000, None)
if owner.unpack()[0]:
    raise SystemExit('Use sh test.sh: the test receiver requires a private session bus.')

interface = Gio.DBusNodeInfo.new_for_xml('''
<node><interface name="org.freedesktop.Notifications">
  <method name="Notify">
    <arg type="s" direction="in"/><arg type="u" direction="in"/>
    <arg type="s" direction="in"/><arg type="s" direction="in"/>
    <arg type="s" direction="in"/><arg type="as" direction="in"/>
    <arg type="a{sv}" direction="in"/><arg type="i" direction="in"/>
    <arg type="u" direction="out"/>
  </method>
</interface></node>
''').interfaces[0]

messages = []
protocol_errors = []
identifiers = set()


def receive(connection, sender, path, name, method, parameters, invocation):
    del connection, sender, path, name, method
    try:
        app, replaces, icon, summary, body, actions, hints, timeout = parameters.unpack()
        assert app == 'Nemo Progress'
        assert hints['desktop-entry'] == 'nemo-progress'
        assert 'x-dunst-stack-tag' not in hints
        assert 'x-canonical-private-synchronous' not in hints
        assert icon == '' and actions == []
        width, height, stride, alpha, bits, channels, pixels = hints['image-data']
        assert (width, height, stride, alpha, bits, channels) == (300, 4, 900, False, 8, 3)
        assert len(pixels) == stride * height
        assert replaces == 0 or replaces in identifiers
        identifier = replaces or len(identifiers) + 1
        identifiers.add(identifier)
        messages.append({'summary': summary, 'body': body, 'timeout': timeout})
        invocation.return_value(GLib.Variant('(u)', (identifier,)))
    except Exception as error:
        protocol_errors.append(repr(error))
        invocation.return_dbus_error('org.example.NemoTest.InvalidMessage', str(error))


registration = bus.register_object('/org/freedesktop/Notifications', interface, receive, None, None)
reply = bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
    'org.freedesktop.DBus', 'RequestName',
    GLib.Variant('(su)', ('org.freedesktop.Notifications', 4)),
    GLib.VariantType.new('(u)'), Gio.DBusCallFlags.NONE, 1000, None)
assert reply.unpack()[0] == 1, 'Could not own the private test receiver name'


def run_copy(env):
    start = len(messages)
    child = subprocess.Popen([str(project / 'build/test-copy')], env=env)
    loop = GLib.MainLoop()

    def completed():
        if child.poll() is None:
            return GLib.SOURCE_CONTINUE
        loop.quit()
        return GLib.SOURCE_REMOVE

    def timed_out():
        child.kill()
        loop.quit()
        return GLib.SOURCE_REMOVE

    poll = GLib.timeout_add(20, completed)
    deadline = GLib.timeout_add_seconds(30, timed_out)
    try:
        loop.run()
        child.wait()
        if child.returncode:
            raise subprocess.CalledProcessError(child.returncode, child.args)
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
        for source in (poll, deadline):
            if GLib.MainContext.default().find_source_by_id(source):
                GLib.source_remove(source)
    assert not protocol_errors, protocol_errors
    return messages[start:]


try:
    normal = run_copy(environment)
    assert any(message['summary'] == '複製完成 · 已寫入磁碟' for message in normal)
    failure = run_copy(environment | {
        'LD_PRELOAD': str(project / 'build/test-sync-error.so') + ':' + environment['LD_PRELOAD'],
        'NEMO_PROGRESS_EXPECT_IO_ERROR': '1',
    })
    assert any(message['summary'] == '複製失敗' for message in failure)
    assert not any('完成' in message['summary'] for message in failure)
    print('PASS: copy outcomes and D-Bus message contract; no desktop notifications created')
finally:
    bus.unregister_object(registration)
    runtime.cleanup()
