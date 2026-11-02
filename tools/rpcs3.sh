#!/bin/sh
# RPCS3 helpers for testing on macOS.
#   tools/rpcs3.sh run [self]       (re)launch RPCS3 on ./ps3gba.self (dev build: ./build.sh DEV=1)
#                                   PPU=interp|llvm: force the PPU decoder (interpreter or LLVM)
#                                   NET=1: network on (phone server at http://127.0.0.1:8000)
#                                   RES=1280x720: another output resolution (UI layout checks)
#   tools/rpcs3.sh cmd "0 START" ...  queue dev input commands (see main.c, DEV_TOOLS)
#   tools/rpcs3.sh shot out.png     ask the app for a screenshot and convert it to PNG
#   tools/rpcs3.sh log              show the app log
set -e
cd "$(dirname "$0")/.."
D="$HOME/Library/Application Support/rpcs3/dev_hdd0/ps3gba"
case "$1" in
run)
	pkill -9 rpcs3 || true
	sleep 1
	# Input queued for the killed instance (e.g. jitcheck's last random batch): the new one
	# would read it at its first frame, in the menus, and stall on its WAITGBA.
	rm -f "$D/dev_input.txt"
	set -- "$PWD/${2:-ps3gba.self}"
	if [ -n "$PPU$NET$RES" ]; then
		dec="Recompiler (LLVM)"
		[ "$PPU" = interp ] && dec="Interpreter (static)" # the only PPU interpreter in 0.0.43
		C="$HOME/Library/Application Support/rpcs3/config_ppu_${PPU:-default}_net_${NET:-0}_res_${RES:-default}.yml"
		sed -e "${PPU:+s/^  PPU Decoder: .*/  PPU Decoder: $dec/}" \
			-e "${NET:+s/^  Internet enabled: .*/  Internet enabled: Connected/}" \
			-e "${RES:+s/^  Resolution: .*/  Resolution: $RES/}" \
			"$HOME/Library/Application Support/rpcs3/config.yml" > "$C"
		set -- --config "$C" "$@"
	fi
	open -a /Applications/RPCS3.app --args "$@"
	;;
cmd)
	shift
	printf '%s\n' "$@" > "$D/dev_input.txt.part" && mv "$D/dev_input.txt.part" "$D/dev_input.txt"
	;;
shot)
	# The file exists from its first byte: wait for the app's log line instead.
	n=$(grep -c "dev: screenshot saved" "$D/log.txt" || true)
	printf 'SHOT\n' > "$D/dev_input.txt"
	for i in $(seq 120); do [ "$(grep -c "dev: screenshot saved" "$D/log.txt")" -gt "$n" ] && break; sleep 1; done
	python3 -c "from PIL import Image; Image.open('$D/shot.bmp').save('$2')"
	;;
log)
	cat "$D/log.txt"
	;;
*)
	sed -n 2,8p "$0"
	exit 1
	;;
esac
