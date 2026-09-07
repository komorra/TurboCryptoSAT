#!/usr/bin/env bash
# Builds TurboCryptoSAT. Uses CMake when available, otherwise compiles directly.
set -eo pipefail  # not -u: "$@" with no arguments trips older bash
cd "$(dirname "$0")"

if command -v cmake >/dev/null 2>&1; then
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release -j
    echo "built: build/turbocryptosat"
    exit 0
fi

echo "cmake not found, compiling directly with ${CXX:-c++}"
mkdir -p build
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -pthread \
    src/main.cpp src/cdcl.cpp src/cnf.cpp src/gates.cpp src/genbench.cpp src/gf2.cpp src/platform.cpp \
    src/propagator.cpp src/selftest.cpp src/signatures.cpp src/solver.cpp src/tune.cpp src/ui.cpp \
    -o build/turbocryptosat
echo "built: build/turbocryptosat"
