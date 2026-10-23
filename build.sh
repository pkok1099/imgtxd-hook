#!/usr/bin/env bash
# Build libimgtxd_hook.so (arm64) with the bundled NDK.
set -euo pipefail
cd "$(dirname "$0")"

NDK="${NDK:-$HOME/workspace/android-ndk}"
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++"

mkdir -p build
"$CXX" -std=c++17 -O2 -fPIC -shared -Wall -Wextra -static-libstdc++ \
    -fvisibility=hidden -fvisibility-inlines-hidden \
    -Wl,--version-script=src/exports.map \
    -Isrc \
    src/main.cpp src/mod/logger.cpp \
    -o build/libimgtxd_hook.so \
    -llog -ldl

echo "built: build/libimgtxd_hook.so"
