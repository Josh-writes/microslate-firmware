#!/bin/sh
# Build and run the host tests for the sync core on hinton-server (no device, no PlatformIO).
# Usage (from the repo root):  sh test/sync/run-on-hinton.sh
# Only the pure-C++ files are built: src/sync/sync_*.cpp, src/sync/onedrive*.cpp src/sync/google_drive*.cpp src/sync/dropbox*.cpp and the tests.
set -e
tar czf - --exclude="*.local.*" src/note_ext.h src/sync test/sync | ssh hinton-local 'rm -rf /tmp/mstest && mkdir -p /tmp/mstest && cd /tmp/mstest && tar xzmf - &&
  for t in test/sync/test_*.cpp; do
    n=$(basename "$t" .cpp)
    g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined -I src/sync -I test/sync "$t" \
      $(ls src/sync/sync_*.cpp src/sync/onedrive*.cpp src/sync/google_drive*.cpp src/sync/dropbox*.cpp 2>/dev/null) -o /tmp/mstest/$n &&
    echo "== $n" && /tmp/mstest/$n
  done'
