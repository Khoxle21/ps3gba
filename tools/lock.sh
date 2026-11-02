#!/bin/sh
# Device locks for parallel sessions on one machine (e.g. one worktree per feature): RPCS3 and each
# console have one owner at a time. The owner is LOCK_OWNER, else the current git branch. Tools that
# drive a device claim it (tools/rpcs3.sh, tools/ps3.sh); a lock not used for 20 minutes expires.
#   tools/lock.sh status            who holds what
#   tools/lock.sh release <device>  give a device back (rpcs3, ps3-<ip>) when done with it
# Sourced by the tools: claim <device> refuses (exit 1) while another owner holds it.
lock_dir="$HOME/.ps3gba/locks"
lock_idle=1200

lock_owner() { echo "${LOCK_OWNER:-$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)}"; }
lock_age() { echo $(( $(date +%s) - $(stat -f %m "$1" 2>/dev/null || stat -c %Y "$1") )); }

claim() {
	mkdir -p "$lock_dir"
	f="$lock_dir/$1"
	owner=$(lock_owner)
	if [ -f "$f" ] && [ "$(cat "$f")" != "$owner" ] && [ "$(lock_age "$f")" -lt $lock_idle ]; then
		echo "$1 is in use by '$(cat "$f")' (last used $(( $(lock_age "$f") / 60 )) min ago): wait," \
			"or tools/lock.sh release $1 if that session is done" >&2
		exit 1
	fi
	echo "$owner" > "$f" # claims it, or refreshes our own claim
}

if [ "$(basename -- "$0")" = lock.sh ]; then
	case "$1" in
	status)
		for f in "$lock_dir"/*; do
			[ -f "$f" ] || continue
			age=$(lock_age "$f")
			state=$([ "$age" -lt $lock_idle ] && echo held || echo expired)
			echo "$(basename "$f"): $(cat "$f"), last used $((age / 60)) min ago ($state)"
		done
		;;
	release) rm -f "$lock_dir/${2:?device (rpcs3, ps3-<ip>)}" ;;
	*) echo "usage: tools/lock.sh status | release <device>" >&2; exit 1 ;;
	esac
fi
