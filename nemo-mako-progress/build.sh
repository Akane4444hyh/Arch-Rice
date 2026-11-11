#!/bin/sh
set -eu
cd "$(dirname "$0")"
install_dir="$HOME/.local/lib/nemo-progress"
mkdir -p build
cc -std=gnu11 -O2 -fPIC -shared -Wall -Wextra -Werror \
  nemo-progress.c copy-bridge.c -o build/libnemo-progress.so \
  $(pkg-config --cflags --libs gtk+-3.0 gio-2.0 gmodule-2.0) -lm -ldl
