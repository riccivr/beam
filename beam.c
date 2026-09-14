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
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
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
  #define close_socket(s) close(s)
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
#endif

#ifndef VERSION
#define VERSION "1.0.0"
#endif

char *argv0;
static volatile sig_atomic_t running = 1;
static char global_temp_file[1024] = "";

static void cleanup_temp_file(void) {
    if (global_temp_file[0]) {
        unlink(global_temp_file);
        global_temp_file[0] = '\0';
    }
}

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
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

void beam_generate_token(char *token, size_t len) {
    static const char hex[] = "0123456789abcdef";
    uint8_t rand_bytes[16];
    bool urandom_ok = false;

#ifndef _WIN32
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(rand_bytes, 1, sizeof(rand_bytes), f) == sizeof(rand_bytes)) {
            urandom_ok = true;
        }
        fclose(f);
    }
#endif

    if (!urandom_ok) {
        srand((unsigned int)(time(NULL) ^ clock() ^ (uintptr_t)&token));
        for (size_t i = 0; i < sizeof(rand_bytes); i++) {
            rand_bytes[i] = (uint8_t)(rand() & 0xFF);
        }
    }

    size_t out_idx = 0;
    for (size_t i = 0; i < sizeof(rand_bytes) && (out_idx + 2) < len; i++) {
        token[out_idx++] = hex[(rand_bytes[i] >> 4) & 0x0F];
        token[out_idx++] = hex[rand_bytes[i] & 0x0F];
    }
    token[out_idx] = '\0';
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
            /* Prefer 192.168.*, 10.*, 172.16-31.*, or 100.* (Tailscale) */
            if (strncmp(ip, "192.168.", 8) == 0 ||
                strncmp(ip, "10.", 3) == 0 ||
                strncmp(ip, "100.", 4) == 0 ||
                strncmp(ip, "172.", 4) == 0) {
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

static void send_http_response(SOCKET sock, int status, const char *status_text,
                               const char *extra_headers, const char *body, size_t body_len) {
    char header[1024];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Length: %zu\r\n"
                        "%s"
                        "Connection: keep-alive\r\n\r\n",
                        status, status_text, body_len, extra_headers ? extra_headers : "");
    send(sock, header, (size_t)hlen, 0);
    if (body && body_len > 0) {
        send(sock, body, body_len, 0);
    }
}

static void render_html_player(char *html, size_t maxlen, const BeamConfig *cfg) {
    char size_str[32];
    beam_format_size(cfg->filesize, size_str, sizeof(size_str));

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
             cfg->filename,
             cfg->token,
             cfg->filename, size_str,
             cfg->token, cfg->filename);
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

