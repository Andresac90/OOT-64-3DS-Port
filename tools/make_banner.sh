#!/bin/sh
# Regenerates port/banner.bnr (HOME Menu banner) from tools/make_banner.py. Needs Python 3 + Pillow and
# bannertool (https://github.com/diasurgical/bannertool) on PATH or in $BANNERTOOL. The build itself only uses
# the committed port/banner.bnr.
set -e
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
python3 tools/make_banner.py "$TMP"
"${BANNERTOOL:-bannertool}" makebanner -i "$TMP/banner.png" -a "$TMP/banner.wav" -o port/banner.bnr
rm -rf "$TMP"
echo "port/banner.bnr updated"
