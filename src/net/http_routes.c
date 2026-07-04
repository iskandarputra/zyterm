/**
 * @file    http_routes.c
 * @brief   HTTP request parsing, the route table, and response builders.
 *
 * The heart of the bridge: parse the request line, dispatch on the exact path
 * (ZT-039 — never strstr over the whole buffer), and either answer a one-shot
 * request (/, /metrics, /api/state, POST /tx, --webroot files) or promote the
 * slot to an SSE/WS stream. Response builders (send_json/send_text_c/…) and the
 * WebSocket upgrade handshake live here too. The trust checks come from
 * http_auth.c; the connection table and write helpers from http.c; the crypto
 * from http_sha1.c; the built-in UI from http_asset.c.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "http_internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <sys/stat.h>
#include <unistd.h>

/* ---- Response builders ---- */
/** Respond with a JSON body + 200 OK + optional CORS. */
static void send_json(zt_ctx *c, int fd, const char *json) {
    char hdr[384];
    int  h = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
                       "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-cache\r\n"
                       "%s\r\n",
                      strlen(json), cors_block(c));
    (void)http_write_all(fd, hdr, snprintf_len(h, sizeof hdr));
    (void)http_write_all(fd, json, strlen(json));
}

/** Reply to a CORS preflight. */
static void send_preflight(zt_ctx *c, int fd) {
    char hdr[384];
    int  h = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n"
                       "%s\r\n",
                      cors_block(c));
    (void)http_write_all(fd, hdr, snprintf_len(h, sizeof hdr));
}

/** Escape a C string for JSON output into @p out (up to @p cap-1 bytes). */
static void json_escape(const char *src, char *out, size_t cap) {
    size_t o = 0;
    if (!src) {
        if (cap) out[0] = '\0';
        return;
    }
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 8 < cap; p++) {
        switch (*p) {
        case '"':
            out[o++] = '\\';
            out[o++] = '"';
            break;
        case '\\':
            out[o++] = '\\';
            out[o++] = '\\';
            break;
        case '\n':
            out[o++] = '\\';
            out[o++] = 'n';
            break;
        case '\r':
            out[o++] = '\\';
            out[o++] = 'r';
            break;
        case '\t':
            out[o++] = '\\';
            out[o++] = 't';
            break;
        default:
            if (*p < 0x20)
                o += (size_t)snprintf(out + o, cap - o, "\\u%04x", *p);
            else
                out[o++] = (char)*p;
            break;
        }
    }
    if (o < cap)
        out[o] = '\0';
    else
        out[cap - 1] = '\0';
}

/** Build the JSON payload for GET /api/state. */
static void build_state_json(const zt_ctx *c, char *out, size_t cap) {
    char dev[256];
    json_escape(c->serial.device ? c->serial.device : "", dev, sizeof dev);
    snprintf(out, cap,
             "{"
             "\"version\":\"" ZT_VERSION "\","
             "\"device\":\"%s\","
             "\"baud\":%u,"
             "\"data_bits\":%d,"
             "\"parity\":\"%c\","
             "\"stop_bits\":%d,"
             "\"flow\":%d,"
             "\"connected\":%s,"
             "\"paused\":%s,"
             "\"hex\":%s,"
             "\"local_echo\":%s,"
             "\"timestamps\":%s,"
             "\"rx_bytes\":%llu,"
             "\"tx_bytes\":%llu,"
             "\"rx_lines\":%llu,"
             "\"rx_bps\":%.1f,"
             "\"frame_rx\":%u,"
             "\"frame_crc_err\":%u,"
             "\"rows\":%d,"
             "\"cols\":%d"
             "}",
             dev, c->serial.baud, c->serial.data_bits,
             c->serial.parity ? c->serial.parity : 'n', c->serial.stop_bits, c->serial.flow,
             c->serial.fd >= 0 ? "true" : "false", c->core.paused ? "true" : "false",
             c->log.hex_mode ? "true" : "false", c->proto.local_echo ? "true" : "false",
             c->proto.show_ts ? "true" : "false", (unsigned long long)c->core.rx_bytes,
             (unsigned long long)c->core.tx_bytes, (unsigned long long)c->core.rx_lines,
             c->tui.rx_bps, c->proto.rx_count, c->proto.crc_err, c->tui.rows, c->tui.cols);
}

