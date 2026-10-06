#!/bin/sh
# Regenerates port/banner.bnr (HOME Menu banner, original artwork) as a stereoscopic banner: tools/make_banner3d.py
# (the sky behind the screen, the title at it, the ocarina in front). Needs Python 3.10+ with Pillow and gltflib,
# pycgfx (https://github.com/skyfloogle/pycgfx) in tools/pycgfx/ or $PYCGFX, and bannertool
# (https://github.com/diasurgical/bannertool) on PATH or in $BANNERTOOL. The build itself only uses the committed
# port/banner.bnr.
set -e
cd "$(dirname "$0")/.."
PY=python3
[ -x .venv/bin/python3 ] && PY=.venv/bin/python3
"$PY" tools/make_banner3d.py port/banner.bnr --flat --preview port/banner_preview.png
echo "port/banner.bnr updated (preview: port/banner_preview.png)"
