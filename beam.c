#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#endif

#include "beam.h"
#include "arg.h"
#include "qr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef int socklen_t;
  #define close_socket(s) closesocket(s)
  #define poll WSAPoll
  #define strncasecmp _strnicmp
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <poll.h>
  #include <sys/wait.h>
  #include <termios.h>
  #include <netinet/tcp.h>
#ifdef __linux__
  #include <sys/sendfile.h>
#endif
  #define close_socket(s) close(s)
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
#endif

#define KEEP_ALIVE_MS 15000
#define KEEP_ALIVE_ONESHOT_MS 2000
#define WORKER_EXIT_FULL 10
#define WORKER_EXIT_RANGE 11

static long monotonic_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000L + (long)tv.tv_usec / 1000L;
}

#ifndef VERSION
#define VERSION "1.0.0"
#endif

char *argv0;
static volatile sig_atomic_t running = 1;
static pid_t tunnel_pid = -1;
static int tunnel_log_fd = -1;
static char global_temp_file[1024] = "";
static char global_faststart_file[1024] = "";

#ifndef _WIN32
static struct termios orig_termios;
static bool termios_set = false;

static void restore_terminal(void) {
    if (termios_set) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        termios_set = false;
    }
}

static void enable_raw_terminal(int fd) {
    if (fd >= 0 && isatty(fd)) {
        if (tcgetattr(fd, &orig_termios) == 0) {
            struct termios raw = orig_termios;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(fd, TCSANOW, &raw) == 0) {
                termios_set = true;
                atexit(restore_terminal);
            }
        }
    }
}

static void sig_child(int sig) {
    (void)sig;
}
#endif

static void cleanup_temp_file(void) {
    if (global_temp_file[0]) {
        unlink(global_temp_file);
        global_temp_file[0] = '\0';
    }
    if (global_faststart_file[0]) {
        unlink(global_faststart_file);
        global_faststart_file[0] = '\0';
    }
}

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

static bool send_all(SOCKET sock, const void *data, size_t len) {
    const char *buf = (const char *)data;
    size_t off = 0;

    while (off < len) {
        int n = send(sock, buf + off, len - off, 0);
        if (n < 0) {
#ifndef _WIN32
            if (errno == EINTR)
                continue;
#endif
            return false;
        }
        if (n == 0)
            return false;
        off += (size_t)n;
    }
    return true;
}

static void set_tcp_nodelay(SOCKET sock) {
#ifdef TCP_NODELAY
    int on = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const void *)&on, sizeof(on));
#else
    (void)sock;
#endif
}

static void set_media_sockopts(SOCKET sock) {
    int buf = 512 * 1024;

    set_tcp_nodelay(sock);
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const void *)&buf, sizeof(buf));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const void *)&buf, sizeof(buf));
#ifdef TCP_QUICKACK
    {
        int on = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_QUICKACK, (const void *)&on, sizeof(on));
    }
#endif
}

static bool send_file_range(SOCKET sock, FILE *fp, int64_t offset, int64_t length) {
    if (length <= 0)
        return true;

#if defined(__linux__) && !defined(_WIN32)
    {
        int fd = fileno(fp);
        off_t off = (off_t)offset;
        while (length > 0 && running) {
            ssize_t n = sendfile(sock, fd, &off, (size_t)length);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break; /* fall back to read/send below from current offset */
            }
            if (n == 0)
                break;
            length -= (int64_t)n;
        }
        if (length == 0)
            return true;
        offset = (int64_t)off;
    }
#endif

#if defined(_WIN32)
    _fseeki64(fp, offset, SEEK_SET);
#else
    if (fseeko(fp, (off_t)offset, SEEK_SET) != 0)
        return false;
#endif
    while (length > 0 && running) {
        char chunk[65536];
        size_t to_read = (length > (int64_t)sizeof(chunk)) ? sizeof(chunk) : (size_t)length;
        size_t bytes_read = fread(chunk, 1, to_read, fp);
        if (bytes_read == 0)
            return false;
        if (!send_all(sock, chunk, bytes_read))
            return false;
        length -= (int64_t)bytes_read;
    }
    return length == 0;
}

static const char *ci_strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            const char *h = haystack;
            const char *n = needle;
            while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
                h++;
                n++;
            }
            if (!*n) return haystack;
        }
    }
    return NULL;
}

static void get_cache_dir(char *buf, size_t maxlen) {
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        snprintf(buf, maxlen, "%s/beam", xdg);
    } else if (home && *home) {
        snprintf(buf, maxlen, "%s/.cache/beam", home);
    } else {
        snprintf(buf, maxlen, "/tmp/beam_cache");
    }
}

static void ensure_dir(const char *path) {
    char temp[1024];
    snprintf(temp, sizeof(temp), "%s", path);
    for (char *p = temp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
#ifdef _WIN32
            CreateDirectoryA(temp, NULL);
#else
            mkdir(temp, 0700);
#endif
            *p = '/';
        }
    }
#ifdef _WIN32
    CreateDirectoryA(temp, NULL);
#else
    mkdir(temp, 0700);
#endif
}

static void save_last_state(const char *filepath, const char *token, const char *url) {
    char dir[512];
    get_cache_dir(dir, sizeof(dir));
    ensure_dir(dir);

    char path_file[1024];
    snprintf(path_file, sizeof(path_file), "%s/last_file", dir);
    FILE *fp = fopen(path_file, "w");
    if (fp) {
        fputs(filepath, fp);
        fclose(fp);
    }

    if (token && *token) {
        char tok_file[1024];
        snprintf(tok_file, sizeof(tok_file), "%s/last_token", dir);
        fp = fopen(tok_file, "w");
        if (fp) {
            fputs(token, fp);
            fclose(fp);
        }
    }

    if (url && *url) {
        char url_file[1024];
        snprintf(url_file, sizeof(url_file), "%s/last_url", dir);
        fp = fopen(url_file, "w");
        if (fp) {
            fputs(url, fp);
            fclose(fp);
        }
    }
}

