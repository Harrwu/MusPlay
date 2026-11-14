# Private music web app

Start the server on the computer that holds the music:

```sh
./ios/server/run.sh
```

It looks for a folder named `music` in the current working directory by
default. The included launcher starts from the project root, so it serves
`<project>/music`; use `--root /path/to/music` to choose another library. The
web page automatically connects to the server that served it and shows the
resolved library path under its title; no address entry is needed. It binds
port **1411** and prints the LAN address. Open that address in Safari
on the iPhone, then use Safari's **Share → Add to Home Screen** to keep it like
an app. The library is streamed from disk; the server keeps no audio in RAM and
uses one request worker with 32 KiB transfer buffers. It emits the catalogue
incrementally and does not cache it or the audio in RAM. After updating the
server code, stop the old server with Ctrl+C and run the launcher again; an
already-running server keeps serving its old page.

For remote access, install Tailscale on both devices and open
`http://<computer-tailscale-ip>:1411`. Router port forwarding is not needed.
Keep the server running and bound to its default all-interface address.

The web app first shows artists, then loads the selected artist's albums and
tracks. It displays each album's pixel-art `cover.ppm` artwork and includes
play/pause, next/previous, and seeking. It also publishes track metadata and
playback actions through the browser Media Session API, enabling Lock Screen
controls on Safari versions that support them. This does not add Dynamic Island
Live Activities. It serves the original FLAC/MP3 files;
Safari/iOS support varies by format. Unsupported formats need transcoding,
which uses more CPU and RAM.
