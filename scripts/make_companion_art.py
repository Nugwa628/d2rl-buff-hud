"""Writes the companion's own pictures under companion/data/.../ui/d2rloader/buff-panel/.

- small/*.dc6: the D2R-XL 2012 legacy skill icon sheets and the elixir and potion
  pictures at half size. Legacy DC6 pictures draw at their own size whatever a
  layout's "scale", so the half-size legacy panel needs half-size files.
- debuffframe.dc6 / debuffframe.sprite: the red frame around a debuff icon
  (legacy 24 px, HD 78 px like a 0.6-scaled skill icon).

usage: py -3 scripts/make_companion_art.py <MXL2012-D2R checkout with a built work/ folder>
Pure Python, no third-party packages.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LEGACY_OUT = ROOT / "companion/data/global/ui/d2rloader/buff-panel"
HD_OUT = ROOT / "companion/data/hd/global/ui/d2rloader/buff-panel"
ATLASES = ["amskillicon", "soskillicon", "neskillicon", "paskillicon", "baskillicon", "drskillicon",
           "asskillicon", "waskillicon", "skillicon"]
ITEMS = ["invhp1", "invhp2", "invhp3", "invhp4", "invhp5", "invmp1", "invmp2", "invmp3", "invmp4", "invmp5",
         "invvps", "invvpl", "invwps", "invnps", "invyps", "invxyz"]
RED = (200, 24, 16)


def parse_dc6(data: bytes) -> list[tuple[int, int, int, int, bytes]]:
    _ver, _flags, _enc, _term, dirs, per_dir = struct.unpack_from("<6I", data, 0)
    pointers = struct.unpack_from(f"<{dirs * per_dir}I", data, 24)
    frames = []
    for p in pointers:
        _flip, w, h, ox, oy, _unk, _next, length = struct.unpack_from("<8i", data, p)
        frames.append((w, h, ox, oy, data[p + 32:p + 32 + length]))
    return frames


def decode(w: int, h: int, block: bytes) -> list[list[int]]:
    """Palette indices top-down; -1 is transparent."""
    rows = [[-1] * w for _ in range(h)]
    x, y, i = 0, h - 1, 0
    while i < len(block) and y >= 0:
        b = block[i]
        i += 1
        if b == 0x80:
            x, y = 0, y - 1
        elif b & 0x80:
            x += b & 0x7F
        else:
            for k in range(b):
                if x < w:
                    rows[y][x] = block[i + k]
                x += 1
            i += b
    return rows


def encode(rows: list[list[int]]) -> bytes:
    out = bytearray()
    for row in reversed(rows):
        x, w = 0, len(row)
        while x < w:
            if row[x] < 0:
                n = 0
                while x < w and row[x] < 0 and n < 0x7F:
                    x, n = x + 1, n + 1
                if x < w:
                    out.append(0x80 | n)
            else:
                run = []
                while x < w and row[x] >= 0 and len(run) < 0x7F:
                    run.append(row[x])
                    x += 1
                out.append(len(run))
                out += bytes(run)
        out.append(0x80)
    return bytes(out)


def build_dc6(frames: list[tuple[int, int, int, int, bytes]]) -> bytes:
    head = struct.pack("<6I", 6, 1, 0, 0xEEEEEEEE, 1, len(frames))
    offset = 24 + 4 * len(frames)
    pointers, blobs = [], []
    for w, h, ox, oy, block in frames:
        pointers.append(offset)
        blob = struct.pack("<8i", 0, w, h, ox, oy, 0, 0, len(block)) + block + b"\xee\xee\xee"
        blobs.append(blob)
        offset += len(blob)
    return head + struct.pack(f"<{len(pointers)}I", *pointers) + b"".join(blobs)


def nearest(palette: list[tuple[int, int, int]], rgb: tuple[float, float, float]) -> int:
    best, best_d = 1, None
    for index in range(1, 256):
        p = palette[index]
        d = (p[0] - rgb[0]) ** 2 + (p[1] - rgb[1]) ** 2 + (p[2] - rgb[2]) ** 2
        if best_d is None or d < best_d:
            best, best_d = index, d
    return best


def halve(rows: list[list[int]], palette, cache: dict) -> list[list[int]]:
    h, w = len(rows), len(rows[0]) if rows else 0
    out = []
    for y in range(0, h - h % 2, 2):
        line = []
        for x in range(0, w - w % 2, 2):
            cells = [rows[y][x], rows[y][x + 1], rows[y + 1][x], rows[y + 1][x + 1]]
            solid = [c for c in cells if c >= 0]
            if len(solid) < 2:
                line.append(-1)
                continue
            rgb = tuple(sum(palette[c][k] for c in solid) / len(solid) for k in range(3))
            key = tuple(int(v) >> 2 for v in rgb)
            if key not in cache:
                cache[key] = nearest(palette, rgb)
            line.append(cache[key])
        out.append(line)
    return out


def halve_file(source: Path, target: Path, palette, cache: dict) -> None:
    frames = []
    for w, h, ox, oy, block in parse_dc6(source.read_bytes()):
        small = halve(decode(w, h, block), palette, cache)
        frames.append((w // 2, h // 2, ox // 2, oy // 2, encode(small)))
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(build_dc6(frames))


def frame_rows(size: int, border: int, inside: int) -> list[list[int]]:
    return [[inside if min(x, y, size - 1 - x, size - 1 - y) < border else -1 for x in range(size)] for y in range(size)]


def write_hd_frame(path: Path, size: int, border: int) -> None:
    rgba = bytearray()
    for y in range(size):
        for x in range(size):
            edge = min(x, y, size - 1 - x, size - 1 - y)
            rgba += bytes((*RED, 255)) if edge < border else b"\0\0\0\0"
    header = b"SpA1" + struct.pack("<HHIIIIIIII", 31, size, size, size, 0, 1, 0, 0, len(rgba), 4)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + bytes(rgba))


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    work = Path(sys.argv[1]) / "work"
    mod, base = work / "mod/data/global", work / "d2r-baseline"
    pal = (base / "palette/pal.dat").read_bytes()
    palette = [(pal[i * 3 + 2], pal[i * 3 + 1], pal[i * 3]) for i in range(256)]
    cache: dict = {}
    for name in ATLASES:
        source = next(p for p in (mod / "ui/spells" / f"{name}.dc6", base / "ui/spells" / f"{name}.dc6") if p.exists())
        halve_file(source, LEGACY_OUT / "small" / f"{name}.dc6", palette, cache)
    for name in ITEMS:
        source = next(p for p in (mod / "items" / f"{name}.dc6", base / "items" / f"{name}.dc6") if p.exists())
        halve_file(source, LEGACY_OUT / "small" / f"{name}.dc6", palette, cache)
    red = nearest(palette, RED)
    (LEGACY_OUT / "debuffframe.dc6").write_bytes(build_dc6([(24, 24, 0, 0, encode(frame_rows(24, 2, red)))]))
    write_hd_frame(HD_OUT / "debuffframe.sprite", 78, 4)
    print(f"wrote {len(ATLASES) + len(ITEMS)} half-size pictures and the debuff frames (legacy red index {red})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
