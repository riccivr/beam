#ifndef BEAM_H
#define BEAM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define DEFAULT_TTL (5 * 3600) /* 5 hours */
#define TOKEN_HEX_LEN 32

typedef struct {
    char filepath[1024];
    char filename[1024];
    int64_t filesize;
    const char *mimetype;
    char token[TOKEN_HEX_LEN + 1];

    char host[256];
    char bind_ip[64];
    int port;
    int ttl_seconds;
    int max_downloads;
    int download_count;

    bool public_tunnel;
    bool copy_clipboard;
    bool no_qr;
    bool web_player;
    bool resume_last;
    bool faststart;
    bool no_faststart;

    time_t start_time;
    char public_url[1024];
} BeamConfig;

/* MIME detection */
const char *beam_mime_type(const char *path);
bool beam_is_media(const char *mimetype);

/* Utilities */
void beam_format_size(int64_t bytes, char *buf, size_t maxlen);
int  beam_parse_duration(const char *str);
int  beam_generate_token(char *token, size_t len);
int  beam_detect_local_ip(char *buf, size_t maxlen);

#endif /* BEAM_H */
