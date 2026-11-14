#!/usr/bin/env python3
"""Download audio and cover art from a public Internet Archive item."""
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
LIBRARY = os.path.join(ROOT, "music")
AUDIO = (".flac", ".mp3", ".ogg", ".wav", ".m4a", ".opus")
IMAGES = (".jpg", ".jpeg", ".png")
UA = {"User-Agent": "GruvboxMusicPlayer/1.0 (Internet Archive downloader)"}


def item_id(url: str) -> str:
    m = re.search(r"archive\.org/(?:details|download|metadata)/([^/?#]+)", url)
    if m:
        return urllib.parse.unquote(m.group(1))
    if re.fullmatch(r"[\w.\-]+", url):
        return url
    raise ValueError("Enter an archive.org item link or identifier.")


def fetch_item(ident: str) -> dict:
    url = "https://archive.org/metadata/" + urllib.parse.quote(ident)
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
        meta = json.load(r)
    if not meta.get("files"):
        raise ValueError("No files found for this Archive item.")
    return meta


def audio_files(meta: dict) -> list:
    files = [f for f in meta["files"] if f.get("name", "").lower().endswith(AUDIO)]
    originals = [f for f in files if f.get("source") == "original"]
    return sorted(originals or files, key=lambda f: f["name"].casefold())


def human(n) -> str:
    n = float(n or 0)
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


def safe(name: str) -> str:
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "-", str(name)).strip(" .")
    return name or "Unknown"


def download(ident: str, entry: dict, dest: str, progress=None) -> bool:
    name = entry["name"]
    url = f"https://archive.org/download/{urllib.parse.quote(ident)}/{urllib.parse.quote(name)}"
    part = dest + ".part"
    have = os.path.getsize(part) if os.path.exists(part) else 0
    total = int(entry.get("size") or 0)
    headers = dict(UA, **({"Range": f"bytes={have}-"} if have else {}))
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60) as r:
            if have and r.status != 206:
                have = 0
            with open(part, "ab" if have else "wb") as out:
                done = have
                while chunk := r.read(1 << 16):
                    out.write(chunk)
                    done += len(chunk)
                    if progress:
                        progress(f"{name}: {human(done)} / {human(total)}")
        if total and os.path.getsize(part) != total:
            raise IOError(f"incomplete file ({human(os.path.getsize(part))} / {human(total)})")
        os.replace(part, dest)
        return True
    except Exception:
        raise


def download_cover(ident: str, files: list, folder: str, progress=None) -> str:
    dest = os.path.join(folder, "cover.jpg")
    if os.path.isfile(dest):
        return "Existing cover kept"
    by_name = {f.get("name", "").casefold(): f for f in files}
    preferred = ("__ia_thumb.jpg", "_itemimage.jpg", "cover.jpg", "cover.jpeg", "cover.png")
    candidate = next((by_name[n] for n in preferred if n in by_name), None)
    if candidate:
        try:
            ext = os.path.splitext(candidate["name"])[1].lower()
            if ext in IMAGES:
                dest = os.path.join(folder, "cover" + ext)
            download(ident, candidate, dest)
            return "Downloaded cover image"
        except Exception:
            if os.path.exists(dest + ".part"):
                os.remove(dest + ".part")
    try:
        url = "https://archive.org/services/img/" + urllib.parse.quote(ident)
        with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=45) as r:
            image = r.read(16 << 20)
            content_type = r.headers.get_content_type()
        if content_type not in ("image/jpeg", "image/png") or len(image) < 128:
            raise ValueError("Archive item has no usable cover image.")
        if image.startswith(b"\x89PNG\r\n\x1a\n"):
            dest = os.path.join(folder, "cover.png")
        with open(dest, "wb") as out:
            out.write(image)
        return "Downloaded cover image"
    except Exception as exc:
        return f"No cover available ({exc})"


