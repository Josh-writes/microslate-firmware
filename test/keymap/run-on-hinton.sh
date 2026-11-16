#!/bin/sh
# Build and run the keymap host tests on hinton-server (no device, no PlatformIO).
# Usage (from the repo root):  sh test/keymap/run-on-hinton.sh
set -e
tar czf - lib/Keymap test/keymap layouts | ssh hinton-server 'rm -rf /tmp/mskeymap && mkdir -p /tmp/mskeymap && cd /tmp/mskeymap && tar xzmf - &&
  g++ -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined -I lib/Keymap test/keymap/test_keymap.cpp lib/Keymap/Keymap.cpp -o /tmp/mskeymap/t &&
  /tmp/mskeymap/t'
