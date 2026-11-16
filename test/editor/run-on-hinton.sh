#!/bin/sh
# Build and run the editor host tests on hinton-server (no device, no PlatformIO): line layout, undo history and note line endings.
# Usage (from the repo root):  sh test/editor/run-on-hinton.sh
set -e
tar czf - src/editor_layout.h src/editor_layout.cpp src/edit_history.h src/edit_history.cpp src/note_format.h src/note_format.cpp test/editor | ssh hinton-server 'rm -rf /tmp/mseditor && mkdir -p /tmp/mseditor && cd /tmp/mseditor && tar xzmf - &&
  g++ -std=c++17 -O1 -Wall -Wextra -g -fsanitize=address,undefined -I src test/editor/test_layout.cpp src/editor_layout.cpp -o /tmp/mseditor/t &&
  /tmp/mseditor/t &&
  g++ -std=c++17 -O1 -Wall -Wextra -g -fsanitize=address,undefined -I src test/editor/test_history.cpp src/edit_history.cpp -o /tmp/mseditor/h &&
  /tmp/mseditor/h &&
  g++ -std=c++17 -O1 -Wall -Wextra -g -fsanitize=address,undefined -I src test/editor/test_note_format.cpp src/note_format.cpp -o /tmp/mseditor/f &&
  /tmp/mseditor/f'
