#!/usr/bin/env python3
"""Low-memory, local music library and streaming web app."""
from __future__ import annotations

import argparse
import html
import json
import mimetypes
import re
import socket
import struct
import zlib
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

AUDIO_EXTENSIONS = {".flac", ".mp3", ".m4a", ".aac", ".wav", ".ogg", ".opus"}
CHUNK_SIZE = 32 * 1024

PAGE = r'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><meta name="theme-color" content="#282828"><meta name="apple-mobile-web-app-capable" content="yes"><title>Music</title><style>
:root{color-scheme:dark;--bg:#17171b;--panel:#25252d;--text:#f2f2f5;--dim:#a1a1ad;--accent:#62d6d2}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;padding:env(safe-area-inset-top) 16px calc(158px + env(safe-area-inset-bottom));max-width:800px;margin-inline:auto}header{padding:18px 2px 10px}h1{font-size:30px;letter-spacing:-.5px;margin:0}.library-path{font-size:12px;color:var(--dim);margin:6px 0 12px;overflow-wrap:anywhere}.status{font-size:13px;color:var(--dim);min-height:20px;margin:8px 2px}.section-title{font-size:13px;text-transform:uppercase;letter-spacing:1px;color:var(--dim);margin:18px 2px 10px}.artist{width:100%;display:flex;align-items:center;text-align:left;gap:14px;padding:12px;margin:8px 0;background:var(--panel);border:1px solid #34343e;border-radius:14px;color:var(--text);font:inherit}.artist-art,.album-art{width:72px;height:72px;object-fit:cover;image-rendering:pixelated;border-radius:9px;background:#303039}.artist-copy{min-width:0}.artist-copy strong,.artist-copy small{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.artist-copy strong{font-size:18px}.artist-copy small{color:var(--dim);font-size:13px;margin-top:5px}.back{background:transparent;border:0;color:var(--accent);font:inherit;padding:8px 0}.artist-heading{display:flex;align-items:center;gap:16px;margin:6px 0 20px}.artist-heading img{width:104px;height:104px;object-fit:cover;image-rendering:pixelated;border-radius:12px}.artist-heading h2{margin:0;font-size:24px}.album{margin:14px 0 20px;padding:14px;background:var(--panel);border:1px solid #34343e;border-radius:16px}.album-head{display:flex;align-items:center;gap:12px;margin-bottom:8px}.album-art{width:58px;height:58px}.album-title{font-size:17px;font-weight:700}.track{display:block;width:100%;text-align:left;padding:11px 9px;border:0;border-top:1px solid #393943;background:transparent;color:var(--text);font:inherit}.track small{display:block;color:var(--dim);font-size:12px;margin-top:4px}.track.active{color:var(--accent)}.empty{color:var(--dim);text-align:center;padding:28px 12px}footer{position:fixed;z-index:2;bottom:0;left:0;right:0;background:#202026f5;border-top:1px solid #393943;padding:12px 16px calc(12px + env(safe-area-inset-bottom));backdrop-filter:blur(18px)}.inner{max-width:768px;margin:auto}.now{display:flex;align-items:center;gap:12px}.meta{min-width:0;flex:1}.meta strong,.meta small{display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.meta small{color:var(--dim);margin-top:3px}.play{width:48px;height:48px;border-radius:50%;border:0;background:var(--accent);color:#101518;font-size:20px}.seek{width:100%;height:24px;accent-color:var(--accent)}.controls{display:flex;justify-content:center;gap:24px}.controls button{min-width:52px;border:0;background:transparent;color:var(--text);font-size:20px}
</style></head><body><header><h1>Music</h1><div id="path" class="library-path"></div></header><div id="status" class="status">Connecting to your music…</div><main id="library"></main><footer><div class="inner"><div class="now"><div class="meta"><strong id="title">Not playing</strong><small id="subtitle">Choose a track</small></div><button id="play" class="play" aria-label="Play or pause">▶</button></div><input id="seek" class="seek" type="range" min="0" max="1000" value="0"><div class="controls"><button id="prev" aria-label="Previous track">|◀</button><button id="skip" aria-label="Next track">▶|</button></div><audio id="audio" preload="none" playsinline></audio></div></footer>
<script>
(()=>{const $=id=>document.getElementById(id),audio=$('audio'),base=location.origin;let artists=[],albums=[],tracks=[],current=-1,selectedArtist='';function msg(s){$('status').textContent=s}function el(tag,cls,text){const n=document.createElement(tag);if(cls)n.className=cls;if(text!==undefined)n.textContent=text;return n}function coverUrl(path){return base+'/cover/'+path.split('/').map(encodeURIComponent).join('/')}function playTrack(i){if(i<0||i>=tracks.length)return;current=i;const t=tracks[i];audio.src=base+t.stream;audio.play().then(()=>{$('play').textContent='Ⅱ'}).catch(()=>{$('play').textContent='▶';msg('Could not play this format in Safari')});$('title').textContent=t.title;$('subtitle').textContent=t.artist+' · '+t.album;renderAlbums()}function renderArtists(){const box=$('library');box.replaceChildren();box.append(el('div','section-title','Choose an artist'));for(const a of artists){const b=el('button','artist'),img=el('img','artist-art');img.src=a.cover;img.alt='';const copy=el('span','artist-copy');copy.append(el('strong','',a.name),el('small','',a.albums+' albums'));b.append(img,copy);b.onclick=()=>openArtist(a.name);box.append(b)}}function renderAlbums(){const box=$('library');box.replaceChildren();const back=el('button','back','‹ All artists');back.onclick=()=>{selectedArtist='';renderArtists();msg(artists.length+' artists')};box.append(back);const a=artists.find(x=>x.name===selectedArtist);const heading=el('div','artist-heading'),img=el('img');img.src=a.cover;img.alt='';heading.append(img,el('h2','',selectedArtist));box.append(heading);if(!albums.length){box.append(el('div','empty','No albums found'));return}for(const album of albums){const card=el('section','album'),head=el('div','album-head'),art=el('img','album-art');art.src=album.cover;art.alt='';head.append(art,el('div','album-title',album.name));card.append(head);for(const t of album.tracks){const idx=tracks.push(t)-1,b=el('button','track'+(idx===current?' active':''));b.append(el('span','',t.title),el('small','',t.artist+' · '+t.album));b.onclick=()=>playTrack(idx);card.append(b)}box.append(card)}}async function openArtist(name){selectedArtist=name;msg('Loading '+name+'…');try{const r=await fetch(base+'/api/artist/'+encodeURIComponent(name),{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);albums=(await r.json()).albums||[];tracks=[];renderAlbums();msg(albums.length+' albums · '+albums.reduce((n,a)=>n+a.tracks.length,0)+' tracks')}catch(e){msg('Could not load artist: '+e.message)}}async function init(){try{msg('Loading artists…');const [r,info]=await Promise.all([fetch(base+'/api/artists',{cache:'no-store'}),fetch(base+'/api/info',{cache:'no-store'})]);if(!r.ok||!info.ok)throw Error('HTTP '+(!r.ok?r.status:info.status));artists=(await r.json()).artists||[];$('path').textContent='Library: '+(await info.json()).root;renderArtists();msg(artists.length+' artists')}catch(e){msg('Could not connect: '+e.message)}}$('play').onclick=()=>{if(!audio.src){if(tracks.length)playTrack(Math.max(0,current));return}if(audio.paused){audio.play();$('play').textContent='Ⅱ'}else{audio.pause();$('play').textContent='▶'}};$('prev').onclick=()=>{if(tracks.length)playTrack((current-1+tracks.length)%tracks.length)};$('skip').onclick=()=>{if(tracks.length)playTrack((current+1)%tracks.length)};audio.onended=()=>{if(tracks.length)playTrack((current+1)%tracks.length)};audio.ontimeupdate=()=>{if(audio.duration)$('seek').value=Math.round(audio.currentTime/audio.duration*1000)};$('seek').oninput=()=>{if(audio.duration)audio.currentTime=audio.duration*$('seek').value/1000};init()})();
</script></body></html>'''

# Reuse one flat, artist-local queue when the screen re-renders after playback.
PAGE = PAGE.replace("const idx=tracks.push(t)-1", "const idx=tracks.indexOf(t)")
PAGE = PAGE.replace(
    "albums=(await r.json()).albums||[];tracks=[];renderAlbums();msg(albums.length+' albums · '+albums.reduce((n,a)=>n+a.tracks.length,0)+' tracks')",
    "albums=(await r.json()).albums||[];tracks=albums.flatMap(a=>a.tracks);renderAlbums();msg(albums.length+' albums · '+tracks.length+' tracks')")
PAGE = PAGE.replace(
    "--bg:#17171b;--panel:#25252d;--text:#f2f2f5;--dim:#a1a1ad;--accent:#62d6d2",
    "--bg:#282828;--panel:#3c3836;--text:#ebdbb2;--dim:#a89984;--accent:#fabd2f")
PAGE = PAGE.replace(
    "</style>",
    "body{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;background:linear-gradient(180deg,#32302f 0,#282828 300px)}"
    "h1{color:#fabd2f;text-transform:uppercase;letter-spacing:1px}.library-path{color:#83a598}"
    ".status:before{content:'› ';color:#b8bb26}.section-title{color:#a89984}"
    ".artist,.album{border-radius:5px;border-color:#504945;box-shadow:3px 3px #1d2021}"
    ".artist{border-left:3px solid #83a598;background:#3c3836}.artist-art,.album-art,.artist-heading img{border-radius:2px;border:1px solid #665c54}"
    ".album{border-left:3px solid #d3869b;background:#3c3836}.album-title{color:#fabd2f}.track{border-color:#504945}"
    ".track.active{color:#b8bb26}.back{color:#83a598}.play{background:#fabd2f;border-radius:4px;color:#282828}"
    "footer{background:#1d2021f5;border-top:1px solid #665c54}.controls button{color:#fe8019}.seek{accent-color:#fabd2f}"
    "</style>")
PAGE = PAGE.replace(
    "</header><div id=\"status\" class=\"status\">Connecting to your music…</div><main id=\"library\"></main>",
    "</header><div id=\"status\" class=\"status\">Connecting to your music…</div>"
    "<button id=\"queue-toggle\" class=\"queue-toggle\">Queue (0)</button><aside id=\"queue-panel\" class=\"queue-panel\" hidden></aside>"
    "<main id=\"library\"></main>")
PAGE = PAGE.replace(
    "</style>",
    ".artist-art,.album-art,.artist-heading img{image-rendering:auto}.album-head{margin:14px 0 4px}"
    ".album-head button{color:var(--text);font:inherit}.album-open{width:100%;display:flex;align-items:center;gap:12px;text-align:left;padding:10px;background:#3c3836;border:1px solid #504945;border-radius:5px}"
    ".album-open img{width:64px;height:64px;object-fit:cover;image-rendering:auto;border:1px solid #665c54}.album-open span{flex:1;font-weight:700;color:#fabd2f}"
    ".album-tracks{padding:4px 0 12px}.track-row{display:flex;align-items:center;border-bottom:1px solid #504945}.track{flex:1}.track-add,.shuffle-album,.queue-toggle,.queue-remove{border:1px solid #665c54;background:#3c3836;color:#fabd2f;border-radius:4px;padding:8px 10px;font:inherit}"
    ".album-tools{display:flex;justify-content:flex-end;padding:8px 0}.queue-toggle{margin:4px 2px}.queue-panel{padding:10px;background:#3c3836;border:1px solid #665c54;border-radius:5px;margin:6px 0 12px}.queue-item{display:flex;justify-content:space-between;align-items:center;padding:8px;border-bottom:1px solid #504945}.queue-item button{color:#ebdbb2;background:transparent;border:0;font:inherit;text-align:left}.now-playing{color:#b8bb26}@media(min-width:600px){.queue-panel{max-height:240px;overflow:auto}}</style>")
PAGE = PAGE.replace(
    "$('<unused>')", "")
PAGE = PAGE.replace(
    "$('subtitle').textContent=t.artist+' · '+t.album;renderAlbums()",
    "$('subtitle').textContent=t.artist+' · '+t.album;"
    "if('mediaSession' in navigator&&'MediaMetadata' in window){navigator.mediaSession.metadata=new MediaMetadata({title:t.title,artist:t.artist,album:t.album,artwork:t.cover?[{src:base+t.cover}]:[]});"
    "navigator.mediaSession.setActionHandler('play',()=>audio.play());navigator.mediaSession.setActionHandler('pause',()=>audio.pause());"
    "navigator.mediaSession.setActionHandler('previoustrack',()=>{if(tracks.length)play((current-1+tracks.length)%tracks.length)});"
    "navigator.mediaSession.setActionHandler('nexttrack',()=>{if(tracks.length)play((current+1)%tracks.length)});"
    "navigator.mediaSession.setActionHandler('seekto',e=>{if(e.seekTime!==undefined)audio.currentTime=e.seekTime})}renderAlbums()")
_script_start = PAGE.index("<script>\n") + len("<script>\n")
_script_end = PAGE.index("\n</script>", _script_start)
PAGE = PAGE[:_script_start] + r'''(()=>{
const $=id=>document.getElementById(id),audio=$('audio'),base=location.origin;
let artists=[],albums=[],tracks=[],currentTrack=null,selectedArtist='',queue=[],history=[],queueVisible=false;
function msg(s){$('status').textContent=s}
function el(tag,cls,text){const n=document.createElement(tag);if(cls)n.className=cls;if(text!==undefined)n.textContent=text;return n}
function playTrack(t,upcoming=[],record=true){if(!t)return;if(record&&currentTrack&&currentTrack.id!==t.id)history.push(currentTrack);currentTrack=t;queue=upcoming.slice();audio.src=base+t.stream;audio.play().then(()=>{$('play').textContent='Ⅱ'}).catch(()=>{$('play').textContent='▶';msg('Could not play this format in Safari')});$('title').textContent=t.title;$('subtitle').textContent=t.artist+' · '+t.album;if('mediaSession'in navigator&&'MediaMetadata'in window){navigator.mediaSession.metadata=new MediaMetadata({title:t.title,artist:t.artist,album:t.album,artwork:t.cover?[{src:base+t.cover}]:[]});for(const [a,fn] of Object.entries({play:()=>audio.play(),pause:()=>audio.pause(),previoustrack:previous,nexttrack:next,seekto:e=>{if(e.seekTime!==undefined)audio.currentTime=e.seekTime}})){try{navigator.mediaSession.setActionHandler(a,fn)}catch(e){}}}renderAlbums();renderQueue()}
function next(){if(queue.length){const t=queue.shift();playTrack(t,queue);return}if(currentTrack){audio.pause();audio.currentTime=0;$('play').textContent='▶';currentTrack=null;$('title').textContent='Queue finished';$('subtitle').textContent='Choose an album or track';renderAlbums();renderQueue()}}
function previous(){if(audio.currentTime>3){audio.currentTime=0;return}if(history.length){const t=history.pop(),upcoming=queue.slice();if(currentTrack)upcoming.unshift(currentTrack);playTrack(t,upcoming,false)}}
function renderArtists(){const box=$('library');box.replaceChildren();box.append(el('div','section-title','Choose an artist'));for(const a of artists){const b=el('button','artist'),img=el('img','artist-art');img.src=a.cover?base+a.cover:'';img.alt='';const copy=el('span','artist-copy');copy.append(el('strong','',a.name),el('small','',a.albums+' albums'));b.append(img,copy);b.onclick=()=>openArtist(a.name);box.append(b)}}
function renderAlbums(){const box=$('library');box.replaceChildren();if(!selectedArtist){renderArtists();return}const back=el('button','back','‹ All artists');back.onclick=()=>{selectedArtist='';renderArtists();msg(artists.length+' artists')};box.append(back);const a=artists.find(x=>x.name===selectedArtist);const heading=el('div','artist-heading'),img=el('img');img.src=a?.cover?base+a.cover:'';img.alt='';heading.append(img,el('h2','',selectedArtist));box.append(heading);if(!albums.length){box.append(el('div','empty','No albums found'));return}for(const album of albums){const card=el('section','album'),head=el('div','album-head'),open=el('button','album-open'),art=el('img','album-art');art.src=album.cover?base+album.cover:'';art.alt='';open.append(art,el('span','',album.name),el('span','',album.open?'⌄':'›'));const count=el('small','',album.tracks.length+' tracks');open.append(count);open.onclick=()=>{album.open=!album.open;renderAlbums()};head.append(open);card.append(head);if(album.open){const tools=el('div','album-tools'),shuffle=el('button','shuffle-album','⤨ Shuffle album');shuffle.onclick=()=>{const order=album.tracks.slice();for(let i=order.length-1;i>0;i--){const j=Math.floor(Math.random()*(i+1));[order[i],order[j]]=[order[j],order[i]]}playTrack(order[0],order.slice(1))};tools.append(shuffle);card.append(tools);const rows=el('div','album-tracks');for(let i=0;i<album.tracks.length;i++){const t=album.tracks[i],row=el('div','track-row'),b=el('button','track'+(currentTrack?.id===t.id?' active':''));b.append(el('span','',t.title),el('small','',t.artist+' · '+t.album));b.onclick=()=>playTrack(t,album.tracks.slice(i+1));const add=el('button','track-add','+');add.title='Add to queue';add.onclick=()=>{queue.push(t);renderQueue();msg('Added to queue: '+t.title)};row.append(b,add);rows.append(row)}card.append(rows)}box.append(card)}}
async function openArtist(name){selectedArtist=name;msg('Loading '+name+'…');try{const r=await fetch(base+'/api/artist/'+encodeURIComponent(name),{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);albums=(await r.json()).albums||[];albums.forEach(a=>a.open=false);tracks=albums.flatMap(a=>a.tracks);renderAlbums();msg(albums.length+' albums · '+tracks.length+' tracks')}catch(e){msg('Could not load artist: '+e.message)}}
function renderQueue(){const panel=$('queue-panel');panel.hidden=!queueVisible;const toggle=$('queue-toggle');toggle.textContent='Queue ('+queue.length+')';panel.replaceChildren();if(!queueVisible)return;panel.append(el('strong','','Up next'));if(!queue.length)panel.append(el('div','empty','Queue is empty'));queue.forEach((t,i)=>{const row=el('div','queue-item'),play=el('button',i===0?'now-playing':'',((i===0?'Next · ':'')+t.title+' — '+t.artist));play.onclick=()=>{const rest=queue.slice(i+1);playTrack(t,rest)};const rm=el('button','queue-remove','×');rm.setAttribute('aria-label','Remove '+t.title);rm.onclick=()=>{queue.splice(i,1);renderQueue()};row.append(play,rm);panel.append(row)})}
async function init(){try{msg('Loading artists…');const [r,info]=await Promise.all([fetch(base+'/api/artists',{cache:'no-store'}),fetch(base+'/api/info',{cache:'no-store'})]);if(!r.ok||!info.ok)throw Error('HTTP '+(!r.ok?r.status:info.status));artists=(await r.json()).artists||[];$('path').textContent='Library: '+(await info.json()).root;renderArtists();msg(artists.length+' artists')}catch(e){msg('Could not connect: '+e.message)}}
$('queue-toggle').onclick=()=>{queueVisible=!queueVisible;renderQueue()};$('play').onclick=()=>{if(!audio.src){if(queue.length)next();return}if(audio.paused){audio.play();$('play').textContent='Ⅱ'}else{audio.pause();$('play').textContent='▶'}};$('prev').onclick=previous;$('skip').onclick=next;audio.onended=next;audio.ontimeupdate=()=>{if(audio.duration)$('seek').value=Math.round(audio.currentTime/audio.duration*1000)};$('seek').oninput=()=>{if(audio.duration)audio.currentTime=audio.duration*$('seek').value/1000};renderQueue();init()
})();''' + PAGE[_script_end:]
PAGE = PAGE.replace(
    "audio.play().then(()=>{$('play').textContent='Ⅱ'})",
    "audio.play().then(()=>{$('play').textContent='Ⅱ';if('mediaSession' in navigator)navigator.mediaSession.playbackState='playing'})")
PAGE = PAGE.replace(
    "audio.pause();$('play').textContent='▶'",
    "audio.pause();$('play').textContent='▶';if('mediaSession' in navigator)navigator.mediaSession.playbackState='paused'")



def audio_files(root: Path):
    try:
        paths = sorted(root.iterdir(), key=lambda p: p.name.casefold())
    except OSError:
        return
    for path in paths:
        if path.is_dir():
            yield from audio_files(path)
        elif path.is_file() and path.suffix.lower() in AUDIO_EXTENSIONS:
            yield path


def cover_file(album_dir: Path) -> Path:
    for name in ("cover.jpg", "cover.jpeg", "cover.png", "cover.ppm"):
        candidate = album_dir / name
        if candidate.is_file():
            return candidate
    return album_dir / "cover.ppm"


def artists_in(root: Path):
    found = {}
    for path in audio_files(root):
        rel = path.relative_to(root)
        artist = rel.parts[0] if len(rel.parts) > 1 else "Unknown Artist"
        album_dir = root / artist / rel.parts[1] if len(rel.parts) > 2 else path.parent
        item = found.setdefault(artist, {"name": artist, "albums": set(), "cover": ""})
        item["albums"].add(album_dir.name)
        cover = album_dir / "cover.jpg"
        if not cover.is_file(): cover = album_dir / "cover.jpeg"
        if not cover.is_file(): cover = album_dir / "cover.png"
        if not cover.is_file(): cover = album_dir / "cover.ppm"
        if not item["cover"] and cover.is_file():
            item["cover"] = "/cover/" + urllib.parse.quote(cover.relative_to(root).as_posix(), safe="/")
    return [{"name": a["name"], "albums": len(a["albums"]), "cover": a["cover"]}
            for a in sorted(found.values(), key=lambda x: x["name"].casefold())]


def artist_albums(root: Path, artist: str):
    artist_root = (root / artist).resolve()
    try: artist_root.relative_to(root)
    except ValueError: return None
    if not artist_root.is_dir(): return None
    albums = {}
    for path in audio_files(artist_root):
        rel = path.relative_to(root)
        name = rel.parts[1] if len(rel.parts) > 2 else artist_root.name
        album_dir = path.parent
        item = albums.setdefault(name, {"name": name, "cover": "", "tracks": []})
        cover = album_dir / "cover.jpg"
        if not cover.is_file(): cover = album_dir / "cover.jpeg"
        if not cover.is_file(): cover = album_dir / "cover.png"
        if not cover.is_file(): cover = album_dir / "cover.ppm"
        if not item["cover"] and cover.is_file():
            item["cover"] = "/cover/" + urllib.parse.quote(cover.relative_to(root).as_posix(), safe="/")
        rel_path = path.relative_to(root).as_posix()
        item["tracks"].append({"id": rel_path, "title": path.stem, "artist": artist,
                               "album": name, "stream": "/stream/" + urllib.parse.quote(rel_path, safe="/")})
    return [albums[k] for k in sorted(albums, key=str.casefold)]


def ppm_bmp(path: Path) -> bytes:
    data = path.read_bytes()
    if not data.startswith(b"P6"): raise ValueError("expected binary PPM image")
    i, tokens = 2, []
    while len(tokens) < 3:
        while i < len(data) and data[i] in b" \t\r\n": i += 1
        if i < len(data) and data[i] == 35:
            while i < len(data) and data[i] not in b"\r\n": i += 1
            continue
        start = i
        while i < len(data) and data[i] not in b" \t\r\n": i += 1
        tokens.append(int(data[start:i]))
    width, height, maxval = tokens
    if not 0 < width <= 2048 or not 0 < height <= 2048 or maxval != 255:
        raise ValueError("unsupported PPM image")
    if i >= len(data) or data[i] not in b" \t\r\n": raise ValueError("missing PPM separator")
    i += 2 if data[i:i+2] == b"\r\n" else 1
    rgb = data[i:i + width * height * 3]
    if len(rgb) != width * height * 3: raise ValueError("truncated PPM")
    stride = (width * 3 + 3) & ~3
    pixels = bytearray(stride * height)
    for y in range(height):
        for x in range(width):
            r, g, b = rgb[(y * width + x) * 3:(y * width + x) * 3 + 3]
            at = (height - 1 - y) * stride + x * 3
            pixels[at:at+3] = bytes((b, g, r))
    return b"BM" + struct.pack("<IHHI", 54+len(pixels), 0, 0, 54) + struct.pack(
        "<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixels), 2835, 2835, 0, 0) + pixels


def artists_in(root: Path):
    found = {}
    for path in audio_files(root):
        rel = path.relative_to(root)
        artist = rel.parts[0] if len(rel.parts) > 1 else "Unknown Artist"
        album_dir = root / artist / rel.parts[1] if len(rel.parts) > 2 else path.parent
        item = found.setdefault(artist, {"name": artist, "albums": set(), "cover": ""})
        item["albums"].add(album_dir.name)
        cover = cover_file(album_dir)
        if not item["cover"] and cover.is_file():
            item["cover"] = "/cover/" + urllib.parse.quote(cover.relative_to(root).as_posix(), safe="/")
    return [{"name": a["name"], "albums": len(a["albums"]), "cover": a["cover"]} for a in sorted(found.values(), key=lambda x: x["name"].casefold())]


def artist_albums(root: Path, artist: str):
    artist_root = (root / artist).resolve()
    try:
        artist_root.relative_to(root)
    except ValueError:
        return None
    if not artist_root.is_dir():
        return None
    albums = {}
    for path in audio_files(artist_root):
        rel = path.relative_to(root)
        album_name = rel.parts[1] if len(rel.parts) > 2 else artist_root.name
        album_dir = path.parent
        item = albums.setdefault(album_name, {"name": album_name, "cover": "", "tracks": []})
        cover = cover_file(album_dir)
        if not item["cover"] and cover.is_file():
            item["cover"] = "/cover/" + urllib.parse.quote(cover.relative_to(root).as_posix(), safe="/")
        rel_path = path.relative_to(root).as_posix()
        item["tracks"].append({"id": rel_path, "title": path.stem, "artist": artist, "album": album_name,
                               "cover": item["cover"],
                               "stream": "/stream/" + urllib.parse.quote(rel_path, safe="/")})
    return [albums[k] for k in sorted(albums, key=str.casefold)]


def ppm_bmp(path: Path) -> bytes:
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise ValueError("expected binary PPM image")
    i, tokens = 2, []
    while len(tokens) < 3:
        while i < len(data) and data[i] in b" \t\r\n": i += 1
        if i < len(data) and data[i] == 35:
            while i < len(data) and data[i] not in b"\r\n": i += 1
            continue
        start = i
        while i < len(data) and data[i] not in b" \t\r\n": i += 1
        tokens.append(int(data[start:i]))
    width, height, maxval = tokens
    if not (0 < width <= 2048 and 0 < height <= 2048 and maxval == 255):
        raise ValueError("unsupported PPM dimensions or colour depth")
    if i >= len(data) or data[i] not in b" \t\r\n":
        raise ValueError("missing PPM header separator")
    i += 2 if data[i:i + 2] == b"\r\n" else 1
    rgb = data[i:i + width * height * 3]
    if len(rgb) != width * height * 3:
        raise ValueError("truncated PPM image")
    stride = (width * 3 + 3) & ~3
    pixels = bytearray(stride * height)
    for y in range(height):
        src = y * width * 3
        dst = (height - 1 - y) * stride
        for x in range(width):
            r, g, b = rgb[src + 3*x:src + 3*x + 3]
            pixels[dst + 3*x:dst + 3*x + 3] = bytes((b, g, r))
    header = b"BM" + struct.pack("<IHHI", 54 + len(pixels), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    return header + dib + pixels


def ppm_png(path: Path) -> bytes:
    """Convert the tiny P6 album covers to PNG for iOS media metadata."""
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise ValueError("expected binary PPM image")
    i, tokens = 2, []
    while len(tokens) < 3:
        while i < len(data) and data[i] in b" \t\r\n":
            i += 1
        if i < len(data) and data[i] == 35:
            while i < len(data) and data[i] not in b"\r\n":
                i += 1
            continue
        start = i
        while i < len(data) and data[i] not in b" \t\r\n":
            i += 1
        tokens.append(int(data[start:i]))
    width, height, maxval = tokens
    if not (0 < width <= 2048 and 0 < height <= 2048 and maxval == 255):
        raise ValueError("unsupported PPM dimensions or colour depth")
    if i >= len(data) or data[i] not in b" \t\r\n":
        raise ValueError("missing PPM header separator")
    i += 2 if data[i:i + 2] == b"\r\n" else 1
    rgb = data[i:i + width * height * 3]
    if len(rgb) != width * height * 3:
        raise ValueError("truncated PPM image")

    def chunk(kind, payload):
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)

    scanlines = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(scanlines, 6)) + chunk(b"IEND", b""))


class Handler(BaseHTTPRequestHandler):
    server_version = "LocalMusic/1.0"

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        if parsed.path == "/":
            return self.send_bytes(PAGE.encode(), "text/html; charset=utf-8", "no-cache")
        if parsed.path == "/manifest.json":
            body = json.dumps({"name": "Music", "short_name": "Music", "start_url": "/", "display": "standalone",
                               "background_color": "#282828", "theme_color": "#282828"}).encode()
            return self.send_bytes(body, "application/manifest+json", "no-cache")
        if parsed.path == "/api/info":
            return self.send_json({"root": str(self.server.root)})
        if parsed.path == "/api/artists":
            return self.send_json({"artists": artists_in(self.server.root)})
        if parsed.path.startswith("/api/artist/"):
            artist = urllib.parse.unquote(parsed.path[len("/api/artist/"):])
            albums = artist_albums(self.server.root, artist)
            return self.send_json({"albums": albums}) if albums is not None else self.send_error(404)
        if parsed.path.startswith("/cover/"):
            rel = urllib.parse.unquote(parsed.path[len("/cover/"):])
            path = (self.server.root / rel).resolve()
            try: path.relative_to(self.server.root)
            except ValueError: return self.send_error(403)
            if path.name not in {"cover.ppm", "cover.jpg", "cover.jpeg", "cover.png"} or not path.is_file(): return self.send_error(404)
            try:
                if path.suffix.lower() == ".ppm":
                    return self.send_bytes(ppm_png(path), "image/png", "public, max-age=3600")
                return self.send_bytes(path.read_bytes(), mimetypes.guess_type(path.name)[0] or "application/octet-stream",
                                       "public, max-age=3600")
            except (OSError, ValueError, struct.error): return self.send_error(415)
        if parsed.path.startswith("/stream/"):
            rel = urllib.parse.unquote(parsed.path[len("/stream/"):])
            path = (self.server.root / rel).resolve()
            try: path.relative_to(self.server.root)
            except ValueError: return self.send_error(403)
            if not path.is_file() or path.suffix.lower() not in AUDIO_EXTENSIONS: return self.send_error(404)
            return self.send_file(path)
        return self.send_error(404)

    def send_json(self, value):
        self.send_bytes(json.dumps(value, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8")

    def send_bytes(self, data, content_type, cache="no-store"):
        self.send_response(200); self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data))); self.send_header("Cache-Control", cache)
        self.end_headers(); self.wfile.write(data)

    def send_file(self, path: Path):
        size = path.stat().st_size; start, end, status = 0, size - 1, 200
        header = self.headers.get("Range")
        if header:
            match = re.fullmatch(r"bytes=(\d*)-(\d*)", header.strip())
            if not match or (not match.group(1) and not match.group(2)): return self.send_error(416)
            if match.group(1):
                start = int(match.group(1))
                if match.group(2): end = int(match.group(2))
            else: start = max(0, size - int(match.group(2)))
            if start >= size or end < start:
                self.send_response(416); self.send_header("Content-Range", f"bytes */{size}"); self.end_headers(); return
            end = min(end, size - 1); status = 206
        length = end - start + 1
        self.send_response(status); self.send_header("Content-Type", mimetypes.guess_type(path.name)[0] or "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes"); self.send_header("Content-Length", str(length))
        if status == 206: self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        with path.open("rb") as f:
            f.seek(start); remaining = length
            while remaining:
                block = f.read(min(CHUNK_SIZE, remaining))
                if not block: break
                self.wfile.write(block); remaining -= len(block)

    def log_message(self, fmt, *args): pass


def local_ip():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try: sock.connect(("192.0.2.1", 80)); return sock.getsockname()[0]
    except OSError: return "127.0.0.1"
    finally: sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd() / "music", help="music library root (default: ./music)")
    parser.add_argument("--host", default="0.0.0.0", help="bind address")
    parser.add_argument("--port", type=int, default=1411)
    args = parser.parse_args(); root = args.root.expanduser().resolve()
    if not root.is_dir(): parser.error(f"music folder not found: {root} (create ./music or pass --root PATH)")
    server = HTTPServer((args.host, args.port), Handler); server.root = root
    print(f"Serving {root}"); print(f"LAN URL:   http://{local_ip()}:{args.port}")
    print(f"Tailscale: http://<computer-tailscale-ip>:{args.port}"); print("Press Ctrl+C to stop.")
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally: server.server_close()


if __name__ == "__main__": main()