static bool load_last_state(char *filepath, size_t file_len, char *token, size_t tok_len) {
    char dir[512];
    get_cache_dir(dir, sizeof(dir));

    char path_file[1024];
    snprintf(path_file, sizeof(path_file), "%s/last_file", dir);
    FILE *fp = fopen(path_file, "r");
    if (!fp) return false;

    if (!fgets(filepath, (int)file_len, fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    /* Strip trailing newline */
    char *nl = strpbrk(filepath, "\r\n");
    if (nl) *nl = '\0';

    /* Verify file still exists on disk */
    struct stat st;
    if (stat(filepath, &st) != 0 || S_ISDIR(st.st_mode)) {
        return false;
    }

    if (token && tok_len > 0) {
        char tok_file[1024];
        snprintf(tok_file, sizeof(tok_file), "%s/last_token", dir);
        fp = fopen(tok_file, "r");
        if (fp) {
            if (fgets(token, (int)tok_len, fp)) {
                nl = strpbrk(token, "\r\n");
                if (nl) *nl = '\0';
            }
            fclose(fp);
        }
    }

    return true;
}

const char *beam_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    char lower[32];
    size_t i = 0;
    while (ext[i] && i < sizeof(lower) - 1) {
        lower[i] = (char)tolower((unsigned char)ext[i]);
        i++;
    }
    lower[i] = '\0';

    /* Video */
    if (strcmp(lower, ".mp4") == 0) return "video/mp4";
    if (strcmp(lower, ".webm") == 0) return "video/webm";
    if (strcmp(lower, ".mkv") == 0) return "video/x-matroska";
    if (strcmp(lower, ".mov") == 0) return "video/quicktime";
    if (strcmp(lower, ".avi") == 0) return "video/x-msvideo";
    if (strcmp(lower, ".ts") == 0) return "video/mp2t";

    /* Audio */
    if (strcmp(lower, ".mp3") == 0) return "audio/mpeg";
    if (strcmp(lower, ".wav") == 0) return "audio/wav";
    if (strcmp(lower, ".ogg") == 0) return "audio/ogg";
    if (strcmp(lower, ".m4a") == 0) return "audio/mp4";
    if (strcmp(lower, ".flac") == 0) return "audio/flac";
    if (strcmp(lower, ".aac") == 0) return "audio/aac";

    /* Subtitles */
    if (strcmp(lower, ".srt") == 0) return "text/plain; charset=utf-8";
    if (strcmp(lower, ".vtt") == 0) return "text/vtt; charset=utf-8";

    /* Images */
    if (strcmp(lower, ".jpg") == 0 || strcmp(lower, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(lower, ".png") == 0) return "image/png";
    if (strcmp(lower, ".gif") == 0) return "image/gif";
    if (strcmp(lower, ".webp") == 0) return "image/webp";
    if (strcmp(lower, ".svg") == 0) return "image/svg+xml";

    /* Documents & Archives */
    if (strcmp(lower, ".pdf") == 0) return "application/pdf";
    if (strcmp(lower, ".zip") == 0) return "application/zip";
    if (strcmp(lower, ".tar") == 0) return "application/x-tar";
    if (strcmp(lower, ".gz") == 0 || strcmp(lower, ".tgz") == 0) return "application/gzip";
    if (strcmp(lower, ".txt") == 0) return "text/plain; charset=utf-8";
    if (strcmp(lower, ".json") == 0) return "application/json";

    return "application/octet-stream";
}

bool beam_is_media(const char *mimetype) {
    return (strncmp(mimetype, "video/", 6) == 0 || strncmp(mimetype, "audio/", 6) == 0);
}

void beam_format_size(int64_t bytes, char *buf, size_t maxlen) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    double size = (double)bytes;
    while (size >= 1024.0 && u < 4) {
        size /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(buf, maxlen, "%" PRId64 " B", bytes);
    } else {
        snprintf(buf, maxlen, "%.1f %s", size, units[u]);
    }
}

int beam_parse_duration(const char *str) {
    if (!str || !*str) return DEFAULT_TTL;

    char *end = NULL;
    long val = strtol(str, &end, 10);
    if (val <= 0) return DEFAULT_TTL;

    if (!end || *end == '\0' || *end == 's' || *end == 'S') {
        return (int)val;
    }
    if (*end == 'm' || *end == 'M') {
        return (int)(val * 60);
    }
    if (*end == 'h' || *end == 'H') {
        return (int)(val * 3600);
    }
    if (*end == 'd' || *end == 'D') {
        return (int)(val * 86400);
    }

    return (int)val;
}

int beam_generate_token(char *token, size_t len) {
    static const char hex[] = "0123456789abcdef";
    uint8_t rand_bytes[TOKEN_HEX_LEN / 2];
    bool urandom_ok = false;

    if (!token || len < TOKEN_HEX_LEN + 1)
        return -1;

#ifndef _WIN32
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(rand_bytes, 1, sizeof(rand_bytes), f) == sizeof(rand_bytes))
            urandom_ok = true;
        fclose(f);
    }
#else
    {
        size_t i;
        urandom_ok = true;
        for (i = 0; i < sizeof(rand_bytes); i++) {
            unsigned int r = 0;
            if (rand_s(&r) != 0) {
                urandom_ok = false;
                break;
            }
            rand_bytes[i] = (uint8_t)(r & 0xFF);
        }
    }
#endif

    if (!urandom_ok)
        return -1;

    size_t out_idx = 0;
    for (size_t i = 0; i < sizeof(rand_bytes) && (out_idx + 2) < len; i++) {
        token[out_idx++] = hex[(rand_bytes[i] >> 4) & 0x0F];
        token[out_idx++] = hex[rand_bytes[i] & 0x0F];
    }
    token[out_idx] = '\0';
    return (out_idx == TOKEN_HEX_LEN) ? 0 : -1;
}

int beam_detect_local_ip(char *buf, size_t maxlen) {
#ifndef _WIN32
    struct ifaddrs *ifaddr, *ifa;
    char candidate[64] = "";
    bool found_candidate = false;

    if (getifaddrs(&ifaddr) == -1) {
        snprintf(buf, maxlen, "127.0.0.1");
        return 0;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip))) {
            /* Prefer RFC1918 / CGNAT (Tailscale 100.64/10) */
            unsigned a = 0, b = 0, c = 0, d = 0;
            if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
                (a == 10 ||
                 (a == 192 && b == 168) ||
                 (a == 172 && b >= 16 && b <= 31) ||
                 (a == 100 && b >= 64 && b <= 127))) {
                snprintf(buf, maxlen, "%s", ip);
                freeifaddrs(ifaddr);
                return 0;
            }
            if (!found_candidate) {
                snprintf(candidate, sizeof(candidate), "%s", ip);
                found_candidate = true;
            }
        }
    }
    freeifaddrs(ifaddr);

    if (found_candidate) {
        snprintf(buf, maxlen, "%s", candidate);
        return 0;
    }
#endif

    snprintf(buf, maxlen, "127.0.0.1");
    return 0;
}

static bool copy_to_clipboard(const char *text) {
    FILE *p = NULL;
    const char *cmds[] = {
        "clipbridge 2>/dev/null",
        "wl-copy 2>/dev/null",
        "xclip -selection clipboard 2>/dev/null",
        "pbcopy 2>/dev/null",
        "clip.exe 2>/dev/null",
        NULL
    };

    for (int i = 0; cmds[i] != NULL; i++) {
        p = popen(cmds[i], "w");
        if (p) {
            fputs(text, p);
            if (pclose(p) == 0) {
                return true;
            }
        }
    }
    return false;
}

static void cleanup_tunnel(void) {
#ifndef _WIN32
    if (tunnel_log_fd >= 0) {
        close(tunnel_log_fd);
        tunnel_log_fd = -1;
    }
    if (tunnel_pid > 0) {
        kill(-tunnel_pid, SIGTERM);
        kill(-tunnel_pid, SIGKILL);
        kill(tunnel_pid, SIGTERM);
        kill(tunnel_pid, SIGKILL);
        waitpid(tunnel_pid, NULL, 0);
        tunnel_pid = -1;
    }
#endif
}

