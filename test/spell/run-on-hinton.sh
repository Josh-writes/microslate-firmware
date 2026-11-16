#!/bin/sh
# Build and run the spellcheck host tests on hinton-server (no device, no PlatformIO).
# Usage (from the repo root):  sh test/spell/run-on-hinton.sh
set -e
tar czf - src/spell data/spell/en_words.dat test/spell | ssh hinton-server 'rm -rf /tmp/msspell && mkdir -p /tmp/msspell && cd /tmp/msspell && tar xzmf - &&
  g++ -std=c++17 -O1 -Wall -Wextra -g -fsanitize=address,undefined -I src test/spell/test_spell.cpp src/spell/spell_dict.cpp -o /tmp/msspell/t &&
  /tmp/msspell/t'
