#!/bin/sh
set -eu
cd "$(dirname "$0")"
sh build.sh
cc -std=gnu11 -Wall -Wextra -Werror test-copy.c -o build/test-copy \
  $(pkg-config --cflags --libs gtk+-3.0 gio-2.0)
cc -shared -fPIC -Wall -Wextra -Werror test-sync-error.c -o build/test-sync-error.so
dbus-run-session -- python test-notifications.py "$@"
