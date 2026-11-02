#!/bin/sh
# JIT self-check in RPCS3 (docs/jit.md): boots the dev build, starts a game on its first save
# row with an interpreted reference core, then feeds batches of random presses (walk, A, B,
# START) every 45 s and prints the first mismatch, if any. Saves are backed up and restored.
#   tools/jitcheck.sh                MINUTES=15 SEED=1 MODE= (MODE=chain: blocks stay chained)
#                                    GAME=<part of the file name> (default: FIRERED, else FireRed)
#   PROFILE=1 tools/jitcheck.sh      no check: log where instructions run (jitprof) instead
#   SOUND=1 tools/jitcheck.sh        also toggle sound sampling on and off (DATA_DIR/soundcheck,
#                                    the reference core follows); SOUND=twin: no JIT check, an
#                                    interpreted twin with the opposite setting is compared every
#                                    frame instead (GB/GBC games too, e.g. GAME=Crystal)
# Needs ./build.sh DEV=1 first, and ROMs (+ saves) in RPCS3's dev_hdd0/ps3gba.
set -e
cd "$(dirname "$0")/.."
if [ -f .env ]; then . ./.env; fi # your settings (copy .env.example), not in git
GAME="${GAME:-${FIRERED:-FireRed}}"
D="$HOME/Library/Application Support/rpcs3/dev_hdd0/ps3gba"
B=$(mktemp -d)
cp -R "$D/saves" "$B/"
trap 'rm -rf "$D/saves"; cp -R "$B/saves" "$D/"; rm -rf "$B"; rm -f "$D/jitcheck" "$D/jitprof" "$D/soundcheck"' EXIT
rm -f "$D/jitcheck" "$D/jitprof" "$D/soundcheck" "$D/nojit" "$D/log.txt"
if [ "$PROFILE" = 1 ]; then : > "$D/jitprof"; elif [ "$SOUND" != twin ]; then echo "${MODE:-}" > "$D/jitcheck"; fi
if [ -n "$SOUND" ]; then : > "$D/soundcheck"; fi
PPU=llvm tools/rpcs3.sh run
n=0; until grep -q "ready:" "$D/log.txt" 2>/dev/null || [ $n -ge 90 ]; do sleep 1; n=$((n+1)); done
# Menus wait in display frames, the game in emulated frames (WAITGBA). Then the game's own
# title screen and continue menu.
case "$GAME" in
*Unbound*) set -- "WAITGBA 1000" "0 START" "WAITGBA 200" "0 CROSS" "WAITGBA 300" ;;
*) set -- "WAITGBA 400" "0 START" "WAITGBA 150" "0 CROSS" "WAITGBA 150" "0 CROSS" "WAITGBA 150" "0 CROSS" "WAITGBA 150" "0 CROSS" "WAITGBA 300" ;;
esac
tools/rpcs3.sh cmd "0 CONNECT" "WAIT 30" "0 START" "WAIT 30" "0 GAME $GAME" "WAIT 20" "0 CROSS" "WAIT 30" "0 CROSS" "$@"
# A press lost in the menus leaves the game unstarted: say so instead of "no mismatch".
n=0; until grep -q "jit check: on\|jit profile: on" "$D/log.txt" || [ $n -ge 120 ]; do sleep 1; n=$((n+1)); done
grep -q "jit check: on\|jit profile: on" "$D/log.txt" || { echo "the game did not start: nothing checked"; exit 1; }
end=$(( $(date +%s) + ${MINUTES:-15} * 60 ))
seed=${SEED:-1}
while [ "$(date +%s)" -lt "$end" ]; do
	sleep 45
	grep -q "MISMATCH" "$D/log.txt" && break
	python3 -c "
import random
r = random.Random($seed)
for _ in range(60):
    k = r.random()
    print('0 ' + (r.choice(['UP', 'DOWN', 'LEFT', 'RIGHT']) if k < 0.55 else 'CROSS' if k < 0.75 else 'CIRCLE' if k < 0.92 else 'START'))
    print('WAITGBA %d' % r.randint(8, 40))" > "$D/dev_input.txt.part"
	mv "$D/dev_input.txt.part" "$D/dev_input.txt"
	seed=$((seed + 1))
done
grep "MISMATCH" -A 12 "$D/log.txt" || echo "no mismatch"
grep "soundcheck P1" "$D/log.txt" | tail -1 || true
grep "jit P1" "$D/log.txt" | tail -1
grep "jit profile" "$D/log.txt" | tail -26 || true
