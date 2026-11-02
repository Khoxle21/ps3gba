#!/bin/sh
# Benchmark scene on the CFW PS3: N players (default 4) on Pokemon Unbound, each on its own
# "BENCH <n>" save (in its saves folder, restored from ps3gba/bench/ first, so
# newest first in the list: rows 0-3, which player gets which varies), into the overworld: two
# in the forest, one in town, one by the water, some talking to an NPC. Run
# right after tools/ps3.sh deploy, wait ~75 s after "P4 JIT" in the log, check with a screenshot
# (presses during fades get lost), then: tools/ps3.sh log | tools/perfsum.py
#   [N=<players>] tools/bench_unbound.sh
set -e
cd "$(dirname "$0")/.."
# Unbound saves by itself now and then, even without input: start from the pristine copies in
# ps3gba/bench/ (outside saves/, so the app doesn't list them). The saves aren't open yet.
if [ -f .env ]; then . ./.env; fi # your settings (copy .env.example), not in git
H="ftp://${PS3:?set PS3=<console IP> (or in .env, see .env.example)}/dev_hdd0/ps3gba"
dir=$(curl -sf -l "$H/saves/" | grep -m 1 Unbound | sed 's/ /%20/g') || { echo "no Unbound saves folder" >&2; exit 1; }
tmp=$(mktemp)
for n in 1 2 3 4; do
	curl -sf -o "$tmp" "$H/bench/BENCH%20$n.sav"
	curl -sf -T "$tmp" "$H/saves/$dir/BENCH%20$n.sav"
done
rm -f "$tmp"
cmds=""
add() { cmds="$cmds$1
"; }
last=$((${N:-4} - 1))
for p in $(seq 0 $last); do add "$p CONNECT"; done
add "WAIT 30"
for p in $(seq 0 $last); do
	# A row another player took is refused: try rows 0-3 (extra presses reach the game's boot).
	add "$p START"; add "$p GAME Unbound"; add "$p CROSS"; add "WAIT 40"; add "$p CROSS"
	for t in 1 2 3; do add "$p DOWN"; add "$p CROSS"; done
done
add "WAIT 900"
for b in START:300 CROSS:400 CROSS:400; do
	for p in $(seq 0 $last); do add "$p ${b%:*}"; done
	add "WAIT ${b#*:}"
done
tools/ps3.sh cmd "$(printf '%s' "$cmds")"
