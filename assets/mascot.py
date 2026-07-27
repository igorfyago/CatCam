"""CatCam mascot: full-body maneki-neko cat, plush geometry, flat vector PIL.

Draw at 4x (2048), LANCZOS-downscale to 512, RGBA transparent.
Outline technique: for every compound group, draw the whole group in plum
expanded by LW first (circles/ellipses: radius+LW; polygons: 24-offset
dilation stamping), then the exact group in fur on top. Uniform rim, no
interior seams inside a group.
"""

import math
from PIL import Image, ImageDraw

S = 4
CANVAS = 512
LW = 0.0  # 2026: no outlines, silhouette + tonal layers only

FUR = (245, 158, 76, 255)     # F59E4C richer orange
INK = (51, 39, 61, 255)       # 33273D features only
DEEP = (221, 126, 51, 255)    # DD7E33 tonal layer (depth without outlines)
CREAM = (255, 243, 224, 255)  # FFF3E0
BLUE = (76, 125, 184, 255)    # 4C7DB8 lens

# global fit transform (design space -> logo space)
GK, GX, GY = 0.94, -14, 4

img = Image.new("RGBA", (CANVAS * S, CANVAS * S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)


def tx(x):
    return (x - 256) * GK + 256 + GX


def ty(y):
    return (y - 256) * GK + 256 + GY


def tr(r):
    return r * GK


def circle(cx, cy, r, col, grow=0.0):
    R = tr(r) + grow
    x, y = tx(cx), ty(cy)
    d.ellipse([(x - R) * S, (y - R) * S, (x + R) * S, (y + R) * S], fill=col)


def ell(cx, cy, rx, ry, col, grow=0.0):
    Rx, Ry = tr(rx) + grow, tr(ry) + grow
    x, y = tx(cx), ty(cy)
    d.ellipse([(x - Rx) * S, (y - Ry) * S, (x + Rx) * S, (y + Ry) * S], fill=col)


def polyg(pts, col, grow=0.0):
    tp = [(tx(p[0]), ty(p[1])) for p in pts]
    if grow > 0:
        for i in range(24):
            a = 2 * math.pi * i / 24
            dx, dy = grow * math.cos(a), grow * math.sin(a)
            d.polygon([((p[0] + dx) * S, (p[1] + dy) * S) for p in tp], fill=col)
    d.polygon([(p[0] * S, p[1] * S) for p in tp], fill=col)


def rounded_tri(pts, r, col):
    """Exact Minkowski sum of a triangle with a disc of radius r:
    edges offset outward by r (hexagon) + circles at the vertices."""
    cx = sum(p[0] for p in pts) / 3.0
    cy = sum(p[1] for p in pts) / 3.0
    hexpts = []
    n = len(pts)
    for i in range(n):
        a, b = pts[i], pts[(i + 1) % n]
        ex, ey = b[0] - a[0], b[1] - a[1]
        L = math.hypot(ex, ey)
        nx, ny = ey / L, -ex / L
        mx, my = (a[0] + b[0]) / 2 - cx, (a[1] + b[1]) / 2 - cy
        if nx * mx + ny * my < 0:
            nx, ny = -nx, -ny
        hexpts.append((a[0] + nx * r, a[1] + ny * r))
        hexpts.append((b[0] + nx * r, b[1] + ny * r))
    polyg(hexpts, col)
    for p in pts:
        circle(p[0], p[1], r, col)


def rotp(p, c, deg):
    a = math.radians(deg)
    x, y = p[0] - c[0], p[1] - c[1]
    return (c[0] + x * math.cos(a) - y * math.sin(a),
            c[1] + x * math.sin(a) + y * math.cos(a))


def cubic(p0, c1, c2, p3, t):
    u = 1 - t
    return (u**3 * p0[0] + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t**3 * p3[0],
            u**3 * p0[1] + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t**3 * p3[1])


def tube(p0, c1, c2, p3, r0, r1, col, grow=0.0, n=90):
    for i in range(n + 1):
        t = i / n
        x, y = cubic(p0, c1, c2, p3, t)
        circle(x, y, r0 + (r1 - r0) * t, col, grow)


def stroke(p0, p1, col, w):
    a = (tx(p0[0]) * S, ty(p0[1]) * S)
    b = (tx(p1[0]) * S, ty(p1[1]) * S)
    d.line([a, b], fill=col, width=max(1, int(w * S)))
    r = w * S / 2
    for p in (a, b):
        d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=col)


def arcw(cx, cy, rx, ry, a0, a1, col, w):
    x, y = tx(cx), ty(cy)
    Rx, Ry = tr(rx), tr(ry)
    d.arc([(x - Rx) * S, (y - Ry) * S, (x + Rx) * S, (y + Ry) * S],
          a0, a1, fill=col, width=max(1, int(w * S)))


# ----- geometry -----
HC = (247, 174)   # head center
TILT = -10        # head tilt toward viewer's left (counter to raised right paw)

