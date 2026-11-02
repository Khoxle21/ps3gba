#!/bin/sh
# Benchmark scene on the CFW PS3: N players (1-4) on FireRed saves, plugged into the link cable,
# standing at the cable club desk. Run on a freshly launched dev build (tools/ps3.sh deploy), wait ~60 s, then summarize
# with: tools/ps3.sh log | tools/perfsum.py
#   tools/bench.sh <players>     FIRERED=<part of FireRed's file name> (default FireRed)
# Saves are listed newest first and their mtimes tie, so each player's list order can differ:
# a player tries rows 0-3 until one isn't taken (the extra presses hit the game's boot: harmless).
set -e
cd "$(dirname "$0")/.."
if [ -f .env ]; then . ./.env; fi # your settings (copy .env.example), not in git
n="${1:?players}"
cmds=""
add() { cmds="$cmds$1
"; }
p=0; while [ $p -lt "$n" ]; do add "$p CONNECT"; p=$((p + 1)); done
add "WAIT 30"
p=0; while [ $p -lt "$n" ]; do
	add "$p START"; add "$p GAME ${FIRERED:-FireRed}"; add "$p CROSS"; add "$p CROSS"
	for t in 1 2 3; do add "$p DOWN"; add "$p CROSS"; done
	p=$((p + 1))
done
add "WAIT 800"
# Games start unplugged: plug every player into the link cable (the pause menu's switch).
p=0; while [ $p -lt "$n" ]; do add "$p LINK ON"; p=$((p + 1)); done
# Skip the intro, title screen -> menu, Continue, then B through the "previously on your quest"
# recap. Presses landing during a fade are lost, hence the long waits; no extra A (at the desk it
# talks to the receptionist), and B at the menu goes back to the title. Check with a screenshot.
for b in START:300 CROSS:300 CROSS:400 CIRCLE:200 CIRCLE:200 CIRCLE:200 CIRCLE:0; do
	p=0; while [ $p -lt "$n" ]; do add "$p ${b%:*}"; p=$((p + 1)); done
	add "WAIT ${b#*:}"
done
tools/ps3.sh cmd "$(printf '%s' "$cmds")"