/* ---- Safe webroot file serving ---- */

/** Guess Content-Type from a path extension. */
static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    dot++;
    if (!strcasecmp(dot, "html") || !strcasecmp(dot, "htm")) return "text/html; charset=utf-8";
    if (!strcasecmp(dot, "js") || !strcasecmp(dot, "mjs"))
        return "application/javascript; charset=utf-8";
    if (!strcasecmp(dot, "css")) return "text/css; charset=utf-8";
    if (!strcasecmp(dot, "json")) return "application/json; charset=utf-8";
    if (!strcasecmp(dot, "map")) return "application/json; charset=utf-8";
    if (!strcasecmp(dot, "svg")) return "image/svg+xml";
    if (!strcasecmp(dot, "png")) return "image/png";
    if (!strcasecmp(dot, "jpg") || !strcasecmp(dot, "jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, "gif")) return "image/gif";
    if (!strcasecmp(dot, "ico")) return "image/x-icon";
    if (!strcasecmp(dot, "webp")) return "image/webp";
    if (!strcasecmp(dot, "wasm")) return "application/wasm";
    if (!strcasecmp(dot, "txt")) return "text/plain; charset=utf-8";
    if (!strcasecmp(dot, "woff")) return "font/woff";
    if (!strcasecmp(dot, "woff2")) return "font/woff2";
    return "application/octet-stream";
}

/** Try to serve @p urlpath from @p root. Returns 1 on success, 0 on not-found. */
static int serve_webroot(zt_ctx *c, int fd, const char *root, const char *urlpath) {
    if (!root || !*root) return 0;

    /* Reject query strings: strip them so the file name is clean. */
    char   clean[512];
    size_t n = 0;
    for (; urlpath[n] && urlpath[n] != '?' && urlpath[n] != '#' && n < sizeof clean - 1; n++)
        clean[n] = urlpath[n];
    clean[n] = '\0';

    /* Reject any traversal attempt. */
    if (strstr(clean, "..")) return 0;

    /* Map "/" → "/index.html". */
    const char *rel = clean[0] == '/' ? clean + 1 : clean;
    if (!*rel) rel = "index.html";

    char full[1024];
    int  fn = snprintf(full, sizeof full, "%s/%s", root, rel);
    if (fn <= 0 || fn >= (int)sizeof full) return 0;

    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) return 0;

    int ffd = open(full, O_RDONLY | O_CLOEXEC);
    if (ffd < 0) return 0;

    char hdr[512];
    int  hn = snprintf(hdr, sizeof hdr,
                       "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                        "Connection: close\r\n%s\r\n",
                       mime_for(full), (long long)st.st_size, cors_block(c));
    (void)http_write_all(fd, hdr, snprintf_len(hn, sizeof hdr));

    char    buf[8192];
    ssize_t r;
    while ((r = read(ffd, buf, sizeof buf)) > 0) {
        if (http_write_all(fd, buf, (size_t)r) != 0) break;
    }
    close(ffd);
    return 1;
}

/** Like send_text() but includes CORS headers when @p c->net.http_cors is set. */
void send_text_c(zt_ctx *c, int fd, const char *status, const char *ctype, const char *body,
                 size_t n) {
    char hdr[384];
    int  h = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                       "Connection: close\r\nCache-Control: no-cache\r\n%s\r\n",
                      status, ctype, n, cors_block(c));
    (void)http_write_all(fd, hdr, snprintf_len(h, sizeof hdr));
    (void)http_write_all(fd, body, n);
}

/* Answer a /ws upgrade: echo the RFC 6455 accept-key and switch protocols. */
static void handle_ws_upgrade(int fd, const char *key) {
    char accept[64];
    zt_ws_accept_key(key, accept, sizeof accept);
    char resp[256];
    int  rn = snprintf(resp, sizeof resp,
                       "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: %s\r\n\r\n",
                       accept);
    (void)http_write_all(fd, resp, snprintf_len(rn, sizeof resp));
}

