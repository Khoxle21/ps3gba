#!/bin/sh
# mGBA exactness test (tests/mgba_exact/harness.c): pristine mGBA (the submodule's commit) and
# the submodule's working tree as it is now (patches/mgba/ applied, plus uncommitted changes) run
# the same games, saves and random input; prints, per game, "identical over N frames" or the first
# differing frame and, per value (video, memory, state, sound, cycles, registers), from which frame
# it differs.
#   tools/mgba_exact.sh          FRAMES=20000 SEED=1 GAMES=<regex on ROM file names>
#                                OUT=build/mgba_exact (builds, per-frame logs) SHOTS=1 (screens)
#   PATCH="a.patch b.patch"      pristine + these patches instead of the working tree (bisecting)
# Every .gba in RPCS3's dev_hdd0/ps3gba/roms runs, with its first save if it has one (mounted
# read-only). Builds and runs in the ps3gba-toolchain container (Linux on the host's CPU, the
# PS3's -DCOLOR_16_BIT -DCOLOR_5_6_5); only changed sources rebuild.
set -e
cd "$(dirname "$0")/.."
D="$HOME/Library/Application Support/rpcs3/dev_hdd0/ps3gba"
OUT=${OUT:-$PWD/build/mgba_exact}
FRAMES=${FRAMES:-20000}
mkdir -p "$OUT"
rev=$(git -C third_party/mgba rev-parse HEAD)
if [ "$(cat "$OUT/pristine.rev" 2>/dev/null)" != "$rev" ]; then
	rm -rf "$OUT/src-pristine" "$OUT/build-pristine"
	mkdir -p "$OUT/src-pristine"
	git -C third_party/mgba archive HEAD | tar -x -C "$OUT/src-pristine"
	echo "$rev" > "$OUT/pristine.rev"
fi
B=patched src=third_party/mgba
if [ -n "$PATCH" ]; then
	B=$(for p in $PATCH; do basename "$p" .patch; done | paste -sd+ -)
	src="$OUT/tmp"
	rm -rf "$src"
	cp -R "$OUT/src-pristine" "$src"
	for p in $PATCH; do patch -p1 -s -d "$src" < "$p"; done
fi
# No -t: a changed file gets a new mtime, so make rebuilds it even if its old mtime was older.
rsync -r --checksum --delete --exclude .git "$src/" "$OUT/src-$B/"
rm -rf "$OUT/tmp"

docker run --rm -v "$OUT:/out" -v "$PWD/tests/mgba_exact:/harness:ro" -v "$D:/data:ro" -w /out \
	-e FRAMES="$FRAMES" -e SEED="${SEED:-1}" -e GAMES="${GAMES:-.}" -e SHOTS="$SHOTS" -e B="$B" \
	ps3gba-toolchain sh -ec '
for v in pristine $B; do
	[ -f build-$v/CMakeCache.txt ] || CFLAGS="-DCOLOR_16_BIT -DCOLOR_5_6_5" cmake -S src-$v -B build-$v \
		-DCMAKE_BUILD_TYPE=Release -DLIBMGBA_ONLY=ON -DUSE_PTHREADS=ON > build-$v.log
	cmake --build build-$v -j"$(nproc)" > build-$v.log 2>&1 || { tail -40 build-$v.log; exit 1; }
	# the harness uses mGBA structs, whose layout depends on the build defines (as in the Makefile)
	flags=$(sed -n "s/^C_\(DEFINES\|FLAGS\) = //p" build-$v/CMakeFiles/mgba.dir/flags.make | tr " " "\n" | grep -E "^-[DU]|^-fwrapv" | tr "\n" " ")
	gcc -O2 -Wall -o harness-$v /harness/harness.c -Isrc-$v/include -Ibuild-$v/include $flags build-$v/libmgba.a -lm -lpthread
done
rm -rf logs shots
mkdir -p logs
start=$(date +%s)
for rom in /data/roms/*.gba; do
	game=$(basename "$rom" .gba)
	echo "$game" | grep -qE "$GAMES" || continue
	save=$(ls "/data/saves/$game/"*.sav 2>/dev/null | head -1)
	echo "$game: $([ -n "$save" ] && basename "$save" || echo no save)"
	for v in pristine $B; do
		shots=
		[ -z "$SHOTS" ] || { shots="/out/shots/$game/$v"; mkdir -p "$shots"; }
		MGBA_EXACT_SHOTS=$shots ./harness-$v "$rom" "${save:--}" "$FRAMES" "$SEED" > "logs/$game.$v" &
	done
done
wait
echo "ran in $(($(date +%s) - start)) s"
'

fail=0
for p in "$OUT"/logs/*.pristine; do
	game=$(basename "$p" .pristine)
	b="$OUT/logs/$game.$B"
	if [ "$(wc -l < "$p")" -ne "$FRAMES" ] || [ "$(wc -l < "$b")" -ne "$FRAMES" ]; then
		echo "$game: a run stopped early (pristine $(wc -l < "$p"), $B $(wc -l < "$b") frames)"
		fail=1
	elif cmp -s "$p" "$b"; then
		echo "$game: identical over $FRAMES frames"
	else
		line=$(cmp "$p" "$b" | sed 's/.* line //')
		echo "$game: first difference at frame $line"
		echo "  pristine: $(sed -n "${line}p" "$p")"
		echo "  $B: $(sed -n "${line}p" "$b")"
		paste -d " " "$p" "$b" | awk -v frames="$FRAMES" '
			{ n = NF / 2; for (i = 2; i <= n; i++) if (!(i in first) && $i != $(i + n)) first[i] = $1 }
			END {
				for (i = 2; i <= n; i++) {
					split($i, k, "=")
					if (i in first) d = d " " k[1] " (" first[i] ")"; else same = same " " k[1]
				}
				print "  differ from frame:" d
				print "  same over all " frames " frames:" same
			}'
		fail=1
	fi
done
exit $fail
