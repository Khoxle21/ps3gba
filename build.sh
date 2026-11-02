#!/bin/sh
# Builds libmgba and the PS3 app inside the toolchain container.
# Usage: ./build.sh [make target]   e.g. ./build.sh clean
set -e
cd "$(dirname "$0")"
# Created on the host: right after `rm -rf build` here, the container could still see the old
# folder (Docker's file sharing caches it), skip its mkdir and fail writing the first object.
mkdir -p build

# Our mGBA changes (patches/mgba/), applied to the submodule once; fails if one no longer applies.
for p in patches/mgba/*.patch; do
  git -C third_party/mgba apply --reverse --check "../../$p" 2>/dev/null ||
    git -C third_party/mgba apply "../../$p" ||
    { echo "$p does not apply: reset the submodule (git -C third_party/mgba checkout .)" >&2; exit 1; }
done

docker run --rm -v "$PWD":/src -w /src ps3gba-toolchain sh -ec '
  if [ ! -f mgba-build/CMakeCache.txt ]; then
    CFLAGS="-DCOLOR_16_BIT -DCOLOR_5_6_5 -DENABLE_VFS_FD" cmake -S third_party/mgba -B mgba-build \
      -DCMAKE_TOOLCHAIN_FILE=/src/cmake/ps3.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DLIBMGBA_ONLY=ON -DUSE_PTHREADS=ON -DCMAKE_REQUIRED_LIBRARIES=pthread
  fi
  cmake --build mgba-build -j"$(nproc)"
  make "$@"
' sh "$@"
