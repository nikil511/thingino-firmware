import sys, numpy as np
from PIL import Image, ImageDraw
d = sys.argv[1]
def rd(n):
    b = open(f"{d}/{n}.pgm", "rb").read(); p = b.split(b"\n", 3); w, h = map(int, p[1].split())
    return np.frombuffer(p[3][:w*h], np.uint8).reshape(h, w).astype(np.float32)
ref = rd("mode0")
# expected placement from the driver's window formula (array 1928x1088, 1080p output at offset 4,4)
def expect(w, h, full):
    ys = (1088 - (h + 8)) // 2
    if full: return 0, ys
    aw = w + 32; xs = (1928 - aw) // 2
    return xs + 16 - 4, ys + 4 - 4
modes = {1: (1280, 720, 0), 2: (1920, 540, 1), 3: (960, 540, 0), 4: (640, 480, 0), 5: (640, 360, 0), 6: (1920, 360, 1)}
def norm(a): a = a - a.mean(); return a / (a.std() + 1e-6)
res = {}
for m, (w, h, full) in modes.items():
    img = rd(f"mode{m}"); ih, iw = img.shape
    ex, ey = expect(w, h, full)
    best = None
    # coarse-to-fine: search +-48 px around expected placement, step 2, then refine +-2
    for step, rng, cx, cy in ((4, 64, ex, ey), (1, 4, None, None)):
        if cx is None: cx, cy = best[1], best[2]
        for dy in range(-rng, rng + 1, step):
            for dx in range(-rng, rng + 1, step):
                x, y = cx + dx, cy + dy
                if x < 0 or y < 0 or x + iw > 1920 or y + ih > 1080: continue
                a = norm(ref[y:y+ih:4, x:x+iw:4]); b = norm(img[::4, ::4])
                s = float((a * b).mean())
                if best is None or s > best[0]: best = (s, x, y)
    res[m] = (best, ex, ey, iw, ih)
    print(f"mode {m}: {iw}x{ih} found at ({best[1]},{best[2]}) corr {best[0]:.3f}; expected ({ex},{ey}); "
          f"H frac {iw/1920:.3f} V frac {ih/1080:.3f}")
# overlay
im = Image.fromarray(ref.astype(np.uint8)).convert("RGB"); dr = ImageDraw.Draw(im)
cols = {1: (255, 80, 80), 2: (80, 200, 255), 3: (255, 200, 0), 4: (120, 255, 120), 5: (255, 120, 255), 6: (255, 255, 255)}
for m, (best, ex, ey, iw, ih) in res.items():
    x, y = best[1], best[2]
    for t in range(3): dr.rectangle([x + t, y + t, x + iw - 1 - t, y + ih - 1 - t], outline=cols[m])
    dr.text((x + 8, y + 6), f"mode {m}", fill=cols[m])
im.save(sys.argv[2]); print("saved", sys.argv[2])
