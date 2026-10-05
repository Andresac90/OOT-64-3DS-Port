#!/usr/bin/env python3
"""make_banner3d.py - stereoscopic HOME Menu banner: picture layers at different depths.

The HOME Menu draws a title's banner as a 3D model (CGFX) with its own camera, in stereo with the 3D slider. bannertool
alone makes a single flat picture, so the banner had no depth. Here the banner is three textured planes:
  - the night sky, behind the screen,
  - the title text, at the screen,
  - the figure (an ocarina, original artwork; or Link rendered from your own game data), in front of the screen.
Each plane is placed and sized for the HOME Menu camera (pycgfx's banner-camera.gltf: 30 degree vertical field of
view, 5:3, 44.786 units in front of the scene, looking at y = 1), so all three land where the flat layout puts them
and only the 3D slider separates them. The planes are unlit: they show their pictures exactly.

Needs Python 3.10+ with Pillow and gltflib, pycgfx (https://github.com/skyfloogle/pycgfx; not bundled: no license
file upstream) in $PYCGFX or tools/pycgfx/, and bannertool (https://github.com/diasurgical/bannertool) on PATH or in
$BANNERTOOL.

usage: make_banner3d.py OUT.bnr [--figure FIGURE.png] [--preview PREVIEW.png]
  FIGURE (transparent background) replaces the ocarina: tools/make_link_banner.py passes Link's render for the
  local-only port/banner_local.bnr. Without it: the original artwork, for port/banner.bnr (tools/make_banner.sh).
"""
import argparse, io, math, os, shutil, struct, subprocess, sys, tempfile, wave

from PIL import Image, ImageDraw, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# the HOME Menu banner camera (pycgfx banner-camera.gltf); at depth 0 the 400x240 top screen spans 40 x 24 units
CAM_Z, CAM_Y = 44.786, 1.0
SKY_Z, TITLE_Z, FIGURE_Z = -18.0, 0.0, 7.0  # behind the screen, at it, in front of it


def screen_rect(x0, y0, x1, y1, z):
    """a rectangle of top-screen pixels as world corners on the plane at depth z (seen from the camera, it covers
    exactly those pixels): returns (left, top, right, bottom) in world units"""
    s = (CAM_Z - z) / CAM_Z
    return ((x0 - 200) * 0.1 * s, CAM_Y + (120 - y0) * 0.1 * s,
            (x1 - 200) * 0.1 * s, CAM_Y + (120 - y1) * 0.1 * s)


# ---- the pictures --------------------------------------------------------------------------------------------------
def draw_sky():
    """256x128 for a plane covering the screen plus a margin (440x264 px): night gradient and stars"""
    W, H, S = 256, 128, 4
    img = Image.new("RGB", (W * S, H * S))
    d = ImageDraw.Draw(img)
    for y in range(H * S):
        t = y / (H * S - 1)
        d.line([(0, y), (W * S, y)], fill=(int(14 + 26 * t), int(22 + 46 * t), int(60 + 64 * t)))
    # the texture is stretched 440:264 over 256:128 - stars drawn wider by that ratio stay round on screen
    k = (264 / 128) / (440 / 256)
    for (x, y, r) in ((60, 70, 3), (190, 40, 2), (300, 90, 3), (420, 30, 2), (600, 60, 3), (760, 35, 2),
                      (900, 80, 3), (980, 200, 2), (520, 150, 2), (120, 230, 2), (700, 250, 3), (850, 300, 2)):
        rx, ry = r * S // 2 * k, r * S // 2
        d.ellipse([x - rx, y - ry, x + rx, y + ry], fill=(230, 235, 255))
    return img.resize((W, H), Image.LANCZOS)


