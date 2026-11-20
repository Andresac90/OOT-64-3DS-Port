#!/bin/bash
# net3ds.sh — reach the 3DS's SD card over Wi-Fi instead of taking the card out (2026-10-15).
#
# On the 3DS, run ftpd (https://github.com/mtheall/ftpd, install its .cia once with FBI) for these commands:
#   tools/net3ds.sh IP log                 copy sdmc:/3ds/oot/boot.log (+ boot_prev.log, exit.log) to build/3ds/hwlogs/
#   tools/net3ds.sh IP put [file.cia]      upload a build (default build/3ds/oot.cia) to sdmc:/cias/, then install it
#                                          with FBI on the console (SD -> cias)
#   tools/net3ds.sh IP settings_b FILE     upload FILE as sdmc:/3ds/oot/settings_b.txt
#   tools/net3ds.sh IP ls [path]           list a folder (default /3ds/oot/)
# IP is the address ftpd shows on its screen; its port is 5000 unless PORT=... is set.
#
# Or install straight from this computer, without copying the file first: in FBI choose Remote Install -> Receive
# URLs over the network, then
#   tools/net3ds.sh IP fbi [file.cia]      serves the file over HTTP from this computer and sends its URL to FBI
#   tools/net3ds.sh IP fbi https://...     sends a download URL to FBI (e.g. ftpd's .cia from its GitHub release)
# (macOS may ask to allow incoming connections for python3: allow it.)
# Builds only ever go to your own console on your own network; never publish them (see README, Legal).
set -eu
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IP="${1:?usage: net3ds.sh IP log|put|settings_b|ls|fbi ...}"
CMD="${2:?usage: net3ds.sh IP log|put|settings_b|ls|fbi ...}"
PORT="${PORT:-5000}"
FTP="ftp://$IP:$PORT"
CURL=(curl --silent --show-error --fail --connect-timeout 5 --ftp-pasv)

case "$CMD" in
  log)
    OUT="$REPO/build/3ds/hwlogs/$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$OUT"
    for f in boot.log boot_prev.log exit.log; do
      "${CURL[@]}" "$FTP/3ds/oot/$f" -o "$OUT/$f" || echo "($f not found)"
    done
    ls -la "$OUT"
    ;;
  put)
    F="${3:-$REPO/build/3ds/oot.cia}"
    "${CURL[@]}" --ftp-create-dirs -T "$F" "$FTP/cias/$(basename "$F")"
    echo "uploaded $(basename "$F") ($(md5 -q "$F" 2>/dev/null || md5sum "$F" | cut -d' ' -f1)) to sdmc:/cias/"
    ;;
  settings_b)
    F="${3:?settings file}"
    "${CURL[@]}" -T "$F" "$FTP/3ds/oot/settings_b.txt"
    echo "uploaded settings_b.txt:"; cat "$F"
    ;;
  ls)
    "${CURL[@]}" "$FTP${3:-/3ds/oot/}"
    ;;
  fbi)
    F="${3:-$REPO/build/3ds/oot.cia}"
    if [[ "$F" == http* ]]; then # a URL: FBI downloads it itself (e.g. ftpd's release .cia)
      python3 -c 'import socket,struct,sys; u=sys.argv[2].encode(); s=socket.create_connection((sys.argv[1],5000),timeout=10); s.sendall(struct.pack(">I",len(u))+u); print("sent; confirm on the 3DS")' "$IP" "$F"
      exit 0
    fi
    # only this one file is served (a temporary folder holding a link to it), not the build folder
    DIR="$(mktemp -d)"; NAME="$(basename "$F")"
    ln -s "$(cd "$(dirname "$F")" && pwd)/$NAME" "$DIR/$NAME"
    HOST="$(ipconfig getifaddr en0 2>/dev/null || hostname -I | cut -d' ' -f1)"
    HPORT=8080
    python3 -m http.server "$HPORT" --bind "$HOST" --directory "$DIR" >/dev/null 2>&1 &
    SRV=$!
    trap 'kill $SRV 2>/dev/null; rm -rf "$DIR"' EXIT
    sleep 1
    URL="http://$HOST:$HPORT/$NAME"
    # FBI's protocol: a 4-byte big-endian length, then the URLs separated by newlines; it answers with one byte when done
    python3 - "$IP" "$URL" <<'EOF'
import socket, struct, sys
ip, url = sys.argv[1], sys.argv[2].encode()
s = socket.create_connection((ip, 5000), timeout=10)
s.sendall(struct.pack(">I", len(url)) + url)
print("sent", url.decode(), "- confirm the install on the 3DS")
s.settimeout(900)
try:
    s.recv(1)
except Exception:
    pass
print("FBI finished")
EOF
    ;;
  *)
    echo "unknown command $CMD"; exit 1 ;;
esac
