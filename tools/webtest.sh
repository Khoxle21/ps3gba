#!/bin/sh
# Phone server tests against the app running in RPCS3 (NET=1 tools/rpcs3.sh run, dev build):
# uploads (games, saves, covers, patches), downloads, names, and everything that must be refused (bad tokens, path traversal,
# oversized, interrupted and stalled uploads). Removes what it added from RPCS3's drive.
set -e
cd "$(dirname "$0")/.."
D="$HOME/Library/Application Support/rpcs3/dev_hdd0/ps3gba"
link=$(sed -n 's/^web: P1 \(http[^ ]*\)$/\1/p' "$D/log.txt" | tail -1)
[ -n "$link" ] || { echo "no phone link in the log yet"; exit 1; }
B="http://127.0.0.1:8000/${link#http://*/}" # the server listens on every interface
GAME="Web Test (USA)"
ROM=$(ls "$D"/roms/*.gba | head -1)
GBROM=$(ls "$D"/roms/*/*.gb 2>/dev/null | head -1) # GB tests skipped without one
SAVE=$(ls "$D"/saves/*/*.sav | head -1)
COVER=$(ls "$D"/covers/*.png | head -1)
T=$(mktemp -d)
cp "$D/players.txt" "$T/players.txt" 2>/dev/null || true
cleanup() {
	rm -rf "$D/roms/$GAME.gba" "$D/saves/$GAME" "$D/covers/$GAME.png" "$D/roms/gb/$GAME.gb" "$D/saves/$GAME.gb" "$D/roms/x.gba" \
		"$D/patches/$GAME.ips" "$T"
	if [ -f "$T/players.txt" ]; then cp "$T/players.txt" "$D/players.txt"; else rm -f "$D/players.txt"; fi
}
trap cleanup EXIT
fails=0

# check <name> <expected status> <curl args...>: prints the status and body.
check() {
	name=$1 want=$2
	shift 2
	got=$(curl -s -o "$T/body" -w '%{http_code}' --max-time 60 "$@" || true)
	if [ "$got" = "$want" ]; then echo "ok   $name: $got $(head -c 80 "$T/body")"
	else echo "FAIL $name: got $got, want $want: $(head -c 200 "$T/body")"; fails=$((fails + 1)); fi
}
same() { # same <name> <file a> <file b>
	if cmp -s "$2" "$3"; then echo "ok   $1"; else echo "FAIL $1: $2 differs from $3"; fails=$((fails + 1)); fi
}
q() { python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$1"; }
G=$(q "$GAME")

check "page" 200 "$B"
grep -q '<title>ps3gba</title>' "$T/body" || { echo "FAIL page content"; fails=$((fails + 1)); }
check "no trailing slash" 301 "${B%/}"
check "bad token" 404 "http://127.0.0.1:8000/00000000/games"
check "no token" 404 "http://127.0.0.1:8000/"
check "font" 200 "${B}font.ttf"
same "font bytes" "$T/body" data/pressstart2p.ttf
check "games" 200 "${B}games"

check "rom" 200 --data-binary @"$ROM" "${B}rom?name=$G.gba"
same "rom bytes" "$ROM" "$D/roms/$GAME.gba"
check "rom twice" 409 --data-binary @"$ROM" "${B}rom?name=$G.gba"
head -c 300000 /dev/urandom > "$T/fake.gba"
check "not a rom" 415 --data-binary @"$T/fake.gba" "${B}rom?name=fake.gba"
head -c $((33 << 20)) /dev/zero > "$T/big.gba"
check "rom too big (Expect)" 413 --data-binary @"$T/big.gba" "${B}rom?name=big.gba"
check "rom too big (no Expect)" 413 -H 'Expect:' --data-binary @"$T/big.gba" "${B}rom?name=big.gba"
check "rom traversal" 400 --data-binary @"$ROM" "${B}rom?name=..%2Fevil.gba"
check "rom traversal 2" 400 --data-binary @"$ROM" "${B}rom?name=%2e%2e%2fevil.gba"
check "rom backslash" 400 --data-binary @"$ROM" "${B}rom?name=a%5Cb.gba"
check "rom NUL" 400 --data-binary @"$ROM" "${B}rom?name=a%00b.gba"
check "rom dot name" 400 --data-binary @"$ROM" "${B}rom?name=..gba"
check "rom not .gba" 400 --data-binary @"$ROM" "${B}rom?name=x.txt"
check "rom bad escape" 400 --data-binary @"$ROM" "${B}rom?name=%zz.gba"
check "gba rom as .gb" 415 --data-binary @"$ROM" "${B}rom?name=$G.gb"
if [ -n "$GBROM" ]; then
	check "gb rom" 200 --data-binary @"$GBROM" "${B}rom?name=$G.GB"
	same "gb rom bytes, in roms/gb/" "$GBROM" "$D/roms/gb/$GAME.gb"
	check "gb rom twice" 409 --data-binary @"$GBROM" "${B}rom?name=$G.gb"
	check "gb rom as .gba" 415 --data-binary @"$GBROM" "${B}rom?name=x.gba"
	check "games" 200 "${B}games"
	grep -q "\"$GAME.gb\"" "$T/body" && grep -q "\"$GAME\"" "$T/body" || { echo "FAIL games list"; fails=$((fails + 1)); }
	check "gb save" 200 --data-binary @"$SAVE" "${B}save?game=$(q "$GAME.gb")&name=PHONE"
	[ -f "$D/saves/$GAME.gb/PHONE.sav" ] || { echo "FAIL gb save path"; fails=$((fails + 1)); }
fi

check "save" 200 --data-binary @"$SAVE" "${B}save?game=$G&name=PHONE"
check "save again: new name" 200 --data-binary @"$SAVE" "${B}save?game=$G&name=PHONE"
grep -q 'PHONE 2' "$T/body" || { echo "FAIL second save not renamed"; fails=$((fails + 1)); }
check "saves" 200 "${B}saves?game=$G"
grep -q '"PHONE 2"' "$T/body" || { echo "FAIL saves list"; fails=$((fails + 1)); }
check "download" 200 -D "$T/headers" "${B}save?game=$G&name=PHONE"
same "download bytes" "$T/body" "$SAVE"
grep -qi 'content-disposition: attachment' "$T/headers" || { echo "FAIL no attachment header"; fails=$((fails + 1)); }
check "download missing" 404 "${B}save?game=$G&name=NOPE"
check "download traversal" 404 "${B}save?game=$G&name=..%2F..%2Froms%2Fx"
check "save game traversal" 404 --data-binary @"$SAVE" "${B}save?game=..%2Froms&name=X"
check "save unknown game" 404 --data-binary @"$SAVE" "${B}save?game=Nope&name=X"
check "save name traversal" 400 --data-binary @"$SAVE" "${B}save?game=$G&name=..%2Fx"
check "save name too long" 400 --data-binary @"$SAVE" "${B}save?game=$G&name=ABCDEFGHIJKLMNOPQ"
head -c $((257 << 10)) /dev/zero > "$T/big.sav"
check "save too big" 413 --data-binary @"$T/big.sav" "${B}save?game=$G&name=BIG"

printf 'PATCH\000\001\000\000\001\052EOF' > "$T/test.ips" # one byte at 0x100
check "patch" 200 --data-binary @"$T/test.ips" "${B}patch?name=$G.IPS"
same "patch bytes, in patches/" "$T/test.ips" "$D/patches/$GAME.ips"
check "patch twice" 409 --data-binary @"$T/test.ips" "${B}patch?name=$G.ips"
check "not a patch" 415 --data-binary @"$SAVE" "${B}patch?name=fake.ups"
check "patch not .ips .ups .bps" 400 --data-binary @"$T/test.ips" "${B}patch?name=x.xdelta"
check "patch traversal" 400 --data-binary @"$T/test.ips" "${B}patch?name=..%2Fevil.ips"
head -c $((65 << 20)) /dev/zero > "$T/big.bps"
check "patch too big" 413 --data-binary @"$T/big.bps" "${B}patch?name=big.bps"
check "33 MB patch: size fine, not a patch" 415 --data-binary @"$T/big.gba" "${B}patch?name=big.bps"

check "cover" 200 --data-binary @"$COVER" "${B}cover?game=$G"
check "cover read" 200 "${B}cover?game=$G"
same "cover bytes" "$T/body" "$COVER"
check "cover not png" 415 --data-binary @"$SAVE" "${B}cover?game=$G"

check "name" 200 -X POST -d '' "${B}name?name=Player"
[ "$(head -1 "$D/players.txt")" = Player ] || { echo "FAIL players.txt"; fails=$((fails + 1)); }
check "name too long" 400 -X POST -d '' "${B}name?name=ABCDEFGHIJKLM"
check "unknown route" 404 "${B}nope"

# Raw socket cases: no length, a body cut short, a phone that stalls, idle connections.
python3 - "$B" <<'EOF' || fails=$((fails + 1))
import socket, sys, time, urllib.parse, os, threading
url = urllib.parse.urlsplit(sys.argv[1])
path = url.path
def conn():
	return socket.create_connection(("127.0.0.1", 8000), timeout=30)
def status(s):
	data = b""
	while True:
		chunk = s.recv(4096)
		if not chunk: break
		data += chunk
	return data.split(b" ")[1].decode() if data else "closed"
ok = True
def expect(name, got, want):
	global ok
	print(("ok  " if got == want else "FAIL") + f" {name}: {got}" + ("" if got == want else f", want {want}"))
	ok &= got == want

s = conn(); s.sendall(f"POST {path}rom?name=x.gba HTTP/1.1\r\nHost: x\r\n\r\n".encode())
expect("no length", status(s), "411")

s = conn(); s.sendall(f"POST {path}rom?name=cut.gba HTTP/1.1\r\nContent-Length: 1000000\r\n\r\n".encode() + b"x" * 1000); s.close()
s = conn(); t = time.time(); s.sendall(f"POST {path}rom?name=stall.gba HTTP/1.1\r\nContent-Length: 1000000\r\n\r\n".encode() + b"x" * 1000)
try:
	status(s)
except ConnectionResetError:
	pass
expect("stalled upload dropped after ~10 s", 8 < time.time() - t < 14, True)

idle = [conn() for _ in range(5)]
t = time.time(); s = conn(); s.sendall(f"GET {path}games HTTP/1.1\r\n\r\n".encode())
expect("served behind 5 idle connections", status(s), "200")
print(f"     (waited {time.time() - t:.1f} s)")
for i in idle: i.close()
sys.exit(0 if ok else 1)
EOF
leftovers=$(ls "$D/tmp" | wc -l | tr -d ' ')
[ "$leftovers" = 0 ] && echo "ok   no partial files left" || { echo "FAIL $leftovers partial files in tmp/"; fails=$((fails + 1)); }
[ ! -e "$D/roms/cut.gba" ] && [ ! -e "$D/roms/stall.gba" ] && [ ! -e "$D/roms/fake.gba" ] && echo "ok   no refused ROM on the drive" ||
	{ echo "FAIL a refused ROM was kept"; fails=$((fails + 1)); }
[ ! -e "$D/patches/fake.ups" ] && [ ! -e "$D/patches/big.bps" ] && echo "ok   no refused patch on the drive" ||
	{ echo "FAIL a refused patch was kept"; fails=$((fails + 1)); }
echo "$fails failed"
[ "$fails" = 0 ]
