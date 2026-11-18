#!/bin/sh
# Build and run every host test here, with the local g++ (no device, no PlatformIO, no network). CI runs this too.
# The run-on-hinton.sh scripts in each folder run the same suites on a build server.
# Usage (from the repo root):  sh test/run-host-tests.sh
set -e
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
CXX="${CXX:-g++}"
FLAGS="-std=c++17 -Wall -Wextra -g -fsanitize=address,undefined"

run() {   # name, then the compiler arguments
  name=$1
  shift
  echo "== $name"
  $CXX $FLAGS "$@" -o "$out/$name"
  "$out/$name"
}

run editor_layout -O1 -I src test/editor/test_layout.cpp src/editor_layout.cpp
run editor_history -O1 -I src test/editor/test_history.cpp src/edit_history.cpp
run editor_note_format -O1 -I src test/editor/test_note_format.cpp src/note_format.cpp
run hid -I lib/HidCore test/hid/test_hid_core.cpp lib/HidCore/HidCore.cpp
run keymap -I lib/Keymap test/keymap/test_keymap.cpp lib/Keymap/Keymap.cpp
run sleep -I src/sleep test/sleep/test_sleep.cpp src/sleep/sleep_rules.cpp
run spell -O1 -I src test/spell/test_spell.cpp src/spell/spell_dict.cpp
run transfer -I src/transfer test/transfer/test_transfer.cpp src/transfer/transfer_rules.cpp
sync_src=$(ls src/sync/sync_*.cpp src/sync/onedrive*.cpp src/sync/google_drive*.cpp src/sync/dropbox*.cpp)
for t in test/sync/test_*.cpp; do
  # shellcheck disable=SC2086
  run "sync_$(basename "$t" .cpp)" -Wno-unused-parameter -I src/sync -I test/sync "$t" $sync_src
done
echo "All host tests passed."