static void drain_tunnel_logs(void) {
#ifndef _WIN32
    char junk[4096];
    if (tunnel_log_fd < 0)
        return;
    for (;;) {
        ssize_t n = read(tunnel_log_fd, junk, sizeof(junk));
        if (n > 0)
            continue;
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
#endif
}

#ifndef _WIN32
static bool extract_cf_url(char *line, char *public_url_out, size_t maxlen) {
    char *found = strstr(line, "https://");
    while (found) {
        char *end = found;
        while (*end && !isspace((unsigned char)*end) && *end != '"' && *end != '\'' && *end != ',')
            end++;
        if ((size_t)(end - found) < 12) {
            found = strstr(found + 8, "https://");
            continue;
        }
        {
            char saved = *end;
            *end = '\0';
            if (strstr(found, ".trycloudflare.com")) {
                size_t n = strlen(found);
                while (n > 0 && found[n - 1] == '/')
                    found[--n] = '\0';
                snprintf(public_url_out, maxlen, "%s", found);
                *end = saved;
                return true;
            }
            *end = saved;
        }
        found = strstr(found + 8, "https://");
    }
    return false;
}

static bool start_http_tunnel(int local_port, char *public_url_out, size_t maxlen) {
    int pipefd[2];
    char origin[64];
    const char *bin;

    if (access("/usr/bin/cloudflared", X_OK) != 0 &&
        access("/usr/local/bin/cloudflared", X_OK) != 0 &&
        access("/opt/homebrew/bin/cloudflared", X_OK) != 0) {
        /* still try PATH via execlp */
    }

    if (pipe(pipefd) == -1) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        setpgid(0, 0);

        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }

        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        snprintf(origin, sizeof(origin), "http://127.0.0.1:%d", local_port);
        bin = getenv("BEAM_TUNNEL_BIN");
        if (!bin || !bin[0])
            bin = "cloudflared";
        execlp(bin, bin, "tunnel", "--url", origin, "--no-autoupdate", (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    tunnel_pid = pid;

    char line[2048];
    size_t line_len = 0;
    time_t t_start = time(NULL);

    while (time(NULL) - t_start < 25) {
        struct pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pret = poll(&pfd, 1, 500);
        if (pret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pret == 0)
            continue;

        char ch;
        ssize_t n = read(pipefd[0], &ch, 1);
        if (n <= 0) break;

        if (ch == '\n' || ch == '\r') {
            line[line_len] = '\0';
            if (extract_cf_url(line, public_url_out, maxlen)) {
                /* Keep the read end open and drain it later. Closing it
                 * sends SIGPIPE the next time cloudflared logs, which
                 * kills the tunnel and invalidates the URL. */
                int flags = fcntl(pipefd[0], F_GETFL, 0);
                if (flags >= 0)
                    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
                tunnel_log_fd = pipefd[0];
                /* Edge hostname is printed before the route is always ready. */
                sleep(1);
                return true;
            }
            line_len = 0;
        } else if (line_len + 1 < sizeof(line)) {
            line[line_len++] = ch;
        }
    }

    close(pipefd[0]);
    cleanup_tunnel();
    return false;
}
#endif

static bool open_tunnel(BeamConfig *cfg, char *share_url, size_t maxlen) {
#ifndef _WIN32
    cleanup_tunnel();
    fprintf(stderr, "beam: establishing public tunnel...\n");
    if (start_http_tunnel(cfg->port, cfg->public_url, sizeof(cfg->public_url))) {
        snprintf(share_url, maxlen, "%s/%s", cfg->public_url, cfg->token);
        cfg->public_tunnel = true;
        return true;
    } else {
        fprintf(stderr, "beam: warning: public tunnel failed (install cloudflared)\n");
    }
#else
    (void)cfg; (void)share_url; (void)maxlen;
#endif
    return false;
}

static void html_escape(const char *in, char *out, size_t outlen) {
    size_t o = 0;
    if (!outlen)
        return;
    for (; in && *in && o + 1 < outlen; in++) {
        const char *rep = NULL;
        switch (*in) {
        case '&':  rep = "&amp;"; break;
        case '<':  rep = "&lt;"; break;
        case '>':  rep = "&gt;"; break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default:
            if ((unsigned char)*in < 0x20)
                continue;
            out[o++] = *in;
            continue;
        }
        size_t rl = strlen(rep);
        if (o + rl >= outlen)
            break;
        memcpy(out + o, rep, rl);
        o += rl;
    }
    out[o] = '\0';
}

/* Strip quotes/control bytes so Content-Disposition stays one header line. */
static void header_filename(const char *in, char *out, size_t outlen) {
    size_t o = 0;
    if (!outlen)
        return;
    for (; in && *in && o + 1 < outlen; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\' || c == '\r' || c == '\n')
            continue;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    if (!out[0] && outlen > 4)
        snprintf(out, outlen, "file");
}

static void send_http_response(SOCKET sock, int status, const char *status_text,
                               const char *extra_headers, const char *body, size_t body_len,
                               bool keep_alive) {
    char header[1024];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Length: %zu\r\n"
                        "%s"
                        "Connection: %s\r\n"
                        "%s\r\n",
                        status, status_text, body_len,
                        extra_headers ? extra_headers : "",
                        keep_alive ? "keep-alive" : "close",
                        keep_alive ? "Keep-Alive: timeout=15\r\n" : "");
    if (hlen < 0 || (size_t)hlen >= sizeof(header))
        return;
    if (!send_all(sock, header, (size_t)hlen))
        return;
    if (body && body_len > 0)
        send_all(sock, body, body_len);
}

static void render_html_player(char *html, size_t maxlen, const BeamConfig *cfg) {
    char size_str[32];
    char title_esc[2048];
    char name_esc[2048];
    char dl_esc[2048];

    beam_format_size(cfg->filesize, size_str, sizeof(size_str));
    html_escape(cfg->filename, title_esc, sizeof(title_esc));
    html_escape(cfg->filename, name_esc, sizeof(name_esc));
    html_escape(cfg->filename, dl_esc, sizeof(dl_esc));

    snprintf(html, maxlen,
             "<!DOCTYPE html>\n"
             "<html lang=\"en\">\n"
             "<head>\n"
             "  <meta charset=\"utf-8\">\n"
             "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
             "  <title>%s - beam</title>\n"
             "  <style>\n"
             "    * { box-sizing: border-box; }\n"
             "    body { margin:0; background:#0d1117; color:#c9d1d9; font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,sans-serif; display:flex; flex-direction:column; align-items:center; justify-content:center; min-height:100vh; padding:16px; }\n"
             "    .container { width:100%%; max-width:860px; display:flex; flex-direction:column; align-items:center; }\n"
             "    .video-card { width:100%%; background:#161b22; border:1px solid #30363d; border-radius:12px; overflow:hidden; box-shadow:0 12px 32px rgba(0,0,0,0.6); }\n"
             "    video, audio { width:100%%; display:block; outline:none; background:#000; max-height:80vh; }\n"
             "    .meta-bar { padding:16px 20px; display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:12px; }\n"
             "    .title { font-weight:600; font-size:16px; color:#f0f6fc; word-break:break-word; }\n"
             "    .badge { background:#21262d; border:1px solid #30363d; padding:3px 8px; border-radius:6px; font-size:12px; color:#8b949e; margin-left:8px; }\n"
             "    .btn { display:inline-flex; align-items:center; background:#238636; color:#fff; text-decoration:none; padding:8px 18px; border-radius:6px; font-weight:600; font-size:13px; transition:background 0.15s ease; }\n"
             "    .btn:hover { background:#2ea043; }\n"
             "  </style>\n"
             "</head>\n"
             "<body>\n"
             "  <div class=\"container\">\n"
             "    <div class=\"video-card\">\n"
             "      <video controls playsinline preload=\"metadata\" src=\"/%s/raw\">\n"
             "        Your browser does not support HTML5 video streaming.\n"
             "      </video>\n"
             "      <div class=\"meta-bar\">\n"
             "        <div>\n"
             "          <span class=\"title\">%s</span>\n"
             "          <span class=\"badge\">%s</span>\n"
             "        </div>\n"
             "        <a class=\"btn\" href=\"/%s/raw\" download=\"%s\">Download</a>\n"
             "      </div>\n"
             "    </div>\n"
             "  </div>\n"
             "</body>\n"
             "</html>\n",
             title_esc,
             cfg->token,
             name_esc, size_str,
             cfg->token, dl_esc);
}

/*
 * Extract and validate token from requested URI.
 * Accepts:
 *   /<token>
 *   /<token>/...
 *   /?v=<token>
 *   /?token=<token>
 *   /?<token>
 *   /s/<token>
 *   /s/<token>/...
 */
static bool extract_token(const char *path, const char *expected_token, const char **subpath_out) {
    if (!path || path[0] != '/') return false;

    const char *p = path;
    if (strncmp(p, "/s/", 3) == 0) {
        p += 3;
    } else if (strncmp(p, "/?v=", 4) == 0) {
        p += 4;
    } else if (strncmp(p, "/?token=", 8) == 0) {
        p += 8;
    } else if (strncmp(p, "/?", 2) == 0) {
        p += 2;
    } else {
        p += 1;
    }

    size_t elen = strlen(expected_token);
    if (strncmp(p, expected_token, elen) != 0) {
        return false;
    }

    char next = p[elen];
    if (next != '\0' && next != '/' && next != '?' && next != '#') {
        return false;
    }

    if (subpath_out) {
        *subpath_out = p + elen;
    }
    return true;
}

/* Serve one HTTP request.
 * Returns 1 if a complete non-range GET finished, 2 if a Range body was sent.
 * *keep_alive is updated from proto / Connection header. Does not close sock.
 */
static int process_request(SOCKET client_sock, const BeamConfig *cfg,
                           const char *req_buf, bool *keep_alive) {
    char method[16] = "";
    char path[1024] = "";
    char proto[16] = "";
    if (sscanf(req_buf, "%15s %1023s %15s", method, path, proto) < 2) {
        send_http_response(client_sock, 400, "Bad Request", NULL, "Bad Request\n", 12, false);
        *keep_alive = false;
        return 0;
    }

    if (strncasecmp(proto, "HTTP/1.0", 8) == 0)
        *keep_alive = false;
    else
        *keep_alive = true;

    {
        const char *line = req_buf;
        while (line && *line) {
            const char *eol = strstr(line, "\r\n");
            size_t linelen = eol ? (size_t)(eol - line) : strlen(line);
            if (linelen == 0)
                break;
            if (linelen >= 11 && strncasecmp(line, "Connection:", 11) == 0) {
                const char *v = line + 11;
                while (*v == ' ' || *v == '\t')
                    v++;
                if (strncasecmp(v, "close", 5) == 0)
                    *keep_alive = false;
                else if (strncasecmp(v, "keep-alive", 10) == 0)
                    *keep_alive = true;
            }
            line = eol ? eol + 2 : NULL;
        }
    }

    bool is_get = (strcmp(method, "GET") == 0);
    bool is_head = (strcmp(method, "HEAD") == 0);

    if (!is_get && !is_head) {
        send_http_response(client_sock, 405, "Method Not Allowed", "Allow: GET, HEAD\r\n", "Method Not Allowed\n", 19, *keep_alive);
        return 0;
    }

    const char *subpath = NULL;
    if (!extract_token(path, cfg->token, &subpath)) {
        send_http_response(client_sock, 404, "Not Found", NULL, "404 Not Found\n", 14, *keep_alive);
        return 0;
    }

    bool want_player = false;
    if (cfg->web_player && beam_is_media(cfg->mimetype)) {
        if (!subpath || (strcmp(subpath, "/raw") != 0 && strcmp(subpath, "/raw/") != 0)) {
            want_player = true;
        }
    } else if (subpath && (strcmp(subpath, "/player") == 0 || strcmp(subpath, "/player/") == 0)) {
        want_player = true;
    }

    if (want_player) {
        char html[8192];
        render_html_player(html, sizeof(html), cfg);
        size_t hlen = strlen(html);
        send_http_response(client_sock, 200, "OK", "Content-Type: text/html; charset=utf-8\r\n", is_get ? html : NULL, hlen, *keep_alive);
        return 0;
    }

    FILE *fp = fopen(cfg->filepath, "rb");
    if (!fp) {
        send_http_response(client_sock, 500, "Internal Server Error", NULL, "Failed to read file\n", 20, *keep_alive);
        return 0;
    }
#if defined(POSIX_FADV_SEQUENTIAL) && !defined(_WIN32)
    posix_fadvise(fileno(fp), 0, 0, POSIX_FADV_SEQUENTIAL);
    posix_fadvise(fileno(fp), 0, 0, POSIX_FADV_WILLNEED);
#endif

    int64_t range_start = 0;
    int64_t range_end = cfg->filesize > 0 ? cfg->filesize - 1 : 0;
    bool is_range = false;

    char *range_hdr = NULL;
    {
        const char *line = req_buf;
        while (line && *line) {
            const char *eol = strstr(line, "\r\n");
            size_t linelen = eol ? (size_t)(eol - line) : strlen(line);
            if (linelen == 0)
                break;
            if (linelen >= 13 && strncasecmp(line, "Range:", 6) == 0) {
                const char *v = line + 6;
                while (*v == ' ' || *v == '\t')
                    v++;
                if (strncasecmp(v, "bytes=", 6) == 0)
                    range_hdr = (char *)(v + 6);
                break;
            }
            line = eol ? eol + 2 : NULL;
        }
    }

    bool range_invalid = false;

    if (range_hdr) {
        if (*range_hdr == '-') {
            long long suffix = atoll(range_hdr + 1);
            if (suffix <= 0) {
                range_invalid = true;
            } else {
                range_start = cfg->filesize - suffix;
                if (range_start < 0) range_start = 0;
                range_end = cfg->filesize - 1;
                is_range = true;
            }
        } else {
            char *dash = strchr(range_hdr, '-');
            if (dash) {
                range_start = atoll(range_hdr);
                if (range_start >= cfg->filesize || range_start < 0) {
                    range_invalid = true;
                } else {
                    if (*(dash + 1) != '\0' && *(dash + 1) != '\r' && *(dash + 1) != '\n' && *(dash + 1) != ',') {
                        range_end = atoll(dash + 1);
                    }
                    if (range_end < range_start) {
                        range_invalid = true;
                    } else {
                        if (range_end >= cfg->filesize) {
                            range_end = cfg->filesize - 1;
                        }
                        is_range = true;
                    }
                }
            } else {
                range_invalid = true;
            }
        }
    }

    if (cfg->filesize <= 0 && (is_range || range_hdr))
        range_invalid = true;

    if (range_invalid) {
        char err_hdr[256];
        snprintf(err_hdr, sizeof(err_hdr), "Content-Range: bytes */%" PRId64 "\r\n", cfg->filesize);
        send_http_response(client_sock, 416, "Range Not Satisfiable", err_hdr, "Range Not Satisfiable\n", 22, *keep_alive);
        fclose(fp);
        return 0;
    }

    int64_t content_length = is_range ? (range_end - range_start + 1) : cfg->filesize;
    int status_code = is_range ? 206 : 200;
    const char *status_str = is_range ? "Partial Content" : "OK";

    char headers[1024];
    char disp_name[256];
    int hlen = 0;
    header_filename(cfg->filename, disp_name, sizeof(disp_name));

    hlen = snprintf(headers, sizeof(headers),
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %" PRId64 "\r\n"
                    "%s"
                    "Content-Disposition: inline; filename=\"%s\"\r\n"
                    "Accept-Ranges: bytes\r\n"
                    "Cache-Control: public, max-age=60\r\n"
                    "Connection: %s\r\n"
                    "%s"
                    "\r\n",
                    status_code, status_str,
                    cfg->mimetype,
                    content_length,
                    is_range ? "" : "",
                    disp_name,
                    *keep_alive ? "keep-alive" : "close",
                    *keep_alive ? "Keep-Alive: timeout=15\r\n" : "");

    /* rebuild with Content-Range when needed — snprintf above omitted range on purpose */
    if (is_range) {
        hlen = snprintf(headers, sizeof(headers),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %" PRId64 "\r\n"
                        "Content-Range: bytes %" PRId64 "-%" PRId64 "/%" PRId64 "\r\n"
                        "Content-Disposition: inline; filename=\"%s\"\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Cache-Control: public, max-age=60\r\n"
                        "Connection: %s\r\n"
                        "%s"
                        "\r\n",
                        status_code, status_str,
                        cfg->mimetype,
                        content_length,
                        range_start, range_end, cfg->filesize,
                        disp_name,
                        *keep_alive ? "keep-alive" : "close",
                        *keep_alive ? "Keep-Alive: timeout=15\r\n" : "");
    }

    if (hlen < 0 || (size_t)hlen >= sizeof(headers)) {
        fclose(fp);
        *keep_alive = false;
        return 0;
    }
    if (!send_all(client_sock, headers, (size_t)hlen)) {
        fclose(fp);
        *keep_alive = false;
        return 0;
    }

    int complete = 0;
    if (is_get && content_length > 0) {
        if (!send_file_range(client_sock, fp, is_range ? range_start : 0, content_length))
            *keep_alive = false;
        else
            complete = is_range ? 2 : 1;
    } else if (is_get && content_length == 0 && !is_range) {
        complete = 1;
    }

    fclose(fp);
    return complete;
}

/* Append from sock into buf[*used]. Returns 1 when a full header is present. */
static int recv_request(SOCKET sock, char *buf, size_t buflen, size_t *used, int timeout_ms) {
    long deadline = monotonic_ms() + (long)timeout_ms;

    if (!used || buflen < 2)
        return 0;
    buf[*used] = '\0';
    if (strstr(buf, "\r\n\r\n") != NULL)
        return 1;

    while (*used + 1 < buflen) {
        long now = monotonic_ms();
        int wait_ms = (int)(deadline - now);
        if (wait_ms < 0)
            wait_ms = 0;

        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
#ifndef _WIN32
            if (errno == EINTR)
                continue;
#endif
            return 0;
        }
        if (pr == 0)
            return 0;

        int n = recv(sock, buf + *used, (int)(buflen - 1 - *used), 0);
        if (n <= 0)
            return 0;
        *used += (size_t)n;
        buf[*used] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL)
            return 1;
    }
    return 0;
}