static void handle_client(SOCKET client_sock, BeamConfig *cfg, bool *request_handled) {
    char req_buf[4096];
    int n = recv(client_sock, req_buf, sizeof(req_buf) - 1, 0);
    if (n <= 0) {
        close_socket(client_sock);
        return;
    }
    req_buf[n] = '\0';

    /* Parse request line: METHOD PATH PROTOCOL */
    char method[16] = "";
    char path[1024] = "";
    char proto[16] = "";
    if (sscanf(req_buf, "%15s %1023s %15s", method, path, proto) < 2) {
        send_http_response(client_sock, 400, "Bad Request", NULL, "Bad Request\n", 12);
        close_socket(client_sock);
        return;
    }

    bool is_get = (strcmp(method, "GET") == 0);
    bool is_head = (strcmp(method, "HEAD") == 0);

    if (!is_get && !is_head) {
        send_http_response(client_sock, 405, "Method Not Allowed", "Allow: GET, HEAD\r\n", "Method Not Allowed\n", 19);
        close_socket(client_sock);
        return;
    }

    const char *subpath = NULL;
    if (!extract_token(path, cfg->token, &subpath)) {
        send_http_response(client_sock, 404, "Not Found", NULL, "404 Not Found\n", 14);
        close_socket(client_sock);
        return;
    }

    /* HTML web player page if explicitly enabled or requested */
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
        send_http_response(client_sock, 200, "OK", "Content-Type: text/html; charset=utf-8\r\n", is_get ? html : NULL, hlen);
        close_socket(client_sock);
        *request_handled = true;
        return;
    }

    /* Stream raw file directly with inline disposition so browser plays video */
    FILE *fp = fopen(cfg->filepath, "rb");
    if (!fp) {
        send_http_response(client_sock, 500, "Internal Server Error", NULL, "Failed to read file\n", 20);
        close_socket(client_sock);
        return;
    }

    /* Parse Range header */
    int64_t range_start = 0;
    int64_t range_end = cfg->filesize - 1;
    bool is_range = false;

    char *range_hdr = strstr(req_buf, "Range: bytes=");
    if (!range_hdr) range_hdr = strstr(req_buf, "range: bytes=");

    bool range_invalid = false;

    if (range_hdr) {
        range_hdr += 13;
        if (*range_hdr == '-') {
            /* Suffix range: bytes=-500 */
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

    /* Validate range */
    if (range_invalid) {
        char err_hdr[256];
        snprintf(err_hdr, sizeof(err_hdr), "Content-Range: bytes */%" PRId64 "\r\n", cfg->filesize);
        send_http_response(client_sock, 416, "Range Not Satisfiable", err_hdr, "Range Not Satisfiable\n", 22);
        fclose(fp);
        close_socket(client_sock);
        return;
    }

    int64_t content_length = is_range ? (range_end - range_start + 1) : cfg->filesize;
    int status_code = is_range ? 206 : 200;
    const char *status_str = is_range ? "Partial Content" : "OK";

    char headers[1024];
    int hlen = 0;

    if (is_range) {
        hlen = snprintf(headers, sizeof(headers),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %" PRId64 "\r\n"
                        "Content-Range: bytes %" PRId64 "-%" PRId64 "/%" PRId64 "\r\n"
                        "Content-Disposition: inline; filename=\"%s\"\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Connection: keep-alive\r\n\r\n",
                        status_code, status_str,
                        cfg->mimetype,
                        content_length,
                        range_start, range_end, cfg->filesize,
                        cfg->filename);
    } else {
        hlen = snprintf(headers, sizeof(headers),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %" PRId64 "\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Content-Disposition: inline; filename=\"%s\"\r\n"
                        "Connection: keep-alive\r\n\r\n",
                        status_code, status_str,
                        cfg->mimetype,
                        content_length,
                        cfg->filename);
    }

    send(client_sock, headers, (size_t)hlen, 0);

    /* Stream body if GET */
    if (is_get) {
#if defined(_WIN32)
        _fseeki64(fp, range_start, SEEK_SET);
#else
        fseeko(fp, (off_t)range_start, SEEK_SET);
#endif
        int64_t remaining = content_length;
        char chunk[65536];

        while (remaining > 0 && running) {
            size_t to_read = (remaining > (int64_t)sizeof(chunk)) ? sizeof(chunk) : (size_t)remaining;
            size_t bytes_read = fread(chunk, 1, to_read, fp);
            if (bytes_read == 0) break;

            int sent = send(client_sock, chunk, bytes_read, 0);
            if (sent <= 0) break; /* Client disconnected */
            remaining -= sent;
        }

        if (remaining == 0 && !is_range) {
            cfg->download_count++;
        }
    }

    fclose(fp);
    close_socket(client_sock);
    *request_handled = true;
}

static void usage(void) {
    fprintf(stderr, "usage: %s [-t ttl] [-H host] [-c] [-q] [-1] [-w] [-b ip] [-P port] [file | -]\n", argv0);
    fprintf(stderr, "  -t ttl    Link lifetime (default: 5h; e.g. 30m, 2h, 300s)\n");
    fprintf(stderr, "  -H host   Host or IP for share link (e.g. 192.168.1.50, mynode.ts.net)\n");
    fprintf(stderr, "  -c        Copy share link to system clipboard\n");
    fprintf(stderr, "  -q        Quiet mode: suppress terminal QR code\n");
    fprintf(stderr, "  -1        One-shot: exit after first complete download/view\n");
    fprintf(stderr, "  -w        HTML web player page (default: native browser stream)\n");
    fprintf(stderr, "  -b ip     Bind IP address (default: 0.0.0.0)\n");
    fprintf(stderr, "  -P port   Listening port (default: 8080 or next available)\n");
    fprintf(stderr, "  -v        Show version\n");
    fprintf(stderr, "\nNotes:\n");
    fprintf(stderr, "  If no file is specified, beam reads from standard input.\n");
    fprintf(stderr, "  Piping from autodub echoes progress and shares the dubbed video:\n");
    fprintf(stderr, "    autodub.sh \"https://www.youtube.com/watch?v=...\" | beam -c\n");
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

    bool is_temp_file = false;

    if (argc < 1 || strcmp(argv[0], "-") == 0) {
#ifndef _WIN32
        if (argc < 1 && isatty(STDIN_FILENO)) {
            usage();
        }
#endif
        if (argc >= 1 && strcmp(argv[0], "-") == 0) {
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
    beam_generate_token(cfg.token, sizeof(cfg.token));

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

    if (listen(server_sock, 16) < 0) {
        fprintf(stderr, "beam: listen failed: %s\n", strerror(errno));
        close_socket(server_sock);
        return 1;
    }

    /* Build clean sharing URL: http://<host>:<port>/<token> */
    char share_url[2048];
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

    /* Set signal handlers */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
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

    printf("\n  Press Ctrl+C to stop.\n\n");
    fflush(stdout);

    /* Event Loop */
    cfg.start_time = time(NULL);

    while (running) {
        time_t now = time(NULL);
        if (now - cfg.start_time >= cfg.ttl_seconds) {
            printf("\nbeam: link expired after %d seconds\n", cfg.ttl_seconds);
            break;
        }

        struct pollfd pfd;
        pfd.fd = server_sock;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, 1000); /* 1s timeout to check TTL */
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
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

                bool handled = false;
                handle_client(client_sock, &cfg, &handled);

                if (cfg.max_downloads > 0 && cfg.download_count >= cfg.max_downloads) {
                    printf("beam: download limit reached\n");
                    break;
                }
            }
        }
    }

    /* Cleanup */
    close_socket(server_sock);

#ifdef _WIN32
    WSACleanup();
#endif

    printf("beam: link closed\n");
    return 0;
}
