#!/bin/sh
# ROM patching check, no game needed: tools/make_patch_tests.py makes synthetic ROMs and IPS, UPS
# and BPS patches, then tests/patch/harness.c applies them with mGBA's patch code (pristine mGBA
# plus patches/mgba/patch-formats-big-endian.patch) and compares the results with the expected
# bytes. It runs twice: natively (little-endian) and as s390x (big-endian like the PS3's
# PPU) under qemu, in a small Docker image built on first use (ps3gba-patchtest).
#   tools/patch_test.sh            OUT=build/patch_test
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-$PWD/build/patch_test}
rm -rf "$OUT"
mkdir -p "$OUT/src"
git -C third_party/mgba archive HEAD include src/util | tar -x -C "$OUT/src"
patch -p1 -s -d "$OUT/src" < patches/mgba/patch-formats-big-endian.patch
docker image inspect ps3gba-patchtest > /dev/null 2>&1 || docker build -t ps3gba-patchtest - <<'EOF'
FROM debian:13-slim
RUN apt-get -y update && apt-get -y install --no-install-recommends gcc libc6-dev gcc-s390x-linux-gnu \
	libc6-dev-s390x-cross qemu-user python3 && rm -rf /var/lib/apt/lists/*
EOF
docker run --rm -v "$OUT:/out" -v "$PWD/tools:/tools:ro" -v "$PWD/tests/patch:/harness:ro" -w /out \
	ps3gba-patchtest sh -ec '
python3 /tools/make_patch_tests.py files
src="src/src/util/patch.c src/src/util/patch-ips.c src/src/util/patch-ups.c src/src/util/crc32.c
	src/src/util/circle-buffer.c src/src/util/vfs/vfs-mem.c"
gcc -O2 -Wall -o harness-le /harness/harness.c $src -Isrc/include
# -D__BIG_ENDIAN__: mGBA picks its byte order by it; ppu-gcc defines it, s390x gcc does not.
s390x-linux-gnu-gcc -O2 -Wall -static -D__BIG_ENDIAN__ -o harness-be /harness/harness.c $src -Isrc/include
echo "== little-endian ($(uname -m))"
./harness-le files
echo "== big-endian (s390x, qemu)"
qemu-s390x ./harness-be files
'
