# beam

beam shares a local file over HTTP using a short-lived link.

When you share a video, Chrome, Safari, and other browsers open it directly in their native media player. Seeking works because beam responds to HTTP Range requests (206 Partial Content). When the timer expires, the server exits.

I built beam because I use autodub to dub videos for my girlfriend and wanted a direct way to let her watch them without uploading to third-party services.

```
  beam
  --------------------------------------------------
  File:     dubbed_video.mp4 (42.5 MB, video/mp4)
  Expires:  18000 seconds (5h 0m)
  Link:     http://192.168.1.50:8080/4bdaa908ed74
            (copied to clipboard)

  Scan with camera:

  █████████████████████████████████
  ██████ █▀▀▀▀▀█ █ █ █ █▀▀▀▀▀█ ████
  ██████ █ ███ █ ▀▀▀█▀ █ ███ █ ████
  ██████ █▀▀▀▀▀█ █ █ █ █▀▀▀▀▀█ ████
  ...
```

## How it works

- **Direct browser playback.** Beam serves files with `Content-Disposition: inline` and matching MIME types. Browsers open audio and video files in their native media controls.
- **Clean hash URLs.** Links use a 12-character hex token (for example, `http://192.168.1.50:8080/4bdaa908ed74`). Beam also accepts query format `/?v=<hash>`.
- **Range seeking.** Single-range byte requests let browsers scrub through MP4 and WebM videos without downloading the whole file first.
- **Terminal QR code.** Renders a QR code with Unicode half-blocks so anyone on the same network can scan it from a phone.
- **Auto expiration.** Sets a default five-hour lifetime, or custom durations using `s`, `m`, `h`, or `d`.
- **Public tunnel.** Passing `-p` opens an ephemeral HTTPS reverse tunnel via SSH, returning a public link without requiring accounts or logins.
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

You can also specify a Tailscale node or custom hostname:

```sh
autodub "https://www.youtube.com/watch?v=icsP8f8TRdQ" | beam -H myhost.ts.net -c
```

Or pipe an existing file path:

```sh
ls -t output/*.mp4 | head -1 | beam -p -c
```

## Usage

```sh
# Share a video locally (default 5h lifetime)
beam video.mp4

# Pipe from autodub and share over a public HTTPS tunnel
autodub "https://www.youtube.com/watch?v=icsP8f8TRdQ" | beam -p -c

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
| `-H <host>` | Host or IP for the share link (for example, `192.168.1.50` or `node.ts.net`). |
| `-c` | Copy the share URL to the system clipboard. |
| `-q` | Quiet mode. Suppress the terminal QR code. |
| `-1` | One-shot mode. Exit after the first complete download. |
| `-w` | Serve an HTML player wrapper instead of the raw media stream. |
| `-b <ip>` | Bind IP address. Default is `0.0.0.0`. |
| `-P <port>` | Port to listen on. Default is `8080` or the next available port. |
| `-v` | Print version and exit. |

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
