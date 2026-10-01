#!/usr/bin/env python3
"""fbreport.py - HTML report of a fbdiff run: which scenes render differently from the N64, worst first.

For each scene: mean error, % of pixels off, the N64 | 3DS | heatmap image fbdiff wrote with --png, and
the draws fbdiff attributed the wrong pixels to (render state per draw). Use it to find graphics that
are still wrong or incomplete, then fix them by the attributed draw's state (combiner, texture format,
blend mode...).

usage: fbdiff.py --tour TAG --top 3 --png > fb.txt
       fbreport.py fb.txt TAG [--worst N] [-o report.html]
"""
import argparse, base64, html, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PNG_DIR = os.path.join(REPO, "build", "statediff", "fbdiff")


def parse(path):
    scenes, cur = [], None
    for line in open(path, errors="ignore"):
        m = re.match(r"\s*(\d+)\s+(\S+)\s+mean err\s+([\d.]+)\s+([\d.]+)% px off\s+\((.*)\)", line)
        if m:
            cur = {"idx": int(m.group(1)), "name": m.group(2), "err": float(m.group(3)),
                   "off": float(m.group(4)), "pair": m.group(5), "draws": []}
            scenes.append(cur)
            continue
        m = re.match(r"\s*draw\s+(\d+):\s+(\d+) wrong px of\s+(\d+)\s+N64 avg (#\w+) vs 3DS (#\w+)\s+(.*)", line)
        if m and cur is not None:
            cur["draws"].append({"id": int(m.group(1)), "wrong": int(m.group(2)), "total": int(m.group(3)),
                                 "n64": m.group(4), "ds": m.group(5), "state": m.group(6)})
    return scenes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fbdiff_output")
    ap.add_argument("tag")
    ap.add_argument("--worst", type=int, default=30, help="scenes with images (by mean error)")
    ap.add_argument("-o", "--out", default=os.path.join(PNG_DIR, "report.html"))
    a = ap.parse_args()
    scenes = parse(a.fbdiff_output)
    if not scenes:
        sys.exit("no scenes parsed")
    ranked = sorted(scenes, key=lambda s: -s["err"])
    avg = sum(s["err"] for s in scenes) / len(scenes)
    out = ["<!doctype html><meta charset=utf-8><title>N64 vs 3DS render report</title>",
           "<style>body{font:14px system-ui;margin:16px;background:#111;color:#ddd}"
           "table{border-collapse:collapse}td,th{padding:3px 8px;border-bottom:1px solid #333;text-align:left}"
           "img{max-width:100%;image-rendering:pixelated;border:1px solid #333}.s{margin:24px 0}"
           "code{font-size:12px;color:#9cf}.sw{display:inline-block;width:12px;height:12px;vertical-align:middle}</style>",
           "<h1>N64 vs 3DS render report - %s</h1>" % html.escape(a.tag),
           "<p>%d scenes, average mean error %.2f (0 = identical). Images: N64 | 3DS | error heatmap. "
           "Draws: which draw produced the wrong pixels, N64 vs 3DS average color, and its render state.</p>"
           % (len(scenes), avg),
           "<table><tr><th>#</th><th>scene</th><th>mean err</th><th>% px off</th></tr>"]
    for s in ranked:
        out.append("<tr><td>%d</td><td><a href='#s%d'>%s</a></td><td>%.1f</td><td>%.1f</td></tr>"
                   % (s["idx"], s["idx"], html.escape(s["name"]), s["err"], s["off"]))
    out.append("</table>")
    for s in ranked[:a.worst]:
        out.append("<div class=s id='s%d'><h2>%d %s - mean err %.1f, %.1f%% px off</h2>"
                   % (s["idx"], s["idx"], html.escape(s["name"]), s["err"], s["off"]))
        png = os.path.join(PNG_DIR, "%s_%d.png" % (a.tag, s["idx"]))
        if os.path.exists(png):
            out.append("<img src='data:image/png;base64,%s'>" % base64.b64encode(open(png, "rb").read()).decode())
        for d in s["draws"]:
            out.append("<p>draw %d: %d of %d px wrong - N64 <span class=sw style='background:%s'></span> %s vs 3DS "
                       "<span class=sw style='background:%s'></span> %s<br><code>%s</code></p>"
                       % (d["id"], d["wrong"], d["total"], d["n64"], d["n64"], d["ds"], d["ds"], html.escape(d["state"])))
        out.append("</div>")
    open(a.out, "w").write("\n".join(out))
    print("wrote %s (%d scenes, %d with images, avg err %.2f)" % (a.out, len(scenes), min(a.worst, len(scenes)), avg))


if __name__ == "__main__":
    main()
