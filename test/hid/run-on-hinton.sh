#!/bin/sh
# Build and run the HID core host tests on hinton-server (no device, no PlatformIO).
# Usage (from the repo root):  sh test/hid/run-on-hinton.sh
set -e
tar czf - lib/HidCore test/hid | ssh hinton-server 'rm -rf /tmp/mshid && mkdir -p /tmp/mshid && cd /tmp/mshid && tar xzmf - &&
  g++ -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined -I lib/HidCore test/hid/test_hid_core.cpp lib/HidCore/HidCore.cpp -o /tmp/mshid/t &&
  /tmp/mshid/t'
