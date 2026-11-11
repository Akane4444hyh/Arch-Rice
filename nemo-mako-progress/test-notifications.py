"""Run on an isolated session bus, preserving the user's notification daemon."""
import os
from pathlib import Path
import resource
import subprocess
import sys
import tempfile
import time

# Accessibility/portal daemons use fixed names below XDG_RUNTIME_DIR. Keep even
# their incidental activation away from the real desktop's sockets.
runtime = tempfile.TemporaryDirectory(prefix='nemo-progress-test-runtime-')
old_runtime = os.environ.get('XDG_RUNTIME_DIR', '/run/user/1000')
display = os.environ.get('WAYLAND_DISPLAY', 'wayland-1')
os.environ['WAYLAND_DISPLAY'] = os.path.join(old_runtime, display)
os.environ['XDG_RUNTIME_DIR'] = runtime.name
os.environ['NO_AT_BRIDGE'] = '1'
os.environ['GTK_USE_PORTAL'] = '0'
os.environ['GIO_USE_VFS'] = 'local'

import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
project = Path(__file__).resolve().parent
config = project / 'build/test-mako.conf'
config.write_text('width=330\nheight=96\nicons=1\nmax-icon-size=300\n'
                  'background-color=#1e1e2ef5\ntext-color=#cdd6f4\n'
                  'border-size=1\nborder-color=#45475a\nborder-radius=7\n'
                  'icon-location=bottom\nformat=<b>%s</b>\\n%b\n'
                  'history=0\ngroup-by=none\n'
                  # A private D-Bus session still uses the real Wayland output.
                  # Injected copy errors must never look like user-file errors.
                  '[app-name="Nemo Progress"]\ninvisible=1\n')
daemon = subprocess.Popen(['/usr/bin/mako', '--config', str(config)],
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
try:
    for _ in range(100):
        reply = bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'NameHasOwner',
            GLib.Variant('(s)', ('org.freedesktop.Notifications',)),
            GLib.VariantType.new('(b)'), Gio.DBusCallFlags.NONE, 1000, None)
        if reply.unpack()[0]: break
        time.sleep(.02)
    else: raise RuntimeError('isolated mako did not start')

    if '--reproduce' in sys.argv:
        count = 3
    else:
        count = 1000
    notification_id = 0
    started = time.monotonic()
    for i in range(count):
        pixels = bytes((203, 166, 247)) * (300 * 4)
        hints = {
            'desktop-entry': GLib.Variant('s', 'nemo-progress'),
            'image-data': GLib.Variant('(iiibiiay)', (300, 4, 900, False, 8, 3, pixels)),
        }
        if '--reproduce' in sys.argv:
            hints['x-dunst-stack-tag'] = GLib.Variant('s', 'nemo-transfer')
        try:
            notification_id = bus.call_sync('org.freedesktop.Notifications',
                '/org/freedesktop/Notifications', 'org.freedesktop.Notifications',
                'Notify', GLib.Variant('(susssasa{sv}i)',
                    ('Nemo Progress', notification_id, '', f'Copy {i}',
                     'Isolated notification test', [], hints, 0)),
                GLib.VariantType.new('(u)'), Gio.DBusCallFlags.NONE, 3000, None).unpack()[0]
        except GLib.Error:
            if '--reproduce' in sys.argv:
                print('CONFIRMED: replaces_id + stack tag crashes stock Mako 1.11')
                break
            raise
    else:
        assert '--reproduce' not in sys.argv, 'expected stock Mako bug did not reproduce'
        assert daemon.poll() is None
        print(f'PASS: {count} raw-image replacements; Mako alive; {time.monotonic()-started:.2f}s')
        env = os.environ | {
            'NO_AT_BRIDGE': '1',
            'GTK_USE_PORTAL': '0',
            'GIO_USE_VFS': 'local',
            'G_DEBUG': 'fatal-criticals',
            'GTK_MODULES': str(project / 'build/libnemo-progress.so'),
            'LD_PRELOAD': str(project / 'build/libnemo-progress.so'),
            'NEMO_PROGRESS_TEST_DURABLE': '1',
        }
        subprocess.run([str(project / 'build/test-copy')], env=env, check=True)
        env['LD_PRELOAD'] = str(project / 'build/test-sync-error.so') + ':' + env['LD_PRELOAD']
        env['NEMO_PROGRESS_EXPECT_IO_ERROR'] = '1'
        subprocess.run([str(project / 'build/test-copy')], env=env, check=True)
        assert daemon.poll() is None
        print('PASS: Mako survived actual copy, cancellation, and disk-error notifications')
finally:
    daemon.terminate()
    try: daemon.wait(timeout=3)
    except subprocess.TimeoutExpired:
        daemon.kill(); daemon.wait()
    log = daemon.stderr.read().decode(errors='replace')
    if log: print(log[-1500:], file=sys.stderr)