/* Returns 1 if a complete non-range GET finished, 2 if any Range body was sent. */
static int handle_client(SOCKET client_sock, const BeamConfig *cfg) {
    int served = 0;
    bool keep_alive = true;
    int idle_ms = (cfg->max_downloads > 0) ? KEEP_ALIVE_ONESHOT_MS : KEEP_ALIVE_MS;
    char req_buf[16384];
    size_t have = 0;

    set_media_sockopts(client_sock);

    if (!recv_request(client_sock, req_buf, sizeof(req_buf), &have, 30000)) {
        close_socket(client_sock);
        return 0;
    }

    for (;;) {
        char *hdrend = strstr(req_buf, "\r\n\r\n");
        if (!hdrend)
            break;
        int rc = process_request(client_sock, cfg, req_buf, &keep_alive);
        if (rc == 1)
            served = 1;
        else if (rc == 2 && served == 0)
            served = 2;

        {
            size_t consume = (size_t)(hdrend + 4 - req_buf);
            if (consume > have)
                consume = have;
            have -= consume;
            if (have)
                memmove(req_buf, req_buf + consume, have);
            req_buf[have] = '\0';
        }

        if (!keep_alive || !running)
            break;
        if (strstr(req_buf, "\r\n\r\n") != NULL)
            continue;
        if (!recv_request(client_sock, req_buf, sizeof(req_buf), &have, idle_ms))
            break;
    }

    close_socket(client_sock);
    return served;
}

