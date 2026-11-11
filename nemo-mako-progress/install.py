#!/usr/bin/env python3
"""Install a per-user GTK notification module; preserve the stock Nemo binary."""
from pathlib import Path
from datetime import datetime, timezone
import shutil
import subprocess

HOME = Path.home()
SOURCE = Path(__file__).resolve().parent
LIB = HOME / '.local/lib/nemo-progress'
STAMP = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')


def backup(path):
    if path.exists():
        shutil.copy2(path, path.with_name(path.name + '.bak-' + STAMP))


def main():
    subprocess.run(['sh', str(SOURCE / 'build.sh')], check=True)
    LIB.mkdir(parents=True, exist_ok=True)
    # A running Nemo may have the old inode mapped. Never truncate that inode.
    target = LIB / 'libnemo-progress.so'
    backup(target)
    staged = LIB / ('libnemo-progress.so.new-' + STAMP)
    shutil.copy2(SOURCE / 'build/libnemo-progress.so', staged)
    staged.replace(target)

    wrapper = HOME / '.local/bin/nemo'
    wrapper.parent.mkdir(parents=True, exist_ok=True)
    backup(wrapper)
    wrapper.write_text('#!/bin/sh\n'
        '# Add notification support to the stock Nemo; preserve its display backend.\n'
        f'module="{LIB}/libnemo-progress.so"\n'
        'case ":${GTK_MODULES-}:" in\n'
        '  *":$module:"*) ;;\n'
        '  *) GTK_MODULES="${GTK_MODULES:+$GTK_MODULES:}$module"; export GTK_MODULES ;;\n'
        'esac\n'
        'case ":${LD_PRELOAD-}:" in\n'
        '  *":$module:"*) ;;\n'
        '  *) LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD:}$module"; export LD_PRELOAD ;;\n'
        'esac\n'
        'exec /usr/bin/nemo "$@"\n')
    wrapper.chmod(0o755)

    applications = HOME / '.local/share/applications'
    applications.mkdir(parents=True, exist_ok=True)
    desktop = applications / 'nemo.desktop'
    backup(desktop)
    content = Path('/usr/share/applications/nemo.desktop').read_text()
    content = '\n'.join(line.replace('Exec=nemo ', f'Exec={wrapper} ', 1)
                        if line.startswith('Exec=nemo ') else line
                        for line in content.splitlines()) + '\n'
    desktop.write_text(content)

    services = HOME / '.local/share/dbus-1/services'
    services.mkdir(parents=True, exist_ok=True)
    for name in ('nemo.service', 'nemo.FileManager1.service'):
        target = services / name
        backup(target)
        text = Path('/usr/share/dbus-1/services', name).read_text()
        target.write_text(text.replace('Exec=/usr/bin/nemo ', f'Exec={wrapper} '))

    config = HOME / '.config/mako/config'
    backup(config)
    text = config.read_text()
    marker = '# BEGIN nemo-progress\n'
    if marker in text:
        before, rest = text.split(marker, 1)
        _, after = rest.split('# END nemo-progress\n', 1)
        text = before + after
    text += '''
# BEGIN nemo-progress
[desktop-entry=nemo-progress]
width=330
height=96
font=sans-serif 10
padding=7,10
background-color=#1e1e2ef5
text-color=#cdd6f4
border-radius=7
border-size=1
border-color=#45475a
format=<b>%s</b>\\n%b
icons=1
max-icon-size=300
icon-location=bottom
progress-color=over #00000000
default-timeout=6000
ignore-timeout=0
history=0
group-by=none
# END nemo-progress
'''
    config.write_text(text)
    subprocess.run(['makoctl', 'reload'], check=True)
    subprocess.run(['update-desktop-database', str(applications)], check=True)
    print('Installed user notification module:', LIB)
    print('Stock Nemo preserved: /usr/bin/nemo')
    print('Restart Nemo to load the notification module.')


if __name__ == '__main__':
    main()
