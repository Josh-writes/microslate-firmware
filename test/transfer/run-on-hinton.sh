#!/bin/sh
# Build and run the Transfer rules host tests on hinton-server (no device, no PlatformIO).
# Usage (from the repo root):  sh test/transfer/run-on-hinton.sh
set -e
tar czf - src/note_ext.h src/transfer/transfer_rules.h src/transfer/transfer_rules.cpp test/transfer | ssh hinton-server 'rm -rf /tmp/msxfer && mkdir -p /tmp/msxfer && cd /tmp/msxfer && tar xzmf - &&
  g++ -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined -I src/transfer test/transfer/test_transfer.cpp src/transfer/transfer_rules.cpp -o /tmp/msxfer/t &&
  /tmp/msxfer/t'
