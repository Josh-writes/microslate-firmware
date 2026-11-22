#!/bin/sh
# Build and run the update rules host tests on hinton-server (no device, no PlatformIO).
# Usage (from the repo root):  sh test/update/run-on-hinton.sh
set -e
tar czf - src/update/update_rules.h src/update/update_rules.cpp test/update | ssh hinton-server 'rm -rf /tmp/msupd && mkdir -p /tmp/msupd && cd /tmp/msupd && tar xzmf - &&
  g++ -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined -I src/update test/update/test_update_rules.cpp src/update/update_rules.cpp -o /tmp/msupd/t &&
  /tmp/msupd/t'
