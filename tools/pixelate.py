#!/usr/bin/env python3
"""Turn a PNG/JPG into a pixel-art album cover for player.

Examples:
  ./pixelate.py "music/Radiohead/MINIDISCS [HACKED]"              pick an image from a list, pick a size
  ./pixelate.py art.png "music/Radiohead/MINIDISCS [HACKED]"      pick a size
  ./pixelate.py art.png ALBUM --size large                  no questions at all
  ./pixelate.py art.png ALBUM -s 40 -c 128 --saturation 1.5 custom size / colours
  ./pixelate.py art.png ALBUM --original                    the image as it is: true colours, no
                                                            crop, no effects (sharpest the terminal can show)
  ./pixelate.py art.png ALBUM --preview                     just show the options, save nothing
  player --cover ...                                 same thing, via the player

IMAGE and ALBUM can be given in either order (a folder is the album). With no
image, the images in the album folder, the current folder and ~/Pictures are
listed to choose from. The cover is saved as cover.ppm in the album folder and
re-running REPLACES the existing one.
"""
import argparse
import glob
import os
import sys

from PIL import Image, ImageEnhance

NAMED = {"small": 16, "medium": 24, "large": 32}
ORIGINAL_SIZE = 48   # --original: longest side in pixels (64 is the player's maximum)
IMAGE_EXT = (".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif")


def pixelate(img: Image.Image, size: int, colors: int, saturation: float, contrast: float) -> Image.Image:
    img = img.convert("RGB")
    w, h = img.size
    side = min(w, h)                                   # centre square crop
    img = img.crop(((w - side) // 2, (h - side) // 2, (w + side) // 2, (h + side) // 2))
    # LANCZOS keeps edges and bright details that a plain average washes out.
    small = img.resize((size, size), Image.Resampling.LANCZOS)
    small = ImageEnhance.Color(small).enhance(saturation)
    small = ImageEnhance.Contrast(small).enhance(contrast)
    if colors >= 256:
        return small
    # Octree gives rare bright colours their own palette slots.
    return small.quantize(colors, method=Image.Quantize.FASTOCTREE).convert("RGB")


def original_image(img: Image.Image, size: int) -> Image.Image:
    """The picture itself, just scaled down: keeps its shape and all its colours."""
    img = img.convert("RGB")
    w, h = img.size
    scale = size / max(w, h)
    nw = max(1, round(w * scale))
    nh = max(2, round(h * scale / 2) * 2)              # even height: two pixels per text row
    return img.resize((nw, nh), Image.Resampling.LANCZOS)


def render(img: Image.Image) -> list[str]:
    """Same drawing the player uses: '▀' = top pixel fg, bottom pixel bg."""
    px, (w, h) = img.load(), img.size
    rows = []
    for y in range(0, h, 2):
        row = ""
        for x in range(w):
            t = px[x, y]
            b = px[x, y + 1] if y + 1 < h else t
            row += f"\x1b[38;2;{t[0]};{t[1]};{t[2]}m\x1b[48;2;{b[0]};{b[1]};{b[2]}m▀"
        rows.append(row + "\x1b[0m")
    return rows


def ask(prompt: str) -> str:
    try:
        return input(prompt).strip()
    except (EOFError, KeyboardInterrupt):
        print()
        sys.exit(1)


def choose_image(album: str) -> str:
    places = [album, ".", os.path.expanduser("~/Pictures"), os.path.expanduser("~/Downloads")]
    found, seen = [], set()
    for d in places:
        for p in sorted(glob.glob(os.path.join(glob.escape(d), "*"))):
            real = os.path.realpath(p)
            if p.lower().endswith(IMAGE_EXT) and real not in seen:
                seen.add(real)
                found.append(p)
    if not found:
        sys.exit("No images found in the album folder, here, ~/Pictures or ~/Downloads. "
                 "Pass one: ./pixelate.py IMAGE ALBUM")
    print("\n  Images:")
    for i, p in enumerate(found, 1):
        print(f"  {i:3}) {p}")
    while True:
        s = ask("\nUse which image? [number, or q] ").lower()
        if s == "q":
            sys.exit(0)
        if s.isdigit() and 1 <= int(s) <= len(found):
            return found[int(s) - 1]


def show(options: dict) -> None:
    """options: key -> (name, image), drawn side by side."""
    rendered = {k: render(img) for k, (_, img) in options.items()}
    height = max(len(r) for r in rendered.values())
    print()
    print("  " + "".join(f"{k}) {name} {img.width}px".ljust(img.width + 4)
                         for k, (name, img) in options.items()))
    for y in range(height):
        line = "  "
        for k, (_, img) in options.items():
            r = rendered[k]
            line += (r[y] if y < len(r) else " " * img.width) + "    "
        print(line)
    print()


def size_arg(s: str) -> int:
    n = NAMED.get(s.lower()) or (int(s) if s.isdigit() else 0)
    if not 4 <= n <= 64:
        raise argparse.ArgumentTypeError("use small, medium, large or a number 4-64")
    return n


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", metavar="IMAGE|ALBUM", help="image file and/or album folder")
    ap.add_argument("-s", "--size", type=size_arg, help="small (16) / medium (24) / large (32) / 4-64; skips the question")
    ap.add_argument("-c", "--colors", type=int, default=96, help="palette size, 2-256 (256 = no reduction). default 96")
    ap.add_argument("--saturation", type=float, default=1.3, help="colour boost, 1 = unchanged. default 1.3")
    ap.add_argument("--contrast", type=float, default=1.1, help="contrast boost, 1 = unchanged. default 1.1")
    ap.add_argument("-o", "--output", default="cover.ppm", help="file name inside the album folder. default cover.ppm")
    ap.add_argument("--original", action="store_true",
                    help=f"keep the original image: no pixel-art look, colour cut or crop (size {ORIGINAL_SIZE}, or -s)")
    ap.add_argument("--preview", action="store_true", help="only show the options, don't save")
    a = ap.parse_args()

    image = album = None
    for p in a.paths:
        if os.path.isdir(p):
            album = p
        elif os.path.isfile(p):
            image = p
        else:
            ap.error(f"no such file or folder: {p}")
    if album is None:
        album = os.path.dirname(image) if image else "."
        album = album or "."
    if image is None:
        image = choose_image(album)

    original = Image.open(image)
    args = (a.colors, a.saturation, a.contrast)
    out = os.path.join(album, a.output)

    if a.original:
        chosen = original_image(original, a.size or ORIGINAL_SIZE)
        print()
        print("\n".join("  " + r for r in render(chosen)))
    elif a.size:
        chosen = pixelate(original, a.size, *args)
        print()
        print("\n".join("  " + r for r in render(chosen)))
    else:
        options = {k: (name, pixelate(original, n, *args)) for k, (name, n) in zip("123", NAMED.items())}
        show(options)
        # Option 4: the untouched image, shown below (too wide to sit beside the others).
        orig = original_image(original, ORIGINAL_SIZE)
        print(f"  4) original {orig.width}x{orig.height}px")
        print("\n".join("  " + r for r in render(orig)))
        print()
        options["4"] = ("original", orig)
        if a.preview:
            return 0
        choice = ""
        while choice not in options:
            choice = ask("Keep which one? [1/2/3/4, q to cancel] ").lower()
            if choice == "q":
                print("Cancelled, nothing written.")
                return 0
        chosen = options[choice][1]

    if a.preview:
        return 0
    existed = os.path.exists(out)
    chosen.save(out, format="PPM")                     # overwrites any existing cover
    print(f"{'Replaced' if existed else 'Saved'} {chosen.width}x{chosen.height}px cover -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
