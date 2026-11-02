#!/bin/sh
# Executable-memory probe (tests/jitprobe): can an app run code it wrote? Set PS3=<ip>.
#   tools/jitprobe.sh deploy        build, upload, reset results (ALLOW_POKE=1 enables the lv2 poke probe)
#   tools/jitprobe.sh run           launch once (runs the next probe), then show results
#   tools/jitprobe.sh results       show results
#   tools/jitprobe.sh rpcs3         build and run in RPCS3 (PPU=interp or llvm, default llvm)
# CAVE_MB=n builds an n MB code cave (probe 7 runs and rewrites code at its end); FROM=n starts
# at probe n (e.g. FROM=7 skips the crashing probes); WRITABLE_TEXT=1 marks the code segment RWX;
# PROC_PARAM=1 adds ps3gba's SYS_PROCESS_PARAM; IMPORTS="audio gcm io sysutil net netctl
# sysmodule" imports those system libraries (any subset).
# A crashing probe may hang the app: recover with tools/ps3.sh restart (on HEN, re-enable HEN after).
set -e
cd "$(dirname "$0")/.."
if [ -f .env ]; then . ./.env; fi # your settings (copy .env.example), not in git
PS3="${PS3:?set PS3=<console IP> (or in .env, see .env.example)}"
H="ftp://$PS3/dev_hdd0"
del() { curl -s -o /dev/null -Q "DELE $1" "$H/" || true; }
build() { docker run --rm -e CAVE_MB -e WRITABLE_TEXT -e PROC_PARAM -e IMPORTS -v "$PWD":/src -w /src/tests/jitprobe ps3gba-toolchain sh -c "make clean >/dev/null; make $1"; }
seed() { [ -n "$FROM" ] && printf '%d start\n%d end seeded\n' $((FROM - 1)) $((FROM - 1)); true; }
case "$1" in
deploy)
	build pkg
	for f in PARAM.SFO ICON0.PNG USRDIR/EBOOT.BIN; do
		curl -s --ftp-create-dirs -T "tests/jitprobe/build/pkg/$f" "$H/game/PGBA00099/$f"
	done
	del /dev_hdd0/jitprobe.txt
	if [ -n "$FROM" ]; then seed | curl -s -T - "$H/jitprobe.txt"; fi
	if [ "$ALLOW_POKE" = 1 ]; then echo yes | curl -s -T - "$H/jitprobe_allow_poke"
	else del /dev_hdd0/jitprobe_allow_poke; fi
	echo deployed
	;;
run)
	curl -s -o /dev/null "http://$PS3/play.ps3?PGBA00099"
	sleep 15
	curl -s "$H/jitprobe.txt" || echo "no results yet"
	;;
results) curl -s "$H/jitprobe.txt" ;;
rpcs3)
	build
	R="$HOME/Library/Application Support/rpcs3/dev_hdd0"
	mkdir -p "$R"
	seed > "$R/jitprobe.txt"
	ls -l tests/jitprobe/jitprobe.self
	PPU="${PPU:-llvm}" tools/rpcs3.sh run tests/jitprobe/jitprobe.self
	sleep "${WAIT:-20}"
	cat "$R/jitprobe.txt"
	;;
*) sed -n 2,9p "$0"; exit 1 ;;
esac