#ifndef _WIN32
static bool mp4_moov_is_early(const char *path) {
    FILE *f = fopen(path, "rb");
    unsigned char buf[65536];
    size_t n, i;

    if (!f)
        return true;
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    for (i = 0; i + 4 <= n; i++) {
        if (buf[i] == 'm' && buf[i + 1] == 'o' && buf[i + 2] == 'o' && buf[i + 3] == 'v')
            return true;
    }
    return n < sizeof(buf);
}

static bool remux_faststart(BeamConfig *cfg) {
    char outpath[] = "/tmp/beam_faststart_XXXXXX";
    int fd, st;
    pid_t pid;
    struct stat outst;
    const char *fmt = (cfg->mimetype && strcmp(cfg->mimetype, "video/quicktime") == 0) ? "mov" : "mp4";

    fd = mkstemp(outpath);
    if (fd < 0)
        return false;
    fchmod(fd, 0600);
    close(fd);

    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-y", "-i", cfg->filepath,
               "-c", "copy", "-movflags", "+faststart", "-f", fmt, outpath, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        unlink(outpath);
        return false;
    }
    if (stat(outpath, &outst) != 0 || outst.st_size <= 0) {
        unlink(outpath);
        return false;
    }

    if (global_faststart_file[0]) {
        unlink(global_faststart_file);
        global_faststart_file[0] = '\0';
    }
    snprintf(global_faststart_file, sizeof(global_faststart_file), "%s", outpath);
    snprintf(cfg->filepath, sizeof(cfg->filepath), "%s", outpath);
    cfg->filesize = (int64_t)outst.st_size;
    return true;
}

static bool encode_slim(BeamConfig *cfg) {
    char outpath[] = "/tmp/beam_slim_XXXXXX";
    int fd, st;
    pid_t pid;
    struct stat outst;
    char scale[64];
    const char *crf;
    const char *audio_br;

    if (cfg->slim_height <= 480) {
        crf = "26";
        audio_br = "64k";
    } else {
        crf = "23";
        audio_br = "96k";
    }
    snprintf(scale, sizeof(scale), "scale=-2:'min(%d,ih)'", cfg->slim_height);

    fd = mkstemp(outpath);
    if (fd < 0)
        return false;
    fchmod(fd, 0600);
    close(fd);

    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-y", "-i", cfg->filepath,
               "-vf", scale,
               "-c:v", "libx264", "-preset", "veryfast", "-crf", crf,
               "-c:a", "aac", "-b:a", audio_br, "-ac", "2",
               "-movflags", "+faststart", "-f", "mp4", outpath, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        unlink(outpath);
        return false;
    }
    if (stat(outpath, &outst) != 0 || outst.st_size <= 0) {
        unlink(outpath);
        return false;
    }

    if (global_faststart_file[0]) {
        unlink(global_faststart_file);
        global_faststart_file[0] = '\0';
    }
    snprintf(global_faststart_file, sizeof(global_faststart_file), "%s", outpath);
    snprintf(cfg->filepath, sizeof(cfg->filepath), "%s", outpath);
    cfg->filesize = (int64_t)outst.st_size;
    cfg->mimetype = "video/mp4";
    return true;
}

static bool maybe_slim(BeamConfig *cfg) {
    if (cfg->slim_height <= 0)
        return false;
    if (!cfg->mimetype || strncmp(cfg->mimetype, "video/", 6) != 0)
        return false;
    fprintf(stderr, "beam: encoding slim %dp MP4 (libx264 veryfast)...\n", cfg->slim_height);
    if (encode_slim(cfg)) {
        fprintf(stderr, "beam: serving slim MP4 (%" PRId64 " bytes)\n", cfg->filesize);
        return true;
    }
    fprintf(stderr, "beam: warning: slim encode failed (install ffmpeg with libx264); serving original file\n");
    return false;
}
#endif

