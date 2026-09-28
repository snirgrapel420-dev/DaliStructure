#!/bin/sh
# Dev build without CMake (the real build uses CMakeLists.txt).
set -e
cd "$(dirname "$0")"
mkdir -p build
for f in core/src/*.cpp core/src/dsp/*.cpp core/src/analysis/*.cpp; do
  o=build/$(basename "$f" .cpp).o
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$(find core/include core/src -name '*.h' -newer "$o" 2>/dev/null | head -1)" ]; then
    g++ -std=c++17 -O2 -Wall -Wextra -Icore/include -Icore/src -c "$f" -o "$o"
  fi
done
g++ -std=c++17 -O2 -Wall -Wextra -Icore/include -Icore/src -Itests tests/test_engine.cpp build/*.o -o build/test_engine
