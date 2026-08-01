"""Rebuild site/tablet_mock.webp with a new cat photo but the real app HUD.

Source of truth for HUD = android/app/src/main/res/layout/activity_main.xml
and drawable/*.xml. Screen area of the mock is 960x1280 at offset (55,55)
(measured from the previous mock). Screen is 3.3x the dp layout (960/291dp
width => sp/dp scale 3.3, tablet font scale 1.0).
"""
from PIL import Image, ImageDraw, ImageFont, ImageFilter

S = 3.3                     # px per dp/sp on the 960x1280 screen
SCR_W, SCR_H = 960, 1280
OFF = 55                    # screen offset inside the 1070x1390 mock

def dp(v): return round(v * S)

FONT_REG = "C:/Windows/Fonts/segoeui.ttf"
FONT_MED = "C:/Windows/Fonts/seguisb.ttf"   # sans-serif-medium stand-in

def font(sp, med=True):
    return ImageFont.truetype(FONT_MED if med else FONT_REG, dp(sp))

SCRIM = (0, 0, 0, 0x59)     # #59000000
WHITE = (255, 255, 255, 255)
WHITE70 = (255, 255, 255, 0xB3)
LIVE_RED = (0xE5, 0x39, 0x35, 255)

def pill(dr, box, fill, radius=None):
    r = radius if radius is not None else (box[3]-box[1])//2
    dr.rounded_rectangle(box, radius=r, fill=fill)

def text_size(f, s, spacing=0.0):
    w = f.getbbox(s)[2] + int(spacing * max(len(s)-1, 0))
    return w, f.size

def draw_tracked(dr, xy, s, f, fill, spacing=0.0):
    x, y = xy
    for ch in s:
        dr.text((x, y), ch, font=f, fill=fill)
        x += f.getbbox(ch)[2] + int(spacing)