static bool maybe_faststart(BeamConfig *cfg) {
#ifndef _WIN32
    bool mp4 = (cfg->mimetype &&
                (strcmp(cfg->mimetype, "video/mp4") == 0 ||
                 strcmp(cfg->mimetype, "video/quicktime") == 0));
    bool should;

    if (!mp4 || cfg->no_faststart)
        return false;
    if (!cfg->faststart && cfg->filesize < 4096)
        return false;
    should = cfg->faststart || !mp4_moov_is_early(cfg->filepath);
    if (!should)
        return false;
    if (access("/usr/bin/ffmpeg", X_OK) != 0 && access("/usr/local/bin/ffmpeg", X_OK) != 0) {
        /* still try PATH via execlp */
    }
    fprintf(stderr, "beam: remuxing MP4 for fast start...\n");
    if (remux_faststart(cfg)) {
        fprintf(stderr, "beam: serving fast-start MP4 (%" PRId64 " bytes)\n", cfg->filesize);
        return true;
    }
    fprintf(stderr, "beam: warning: faststart remux failed (install ffmpeg); serving original file\n");
#else
    (void)cfg;
#endif
    return false;
}

static void usage(void) {
    fprintf(stderr, "usage: %s [-t ttl] [-p] [-r] [-k token] [-H host] [-c] [-q] [-1] [-w] [-f] [-F] [-s] [-S] [-b ip] [-P port] [file | -]\n", argv0);
    fprintf(stderr, "  -t ttl    Link lifetime (default: 5h; e.g. 30m, 2h, 300s)\n");
    fprintf(stderr, "  -p        Ephemeral public HTTPS tunnel via cloudflared (for remote sharing)\n");
    fprintf(stderr, "  -r        Reopen/resume last beamed file and link\n");
    fprintf(stderr, "  -k token  Use explicit token hash (32 hex characters)\n");
    fprintf(stderr, "  -H host   Host or IP for share link (e.g. 192.168.1.50, mynode.ts.net)\n");
    fprintf(stderr, "  -c        Copy share link to system clipboard\n");
    fprintf(stderr, "  -q        Quiet mode: suppress terminal QR code\n");
    fprintf(stderr, "  -1        One-shot: exit after first full GET, or after the first viewer goes idle\n");
    fprintf(stderr, "  -w        HTML web player page (default: native browser stream)\n");
    fprintf(stderr, "  -f        Force MP4 faststart remux (ffmpeg -movflags +faststart)\n");
    fprintf(stderr, "  -F        Skip MP4 faststart remux\n");
    fprintf(stderr, "  -s        Slim encode: 720p H.264 + AAC 96k (ffmpeg, for -p)\n");
    fprintf(stderr, "  -S        Extra-slim encode: 480p H.264 + AAC 64k\n");
    fprintf(stderr, "  -b ip     Bind IP address (default: 0.0.0.0)\n");
    fprintf(stderr, "  -P port   Listening port (default: 8080 or next available)\n");
    fprintf(stderr, "  -v        Show version\n");
    fprintf(stderr, "\nNotes:\n");
    fprintf(stderr, "  If no file is specified, beam reopens the last beamed file, or reads stdin.\n");
    fprintf(stderr, "  While running, press [p] to reopen tunnel, [c] to copy link, [q] to exit.\n");
    fprintf(stderr, "  Piping from autodub echoes progress and shares the dubbed video:\n");
    fprintf(stderr, "    autodub \"https://www.youtube.com/watch?v=...\" | beam -p -c -s\n");
    exit(1);
}

static bool check_and_record_file(const char *candidate,
                                  const char *line,
                                  char *dubbed_video,
                                  char *any_video,
                                  char *any_media,
                                  char *last_file) {
    if (!candidate || !*candidate) return false;

    /* Strip surrounding quotes if present */
    char clean[1024];
    const char *start = candidate;
    while (*start && isspace((unsigned char)*start)) start++;
    size_t clen = strlen(start);
    while (clen > 0 && isspace((unsigned char)start[clen - 1])) clen--;

    if ((start[0] == '"' && start[clen - 1] == '"') ||
        (start[0] == '\'' && start[clen - 1] == '\'')) {
        start++;
        clen -= 2;
    }
    if (clen == 0 || clen >= sizeof(clean)) return false;
    memcpy(clean, start, clen);
    clean[clen] = '\0';

    struct stat st;
    if (stat(clean, &st) != 0 || S_ISDIR(st.st_mode)) {
        return false;
    }

    const char *mime = beam_mime_type(clean);
    bool is_video = (strncmp(mime, "video/", 6) == 0);
    bool is_media = beam_is_media(mime);

    bool is_dubbed = (ci_strstr(line, "dubbed") != NULL ||
                      ci_strstr(line, "output") != NULL ||
                      ci_strstr(clean, "dubbed") != NULL);

    if (is_dubbed && is_video) {
        snprintf(dubbed_video, 1024, "%s", clean);
    }
    if (is_video) {
        snprintf(any_video, 1024, "%s", clean);
    }
    if (is_media) {
        snprintf(any_media, 1024, "%s", clean);
    }
    snprintf(last_file, 1024, "%s", clean);
    return true;
}

static void parse_line_candidates(const char *line,
                                  char *dubbed_video,
                                  char *any_video,
                                  char *any_media,
                                  char *last_file) {
    /* 1. Whole line (trimmed) */
    check_and_record_file(line, line, dubbed_video, any_video, any_media, last_file);

    /* 2. Prefixes like "Dubbed:", "output:", "File:", "Saved:" */
    static const char *prefixes[] = {
        "Dubbed:", "output:", "Output:", "Dubbed :", "output :",
        "Video:", "Saved:", "File:", NULL
    };
    for (int i = 0; prefixes[i] != NULL; i++) {
        const char *sub = ci_strstr(line, prefixes[i]);
        if (sub) {
            check_and_record_file(sub + strlen(prefixes[i]), line,
                                  dubbed_video, any_video, any_media, last_file);
        }
    }

    /* 3. Word tokens separated by whitespace */
    char copy[4096];
    snprintf(copy, sizeof(copy), "%s", line);
    char *token = strtok(copy, " \t\r\n");
    while (token) {
        check_and_record_file(token, line, dubbed_video, any_video, any_media, last_file);
        token = strtok(NULL, " \t\r\n");
    }
}

