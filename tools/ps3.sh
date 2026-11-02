#!/bin/sh
# Real PS3 helpers (webMAN MOD: HTTP :80, anonymous FTP :21). PS3=<console IP>, e.g. in .env
#   tools/ps3.sh deploy [VAR=value...]  build the dev package (with those build options, e.g.
#                                   TIMES=1 LINK_CABLES=1), exit the app, upload EBOOT, launch
#   RELEASE=1 tools/ps3.sh deploy   the same with the release build (no log, input, screenshots)
#   tools/ps3.sh cmd "0 START" ...  queue dev input commands
#   tools/ps3.sh shot out.png       screenshot from the console (app must run, dev build)
#   tools/ps3.sh log                show the app log
#   tools/ps3.sh exit|restart       clean exit to XMB / clean reboot (never use the power button)
# Never install with Package Manager: it froze the console once (see docs/ps3-homebrew-notes.md).
set -e
cd "$(dirname "$0")/.."
if [ -f .env ]; then . ./.env; fi # your settings (copy .env.example), not in git
PS3="${PS3:?set PS3=<console IP> (or in .env, see .env.example)}"
H="ftp://$PS3/dev_hdd0"
# Running with a process (the title ID shows with pid=0 while the XMB still loads it). Reading
# files over FTP during that load hung a HEN console: deploy reads the log only after this.
running() { curl -s --connect-timeout 5 "http://$PS3/cpursx.ps3" | tr -d '\n' | sed 's/<[^>]*>//g' | grep -q -E 'PGBA00001[^=]*pid=0*[1-9a-f]'; }
case "$1" in
deploy)
	shift
	rm -f ps3gba.pkg ps3gba.gnpdrm.pkg
	if [ -n "$RELEASE" ]; then ./build.sh "$@" pkg; else ./build.sh DEV=1 "$@" pkg; fi
	if running; then curl -s -o /dev/null "http://$PS3/xmb.ps3\$exit"; sleep 10; fi
	curl -s --ftp-create-dirs -T build/pkg/PARAM.SFO "$H/game/PGBA00001/PARAM.SFO"
	curl -s --ftp-create-dirs -T build/pkg/ICON0.PNG "$H/game/PGBA00001/ICON0.PNG"
	curl -s --ftp-create-dirs -T build/pkg/USRDIR/EBOOT.BIN "$H/game/PGBA00001/USRDIR/EBOOT.BIN"
	sleep 3
	# /play.ps3 is ignored while the XMB settles after an exit: retry until the log restarts.
	# No request at all while the XMB loads the app: on HEN that hung the console (the notes).
	for t in 1 2 3; do
		curl -s -o /dev/null "http://$PS3/play.ps3?PGBA00001"
		sleep "${LAUNCH_WAIT:-15}"
		# A release build writes no log (the old one stays): only check that it runs.
		if running && { [ -n "$RELEASE" ] || curl -s "$H/ps3gba/log.txt" | head -1 | grep -q "dev: starting"; }; then
			echo "launched"; exit 0
		fi
	done
	echo "launch failed"; exit 1
	;;
cmd)
	shift
	tmp=$(mktemp)
	printf '%s\n' "$@" > "$tmp"
	# Upload, then rename: the app polls and deletes the file, a half-written one loses commands.
	curl -s -T "$tmp" "$H/ps3gba/dev_input.part" \
		-Q "-RNFR /dev_hdd0/ps3gba/dev_input.part" -Q "-RNTO /dev_hdd0/ps3gba/dev_input.txt"
	rm -f "$tmp"
	;;
shot)
	# SHOT runs after the commands queued before it: wait for the app's log line, not a fixed delay.
	saved() { curl -s "$H/ps3gba/log.txt" | grep -c "dev: screenshot saved" || true; }
	n=$(saved)
	"$0" cmd SHOT
	t=0
	until [ "$(saved)" -gt "$n" ]; do
		sleep 2; t=$((t + 2))
		[ $t -lt 600 ] || { echo "no screenshot after 600 s" >&2; exit 1; }
	done
	tmp=$(mktemp)
	curl -s -o "$tmp" "$H/ps3gba/shot.bmp"
	python3 -c "from PIL import Image; Image.open('$tmp').save('$2')"
	rm -f "$tmp"
	;;
log) curl -s "$H/ps3gba/log.txt" ;;
exit) curl -s -o /dev/null -w "%{http_code}\n" "http://$PS3/xmb.ps3\$exit" ;;
restart) curl -s -o /dev/null -w "%{http_code}\n" "http://$PS3/restart.ps3" ;;
*) sed -n 2,9p "$0"; exit 1 ;;
esac