EAR_R = 12        # ear corner rounding (dilation radius)
earL = [rotp(p, HC, TILT) for p in [(190, 138), (232, 110), (181, 76)]]
earR = [rotp(p, HC, TILT) for p in [(304, 138), (262, 110), (313, 76)]]


def body_group(col, grow):
    ell(258, 316, 108, 112, col, grow)   # torso
    ell(258, 378, 112, 58, col, grow)    # haunches
    ell(HC[0], HC[1], 88, 80, col, grow)  # head
    rounded_tri(earL, EAR_R + grow, col)  # chunky rounded ears
    rounded_tri(earR, EAR_R + grow, col)


def arm_group(col, grow):
    # springs from the upper torso (shoulder), short and chubby: reads as a
    # limb, not a tail. The paw is a mitten wider than the wrist.
    tube((332, 270), (368, 240), (382, 205), (386, 180), 24, 20, col, grow)
    circle(392, 152, 30, col, grow)


def tail_group(col, grow):
    # root tucked under the body's lower-left edge, wraps the front,
    # tip flicks up and inward, landing on the flank as a clear rounded end
    tube((184, 424), (202, 470), (318, 468), (330, 382), 14, 10.5, col, grow)


# ----- paint (painter's order) -----

# body + head + ears (one continuous silhouette)
body_group(FUR, 0)
# tonal depth comes from the tail + inner ears alone; the silhouette
# carries the rest (restraint is the 2026 move)

# cream chest
ell(254, 332, 46, 66, CREAM)

# inner ears (cream, shrunk toward centroid, rounded)
for pts in (earL, earR):
    cx = sum(p[0] for p in pts) / 3.0
    cy = sum(p[1] for p in pts) / 3.0
    inner = [(cx + 0.42 * (p[0] - cx), cy + 0.42 * (p[1] - cy)) for p in pts]
    rounded_tri(inner, 5, DEEP)

# muzzle patch
mc = rotp((247, 208), HC, TILT)
ell(mc[0], mc[1], 44, 30, CREAM)

# eyes: small dark seeds
for ex in (211, 283):
    e = rotp((ex, 171), HC, TILT)
    ell(e[0], e[1], 7.5, 9.5, INK)

# nose: tiny rounded triangle
nc = rotp((247, 196), HC, TILT)
polyg([(nc[0] - 8, nc[1] - 4), (nc[0] + 8, nc[1] - 4), (nc[0], nc[1] + 6)],
      INK, grow=1.5)

# mouth: two small u-arcs meeting under the nose
for mx in (240, 254):
    m = rotp((mx, 205), HC, TILT)
    arcw(m[0], m[1], 7, 5.5, 0, 180, INK, 4)

# whiskers: fine punctuation
for a, b in [((198, 198), (155, 192)), ((199, 210), (157, 216)),
             ((296, 198), (339, 192)), ((295, 210), (337, 216))]:
    stroke(rotp(a, HC, TILT), rotp(b, HC, TILT), INK, 3)

# collar band + camera-lens tag (the one cool accent, narrative: CatCam)
arcw(250, 240, 58, 34, 30, 150, INK, 14)
circle(251, 286, 22, INK)
circle(251, 286, 16, BLUE)
circle(251, 286, 7, INK)
circle(245.5, 280.5, 4, (255, 255, 255, 255))

# raised arm: IN FRONT of the torso, springing from the shoulder
arm_group(FUR, 0)
ell(392, 161, 12, 10, INK)
for bx, by in [(377, 143), (392, 137), (407, 143)]:
    circle(bx, by, 5, INK)

# tail wrapping the base, in front
tail_group(DEEP, 0)              # tail as the deeper tone = layered depth

# ----- output -----
img.save(r"./neko_2026_2048.png".replace("/", chr(92)))
final = img.resize((CANVAS, CANVAS), Image.LANCZOS)
out = r".\neko_2026.png"
final.save(out)

# debug sheet: render + silhouette + 16px readback
sheet = Image.new("RGBA", (512 * 2 + 160, 512), (255, 255, 255, 255))
sheet.paste(final, (0, 0), final)
sil = Image.new("RGBA", (512, 512), (255, 255, 255, 255))
mask = final.split()[3].point(lambda a: 255 if a > 40 else 0)
plum = Image.new("RGBA", (512, 512), INK)
sil.paste(plum, (0, 0), mask)
sheet.paste(sil, (512, 0))
tiny = final.resize((16, 16), Image.LANCZOS).resize((128, 128), Image.NEAREST)
tinybg = Image.new("RGBA", (128, 128), (255, 255, 255, 255))
tinybg.paste(tiny, (0, 0), tiny)
sheet.paste(tinybg, (512 * 2 + 16, 32))
sheet.save(out.replace("neko_2026.png", "mascot_debug.png"))
print("saved", out)