int main(int argc, char *argv[]) {
    BeamConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.ttl_seconds = DEFAULT_TTL;
    cfg.port = 8080;
    snprintf(cfg.bind_ip, sizeof(cfg.bind_ip), "0.0.0.0");

    ARGBEGIN {
    case 't':
        cfg.ttl_seconds = beam_parse_duration(EARGF(usage()));
        break;
    case 'p':
        cfg.public_tunnel = true;
        break;
    case 'r':
        cfg.resume_last = true;
        break;
    case 'k':
        snprintf(cfg.token, sizeof(cfg.token), "%s", EARGF(usage()));
        break;
    case 'H':
        snprintf(cfg.host, sizeof(cfg.host), "%s", EARGF(usage()));
        break;
    case 'c':
        cfg.copy_clipboard = true;
        break;
    case 'q':
        cfg.no_qr = true;
        break;
    case '1':
        cfg.max_downloads = 1;
        break;
    case 'w':
        cfg.web_player = true;
        break;
    case 'f':
        cfg.faststart = true;
        break;
    case 'F':
        cfg.no_faststart = true;
        break;
    case 's':
        cfg.slim_height = 720;
        break;
    case 'S':
        cfg.slim_height = 480;
        break;
    case 'b':
        snprintf(cfg.bind_ip, sizeof(cfg.bind_ip), "%s", EARGF(usage()));
        break;
    case 'P':
        cfg.port = atoi(EARGF(usage()));
        break;
    case 'v':
        fprintf(stdout, "beam " VERSION "\n");
        return 0;
    default:
        usage();
    } ARGEND;

    atexit(cleanup_temp_file);
#ifndef _WIN32
    atexit(cleanup_tunnel);
#endif

    bool is_temp_file = false;

    if (cfg.resume_last || argc < 1 || strcmp(argv[0], "-") == 0) {
        if (!cfg.resume_last && argc >= 1 && strcmp(argv[0], "-") == 0) {
            /* Raw stdin stream: write to a temporary file */
            char tmppath[] = "/tmp/beam_stream_XXXXXX";
            int fd = mkstemp(tmppath);
            if (fd < 0) {
                fprintf(stderr, "beam: failed to create temporary file: %s\n", strerror(errno));
                return 1;
            }
            char chunk[65536];
            ssize_t n;
            while ((n = read(STDIN_FILENO, chunk, sizeof(chunk))) > 0) {
                if (write(fd, chunk, (size_t)n) != n) {
                    fprintf(stderr, "beam: failed writing to temp file: %s\n", strerror(errno));
                    close(fd);
                    unlink(tmppath);
                    return 1;
                }
            }
            close(fd);
            snprintf(cfg.filepath, sizeof(cfg.filepath), "%s", tmppath);
            snprintf(global_temp_file, sizeof(global_temp_file), "%s", tmppath);
            is_temp_file = true;
        } else if (cfg.resume_last || isatty(STDIN_FILENO)) {
            /* No file given or explicit -r: check for last beamed file */
            char last_file[1024] = "";
            char last_tok[64] = "";
            if (load_last_state(last_file, sizeof(last_file), last_tok, sizeof(last_tok))) {
                snprintf(cfg.filepath, sizeof(cfg.filepath), "%s", last_file);
                if (!cfg.token[0] && last_tok[0]) {
                    snprintf(cfg.token, sizeof(cfg.token), "%.32s", last_tok);
                }
                fprintf(stderr, "beam: reopening last file: %s\n", cfg.filepath);
            } else {
                usage();
            }
        } else {
            /* Piped input from tools like autodub, find, ls, echo, etc. */
            char line[4096];
            char dubbed_video[1024] = "";
            char any_video[1024] = "";
            char any_media[1024] = "";
            char last_file[1024] = "";

            while (fgets(line, sizeof(line), stdin)) {
                /* Echo output in real-time so users see live pipeline progress */
                fputs(line, stdout);
                fflush(stdout);

                parse_line_candidates(line, dubbed_video, any_video, any_media, last_file);
            }

            char chosen_file[1024] = "";
            if (dubbed_video[0]) {
                snprintf(chosen_file, sizeof(chosen_file), "%s", dubbed_video);
            } else if (any_video[0]) {
                snprintf(chosen_file, sizeof(chosen_file), "%s", any_video);
            } else if (any_media[0]) {
                snprintf(chosen_file, sizeof(chosen_file), "%s", any_media);
            } else if (last_file[0]) {
                snprintf(chosen_file, sizeof(chosen_file), "%s", last_file);
            }

            if (!chosen_file[0]) {
                fprintf(stderr, "\nbeam: error: no valid file path detected from piped input\n");
                return 1;
            }

            snprintf(cfg.filepath, sizeof(cfg.filepath), "%s", chosen_file);
        }
    } else {
        snprintf(cfg.filepath, sizeof(cfg.filepath), "%s", argv[0]);
    }

    /* Stat the target file */
    struct stat st;
    if (stat(cfg.filepath, &st) != 0) {
        fprintf(stderr, "beam: cannot access '%s': %s\n", cfg.filepath, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        fprintf(stderr, "beam: '%s' is a directory, only files are supported\n", cfg.filepath);
        return 1;
    }

    cfg.filesize = (int64_t)st.st_size;

    /* Extract filename */
    if (is_temp_file) {
        snprintf(cfg.filename, sizeof(cfg.filename), "stream.mp4");
    } else {
        const char *slash = strrchr(cfg.filepath, '/');
#ifdef _WIN32
        const char *bslash = strrchr(cfg.filepath, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
        if (slash) {
            snprintf(cfg.filename, sizeof(cfg.filename), "%s", slash + 1);
        } else {
            snprintf(cfg.filename, sizeof(cfg.filename), "%s", cfg.filepath);
        }
    }

    cfg.mimetype = beam_mime_type(cfg.filename);

    char saved_filepath[1024];
    snprintf(saved_filepath, sizeof(saved_filepath), "%s", cfg.filepath);

    if (!maybe_slim(&cfg))
        maybe_faststart(&cfg);

    if (!cfg.token[0]) {
        if (beam_generate_token(cfg.token, sizeof(cfg.token)) != 0) {
            fprintf(stderr, "beam: failed to read a CSPRNG for the share token\n");
            return 1;
        }
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "beam: WSAStartup failed\n");
        return 1;
    }
#endif

    /* Setup server socket */
    SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == INVALID_SOCKET) {
        fprintf(stderr, "beam: socket creation failed: %s\n", strerror(errno));
        return 1;
    }

    int opt = 1;
#ifndef _WIN32
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(cfg.bind_ip);

    int port_attempts = 0;
    while (port_attempts < 100) {
        serv_addr.sin_port = htons((uint16_t)cfg.port);
        if (bind(server_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == 0) {
            break;
        }
        cfg.port++;
        port_attempts++;
    }

    if (port_attempts >= 100) {
        fprintf(stderr, "beam: failed to bind to a port\n");
        close_socket(server_sock);
        return 1;
    }

    if (listen(server_sock, 128) < 0) {
        fprintf(stderr, "beam: listen failed: %s\n", strerror(errno));
        close_socket(server_sock);
        return 1;
    }

    /* Build sharing URL */
    char share_url[2048] = "";
    bool tunnel_ok = false;

    if (cfg.public_tunnel) {
        tunnel_ok = open_tunnel(&cfg, share_url, sizeof(share_url));
        if (!tunnel_ok) {
            fprintf(stderr, "beam: warning: public tunnel failed, falling back to local network\n");
        }
    }

    if (!tunnel_ok) {
        char host_str[256];
        if (cfg.host[0]) {
            snprintf(host_str, sizeof(host_str), "%s", cfg.host);
        } else {
            beam_detect_local_ip(host_str, sizeof(host_str));
        }

        if (strstr(host_str, "://")) {
            /* Full scheme specified, e.g. https://tunnel.example.com */
            if (strchr(host_str + 8, ':')) {
                snprintf(share_url, sizeof(share_url), "%s/%s", host_str, cfg.token);
            } else {
                snprintf(share_url, sizeof(share_url), "%s:%d/%s", host_str, cfg.port, cfg.token);
            }
        } else {
            if (strchr(host_str, ':')) {
                /* Port already included in host argument */
                snprintf(share_url, sizeof(share_url), "http://%s/%s", host_str, cfg.token);
            } else {
                snprintf(share_url, sizeof(share_url), "http://%s:%d/%s", host_str, cfg.port, cfg.token);
            }
        }
    }

    /* Save last state for instant reopen */
    save_last_state(saved_filepath, cfg.token, share_url);

    /* Set signal handlers */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, sig_child);
#endif

    /* Terminal banner */
    char size_str[32];
    beam_format_size(cfg.filesize, size_str, sizeof(size_str));

    printf("\n  \033[1;36mbeam\033[0m\n");
    printf("  --------------------------------------------------\n");
    printf("  File:     %s (%s, %s)\n", cfg.filename, size_str, cfg.mimetype);
    printf("  Expires:  %d seconds (%dh %dm)\n",
           cfg.ttl_seconds, cfg.ttl_seconds / 3600, (cfg.ttl_seconds % 3600) / 60);
    printf("  Link:     \033[4;32m%s\033[0m\n", share_url);

    if (cfg.copy_clipboard) {
        if (copy_to_clipboard(share_url)) {
            printf("            \033[90m(copied to clipboard)\033[0m\n");
        }
    }

    if (!cfg.no_qr) {
        printf("\n  Scan with camera:\n\n");
        qr_print_terminal(stdout, share_url);
    }

    printf("\n  Commands: [p] reopen tunnel  [c] copy link  [q] quit\n\n");
    fflush(stdout);

#ifndef _WIN32
    int tty_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK);
    if (tty_fd >= 0) {
        enable_raw_terminal(tty_fd);
    }