def draw_title():
    """256x128, transparent, the text block (shown on 224x112 screen pixels at the screen plane)"""
    W, H, S = 256, 128, 4
    img = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def fit(text, size, width):
        while size > 6:
            try:
                font = ImageFont.load_default(size=size * S)
            except TypeError:  # Pillow < 10.1: fixed bitmap font
                return ImageFont.load_default()
            if d.textbbox((0, 0), text, font=font)[2] <= width * S:
                return font
            size -= 1
        return ImageFont.load_default()

    big = fit("Ocarina of Time", 34, W - 8)
    small = fit("Unofficial native 3DS port", 17, W - 8)
    gold, blue = (255, 236, 160, 255), (200, 220, 255, 255)
    for (y, text, font, col) in ((6, "Ocarina of Time", big, gold), (44, "64", big, gold),
                                 (88, "Unofficial native", small, blue), (106, "3DS port", small, blue)):
        # a soft dark outline keeps the text readable over the sky in both eyes
        for ox, oy in ((-2, 0), (2, 0), (0, -2), (0, 2)):
            d.text((4 * S + ox * S, y * S + oy * S), text, font=font, fill=(8, 12, 30, 140))
        d.text((4 * S, y * S), text, font=font, fill=col)
    return img.resize((W, H), Image.LANCZOS)


def draw_ocarina():
    """the original artwork figure: a generic blue ocarina (as tools/make_banner.py), transparent background"""
    S = 4
    body = Image.new("RGBA", (128 * S, 224 * S), (0, 0, 0, 0))
    b = ImageDraw.Draw(body)
    ox, oy = 50, 340
    b.ellipse([ox, oy, ox + 300, oy + 175], fill=(40, 110, 220, 255), outline=(170, 210, 255, 255), width=8)
    b.rounded_rectangle([ox + 270, oy + 58, ox + 360, oy + 115], radius=16, fill=(200, 205, 215, 255),
                        outline=(240, 240, 250, 255), width=6)
    for hx, hy in ((ox + 80, oy + 62), (ox + 132, oy + 48), (ox + 185, oy + 55), (ox + 105, oy + 115),
                   (ox + 162, oy + 120)):
        b.ellipse([hx - 16, hy - 16, hx + 16, hy + 16], fill=(10, 30, 80, 255))
    b.ellipse([ox + 35, oy + 25, ox + 175, oy + 72], fill=(120, 180, 255, 110))
    body = body.rotate(28, resample=Image.BICUBIC, center=(ox + 180, oy + 88))
    return body.resize((128, 224), Image.LANCZOS)


