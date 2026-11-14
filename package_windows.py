#!/usr/bin/env python3
"""Create a ready-to-run Windows ZIP.

    python package_windows.py                      # release ZIP to share (no music)
    python package_windows.py --with "music/Artist/Album" ...
                                                   # personal ZIP that also includes albums
"""
import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile

ROOT = Path(__file__).resolve().parent
TOP = "Windows Music Player"
FILES = {
    ROOT / "windows" / "player.exe": f"{TOP}/player.exe",
    ROOT / "windows" / "colors.conf": f"{TOP}/colors.conf",
}
README = """Gruvbox Music Player for Windows
================================

1. Extract this whole folder somewhere (for example Documents or Desktop).
2. Double-click player.exe.
3. Click an album to play it. Esc goes back to the album list.

Adding music
------------
Click Download in the player, paste an Internet Archive link
(https://archive.org/details/...) and press Download. Tracks go into the
"music" folder next to player.exe, sorted as Artist / Album.

FLAC files are converted to MP3 automatically. The first time, the player
asks to download the free FFmpeg converter (about 100 MB) into a "tools"
folder next to player.exe.

You can also copy your own Artist / Album folders into the "music" folder.

Keys: space pause, left/right seek, s skip song, b back, n next file, Esc back.
Colours: edit colors.conf next to player.exe.
"""
AUDIO = {".mp3", ".flac", ".ogg", ".wav", ".m4a", ".opus"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--with", dest="albums", action="append", default=[], metavar="ALBUM_DIR",
                        help="album folder (inside music/) to include; makes a personal ZIP")
    args = parser.parse_args()

    missing = [str(p) for p in FILES if not p.is_file()]
    if missing:
        raise SystemExit("Missing files (run windows/build.sh first):\n  " + "\n  ".join(missing))
    music_root = ROOT / "music"
    albums = []
    for a in args.albums:
        path = (ROOT / a).resolve() if not Path(a).is_absolute() else Path(a).resolve()
        if not path.is_dir():
            raise SystemExit(f"Not a folder: {a}")
        if music_root.resolve() not in path.parents:
            raise SystemExit(f"Album must be inside {music_root}: {a}")
        albums.append(path)

    name = "Windows-Music-Player-personal.zip" if albums else "Windows-Music-Player.zip"
    output = ROOT / "dist" / name
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w", ZIP_DEFLATED) as archive:
        for source, arc in FILES.items():
            archive.write(source, arc)
        archive.writestr(f"{TOP}/README.txt", README.replace("\n", "\r\n"))
        archive.writestr(f"{TOP}/music/", "")
        for album in albums:
            for f in sorted(album.rglob("*")):
                if not f.is_file() or f.name.endswith(".part"):
                    continue
                arc = f"{TOP}/music/" + f.relative_to(music_root.resolve()).as_posix()
                # Audio is already compressed; storing it is faster and no bigger.
                archive.write(f, arc, compress_type=ZIP_STORED if f.suffix.lower() in AUDIO else ZIP_DEFLATED)
    size = output.stat().st_size / 1e6
    print(f"Created {output} ({size:.1f} MB)")
    if albums:
        print("This ZIP contains music - keep it for your own use, don't upload it to a public release.")


if __name__ == "__main__":
    main()
