# beam

A suckless, dependency-free C99 CLI tool for sharing short-lived file and video links directly from your machine.

Local-first, fast, and built specifically for sharing dubbed videos, recordings, or files without uploading them to third-party cloud hosts.

```
  BEAM - Ephemeral File & Video Sharing
  --------------------------------------------------
  File:     dubbed_video.mp4 (42.5 MB, video/mp4)
  Expires:  in 18000 seconds (5h 0m)
  Link:     http://192.168.1.50:8080/s/8f3a2c0b1e4d5a6f7b8c9d0e1f2a3b4c
            (copied to clipboard)

  Scan with camera:

  █████████████████████████████████
  ██████ █▀▀▀▀▀█ █ █ █ █▀▀▀▀▀█ ████
  ██████ █ ███ █ ▀▀▀█▀ █ ███ █ ████
  ██████ █▀▀▀▀▀█ █ █ █ █▀▀▀▀▀█ ████
  ...
```

## Features

- **Native Browser Playback**: Opening the link in Chrome, Safari, Firefox, or mobile immediately launches the browser's native video player (scrubbing, volume, fullscreen, and built-in download menu) with zero wrapper bloat.
- **Embedded HTML5 Web Player (`-w`)**: Optional clean dark-mode web player if a custom HTML frame is preferred.
- **HTTP 206 Partial Content (Range Requests)**: Built for video streaming. iOS Safari and Android Chrome can seek, scrub, and buffer video containers (`moov` atoms) seamlessly.
- **Self-Contained ANSI QR Code**: Generates a standard QR code directly in your terminal using Unicode half-blocks (`▀`, `▄`) so phones can scan and watch instantly.
- **Automatic Expiration**: Configurable TTL (`-t 5h`, `-t 30m`, `-t 1d`). Closes the server and self-terminates when time is up.
- **Public Tunnel Support (`-p`)**: Built-in zero-install public HTTPS reverse tunnel via SSH (`localhost.run`) for sharing over cellular or outside your local Wi-Fi.
- **One-Shot Mode (`-1`)**: Automatically terminates after the first full download.
- **Clipboard Integration (`-c`)**: Automatically copies the link to your system clipboard (`clipbridge`, `wl-copy`, `xclip`, `pbcopy`, or `clip.exe`).
- **Suckless Standards**: Pure C99, `arg.h`, `config.mk`, `Makefile`, and `beam.1` man page. Zero dependencies beyond standard C and POSIX/OS sockets.

## Usage

```sh
# Share a video (streams directly into Chrome/Safari native player, 5h TTL)
beam video.mp4

# Share over the internet via public HTTPS tunnel + copy link to clipboard
beam -p -c video.mp4

# Set a custom lifetime (e.g. 30 minutes, 2 hours, 1 day)
beam -t 30m video.mp4

# One-shot transfer (exits after download completes)
beam -1 document.pdf

# Quiet mode (no QR code) on a specific port
beam -q -P 9000 archive.zip

# Use HTML5 web player wrapper page
beam -w video.mp4
```

## Options

| Flag | Description |
|------|-------------|
| `-t <ttl>` | Set link expiration time (default `5h`; supports `s`, `m`, `h`, `d`). |
| `-p` | Spawn an ephemeral public HTTPS reverse tunnel via SSH (`localhost.run`). |
| `-c` | Copy the share URL to system clipboard. |
| `-q` | Quiet mode: suppress the terminal QR code. |
| `-1` | One-shot mode: exit after the first complete transfer. |
| `-w` | Enable custom HTML5 web player wrapper page (default: native browser player). |
| `-b <ip>` | Bind IP address (default `0.0.0.0`). |
| `-P <port>` | Listening port (default `8080` or next available). |
| `-v` | Show version and exit. |

## Building & Installation

Requirements: A C99 compiler (`gcc`, `clang`, or `tcc`) and `make`.

```sh
# Build
make

# Install (default prefix: /usr/local)
sudo make install

# Uninstall
sudo make uninstall
```

## License

MIT (see [LICENSE](LICENSE)).
