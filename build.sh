#!/usr/bin/env bash
# Thin wrapper for the documented CMake configure + build (POSIX).
set -euo pipefail
mkdir -p build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TELEMETRY=ON -DENABLE_EAC=ON
cmake --build build --config Debug --parallel
