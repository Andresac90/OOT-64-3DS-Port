#!/usr/bin/env python3
"""tjunctions.py - count T-junctions in one frame's triangles (settings tjdump=<frame>, sdmc:/3ds/oot/tjdump.bin).

A T-junction is a vertex lying inside another triangle's edge ("mesh vertex on mesh edge": already in the game's
data; any "piece" kind: made by the port's splitting/clipping). Two triangles that share an edge but disagree on
the points along it (one split it, the other did not) rasterize it differently, and the 3DS GPU then leaves pixel
cracks there (thin bright lines or dots where the background shows through). The N64 draws whole triangles, so
the meshes have almost none; the port's shading split (gfx_pc.c gfx_subdiv_tri) adds them.

usage: tjunctions.py [tjdump.bin] [--png out.png]
"""
import math, os, struct, sys
from collections import defaultdict

SCALE = 200.0  # NDC -> pixels (about the top screen's half width)
EPS_LINE = 0.02  # px: vertex-to-edge distance counted as "on the edge"
EPS_END = 0.05   # px: closer than this to an endpoint is that endpoint
CELL = 4.0


def load(path):
    data = open(path, "rb").read()
    n = struct.unpack_from("<i", data, 0)[0]
    tris = []
    for i in range(n):
        v = struct.unpack_from("<8f", data, 4 + 32 * i)
        pts = [(v[0] * SCALE, v[1] * SCALE), (v[2] * SCALE, v[3] * SCALE), (v[4] * SCALE, v[5] * SCALE)]
        if all(math.isfinite(c) for p in pts for c in p):
            tris.append((pts, int(v[6]), int(v[7])))
    return tris


def count(tris):
    grid = defaultdict(list)  # cell -> vertex ids
    verts = []  # (x, y, tri index)
    for ti, (pts, src, kind) in enumerate(tris):
        for (x, y) in pts:
            grid[(int(math.floor(x / CELL)), int(math.floor(y / CELL)))].append(len(verts))
            verts.append((x, y, ti))
    found = set()
    for ti, (pts, src, kind) in enumerate(tris):
        for e in range(3):
            (ax, ay), (bx, by) = pts[e], pts[(e + 1) % 3]
            dx, dy = bx - ax, by - ay
            L = math.hypot(dx, dy)
            if L < 2 * EPS_END:
                continue
            steps = int(L / (CELL / 2)) + 1
            cells = set()
            for s in range(steps + 1):
                x, y = ax + dx * s / steps, ay + dy * s / steps
                cx, cy = int(math.floor(x / CELL)), int(math.floor(y / CELL))
                for ox in (-1, 0, 1):
                    for oy in (-1, 0, 1):
                        cells.add((cx + ox, cy + oy))
            for c in cells:
                for vi in grid.get(c, ()):
                    vx, vy, vt = verts[vi]
                    if vt == ti:
                        continue
                    t = ((vx - ax) * dx + (vy - ay) * dy) / (L * L)
                    if t * L <= EPS_END or (1 - t) * L <= EPS_END:
                        continue
                    if abs((vx - ax) * dy - (vy - ay) * dx) / L > EPS_LINE:
                        continue
                    found.add((round(vx, 3), round(vy, 3), ti, e))
    # a crack needs a NEIGHBOUR whose boundary runs along the edge with an extra point on it: keep junctions
    # where some other triangle has an edge from that vertex lying along the same line (a vertex of another
    # mesh merely touching the edge leaves no gap)
    edges_at = defaultdict(list)  # vertex position -> other ends of edges there
    for ti, (pts, src, kind) in enumerate(tris):
        for e in range(3):
            p, q = pts[e], pts[(e + 1) % 3]
            edges_at[(round(p[0], 3), round(p[1], 3))].append(q)
            edges_at[(round(q[0], 3), round(q[1], 3))].append(p)
    real = set()
    for (vx, vy, ti, e) in found:
        (ax, ay), (bx, by) = tris[ti][0][e], tris[ti][0][(e + 1) % 3]
        dx, dy = bx - ax, by - ay
        L = math.hypot(dx, dy)
        for (wx, wy) in edges_at[(vx, vy)]:
            if abs((wx - ax) * dy - (wy - ay) * dx) / L <= EPS_LINE and math.hypot(wx - vx, wy - vy) > EPS_END:
                real.add((vx, vy, ti, e))
                break
    found = real
    by = defaultdict(int)
    vkind = defaultdict(int)  # a vertex position -> 1 if any triangle there is a piece
    for (x, y, t) in verts:
        k = (round(x, 3), round(y, 3))
        vkind[k] |= tris[t][2]
    for (vx, vy, ti, e) in found:
        v = "piece vertex" if vkind[(vx, vy)] else "mesh vertex"
        by["%s on %s edge" % (v, "piece" if tris[ti][2] else "mesh")] += 1
    return found, by


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path = args[0] if args else os.path.expanduser("~/Library/Application Support/Azahar/sdmc/3ds/oot/tjdump.bin")
    tris = load(path)
    found, by = count(tris)
    pieces = sum(1 for t in tris if t[2])
    print("%d triangles (%d pieces of split/clipped ones), %d T-junctions (%s)" % (
        len(tris), pieces, len(found), ", ".join("%s %d" % kv for kv in sorted(by.items()))))
    if "--png" in sys.argv:
        from PIL import Image, ImageDraw
        out = sys.argv[sys.argv.index("--png") + 1]
        xs = [p[0] for t in tris for p in t[0]]
        ys = [p[1] for t in tris for p in t[0]]
        W, H = 800, 480
        img = Image.new("RGB", (W, H), (0, 0, 0))
        d = ImageDraw.Draw(img)
        def to(p):
            return (W / 2 + p[0] * (W / 2) / (SCALE * 1.2), H / 2 - p[1] * (H / 2) / (SCALE * 1.2))
        for pts, src, kind in tris:
            d.polygon([to(p) for p in pts], outline=(90, 90, 90) if not kind else (40, 90, 160))
        for (vx, vy, ti, e) in found:
            x, y = to((vx, vy))
            d.ellipse([x - 2, y - 2, x + 2, y + 2], fill=(255, 60, 60))
        img.save(out)
        print("wrote", out)


if __name__ == "__main__":
    main()
