#!/bin/sh
# Build and run the sleep screen rules host tests on hinton-server (no device, no PlatformIO).
# Usage (from the repo root):  sh test/sleep/run-on-hinton.sh
set -e
tar czf - src/sleep/sleep_rules.h src/sleep/sleep_rules.cpp test/sleep | ssh hinton-server 'rm -rf /tmp/mssleep && mkdir -p /tmp/mssleep && cd /tmp/mssleep && tar xzmf - &&
  g++ -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined -I src/sleep test/sleep/test_sleep.cpp src/sleep/sleep_rules.cpp -o /tmp/mssleep/t &&
  /tmp/mssleep/t'