#endif

    /* Event Loop */
    cfg.start_time = time(NULL);
#ifndef _WIN32
    int workers = 0;
    int seen_range_viewer = 0;
#endif

    while (running) {
        time_t now = time(NULL);
        if (now - cfg.start_time >= cfg.ttl_seconds) {
            printf("\nbeam: link expired after %d seconds\n", cfg.ttl_seconds);
            break;
        }

#ifndef _WIN32
        /* Auto-reconnect tunnel if cloudflared child exited unexpectedly */
        drain_tunnel_logs();
        if (cfg.public_tunnel && tunnel_pid > 0) {
            int status = 0;
            pid_t wp = waitpid(tunnel_pid, &status, WNOHANG);
            if (wp == tunnel_pid || (wp == -1 && errno == ECHILD)) {
                tunnel_pid = -1;
                if (tunnel_log_fd >= 0) {
                    close(tunnel_log_fd);
                    tunnel_log_fd = -1;
                }
                printf("\nbeam: tunnel disconnected, reconnecting...\n");
                if (open_tunnel(&cfg, share_url, sizeof(share_url))) {
                    save_last_state(saved_filepath, cfg.token, share_url);
                    printf("  Link:     \033[4;32m%s\033[0m\n", share_url);
                    if (cfg.copy_clipboard) {
                        copy_to_clipboard(share_url);
                    }
                    fflush(stdout);
                }
            }
        }

        /* Reap finished worker children and count downloads */
        {
            int st;
            pid_t w;
            while ((w = waitpid(-1, &st, WNOHANG)) > 0) {
                if (w == tunnel_pid) {
                    tunnel_pid = -1;
                    continue;
                }
                if (workers > 0)
                    workers--;
                if (WIFEXITED(st) && WEXITSTATUS(st) == WORKER_EXIT_FULL)
                    cfg.download_count++;
                else if (WIFEXITED(st) && WEXITSTATUS(st) == WORKER_EXIT_RANGE)
                    seen_range_viewer = 1;
            }
        }
        if (cfg.max_downloads > 0 && seen_range_viewer && workers == 0) {
            cfg.download_count++;
            seen_range_viewer = 0;
        }
        if (cfg.max_downloads > 0 && cfg.download_count >= cfg.max_downloads) {
            printf("beam: download limit reached\n");
            break;
        }
#endif

        struct pollfd pfds[2];
        pfds[0].fd = server_sock;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        int nfds = 1;

#ifndef _WIN32
        if (tty_fd >= 0) {
            pfds[1].fd = tty_fd;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;
            nfds = 2;
        }
#endif

        int ret = poll(pfds, (nfds_t)nfds, 1000); /* 1s timeout to check TTL */
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

#ifndef _WIN32
        /* Check keyboard command input */
        if (tty_fd >= 0 && (pfds[1].revents & POLLIN)) {
            char cmd = 0;
            if (read(tty_fd, &cmd, 1) == 1) {
                if (cmd == 'p' || cmd == 't') {
                    printf("\nbeam: opening tunnel on demand...\n");
                    if (open_tunnel(&cfg, share_url, sizeof(share_url))) {
                        save_last_state(saved_filepath, cfg.token, share_url);
                        printf("  Link:     \033[4;32m%s\033[0m\n", share_url);
                        if (copy_to_clipboard(share_url)) {
                            printf("            \033[90m(copied to clipboard)\033[0m\n");
                        }
                        if (!cfg.no_qr) {
                            printf("\n  Scan with camera:\n\n");
                            qr_print_terminal(stdout, share_url);
                        }
                        printf("\n  Commands: [p] reopen tunnel  [c] copy link  [q] quit\n\n");
                        fflush(stdout);
                    }
                } else if (cmd == 'c') {
                    if (copy_to_clipboard(share_url)) {
                        printf("\n  \033[90m(link copied to clipboard)\033[0m\n");
                        fflush(stdout);
                    }
                } else if (cmd == 'q' || cmd == 'x') {
                    running = 0;
                    break;
                }
            }
        }
#endif

        if (ret > 0 && (pfds[0].revents & POLLIN)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            SOCKET client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);

            if (client_sock != INVALID_SOCKET) {
                char client_ip[INET_ADDRSTRLEN] = "unknown";
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

                time_t t = time(NULL);
                struct tm *tm_info = localtime(&t);
                char time_str[32];
                strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

                printf("  [%s] %s\n", time_str, client_ip);
                fflush(stdout);

#ifndef _WIN32
                pid_t child = fork();
                if (child == 0) {
                    close_socket(server_sock);
                    if (tty_fd >= 0) close(tty_fd);
                    int rc = handle_client(client_sock, &cfg);
                    _exit(rc == 1 ? WORKER_EXIT_FULL : (rc == 2 ? WORKER_EXIT_RANGE : 0));
                }
                if (child > 0) {
                    workers++;
                    close_socket(client_sock);
                } else {
                    int rc = handle_client(client_sock, &cfg);
                    if (rc == 1)
                        cfg.download_count++;
                    else if (rc == 2)
                        cfg.download_count++;
                }
#else
                int rc = handle_client(client_sock, &cfg);
                if (rc == 1 || rc == 2)
                    cfg.download_count++;
#endif
            }
        }

#ifndef _WIN32
        /* Re-check downloads after handling connections */
        {
            int st;
            pid_t w;
            while ((w = waitpid(-1, &st, WNOHANG)) > 0) {
                if (w == tunnel_pid) {
                    tunnel_pid = -1;
                    continue;
                }
                if (workers > 0)
                    workers--;
                if (WIFEXITED(st) && WEXITSTATUS(st) == WORKER_EXIT_FULL)
                    cfg.download_count++;
                else if (WIFEXITED(st) && WEXITSTATUS(st) == WORKER_EXIT_RANGE)
                    seen_range_viewer = 1;
            }
        }
        if (cfg.max_downloads > 0 && seen_range_viewer && workers == 0) {
            cfg.download_count++;
            seen_range_viewer = 0;
        }
        if (cfg.max_downloads > 0 && cfg.download_count >= cfg.max_downloads) {
            printf("beam: download limit reached\n");
            break;
        }
#endif
    }

    /* Cleanup */
#ifndef _WIN32
    if (tty_fd >= 0) {
        restore_terminal();
        close(tty_fd);
    }
#endif

    close_socket(server_sock);

#ifndef _WIN32
    cleanup_tunnel();
#else
    WSACleanup();
#endif

    printf("beam: link closed\n");
    return 0;
}