/* Parse a Content-Length header (case-insensitive, line-anchored) from the
 * request header block [buf, buf+hdr_len). Returns the declared byte count,
 * 0 when the header is absent (no body), HC_REQ_CAP+1 when it exceeds what
 * our bounded buffer can hold, or -1 when malformed. Lets hc_pump_new() wait
 * for the whole POST body before dispatch and classify_request() send exactly
 * that many bytes (ZT-034). */
long http_content_length(const char *buf, size_t hdr_len) {
    const char *end = buf + hdr_len;
    const char *p   = buf;
    while (p < end) {
        const char *nl      = memchr(p, '\n', (size_t)(end - p));
        size_t      linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (linelen >= 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
            const char *v  = p + 15;
            const char *le = p + linelen;
            while (v < le && (*v == ' ' || *v == '\t'))
                v++;
            long val = 0;
            bool any = false;
            while (v < le && *v >= '0' && *v <= '9') {
                val = val * 10 + (*v - '0');
                any = true;
                if (val > HC_REQ_CAP) return HC_REQ_CAP + 1; /* too large to buffer */
                v++;
            }
            return any ? val : -1;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 0; /* no Content-Length → no body */
}

/* Parse the request line's method and path (query string and HTTP version
 * stripped) into caller buffers. Returns false on a malformed line. Routing on
 * the parsed path instead of strstr over the whole buffer stops a webroot file
 * like /streamlit.html being served as an SSE stream, and stops a POST /tx
 * whose body merely contains "GET /stream" being hijacked into a stream
 * upgrade so the command never reaches the device (ZT-039). */
static bool parse_request_line(const char *req, char *method, size_t mcap, char *path,
                               size_t pcap) {
    const char *msp = strchr(req, ' ');
    if (!msp) return false;
    size_t ml = (size_t)(msp - req);
    if (ml == 0 || ml >= mcap) return false;
    memcpy(method, req, ml);
    method[ml]      = '\0';

    const char *p   = msp + 1;
    const char *end = p;
    while (*end && *end != ' ' && *end != '?' && *end != '\r' && *end != '\n')
        end++;
    size_t pl = (size_t)(end - p);
    if (pl == 0 || pl >= pcap) return false;
    memcpy(path, p, pl);
    path[pl] = '\0';
    return true;
}

/* Promote an HC_NEW slot to HC_SSE or HC_WS for ongoing streaming, or
 * leave the connection unstored (caller closes). */
void classify_request(zt_ctx *c, int i) {
    hc_t       *h   = &g_conn[i];
    int         cfd = h->fd;
    const char *req = h->req_buf;
    size_t      rn  = h->req_len;

    /* Route on the parsed request-target, not strstr over the whole buffer. */
    char method[8], path[512];
    if (!parse_request_line(req, method, sizeof method, path, sizeof path)) {
        send_text_c(c, cfd, "400 Bad Request", "text/plain", "", 0);
        hc_close(i);
        return;
    }
    bool is_get  = strcmp(method, "GET") == 0;
    bool is_post = strcmp(method, "POST") == 0;

    if (strcmp(method, "OPTIONS") == 0) {
        send_preflight(c, cfd);
        hc_close(i);
        return;
    }

    if (is_get && (strcmp(path, "/stream") == 0 || strcmp(path, "/api/stream") == 0)) {
        /* ZT-013: the live RX stream is sensitive — pin Origin/Host so a
         * cross-origin page can't open an EventSource and read device output. */
        if (!request_origin_ok(req)) {
            send_text_c(c, cfd, "403 Forbidden", "text/plain", "forbidden\n", 10);
            hc_close(i);
            return;
        }
        char hdrbuf[512];
        int  hn = snprintf(hdrbuf, sizeof hdrbuf,
                           "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                            "Cache-Control: no-cache\r\nConnection: keep-alive\r\n"
                            "%s\r\n: zyterm\n\n",
                           cors_block(c));
        (void)http_write_all(cfd, hdrbuf, snprintf_len(hn, sizeof hdrbuf));
        h->type = HC_SSE;
        return;
    }
    if (is_get && strcmp(path, "/ws") == 0) {
        /* ZT-013 (INVARIANTS §7): validate Origin/Host before upgrading, or any
         * web page could open a WS and read the live RX stream cross-origin. */
        if (!request_origin_ok(req)) {
            send_text_c(c, cfd, "403 Forbidden", "text/plain", "forbidden\n", 10);
            hc_close(i);
            return;
        }
        const char *k = strstr(req, "Sec-WebSocket-Key:");
        if (!k) {
            hc_close(i);
            return;
        }
        k += 18;
        while (*k == ' ' || *k == '\t')
            k++;
        const char *eol = strstr(k, "\r\n");
        if (!eol) {
            hc_close(i);
            return;
        }
        char   key[128];
        size_t kl = (size_t)(eol - k);
        if (kl >= sizeof key) {
            hc_close(i);
            return;
        }
        memcpy(key, k, kl);
        key[kl] = '\0';
        handle_ws_upgrade(cfd, key);
        h->type = HC_WS;
        return;
    }
    if (is_get && strcmp(path, "/metrics") == 0) {
        char snap[2048];
        int  n = snprintf(
            snap, sizeof snap,
            "zyterm_rx_bytes %llu\nzyterm_tx_bytes %llu\n"
             "zyterm_rx_lines %llu\nzyterm_frame_crc_err %llu\n",
            (unsigned long long)c->core.rx_bytes, (unsigned long long)c->core.tx_bytes,
            (unsigned long long)c->core.rx_lines, (unsigned long long)c->proto.crc_err);
        send_text_c(c, cfd, "200 OK", "text/plain; version=0.0.4", snap,
                    snprintf_len(n, sizeof snap));
        hc_close(i);
        return;
    }
    if (is_get && (strcmp(path, "/api/state") == 0 || strcmp(path, "/api/info") == 0)) {
        char json[1024];
        build_state_json(c, json, sizeof json);
        send_json(c, cfd, json);
        hc_close(i);
        return;
    }
    if (is_post && (strcmp(path, "/tx") == 0 || strcmp(path, "/api/send") == 0)) {
        /* ZT-004 (INVARIANTS §7): POST writes the serial line. Reject cross-site
         * / DNS-rebound callers (Origin/Host pinning) so a page the operator
         * visits can't push commands to the device, and — when --http-token is
         * set — require a bearer token on top. */
        if (!request_origin_ok(req)) {
            send_text_c(c, cfd, "403 Forbidden", "text/plain", "forbidden\n", 10);
            hc_close(i);
            return;
        }
        if (!request_token_ok(c, req)) {
            send_text_c(c, cfd, "401 Unauthorized", "text/plain", "unauthorized\n", 13);
            hc_close(i);
            return;
        }
        const char *body = strstr(req, "\r\n\r\n");
        if (body && c->serial.fd >= 0) {
            body += 4;
            size_t blen = rn - (size_t)(body - req);
            /* ZT-034: honor Content-Length so a pipelined or over-read tail
             * isn't sent to the device as part of this command. hc_pump_new()
             * has already waited for the full declared body to arrive. */
            long clen = http_content_length(req, (size_t)(body - req));
            if (clen > 0 && (size_t)clen < blen) blen = (size_t)clen;
            c->core.tx_direct(c, (const unsigned char *)body, blen);
        }
        send_text_c(c, cfd, "204 No Content", "text/plain", "", 0);
        hc_close(i);
        return;
    }
    if (is_get) {
        if (c->net.http_webroot && serve_webroot(c, cfd, c->net.http_webroot, path)) {
            hc_close(i);
        } else if (!c->net.http_webroot &&
                   (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
            send_text_c(c, cfd, "200 OK", "text/html; charset=utf-8", zt_http_index,
                        zt_http_index_len);
            hc_close(i);
        } else {
            send_text_c(c, cfd, "404 Not Found", "text/plain; charset=utf-8", "not found\n",
                        10);
            hc_close(i);
        }
        return;
    }
    send_text_c(c, cfd, "405 Method Not Allowed", "text/plain", "", 0);
    hc_close(i);
}
