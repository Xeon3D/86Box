"""Turn pixel art into a Haiku vector icon (HVIF): one shape per colour,
whose paths are the traced outlines of that colour's pixel regions (outer
outlines counter-clockwise, holes clockwise, so any fill rule works).

usage: png2hvif.py in.png out.rdef  (src/CMakeLists.txt adds the signature)"""
import sys
from PIL import Image


def outlines(mask, w, h):
    """Closed boundary loops of the set pixels, as lists of corner points,
    with the region on the left of each edge."""
    edges = {}  # start corner -> list of end corners
    def add(a, b):
        edges.setdefault(a, []).append(b)
    for y in range(h):
        for x in range(w):
            if not mask[y][x]:
                continue
            if y == 0 or not mask[y - 1][x]:
                add((x + 1, y), (x, y))          # top edge, right to left
            if y == h - 1 or not mask[y + 1][x]:
                add((x, y + 1), (x + 1, y + 1))  # bottom, left to right
            if x == 0 or not mask[y][x - 1]:
                add((x, y), (x, y + 1))          # left, downwards
            if x == w - 1 or not mask[y][x + 1]:
                add((x + 1, y + 1), (x + 1, y))  # right, upwards
    loops = []
    while edges:
        start = next(iter(edges))
        loop = [start]
        cur = start
        prev_dir = None
        while True:
            outs = edges[cur]
            if len(outs) > 1 and prev_dir is not None:
                # at a pinch corner, turn so the loops stay simple
                def turn(o):
                    d = (o[0] - cur[0], o[1] - cur[1])
                    return prev_dir[0] * d[1] - prev_dir[1] * d[0]
                outs.sort(key=turn)
                nxt = outs.pop(0)
            else:
                nxt = outs.pop(0)
            if not outs:
                del edges[cur]
            prev_dir = (nxt[0] - cur[0], nxt[1] - cur[1])
            cur = nxt
            if cur == start and start not in edges:
                break
            if cur == start:
                break
            loop.append(cur)
        # drop the corners on straight runs
        pts = []
        n = len(loop)
        for i in range(n):
            a, b, c = loop[i - 1], loop[i], loop[(i + 1) % n]
            if (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]) != 0:
                pts.append(b)
        loops.append(pts)
    return loops


def coord(v):
    """HVIF coordinate: one byte for whole numbers in -32..95, else two."""
    if v == int(v) and -32 <= v <= 95:
        return bytes([int(v) + 32])
    raw = int(round((v + 128.0) * 102.0)) | 0x8000
    return bytes([raw >> 8, raw & 0xff])


def main():
    src, out = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert('RGBA')
    w, h = im.size
    scale = 64.0 / w
    px = im.load()
    colours = {}
    for y in range(h):
        for x in range(w):
            p = px[x, y]
            if p[3] == 0:
                continue
            colours.setdefault(p, []).append((x, y))

    styles, paths, shapes = [], [], []
    order = sorted(colours.items(), key=lambda kv: -len(kv[1]))
    for index, (colour, pixels) in enumerate(order):
        # The colours drawn later paint over this one: grow its region over
        # them where they touch it, which leaves far fewer outlines.
        later = [[False] * w for _ in range(h)]
        for _, other in order[index + 1:]:
            for x, y in other:
                later[y][x] = True
        mask = [[False] * w for _ in range(h)]
        stack = list(pixels)
        for x, y in pixels:
            mask[y][x] = True
        while stack:
            x, y = stack.pop()
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                if 0 <= nx < w and 0 <= ny < h and later[ny][nx] and not mask[ny][nx]:
                    mask[ny][nx] = True
                    stack.append((nx, ny))
        loops = outlines(mask, w, h)
        style_index = len(styles)
        r, g, b, a = colour
        styles.append(bytes([3, r, g, b]) if a == 255 else bytes([1, r, g, b, a]))  # solid colour (no alpha / RGBA)
        first = len(paths)
        for loop in loops:
            assert len(loop) <= 255, len(loop)
            body = bytes([0x02 | 0x08, len(loop)])  # closed, no curves
            for x, y in loop:
                body += coord(x * scale) + coord(y * scale)
            paths.append(body)
        shapes.append((style_index, list(range(first, len(paths)))))

    print(f'{w}x{h}: {len(styles)} styles, {len(paths)} paths, {len(shapes)} shapes')
    assert len(styles) < 256 and len(paths) < 256 and len(shapes) < 256

    data = b'ncif'
    data += bytes([len(styles)]) + b''.join(styles)
    data += bytes([len(paths)]) + b''.join(paths)
    data += bytes([len(shapes)])
    for style, path_ids in shapes:
        data += bytes([10, style, len(path_ids)]) + bytes(path_ids) + bytes([0])  # path source, no flags
    print(len(data), 'bytes')

    hexs = data.hex()
    lines = [hexs[i:i + 64] for i in range(0, len(hexs), 64)]
    with open(out, 'w', newline='\n') as f:
        f.write('/* The 86Box icon (src/unix/assets/%dx%d/net.86box.86Box.png) as a\n'
                '   Haiku vector icon: one shape per colour, traced from the pixels. */\n\n' % (w, h))
        f.write('resource app_flags B_MULTIPLE_LAUNCH;\n\n')
        f.write('resource vector_icon array {\n')
        for line in lines:
            f.write('\t$"%s"\n' % line.upper())
        f.write('};\n')


main()