def figure_texture(figure):
    """the figure fitted into 128x224 (bottom-aligned, centered), on a 128x256 transparent texture"""
    fig = figure.crop(figure.getbbox()) if figure.getbbox() else figure
    scale = min(128 / fig.width, 224 / fig.height)
    fw, fh = max(1, int(fig.width * scale)), max(1, int(fig.height * scale))
    tex = Image.new("RGBA", (128, 256), (0, 0, 0, 0))
    tex.alpha_composite(fig.resize((fw, fh), Image.LANCZOS), ((128 - fw) // 2, 224 - fh))
    return tex


# layout on the top screen (pixels): sky everywhere (+ margin for the eyes' shift), figure left, text right
SKY_PX = (-20, -12, 420, 252)
TITLE_PX = (168, 58, 392, 170)     # 224 x 112: the 256x128 text texture at 2:1
FIGURE_PX = (14, 8, 140, 228)      # 126 x 220: the 128x224 part of the figure texture (v 0 .. 224/256)


# ---- glTF scene ----------------------------------------------------------------------------------------------------
def build_glb(path, layers):
    """layers: (name, image, (x0, y0, x1, y1) screen px, z, alpha mode, v_max)"""
    from gltflib import (GLTF, GLTFModel, Asset, Scene, Node, Mesh, Primitive, Attributes, Buffer, BufferView,
                         Accessor, AccessorType, ComponentType, BufferTarget, Material, PBRMetallicRoughness,
                         TextureInfo, Texture, Image as GImage, Sampler, GLBResource)
    blob = bytearray()
    views, accessors, meshes, nodes, materials, textures, images = [], [], [], [], [], [], []

    def add_view(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        blob.extend(data)
        views.append(BufferView(buffer=0, byteOffset=off, byteLength=len(data), target=target))
        return len(views) - 1

    for i, (name, img, px, z, alpha, vmax) in enumerate(layers):
        l, t, r, b = screen_rect(*px, z)
        pos = [(l, t, z), (r, t, z), (r, b, z), (l, b, z)]
        uv = [(0.0, 0.0), (1.0, 0.0), (1.0, vmax), (0.0, vmax)]
        nrm = [(0.0, 0.0, 1.0)] * 4
        vp = add_view(struct.pack("<12f", *[c for p in pos for c in p]), BufferTarget.ARRAY_BUFFER.value)
        accessors.append(Accessor(bufferView=vp, componentType=ComponentType.FLOAT.value, count=4,
                                  type=AccessorType.VEC3.value, min=[min(p[k] for p in pos) for k in range(3)],
                                  max=[max(p[k] for p in pos) for k in range(3)]))
        a_pos = len(accessors) - 1
        vn = add_view(struct.pack("<12f", *[c for p in nrm for c in p]), BufferTarget.ARRAY_BUFFER.value)
        accessors.append(Accessor(bufferView=vn, componentType=ComponentType.FLOAT.value, count=4,
                                  type=AccessorType.VEC3.value))
        a_nrm = len(accessors) - 1
        vt = add_view(struct.pack("<8f", *[c for p in uv for c in p]), BufferTarget.ARRAY_BUFFER.value)
        accessors.append(Accessor(bufferView=vt, componentType=ComponentType.FLOAT.value, count=4,
                                  type=AccessorType.VEC2.value))
        a_uv = len(accessors) - 1
        vi = add_view(struct.pack("<6H", 0, 3, 2, 0, 2, 1), BufferTarget.ELEMENT_ARRAY_BUFFER.value)
        accessors.append(Accessor(bufferView=vi, componentType=ComponentType.UNSIGNED_SHORT.value, count=6,
                                  type=AccessorType.SCALAR.value))
        a_idx = len(accessors) - 1
        png = io.BytesIO()
        img.save(png, "PNG")
        images.append(GImage(name=name, bufferView=add_view(png.getvalue()), mimeType="image/png"))
        textures.append(Texture(source=len(images) - 1, sampler=0))
        materials.append(Material(name="mt_" + name, alphaMode=alpha, alphaCutoff=0.5 if alpha == "MASK" else None,
                                  pbrMetallicRoughness=PBRMetallicRoughness(
                                      baseColorTexture=TextureInfo(index=len(textures) - 1),
                                      metallicFactor=0.0, roughnessFactor=1.0)))
        meshes.append(Mesh(name=name, primitives=[Primitive(
            attributes=Attributes(POSITION=a_pos, NORMAL=a_nrm, TEXCOORD_0=a_uv), indices=a_idx,
            material=len(materials) - 1)]))
        nodes.append(Node(name=name, mesh=len(meshes) - 1))

    model = GLTFModel(asset=Asset(version="2.0"), scenes=[Scene(nodes=list(range(len(nodes))))], scene=0,
                      nodes=nodes, meshes=meshes, materials=materials, textures=textures, images=images,
                      samplers=[Sampler(magFilter=9729, minFilter=9729, wrapS=33071, wrapT=33071)],
                      buffers=[Buffer(byteLength=len(blob))], bufferViews=views, accessors=accessors)
    GLTF(model=model, resources=[GLBResource(bytes(blob))]).export(path)


def pycgfx_dir():
    for p in (os.environ.get("PYCGFX"), os.path.join(REPO, "tools", "pycgfx")):
        if p and os.path.exists(os.path.join(p, "main.py")):
            return p
    return None


def glb_to_cgfx(glb, out):
    """pycgfx's conversion, then every material unlit: stage 0 = the texture (colour and alpha), the other stages pass
    it on, no fragment lighting - the planes show their pictures as drawn, whatever light the HOME Menu sets"""
    d = pycgfx_dir()
    if d is None:
        sys.exit("pycgfx not found: clone https://github.com/skyfloogle/pycgfx to tools/pycgfx or set $PYCGFX")
    sys.path.insert(0, d)
    import gltflib
    import main as pycgfx  # noqa: E402 (pycgfx's main.py)
    from cgfx.mtob import MTOBFlag  # noqa: E402
    cgfx = pycgfx.convert_gltf(gltflib.GLTF.load(glb, load_file_resources=True))
    def entries(d):  # pycgfx's DictInfo iterates names or objects depending on the version
        return [d[x] if isinstance(x, str) else x for x in d]

    for model in entries(cgfx.data.models):
        for mtob in entries(model.materials):
            mtob.flags &= ~(MTOBFlag.FragmentLight | MTOBFlag.VertexLight)
            tc = mtob.fragment_shader.texture_combiners
            tc[0].src_rgb, tc[0].src_alpha, tc[0].combine_rgb, tc[0].combine_alpha = 0x003, 0x003, 0, 0
            for st in tc[1:]:
                st.src_rgb, st.src_alpha, st.combine_rgb, st.combine_alpha = 0xFFF, 0xFFF, 0, 0
    data = pycgfx.write(cgfx)
    if len(data) > 0x80000:
        sys.exit("CGFX too big for the HOME Menu: %d bytes (max 524288)" % len(data))
    with open(out, "wb") as f:
        f.write(data)
    return len(data)


def chime(path):
    """the original two-note chime (tools/make_banner.py): E5, B5 with a decay, 32 kHz mono"""
    rate, n = 32000, int(32000 * 1.2)
    samples = []
    for i in range(n):
        t, v = i / rate, 0.0
        for start, f in ((0.0, 659.25), (0.18, 987.77)):
            if t >= start:
                v += 0.28 * math.sin(2 * math.pi * f * (t - start)) * math.exp(-4.0 * (t - start))
        samples.append(int(max(-1.0, min(1.0, v)) * 32767))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % n, *samples))


def preview(path, layers, sep=1.6):
    """left and right eye side by side (a pinhole camera shifted by +-sep/2 units, parallel), for checking the layout
    and the depth order without a console"""
    out = Image.new("RGB", (800, 240))
    for e, dx in ((0, -sep / 2), (1, sep / 2)):
        eye = Image.new("RGBA", (400, 240), (0, 0, 0, 255))
        for name, img, px, z, alpha, vmax in layers:
            l, t, r, b = screen_rect(*px, z)
            k = CAM_Z / (CAM_Z - z)  # world -> depth-0 screen units, as seen from the shifted eye
            sx0, sx1 = ((l - dx) * k + dx) * 10 + 200, ((r - dx) * k + dx) * 10 + 200
            sy0, sy1 = 120 - (t - CAM_Y) * k * 10, 120 - (b - CAM_Y) * k * 10
            part = img.crop((0, 0, img.width, int(img.height * vmax))).convert("RGBA")
            part = part.resize((max(1, int(sx1 - sx0)), max(1, int(sy1 - sy0))), Image.LANCZOS)
            eye.alpha_composite(part, (int(sx0), int(sy0))) if sx0 >= 0 and sy0 >= 0 else eye.paste(
                part, (int(sx0), int(sy0)), part)
        out.paste(eye.convert("RGB"), (e * 400, 0))
    out.save(path)


def bannertool():
    for p in (os.environ.get("BANNERTOOL"), shutil.which("bannertool"),
              os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "tools/bin/bannertool")):
        if p and os.path.exists(p):
            return p
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--figure", help="PNG with a transparent background (default: the ocarina)")
    ap.add_argument("--preview", help="write a side-by-side stereo preview PNG")
    a = ap.parse_args()
    tool = bannertool()
    if tool is None:
        sys.exit("bannertool not found (PATH or $BANNERTOOL)")
    fig = Image.open(a.figure).convert("RGBA") if a.figure else draw_ocarina()
    layers = [("sky", draw_sky(), SKY_PX, SKY_Z, "OPAQUE", 1.0),
              ("title", draw_title(), TITLE_PX, TITLE_Z, "BLEND", 1.0),
              ("figure", figure_texture(fig), FIGURE_PX, FIGURE_Z, "BLEND", 224 / 256)]
    if a.preview:
        preview(a.preview, layers)
    with tempfile.TemporaryDirectory() as tmp:
        glb, cgfx, wav = (os.path.join(tmp, n) for n in ("banner.glb", "banner.cgfx", "banner.wav"))
        build_glb(glb, layers)
        size = glb_to_cgfx(glb, cgfx)
        chime(wav)
        r = subprocess.run([tool, "makebanner", "-ci", cgfx, "-a", wav, "-o", a.out], capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(a.out):
            sys.exit("bannertool failed:\n" + r.stdout + r.stderr)
    print("%s: stereoscopic banner (CGFX %d bytes, 3 layers)" % (a.out, size))


if __name__ == "__main__":
    main()
