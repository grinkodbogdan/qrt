#!/usr/bin/env python3
"""ports/netsurf/icons.py DIR - NetSurf's toolbar icons in Kumo's manner: thin white line
drawings on transparency (a grey copy, *_g, for a disabled button), 64 px."""
import math, sys
from PIL import Image, ImageDraw
S, K = 64, 4                       # size, supersampling
def icon(draw_fn, colour):
    im = Image.new("RGBA", (S * K, S * K), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    draw_fn(d, colour, 3 * K)
    return im.resize((S, S), Image.LANCZOS)
def P(x, y): return (x * K, y * K)
def chevron(left):
    def f(d, c, w):
        a, b = (40, 16), (24, 32)
        e = (40, 48)
        if not left: a, b, e = (24, 16), (40, 32), (24, 48)
        d.line([P(*a), P(*b), P(*e)], fill=c, width=w, joint="curve")
    return f
def reload(d, c, w):
    d.arc([P(18, 18), P(46, 46)], start=-60, end=250, fill=c, width=w)
    tip = (32 + 14 * math.cos(math.radians(-60)), 32 + 14 * math.sin(math.radians(-60)))
    d.line([P(tip[0] - 7, tip[1] - 3), P(*tip), P(tip[0] - 1, tip[1] + 8)], fill=c, width=w)
def stop(d, c, w):
    d.line([P(20, 20), P(44, 44)], fill=c, width=w); d.line([P(20, 44), P(44, 20)], fill=c, width=w)
def history(d, c, w):
    d.ellipse([P(16, 16), P(48, 48)], outline=c, width=w)
    d.line([P(32, 22), P(32, 32), P(40, 36)], fill=c, width=w)
def home(d, c, w):
    d.line([P(16, 32), P(32, 18), P(48, 32)], fill=c, width=w)
    d.line([P(21, 28), P(21, 46), P(43, 46), P(43, 28)], fill=c, width=w)
def osk(d, c, w):
    d.rectangle([P(14, 22), P(50, 44)], outline=c, width=w)
    for i in range(4): d.rectangle([P(19 + i * 8, 27), P(22 + i * 8, 30)], fill=c)
    d.line([P(24, 38), P(40, 38)], fill=c, width=w)
def tri(direction):
    def f(d, c, w):
        pts = {"u": [(22, 38), (32, 26), (42, 38)], "d": [(22, 26), (32, 38), (42, 26)],
               "l": [(38, 22), (26, 32), (38, 42)], "r": [(26, 22), (38, 32), (26, 42)]}[direction]
        d.line([P(*p) for p in pts], fill=c, width=w)
    return f
out = sys.argv[1]
icons = {"back": chevron(True), "forward": chevron(False), "reload": reload, "stop": stop,
         "history": history, "home": home}
for name, fn in icons.items():
    icon(fn, (255, 255, 255, 255)).save(f"{out}/{name}.png")
    icon(fn, (102, 102, 102, 255)).save(f"{out}/{name}_g.png")
icon(osk, (255, 255, 255, 255)).save(f"{out}/osk.png")
for d in "udlr":
    icon(tri(d), (255, 255, 255, 255)).resize((32, 32), Image.LANCZOS).save(f"{out}/scroll{d}.png")