def main():
    # ---------- base: device frame from the old mock, screen replaced ----------
    mock = Image.open("site/tablet_mock.webp").convert("RGBA")
    cat = Image.open("assets/image-1785620897235.webp").convert("RGB")

    # cover-fill the 960x1280 screen
    ar_c = cat.width / cat.height
    ar_s = SCR_W / SCR_H
    if ar_c > ar_s:  # crop left/right
        nw = int(cat.height * ar_s)
        x0 = (cat.width - nw)//2
        cat2 = cat.crop((x0, 0, x0+nw, cat.height))
    else:
        nh = int(cat.width / ar_s)
        y0 = max(0, (cat.height - nh)//2 - 80)  # bias a touch up: face is upper third
        cat2 = cat.crop((0, y0, cat.width, y0+nh))
    screen = cat2.resize((SCR_W, SCR_H), Image.LANCZOS).convert("RGBA")

    hud = Image.new("RGBA", (SCR_W, SCR_H), (0,0,0,0))
    d = ImageDraw.Draw(hud)

    # ---------- top stack (marginTop 44dp, centered) ----------
    y = dp(44)
    # status pill: red "LIVE · Wi-Fi" (streaming state), pill_solid tinted
    f13 = font(13)
    sp_006 = 0.06 * f13.size           # letterSpacing 0.06
    label = "LIVE · WI-FI".title().replace("Wi-Fi", "Wi-Fi")
    label = "LIVE · Wi-Fi"
    tw, th = text_size(f13, label.upper(), sp_006)
    ph, pv = dp(16), dp(7)
    pw, phh = tw + 2*ph, th + 2*pv
    x0 = (SCR_W - pw)//2
    pill(d, [x0, y, x0+pw, y+phh], LIVE_RED)
    ty = y + (phh - th)//2 - dp(1)
    draw_tracked(d, (x0+ph, ty), label.upper(), f13, WHITE, sp_006)
    y += phh + dp(10)

    # transport segmented: USB | Wi-Fi (Wi-Fi active = white pill, dark text)
    usb, wifi = "USB", "Wi-Fi"
    pad4 = dp(4)
    seg_pv, seg_ph = dp(6), dp(14)
    w_usb = f13.getbbox(usb)[2] + 2*seg_ph
    w_wifi = f13.getbbox(wifi)[2] + 2*seg_ph
    thh = f13.size + 2*seg_pv
    cont_w = w_usb + w_wifi + 2*pad4
    cont_h = thh + 2*pad4
    x0 = (SCR_W - cont_w)//2
    pill(d, [x0, y, x0+cont_w, y+cont_h], SCRIM)
    # inactive USB
    d.text((x0+pad4+seg_ph, y+pad4+seg_pv-dp(1)), usb, font=f13, fill=WHITE70)
    # active Wi-Fi: white pill + dark text (runtime tint inverts active seg)
    wx = x0+pad4+w_usb
    pill(d, [wx, y+pad4, wx+w_wifi, y+pad4+thh], WHITE)
    d.text((wx+seg_ph, y+pad4+seg_pv-dp(1)), wifi, font=f13, fill=(30,30,30,255))

    # ---------- control stack (bottom, marginBottom 26dp) ----------
    blocks = []  # (height, draw_fn(y_top))
    gap14 = dp(14); gap16 = dp(16)

    # row 1: [Cool 0 Warm]  12dp  [Day | Night(active)]
    def row1(yy):
        th13 = f13.size
        # tone pill: Cool(white) 0(70%) Warm(white) inside scrim, pad 6/4
        cw = f13.getbbox("Cool")[2] + dp(24)
        zw = max(dp(30), f13.getbbox("0")[2])
        ww = f13.getbbox("Warm")[2] + dp(24)
        tone_w = cw + zw + ww + dp(12)
        tone_h = th13 + dp(14) + dp(8)
        # day/night pill
        dw = f13.getbbox("Day")[2] + dp(28)
        nw = f13.getbbox("Night")[2] + dp(28)
        dn_w = dw + nw + dp(8)
        dn_h = th13 + dp(12) + dp(8)
        total = tone_w + dp(12) + dn_w
        x = (SCR_W - total)//2
        h = max(tone_h, dn_h)
        # tone
        pill(d, [x, yy, x+tone_w, yy+tone_h], SCRIM)
        ty = yy + (tone_h-th13)//2 - dp(1)
        d.text((x+dp(6)+dp(12), ty), "Cool", font=f13, fill=WHITE)
        d.text((x+dp(6)+cw+(zw-f13.getbbox("0")[2])//2, ty), "0", font=f13, fill=WHITE70)
        d.text((x+dp(6)+cw+zw+dp(12), ty), "Warm", font=f13, fill=WHITE)
        x2 = x + tone_w + dp(12)
        # day/night
        pill(d, [x2, yy, x2+dn_w, yy+dn_h], SCRIM)
        ty2 = yy + (dn_h-th13)//2 - dp(1)
        d.text((x2+dp(4)+dp(14), ty2), "Day", font=f13, fill=WHITE70)
        nx = x2 + dp(4) + dw
        pill(d, [nx, yy+dp(4), nx+nw, yy+dn_h-dp(4)], WHITE)
        d.text((nx+dp(14), ty2), "Night", font=f13, fill=(30,30,30,255))
        return h
    blocks.append((dp(40), row1))
    blocks.append((gap14, None))

    # row 2: zoom  (−) [1.0×] (+)
    def row2(yy):
        f20 = font(20); f14 = font(14)
        circ = dp(44)
        lw = f14.getbbox("1.0×")[2] + dp(32)
        lh = f14.size + dp(16)
        total = circ + dp(14) + lw + dp(14) + circ
        x = (SCR_W - total)//2
        yc = yy + circ//2
        for i, sym in enumerate(["−", "+"]):
            cx = x + i*(circ + dp(14) + lw + dp(14))
            d.ellipse([cx, yy, cx+circ, yy+circ], fill=SCRIM)
            sw = f20.getbbox(sym)
            d.text((cx + (circ - (sw[2]-sw[0]))//2 - sw[0], yc - f20.size//2 - dp(3)), sym, font=f20, fill=WHITE)
        lx = x + circ + dp(14)
        ly = yc - lh//2
        pill(d, [lx, ly, lx+lw, ly+lh], SCRIM)
        d.text((lx+dp(16), ly + (lh-f14.size)//2 - dp(1)), "1.0×", font=f14, fill=WHITE)
        return circ
    blocks.append((dp(44), row2))
    blocks.append((gap14, None))

    # row 3: mic progress bar (170dp x 4dp, some level)
    def row3(yy):
        w, h = dp(170), dp(4)
        x = (SCR_W - w)//2
        pill(d, [x, yy, x+w, yy+h], (255,255,255,60))
        lvl = int(w*0.45)
        pill(d, [x, yy, x+lvl, yy+h], WHITE)
        return h
    blocks.append((dp(4), row3))
    blocks.append((gap16, None))

    # row 4: shutter row (80dp tall): flip left, ring+idle white circle center
    def row4(yy):
        cy = yy + dp(40)
        # flip: 52dp circle at marginStart 64dp
        fx, fr = dp(64), dp(52)
        d.ellipse([fx, cy-fr//2, fx+fr, cy+fr//2], fill=SCRIM)
        # refresh icon (from ic_flip.xml, white, 26dp)
        ic = dp(26)
        draw_flip_icon(d, fx + fr//2, cy, ic)
        # shutter: 76dp ring (3dp white stroke, #26000000 fill) + 60dp white idle circle
        cx = SCR_W//2
        r76 = dp(76)//2
        d.ellipse([cx-r76, cy-r76, cx+r76, cy+r76], fill=(0,0,0,0x26),
                  outline=WHITE, width=dp(3))
        r60 = dp(60)//2
        d.ellipse([cx-r60, cy-r60, cx+r60, cy+r60], fill=WHITE)
        return dp(80)
    blocks.append((dp(80), row4))

    total_h = sum(h for h, _ in blocks)
    yy = SCR_H - dp(26) - total_h
    for h, fn in blocks:
        if fn: fn(yy)
        yy += h

    out = screen
    out.alpha_composite(hud)
    # subtle inner bezel shading handled by frame already; paste into mock
    mock.paste(out, (OFF, OFF))
    mock.save("site/tablet_mock.webp", "WEBP", quality=92, method=6)
    print("wrote site/tablet_mock.webp", mock.size)

def draw_flip_icon(d, cx, cy, size):
    """Material refresh icon (ic_flip path) rasterised at `size` px."""
    # 24x24 viewport path approximated with arcs + arrowheads
    import math
    r = size * 8/24
    lw = max(2, int(size*1.8/24))
    # two arcs (gap at top-right and bottom-left where arrowheads sit)
    d.arc([cx-r, cy-r, cx+r, cy+r], start=105, end=335, fill=WHITE, width=lw)
    d.arc([cx-r, cy-r, cx+r, cy+r], start=285, end=155+360, fill=WHITE, width=lw)
    # arrowheads: top-right points up-right; bottom-left points down-left
    ah = size*5.5/24
    # top arrowhead at angle ~45deg
    ax, ay = cx + r*math.cos(math.radians(45)), cy - r*math.sin(math.radians(45))
    d.polygon([(ax, ay-ah), (ax+ah*1.15, ay+ah*0.35), (ax-ah*0.15, ay+ah*0.95)], fill=WHITE)
    bx, by = cx - r*math.cos(math.radians(45)), cy + r*math.sin(math.radians(45))
    d.polygon([(bx, by+ah), (bx-ah*1.15, by-ah*0.35), (bx+ah*0.15, by-ah*0.95)], fill=WHITE)

if __name__ == "__main__":
    main()