def download_selection(ident: str, meta: dict, chosen: list, artist: str, album: str, progress=None) -> tuple:
    artist, album = safe(artist), safe(album)
    folder = os.path.join(LIBRARY, artist, album)
    os.makedirs(folder, exist_ok=True)
    failed = []
    for entry in chosen:
        name = os.path.basename(entry["name"])
        dest = os.path.join(folder, name)
        try:
            if not (os.path.isfile(dest) and (not entry.get("size") or os.path.getsize(dest) == int(entry["size"]))):
                download(ident, entry, dest, progress)
            if name.lower().endswith(".flac") and shutil.which("metaflac"):
                subprocess.run(["metaflac", "--remove-tag=ARTIST", "--remove-tag=ALBUMARTIST",
                                "--remove-tag=ALBUM", f"--set-tag=ARTIST={artist}",
                                f"--set-tag=ALBUMARTIST={artist}", f"--set-tag=ALBUM={album}", dest],
                               check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if progress:
                progress("Downloaded: " + name)
        except Exception as exc:
            failed.append(f"{name}: {exc}")
    cover_status = download_cover(ident, meta.get("files", []), folder, progress)
    return folder, failed, cover_status


def cli(url: str) -> int:
    ident = item_id(url)
    meta = fetch_item(ident)
    files = audio_files(meta)
    if not files:
        raise ValueError("This item has no audio files.")
    md = meta.get("metadata", {})
    print(f"\n  {md.get('title', ident)}\n")
    for i, f in enumerate(files, 1):
        print(f"  {i:3}) {f['name']}  ({human(f.get('size'))})")
    while True:
        answer = input("Download all files or choose numbers (1,3,5-8)? [all]: ").strip().lower() or "all"
        if answer == "all":
            chosen = files; break
        try:
            indices = set()
            for part in answer.replace(" ", "").split(","):
                a, _, b = part.partition("-"); indices.update(range(int(a), int(b or a) + 1))
            if indices and all(1 <= i <= len(files) for i in indices):
                chosen = [files[i - 1] for i in sorted(indices)]; break
        except ValueError:
            pass
        print("Please enter all, or valid track numbers.")
    creator = md.get("creator", "")
    if isinstance(creator, list): creator = creator[0] if creator else ""
    artist = input(f"Artist [{creator or 'Unknown Artist'}]: ").strip() or creator or "Unknown Artist"
    album = input(f"Album [{md.get('title', ident)}]: ").strip() or md.get("title", ident)
    folder, failed, cover_status = download_selection(ident, meta, chosen, artist, album,
                                                       lambda s: print("  " + s))
    print(f"\nSaved to: {folder}\n{cover_status}\nCompleted {len(chosen) - len(failed)} / {len(chosen)} tracks.")
    if failed:
        print("Failed (run again to resume):\n  " + "\n  ".join(failed))
    return 1 if failed else 0


def gui() -> int:
    import queue
    import tkinter as tk
    from tkinter import messagebox, ttk

    root = tk.Tk(); root.title("Download music from Internet Archive"); root.geometry("700x560")
    root.minsize(560, 420)
    events = queue.Queue(); state = {"ident": "", "meta": {}, "files": []}
    frame = ttk.Frame(root, padding=14); frame.pack(fill="both", expand=True)
    ttk.Label(frame, text="Internet Archive item URL or identifier").pack(anchor="w")
    url_var = tk.StringVar(); url_entry = ttk.Entry(frame, textvariable=url_var); url_entry.pack(fill="x", pady=(4, 10))
    ttk.Label(frame, text="Choose the tracks to download (Ctrl+A selects all)").pack(anchor="w")
    listbox = tk.Listbox(frame, selectmode="extended", exportselection=False); listbox.pack(fill="both", expand=True, pady=5)
    form = ttk.Frame(frame); form.pack(fill="x", pady=5)
    artist_var, album_var = tk.StringVar(), tk.StringVar()
    ttk.Label(form, text="Artist").grid(row=0, column=0, sticky="w")
    ttk.Entry(form, textvariable=artist_var).grid(row=0, column=1, sticky="ew", padx=(8, 14))
    ttk.Label(form, text="Album").grid(row=0, column=2, sticky="w")
    ttk.Entry(form, textvariable=album_var).grid(row=0, column=3, sticky="ew", padx=(8, 0))
    form.columnconfigure(1, weight=1); form.columnconfigure(3, weight=2)
    status = tk.StringVar(value="Downloads are saved in the player's music folder.")
    ttk.Label(frame, textvariable=status, wraplength=660).pack(anchor="w", pady=5)
    buttons = ttk.Frame(frame); buttons.pack(fill="x")
    load_button = ttk.Button(buttons, text="Load item"); load_button.pack(side="left")
    download_button = ttk.Button(buttons, text="Download selected tracks", state="disabled"); download_button.pack(side="right")

    def worker(fn, *args):
        def run():
            try: events.put(("ok", fn(*args)))
            except Exception as exc: events.put(("error", str(exc)))
        threading.Thread(target=run, daemon=True).start()

    def load_click():
        try: ident = item_id(url_var.get().strip())
        except ValueError as exc: messagebox.showerror("Invalid item", str(exc), parent=root); return
        load_button.configure(state="disabled"); status.set("Loading item details…")
        worker(lambda: (ident, fetch_item(ident)))

    def download_click():
        selected = listbox.curselection()
        if not selected: messagebox.showinfo("Choose tracks", "Select one or more tracks first.", parent=root); return
        chosen = [state["files"][i] for i in selected]
        artist, album = artist_var.get().strip(), album_var.get().strip()
        if not artist or not album: messagebox.showerror("Album details needed", "Enter an artist and album name.", parent=root); return
        load_button.configure(state="disabled"); download_button.configure(state="disabled")
        status.set("Downloading tracks and cover…")
        worker(download_selection, state["ident"], state["meta"], chosen, artist, album,
               lambda message: events.put(("progress", message)))

    load_button.configure(command=load_click); download_button.configure(command=download_click)
    def poll():
        try:
            while True:
                kind, data = events.get_nowait()
                if kind == "progress": status.set(data)
                elif kind == "error":
                    status.set("Could not load/download item."); messagebox.showerror("Download error", data, parent=root)
                    load_button.configure(state="normal"); download_button.configure(state="normal" if state["files"] else "disabled")
                elif isinstance(data, tuple) and len(data) == 2 and isinstance(data[0], str):
                    ident, meta = data; files = audio_files(meta)
                    if not files:
                        status.set("This Archive item contains no supported audio files.")
                        messagebox.showerror("No audio found", "This item has no supported audio files.", parent=root)
                        load_button.configure(state="normal"); continue
                    state.update(ident=ident, meta=meta, files=files); listbox.delete(0, "end")
                    for entry in files: listbox.insert("end", f"{entry['name']}   ({human(entry.get('size'))})")
                    listbox.selection_set(0, "end"); md=meta.get("metadata", {}); creator=md.get("creator", "")
                    artist_var.set(creator[0] if isinstance(creator, list) and creator else creator or "")
                    album_var.set(md.get("title", ident)); status.set(f"Loaded {len(files)} tracks. Choose tracks, then download.")
                    download_button.configure(state="normal"); load_button.configure(state="normal")
                else:
                    folder, failed, cover = data
                    summary = f"Saved to {folder}. {cover}."
                    if failed: summary += " Failed tracks: " + "; ".join(failed)
                    status.set(summary); messagebox.showinfo("Download complete", summary, parent=root)
                    load_button.configure(state="normal"); download_button.configure(state="normal")
        except queue.Empty: pass
        root.after(120, poll)
    url_entry.bind("<Return>", lambda _e: load_click()); poll(); root.mainloop(); return 0


if __name__ == "__main__":
    try:
        if "--gui" in sys.argv:
            raise SystemExit(gui())
        if len(sys.argv) == 2:
            raise SystemExit(cli(sys.argv[1]))
        print("Run with --gui, or pass an Internet Archive URL/identifier.", file=sys.stderr)
        raise SystemExit(2)
    except (ValueError, urllib.error.URLError) as exc:
        print(f"Download failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
