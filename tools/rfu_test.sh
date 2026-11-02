#!/bin/sh
# Wireless Adapter desktop harness (tests/rfu/harness.c): 2-4 GBA cores in one process, each
# with source/rfu.c's adapter, driven by a script (commands at the top of harness.c), all on one
# thread so every run is the same. Screenshots land in OUT/shots as PNG.
#   tools/rfu_test.sh <script> <rom> <save|-> [<rom> <save|->...]
#   OUT=build/rfu_test (builds, inputs, shots)
# Builds and runs in the ps3gba-toolchain container (Linux on the host's CPU, the PS3's
# -DCOLOR_16_BIT -DCOLOR_5_6_5) against the submodule's working tree (patches/mgba/ applied by
# build.sh, if it ran); only changed sources rebuild. ROMs and saves are copied in, never written.
set -e
cd "$(dirname "$0")/.."
[ $# -ge 3 ] || { sed -n '2,9p' "$0"; exit 1; }
OUT=${OUT:-$PWD/build/rfu_test}
mkdir -p "$OUT/in" "$OUT/shots"
rm -f "$OUT"/in/* "$OUT"/shots/*
cp "$1" "$OUT/in/script"
shift
args=
i=0
while [ $# -ge 2 ]; do
	cp "$1" "$OUT/in/rom$i"
	if [ "$2" = - ]; then save=-; else cp "$2" "$OUT/in/save$i"; save=/out/in/save$i; fi
	args="$args /out/in/rom$i $save"
	i=$((i + 1))
	shift 2
done
# No -t: a changed file gets a new mtime, so make rebuilds it even if its old mtime was older.
rsync -r --checksum --delete --exclude .git third_party/mgba/ "$OUT/src/"

docker run --rm -v "$OUT:/out" -v "$PWD:/repo:ro" -w /out ps3gba-toolchain sh -ec '
[ -f build/CMakeCache.txt ] || CFLAGS="-DCOLOR_16_BIT -DCOLOR_5_6_5" cmake -S src -B build \
	-DCMAKE_BUILD_TYPE=Release -DLIBMGBA_ONLY=ON -DUSE_PTHREADS=ON > build.log
cmake --build build -j"$(nproc)" > build.log 2>&1 || { tail -40 build.log; exit 1; }
# the harness uses mGBA structs, whose layout depends on the build defines (as in the Makefile)
flags=$(sed -n "s/^C_\(DEFINES\|FLAGS\) = //p" build/CMakeFiles/mgba.dir/flags.make | tr " " "\n" | grep -E "^-[DU]|^-fwrapv" | tr "\n" " ")
gcc -O2 -Wall -Wextra -Wno-unused-parameter -o harness /repo/tests/rfu/harness.c /repo/source/rfu.c \
	-I/repo/include -Isrc/include -Ibuild/include $flags build/libmgba.a -lm -lpthread
cd shots
../harness ../in/script '"$args"'
'
python3 -c '
import glob, sys
from PIL import Image
for p in glob.glob(sys.argv[1] + "/*.ppm"):
    Image.open(p).save(p[:-4] + ".png")
' "$OUT/shots"
rm -f "$OUT"/shots/*.ppm
