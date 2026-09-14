# beam

`beam` shares a local file over HTTP using a temporary, random URL.

When you share a video, browsers like Chrome and Safari open their built-in player. Seeking works because the server handles HTTP Range requests (`206 Partial Content`). When the timer runs out, the server shuts down.

I built `beam` because I use `autodub` to dub videos for my girlfriend and wanted a quick way to let her watch them without uploading gigabytes to cloud services.

```
  beam
  --------------------------------------------------
  File:     dubbed_video.mp4 (42.5 MB, video/mp4)
  Expires:  18000 seconds (5h 0m)
  Link:     http://192.168.1.50:8080/s/8f3a2c0b1e4d5a6f7b8c9d0e1f2a3b4c/dubbed_video.mp4
            (copied to clipboard)

  Scan with camera:

  █████████████████████████████████
  ██████ █▀▀▀▀▀█ █ █ █ █▀▀▀▀▀█ ████
  ██████ █ ███ █ ▀▀▀█▀ █ ███ █ ████
  ██████ █▀▀▀▀▀█ █ █ █ █▀▀▀▀▀█ ████
  ...
```

## How it works

- **Direct browser playback.** Links include the filename and MIME type. Chrome, Safari, and Firefox open the video directly in their native media player.
- **Range seeking.** Supports single-range byte requests so mobile and desktop browsers can scrub through MP4 and WebM containers.
- **Terminal QR code.** Renders a QR code with Unicode half-blocks (`▀`, `▄`) so someone nearby can scan it with a phone camera.
- **Auto expiration.** Sets a default five-hour lifetime (`-t 5h`), or any duration using `s`, `m`, `h`, or `d`.
- **Public tunnel.** Passing `-p` opens an SSH reverse tunnel through `localhost.run`, which gives you an HTTPS URL reachable over cellular or outside your home network.
- **Clipboard support.** Passing `-c` pipes the link to `clipbridge`, `wl-copy`, `xclip`, `pbcopy`, or `clip.exe`.
- **Piped input.** Accepts file paths or pipeline logs on stdin and extracts the output file automatically.
- **One-shot mode.** Passing `-1` stops the server after the first complete transfer.

## Piping from autodub

You can pipe `autodub` directly into `beam`. `beam` prints the progress output as it runs, detects the completed `.mp4` path from the log, starts the server, and prints the URL:

```sh
./autodub.sh "https://www.youtube.com/watch?v=icsP8f8TRdQ" | beam -p -c
```

You can also pipe an existing output file:

```sh
ls -t output/*.mp4 | head -1 | beam -p -c
```

## Usage

```sh
# Share a video locally (5h TTL)
beam video.mp4

# Pipe from autodub and open a public HTTPS tunnel
./autodub.sh "https://www.youtube.com/watch?v=icsP8f8TRdQ" | beam -p -c

# Share with a public HTTPS tunnel and copy link to clipboard
beam -p -c video.mp4

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
| `-p` | Open a public HTTPS tunnel through SSH (`localhost.run`). |
| `-c` | Copy the share URL to the system clipboard. |
| `-q` | Quiet mode. Suppress the terminal QR code. |
| `-1` | One-shot mode. Exit after the first complete download. |
| `-w` | Serve an HTML player wrapper instead of raw media stream. |
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
