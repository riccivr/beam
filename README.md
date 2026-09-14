# beam

beam shares a local file over HTTP using a short-lived link.

When you share a video, Chrome, Safari, and other browsers open it directly in their native media player. Seeking works because beam responds to HTTP Range requests (206 Partial Content). When the timer expires, the server exits.

I built beam because I use autodub to dub videos for my girlfriend and wanted a direct way to let her watch them without uploading to third-party services.

```
  beam
  --------------------------------------------------
  File:     dubbed_video.mp4 (42.5 MB, video/mp4)
  Expires:  18000 seconds (5h 0m)
  Link:     https://5369e9d69bbb03.lhr.life/4bdaa908ed74
            (copied to clipboard)

  Scan with camera:

  █████████████████████████████████
  ██████ █▀▀▀▀▀█ █ █ █ █▀▀▀▀▀█ ████
  ██████ █ ███ █ ▀▀▀█▀ █ ███ █ ████
  ██████ █▀▀▀▀▀█ █ █ █ █▀▀▀▀▀█ ████
  ...

  Commands: [p] reopen tunnel  [c] copy link  [q] quit
```

## How it works

- **Direct browser playback.** Beam serves files with `Content-Disposition: inline` and matching MIME types. Browsers open audio and video files in their native media controls.
- **Clean hash URLs.** Links use a 32-character hex token from `/dev/urandom` (128 bits). Beam also accepts query format `/?v=<hash>`.
- **Public-tunnel playback.** `-p` SSH forwarding disables compression (video is already compressed), raises socket buffers, and assembles full HTTP requests so Range seeks survive tunnel latency.
- **Range seeking.** Single-range byte requests let browsers scrub through MP4 and WebM videos without downloading the whole file first.
- **Keep-alive + sendfile.** HTTP/1.1 persistent connections and `sendfile()` cut the stalls native players hit when they open many Range requests.
- **MP4 fast start.** If `ffmpeg` is available, beam remuxes MP4/MOV so the `moov` atom is at the front (`-movflags +faststart`). Use `-F` to skip.
- **Terminal QR code.** Renders a QR code with Unicode half-blocks so anyone on the same network can scan it from a phone.
- **Auto expiration.** Sets a default five-hour lifetime, or custom durations using `s`, `m`, `h`, or `d`.
- **Public tunnel.** Passing `-p` opens an ephemeral HTTPS reverse tunnel via SSH, returning a public link without requiring accounts or logins.
- **Reopen anytime.** Running `beam -p -c` (or `beam -r`) without arguments reopens the last beamed file and keeps the same token so the link stays active.
- **Live controls.** While beam is running, press `[p]` to reopen or start the tunnel, `[c]` to copy the link, and `[q]` to quit.
- **Auto reconnect.** If the SSH tunnel drops, beam automatically re-establishes the tunnel and prints the new link.
- **Host override.** Passing `-H host` sets the host, IP, or domain in the link. Useful for Tailscale nodes or custom hostnames.
- **Clipboard support.** Passing `-c` copies the link to the system clipboard via clipbridge, wl-copy, xclip, pbcopy, or clip.exe.
- **Piped input.** Accepts file paths or pipeline logs on stdin. When piped from autodub, beam prioritizes the generated dubbed video over subtitle files and logs.
- **One-shot mode.** Passing `-1` exits immediately after the first complete transfer.

## Piping from autodub

Pipe autodub into beam. Beam prints the pipeline progress as it runs, picks the dubbed video file when finished, starts the server, and prints the link:

```sh
# Share publicly over HTTPS tunnel
autodub "https://www.youtube.com/watch?v=icsP8f8TRdQ" | beam -p -c

# Share locally or across your LAN
autodub "https://www.youtube.com/watch?v=icsP8f8TRdQ" | beam -c
```

## Reopening the tunnel

If you close beam with `Ctrl+C` and want to open the tunnel again later, run:

```sh
# Reopen the last beamed file over the tunnel with the same link
beam -p -c

# Or explicitly resume
beam -r -p -c
```

While beam is running, you can also press `p` at any time to open or refresh the tunnel on demand.

## Usage

```sh
# Share a video locally (default 5h lifetime)
beam video.mp4

# Pipe from autodub and share over a public HTTPS tunnel
autodub "https://www.youtube.com/watch?v=icsP8f8TRdQ" | beam -p -c

# Reopen the last beamed file
beam -p -c
beam -r

# Use a specific token hash
beam -k 4bdaa908ed74 video.mp4

# Use a specific IP or Tailscale domain for the link
beam -H 192.168.1.50 -c video.mp4
beam -H mynode.ts.net -c video.mp4

# Set a custom lifetime
beam -t 30m video.mp4

# Exit after the first download completes
beam -1 document.pdf

# Run without the terminal QR code
beam -q -P 9000 archive.zip

# Optional HTML player wrapper
beam -w video.mp4
```

## Options

| Flag | Description |
|------|-------------|
| `-t <ttl>` | Link lifetime. Default is `5h`. Accepts `s`, `m`, `h`, `d`. |
| `-p` | Open an ephemeral public HTTPS reverse tunnel via SSH. |
| `-r` | Reopen and resume the last beamed file with its existing token. |
| `-k <token>` | Use an explicit 12-character token hash. |
| `-H <host>` | Host or IP for the share link (for example, `192.168.1.50` or `node.ts.net`). |
| `-c` | Copy the share URL to the system clipboard. |
| `-q` | Quiet mode. Suppress the terminal QR code. |
| `-1` | One-shot mode. Exit after the first complete download. |
| `-w` | Serve an HTML player wrapper instead of the raw media stream. |
| `-f` | Force an MP4 faststart remux via ffmpeg before serving. |
| `-F` | Do not remux; serve the original MP4 as-is. |
| `-b <ip>` | Bind IP address. Default is `0.0.0.0`. |
| `-P <port>` | Port to listen on. Default is `8080` or the next available port. |
| `-v` | Print version and exit. |

## Interactive commands

While `beam` is running in your terminal:

- `[p]` Open or reopen the public HTTPS tunnel.
- `[c]` Copy the share link to the clipboard.
- `[q]` Quit and close the link.

## Building and installation

Requires a C99 compiler and `make`.

```sh
make
sudo make install
```

To remove:

```sh
sudo make uninstall
```

## License

MIT (see [LICENSE](LICENSE)).
