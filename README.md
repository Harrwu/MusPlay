# Gruvbox Music Player

A small music player for Windows, macOS, Linux, and iPhone browsers, with
Gruvbox colors and pixel-art album covers.

## Quick start on Windows

1. Download the Windows release ZIP from the GitHub **Releases** page and extract it.
2. Run `player.exe`. It uses the `music` folder beside it (created
   automatically if missing).
3. Put artist and album folders inside that `music` folder. Folders with album
   artwork are grouped as albums; select one to play it.
4. To download from the Internet Archive, choose **Download** in the player.
   The built-in downloader saves selected tracks and available item cover art
   under `windows/music/Artist/Album/`; Python is not required. Copy an
   archive.org link first and the downloader picks it up automatically.
   FLAC tracks are converted to MP3 (tick "keep the original FLAC" to keep both)
   using the free FFmpeg converter, which is downloaded once into a `tools`
   folder beside `player.exe` the first time it is needed.

The Windows release ZIP includes the player with the downloader embedded and
color settings. The regular **Code → Download ZIP** option contains source files only.
To prepare a release ZIP from a built Windows executable, run
`python package_windows.py`. `--with "music/Artist/Album"` makes a separate
`dist/Windows-Music-Player-personal.zip` that also contains those albums, for
your own use only. Upload `dist/Windows-Music-Player.zip` to a GitHub
Release so Windows users do not need to build the app.

## Folder layout

Music is grouped under `music/`; its artist and album folders stay together
there. Platform-specific sources, build scripts, and executables live in their
own folders:

```text
linux/    terminal player and build script
windows/  Windows popup source, executable, and build script
macos/    Cocoa popup source and build script
music/    artist/album folders (the library served to the iPhone)
```

## Playing (Linux)

```sh
./linux/player "music/Radiohead/MINIDISCS [HACKED]"     # play a whole album (each file = one song)
./linux/player "music/Radiohead/MINIDISCS [HACKED]/01 MD111.flac"   # play one file
```

The terminal tab shows `player - <song> - <artist>`.

Playing a single file looks for the songs inside it (handy for one-file
recordings like whole MiniDiscs). The songs found are saved inside FLAC files and
in a `.cue` next to the file (the only place for MP3s). Playing a folder treats
each file as one song. Titles/artists come from FLAC tags or MP3 ID3 tags.

| Key            | Action                                       |
|----------------|----------------------------------------------|
| `space` / `p`  | pause / resume                               |
| `←` `→` (`h` `l`) | jump back / forward 5 seconds             |
| `s`            | skip to next song                            |
| `b`            | restart song (press again quickly: previous) |
| `n`            | next file                                    |
| `q`            | quit                                         |

## Album covers

```sh
./pixelate.py "music/Radiohead/MINIDISCS [HACKED]"                 # choose an image from a list, then a size
./pixelate.py art.png "music/Radiohead/MINIDISCS [HACKED]"         # choose a size
./pixelate.py art.png "music/Radiohead/MINIDISCS [HACKED]" -s large   # no questions (small / medium / large)
```

Saves `cover.ppm` in the album folder; running it again replaces the old one.
Pick option 4 / `--original` to keep the image as it is (full colour, no crop,
no pixel-art effect). Extra options: `-c 128` (more colours), `--saturation 1.5`, `--preview`. See `pixelate --help`.

## Downloading from the Internet Archive

```sh
./tools/ia_download.py https://archive.org/details/SOME-ITEM
```

Lists the files, lets you pick which ones, asks for artist + album, downloads
them into `music/Artist/Album/`, and saves available cover art as `cover.jpg`
or `cover.png`. Interrupted audio downloads can resume when run again. The
Windows player's **Download** button opens a graphical version of this tool.

## Customising the player

```sh
./linux/player --write-config mytheme.conf       # creates a file with every setting
./linux/player --config mytheme.conf "music/Radiohead/MINIDISCS [HACKED]"
```

Without `--config` the player uses the first of: `$PLAYER_CONFIG`,
`./player.conf`, `player.conf` next to the program,
`~/.config/player/config`. Settings include accent colour, cover on/off and
left/right, list length, wave and bar style, symbols and the seek step.

## Other

```sh
./linux/player --scan "music/Radiohead/MINIDISCS [HACKED]"   # re-detect songs inside files
./linux/player --help
```

### Building

```sh
./linux/build.sh
```

Needs `metaflac` (flac) and, for covers, Python 3 with Pillow.

### Windows popup build (cross-compiled from Linux)

```sh
./windows/build.sh
```

After building, `python package_windows.py` prepares a Windows release ZIP with
the executable and color settings in the expected folder layout. The archive
downloader is built into the executable (plain Win32 + WinINet, so it also works under Wine).

Copy `windows/player.exe` and `windows/colors.conf` to Windows and double-click
the executable. It creates `windows/music/` beside itself and scans that folder
automatically. Subfolders are grouped into albums; click an album to
play its tracks and click **‹ Albums** (or press Esc) to return to the library.
Edit `colors.conf` beside the executable to customize `background`, `panel`,
`text`, `dim`, and `accent` with `#RRGGBB` values, or pass `--colors FILE`.
You can also pass a music folder or individual audio files on the command line.
The popup has clickable playback
controls and also accepts space (pause), left/right (seek), `s` (skip song),
`b` (back), `n` (next file), `o` (open folder), and Esc (close). It is a
single self-contained executable, including the Internet Archive downloader.
Finding songs inside one-file recordings and
embedding their markers requires `metaflac.exe` on `PATH`; covers made on Linux
(`cover.ppm`) work as-is.

### macOS popup build

On a Mac with Xcode Command Line Tools and miniaudio installed:

```sh
./macos/build.sh
open macos/player
```

The Cocoa popup accepts file or folder command-line arguments, has an Open dialog,
clickable transport controls, progress seeking, and the same keyboard controls
as the Windows popup. Pass an album folder or audio file when launching it, or
open a folder from inside the window.

### Private iOS streaming app

Run `./ios/server/run.sh` on the computer that holds the music. It serves a
low-memory web player on port `1411`; open the printed LAN URL in iPhone Safari
and choose **Share → Add to Home Screen**. The setup notes are in
[ios/README.md](ios/README.md). Tailscale lets the iPhone connect to the
computer's Tailscale IP without router port forwarding.
# MusPlay
