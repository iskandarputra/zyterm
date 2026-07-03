/**
 * @file http.c
 * @brief Tiny HTTP server with Server-Sent Events and WebSocket streaming.
 *
 * Endpoints:
 *   GET /           — minimal HTML page with a live RX view
 *   GET /stream     — Server-Sent Events stream of RX bytes
 *   GET /ws         — WebSocket upgrade, RFC 6455, binary frames of RX
 *   GET /metrics    — same text as metrics.c snapshot
 *   POST /tx        — write request body to the serial line
 *
 * This is deliberately self-contained (no libwebsockets, no libmicrohttpd)
 * to keep the single-binary ethos: ~400 lines of plain BSD sockets + a
 * small SHA-1 + base64 for the WS handshake.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp for the Content-Length parse (ZT-034) */
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- SHA-1 (public domain, steve reid) ---- */
typedef struct {
    uint32_t      state[5];
    uint64_t      count;
    unsigned char buf[64];
} SHA1_CTX;
#define ROL(v, b) (((v) << (b)) | ((v) >> (32 - (b))))
static void sha1_tr(uint32_t st[5], const unsigned char b[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
               ((uint32_t)b[i * 4 + 2] << 8) | b[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = st[0], bb = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (bb & c) | ((~bb) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = bb ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (bb & c) | (bb & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = bb ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e          = d;
        d          = c;
        c          = ROL(bb, 30);
        bb         = a;
        a          = t;
    }
    st[0] += a;
    st[1] += bb;
    st[2] += c;
    st[3] += d;
    st[4] += e;
}
static void sha1_init(SHA1_CTX *c) {
    c->state[0] = 0x67452301;
    c->state[1] = 0xEFCDAB89;
    c->state[2] = 0x98BADCFE;
    c->state[3] = 0x10325476;
    c->state[4] = 0xC3D2E1F0;
    c->count    = 0;
}
static void sha1_update(SHA1_CTX *c, const unsigned char *d, size_t n) {
    size_t i = (c->count >> 3) & 63;
    c->count += (uint64_t)n << 3;
    size_t j = 64 - i;
    if (n < j) {
        memcpy(c->buf + i, d, n);
        return;
    }
    memcpy(c->buf + i, d, j);
    sha1_tr(c->state, c->buf);
    size_t k = j;
    while (k + 64 <= n) {
        sha1_tr(c->state, d + k);
        k += 64;
    }
    memcpy(c->buf, d + k, n - k);
}
static void sha1_final(SHA1_CTX *c, unsigned char out[20]) {
    uint64_t      bits = c->count;
    unsigned char pad  = 0x80;
    sha1_update(c, &pad, 1);
    unsigned char zero = 0;
    while (((c->count >> 3) & 63) != 56)
        sha1_update(c, &zero, 1);
    unsigned char blen[8];
    for (int i = 0; i < 8; i++)
        blen[i] = (unsigned char)(bits >> (56 - i * 8));
    sha1_update(c, blen, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (unsigned char)(c->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->state[i]);
    }
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void       b64enc(const unsigned char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    out[o] = '\0';
}

/* ---- Connection table ----
 *
 * HC_NEW slots are accepted but still parsing the HTTP request line +
 * headers. The earlier implementation read the request synchronously
 * inside accept_and_classify() with a 20 × usleep(10ms) busy-wait,
 * which let a slowloris client freeze the entire UI / serial loop for
 * up to 200 ms per connection. Now each connection carries its own
 * @c req_buf and @c accepted_at; @c http_tick() pumps reads
 * non-blocking and times out idle requests after @c HC_HEADER_TIMEOUT
 * seconds without blocking the main loop. */
#define HC_HEADER_TIMEOUT_S 5.0
#define HC_REQ_CAP          4096

typedef enum { HC_NEW, HC_SSE, HC_WS } hc_type;
typedef struct {
    int     fd;
    hc_type type;
    /* HC_NEW only: accumulating request bytes until "\r\n\r\n". */
    char            req_buf[HC_REQ_CAP];
    size_t          req_len;
    struct timespec accepted_at;
} hc_t;
#define HC_MAX 16
static hc_t g_conn[HC_MAX];

/* Clamp snprintf's return value to "bytes actually present in @p cap".
 * snprintf returns the would-be length on truncation (>= cap), which
 * when used directly as a write count makes the kernel read past the
 * stack buffer and emit attacker-influenced bytes to the network. */
static inline size_t snprintf_len(int n, size_t cap) {
    if (n < 0 || cap == 0) return 0;
    return ((size_t)n >= cap) ? (cap - 1) : (size_t)n;
}

static void hc_close(int i) {
    if (g_conn[i].fd >= 0) {
        close(g_conn[i].fd);
        g_conn[i].fd = -1;
    }
}

/* Bounded, EAGAIN-aware full write for one-shot HTTP responses.
 *
 * ZT-011 (INVARIANTS §7): the client fds are SOCK_NONBLOCK, and the loop's
 * zt_write_all() only retries EINTR — using it to push a large --webroot file
 * truncated the response on the first EAGAIN. This waits for writability, but
 * with an overall deadline so a stalled peer can't hang the loop (§3). */
#define HTTP_WRITE_DEADLINE_MS 2000
static int http_write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p   = (const unsigned char *)buf;
    size_t               rem = n;
    /* Absolute deadline captured once at entry (ZT-044): the old per-write
     * reset meant a client draining a slow trickle — each read landing inside
     * the 250 ms poll window — kept resetting the timer and held the loop here
     * for the whole (up to ~16.7 KB) response. Bound total wall time regardless
     * of incremental progress so the single-threaded loop stays responsive. */
    struct timespec start;
    now(&start);
    while (rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w > 0) {
            p += (size_t)w;
            rem -= (size_t)w;
            continue;
        }
        if (w == 0) return -1;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        struct pollfd pf = {.fd = fd, .events = POLLOUT};
        int           pr = poll(&pf, 1, 250);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        struct timespec nowt;
        now(&nowt);
        if (ts_diff_sec(&nowt, &start) * 1000.0 >= HTTP_WRITE_DEADLINE_MS)
            return -1; /* stalled */
    }
    return 0;
}

/* Non-blocking "write every byte or fail" for streaming frames. A partial
 * write corrupts SSE/WS framing, so the peer must be dropped — never blocks
 * (broadcast runs on the loop tick). */
static int http_stream_write(int fd, const void *buf, size_t n) {
    const unsigned char *p   = (const unsigned char *)buf;
    size_t               rem = n;
    while (rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w > 0) {
            p += (size_t)w;
            rem -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return -1; /* EAGAIN or hard error → caller closes the peer */
    }
    return 0;
}

/* ---- Origin / Host validation + bearer auth (ZT-004, ZT-013) ----
 *
 * The bridge binds loopback only, but the operator's browser is itself on
 * loopback, so a cross-site page or a DNS-rebound host can still reach it.
 * Defences: pin Host to a loopback literal (rebind defence); pin Origin —
 * which browsers always send cross-origin and on POST — to loopback (CSRF
 * defence); and, when --http-token is set, require a matching bearer token on
 * state-changing routes. See INVARIANTS §7. */

/* Find header @name (e.g. "Host:") in the request; return its trimmed value +
 * length via @out_len, or NULL. Stops at the blank line ending the headers. */
static const char *http_header(const char *req, const char *name, size_t *out_len) {
    size_t nl = strlen(name);
    for (const char *line = req; line && *line;) {
        const char *eol = strstr(line, "\r\n");
        if (!eol || eol == line) break; /* malformed, or blank line = headers end */
        if ((size_t)(eol - line) > nl && strncasecmp(line, name, nl) == 0) {
            const char *v = line + nl;
            while (*v == ' ' || *v == '\t')
                v++;
            *out_len = (size_t)(eol - v);
            return v;
        }
        line = eol + 2;
    }
    return NULL;
}

static bool host_is_loopback(const char *h, size_t n) {
    static const char *ok[] = {"127.0.0.1", "localhost", "[::1]", "::1"};
    for (size_t k = 0; k < sizeof ok / sizeof ok[0]; k++) {
        size_t l = strlen(ok[k]);
        if (n >= l && strncasecmp(h, ok[k], l) == 0 && (n == l || h[l] == ':')) return true;
    }
    return false;
}

static bool origin_is_loopback(const char *v, size_t n) {
    const char *s = memmem(v, n, "://", 3);
    if (!s) return false; /* "null" (file://, sandboxed) or malformed → reject */
    s += 3;
    return host_is_loopback(s, n - (size_t)(s - v));
}

/* Reject cross-origin / DNS-rebound requests on state-changing & streaming
 * routes. Same-origin built-in UI passes; a foreign page or rebound host does
 * not. */
static bool request_origin_ok(const char *req) {
    size_t      hl   = 0;
    const char *host = http_header(req, "Host:", &hl);
    if (!host || !host_is_loopback(host, hl)) return false;
    size_t      ol  = 0;
    const char *org = http_header(req, "Origin:", &ol);
    if (org && !origin_is_loopback(org, ol)) return false;
    return true;
}

/* When --http-token is set, require "Authorization: Bearer <token>". With no
 * token configured the bridge is anonymous-but-origin-pinned (the built-in UI
 * keeps working); set a token to additionally gate writes. */
static bool request_token_ok(const zt_ctx *c, const char *req) {
    if (!c->net.http_token || !*c->net.http_token) return true;
    size_t            al    = 0;
    const char       *a     = http_header(req, "Authorization:", &al);
    static const char pfx[] = "Bearer ";
    size_t            pl    = sizeof pfx - 1;
    if (!a || al <= pl || strncmp(a, pfx, pl) != 0) return false;
    a += pl;
    al -= pl;
    size_t tl = strlen(c->net.http_token);
    return al == tl && memcmp(a, c->net.http_token, tl) == 0;
}

/* ---- Server setup ---- */
int http_start(zt_ctx *c, int port) {
    if (!c || c->net.http_fd >= 0) return -1;
    for (int i = 0; i < HC_MAX; i++)
        g_conn[i].fd = -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family         = AF_INET;
    a.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    a.sin_port           = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    c->net.http_fd   = fd;
    c->net.http_port = port;
    log_notice(c, "http bridge on http://127.0.0.1:%d/", port);
    return 0;
}

void http_stop(zt_ctx *c) {
    if (!c) return;
    for (int i = 0; i < HC_MAX; i++)
        hc_close(i);
    if (c->net.http_fd >= 0) {
        close(c->net.http_fd);
        c->net.http_fd = -1;
    }
}

/* ---- Request handling ---- */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
static const char kIndex[] =
    /* ---- HTML head ---- */
    "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>zyterm</title><style>\n"
    ":root{--bg:#0a0a0f;--sf:#12121a;--sf2:#1a1a24;--bd:#252535;"
    "--tx:#c8cad8;--dm:#585b70;--ac:#7aa2f7;--gn:#9ece6a;--rd:#f7768e;"
    "--yl:#e0af68;--tl:#73daca;--r:6px}\n"
    "*{box-sizing:border-box;margin:0;padding:0}\n"
    "body{font-family:'Cascadia Code','JetBrains Mono','Fira Code','SF Mono',monospace;"
    "background:var(--bg);color:var(--tx);height:100vh;display:flex;"
    "flex-direction:column;overflow:hidden;font-size:13px}\n"
    /* header */
    ".hdr{display:flex;align-items:center;padding:10px 16px;background:var(--sf);"
    "border-bottom:1px solid var(--bd);gap:12px;flex-shrink:0}\n"
    ".hdr .logo{font-size:15px;font-weight:700;color:var(--ac);letter-spacing:-.5px}\n"
    ".hdr .logo span{color:var(--dm);font-weight:400}\n"
    ".dot{width:8px;height:8px;border-radius:50%;background:var(--gn);"
    "box-shadow:0 0 6px var(--gn);transition:all .3s}\n"
    ".dot.off{background:var(--rd);box-shadow:0 0 6px var(--rd)}\n"
    ".st{color:var(--dm);font-size:12px}\n"
    ".dev{color:var(--tl);font-size:12px;font-weight:500}\n"
    ".stats{margin-left:auto;display:flex;gap:16px;color:var(--dm);font-size:12px}\n"
    ".stats b{color:var(--tx);font-weight:500}\n"
    /* toolbar */
    ".bar{display:flex;align-items:center;padding:6px 16px;background:var(--sf);"
    "border-bottom:1px solid var(--bd);gap:6px;flex-shrink:0;flex-wrap:wrap}\n"
    ".btn{background:var(--sf2);color:var(--dm);border:1px solid var(--bd);"
    "border-radius:var(--r);padding:4px 10px;cursor:pointer;font-size:11px;"
    "font-family:inherit;transition:all .15s;display:inline-flex;"
    "align-items:center;gap:4px}\n"
    ".btn:hover{background:var(--bd);color:var(--tx)}\n"
    ".btn.on{background:var(--ac);color:#0a0a0f;border-color:var(--ac)}\n"
    ".btn.on:hover{opacity:.85}\n"
    ".sep{width:1px;height:20px;background:var(--bd);margin:0 4px}\n"
    ".bar label{color:var(--dm);font-size:11px}\n"
    ".bar select{background:var(--sf2);color:var(--tx);border:1px solid var(--bd);"
    "border-radius:var(--r);padding:3px 6px;font-size:11px;font-family:inherit}\n"
    /* log area */
    ".log-wrap{flex:1;overflow:hidden;position:relative}\n"
    "#log{position:absolute;inset:0;overflow-y:auto;overflow-x:hidden;"
    "padding:8px 16px;font-size:12.5px;line-height:1.6;"
    "white-space:pre-wrap;word-break:break-all;"
    "user-select:text;-webkit-user-select:text;"
    "scrollbar-width:thin;scrollbar-color:#333 transparent}\n"
    "#log::-webkit-scrollbar{width:6px}\n"
    "#log::-webkit-scrollbar-track{background:transparent}\n"
    "#log::-webkit-scrollbar-thumb{background:#333;border-radius:3px}\n"
    "#log::-webkit-scrollbar-thumb:hover{background:#555}\n"
    ".ln{display:block;min-height:1.6em}\n"
    ".ts{color:#585b70}\n"
    /* TX echo lines */
    ".ln.txl{opacity:.85}\n"
    ".txp{color:var(--gn);font-weight:600}\n"
    ".txd{color:var(--tl)}\n"
    /* input bar */
    ".inp{display:flex;align-items:center;padding:8px 16px;background:var(--sf);"
    "border-top:1px solid var(--bd);gap:8px;flex-shrink:0}\n"
    ".inp .pr{color:var(--ac);font-weight:600}\n"
    ".inp input{flex:1;background:var(--sf2);color:var(--tx);"
    "border:1px solid var(--bd);border-radius:var(--r);"
    "padding:6px 10px;font-size:13px;font-family:inherit;outline:none}\n"
    ".inp input:focus{border-color:var(--ac)}\n"
    ".inp .go{background:var(--ac);color:#0a0a0f;border:none;"
    "border-radius:var(--r);padding:6px 14px;font-size:12px;"
    "font-weight:600;cursor:pointer;font-family:inherit}\n"
    ".inp .go:hover{opacity:.85}\n"
    /* terminal input mirror */
    ".tinp{display:flex;align-items:center;padding:4px 16px 0;background:var(--sf);"
    "gap:8px;flex-shrink:0;font-size:11px;color:var(--dm)}\n"
    ".tinp .lb{color:var(--yl);font-weight:600;font-size:10px}\n"
    ".tinp .tv{color:var(--tx);opacity:.7}\n"
    /* toast notification */
    ".toast{position:fixed;bottom:80px;left:50%;transform:translateX(-50%);"
    "background:var(--sf2);color:var(--tx);border:1px solid var(--bd);"
    "border-radius:var(--r);padding:6px 16px;font-size:12px;"
    "opacity:0;transition:opacity .3s;pointer-events:none;z-index:10}\n"
    ".toast.vis{opacity:1}\n"
    "</style></head>\n"
    /* ---- HTML body ---- */
    "<body>\n"
    "<div class='hdr'>"
    "<div class='logo'>zyterm <span>live</span></div>"
    "<div class='dot' id='dot'></div>"
    "<div class='st' id='st'>connecting&#8230;</div>"
    "<div class='dev' id='dev'></div>"
    "<div class='stats'>"
    "<span>RX <b id='rx'>0 B</b></span>"
    "<span>TX <b id='txc'>0 B</b></span>"
    "<span>Lines <b id='lc'>0</b></span>"
    "</div></div>\n"
    "<div class='bar'>"
    "<button class='btn on' id='ab' onclick='tScr()'>&#8595; Autoscroll</button>"
    "<button class='btn' id='pb' onclick='tPause()'>&#9646;&#9646; Pause</button>"
    "<button class='btn' id='tb' onclick='tTs()'>&#9201; Timestamps</button>"
    "<div class='sep'></div>"
    "<label>Max lines</label>"
    "<select id='ml' onchange='ML=+this.value'>"
    "<option value='1000'>1,000</option>"
    "<option value='5000'>5,000</option>"
    "<option value='10000' selected>10,000</option>"
    "<option value='50000'>50,000</option>"
    "<option value='0'>Unlimited</option></select>"
    "<div class='sep'></div>"
    "<button class='btn' onclick='sav()'>Save .txt</button>"
    "<button class='btn' onclick='cop()'>Copy</button>"
    "<button class='btn' onclick='clr()'>&#10005; Clear</button>"
    "</div>\n"
    "<div class='log-wrap'><div id='log'></div></div>\n"
    "<div class='tinp' id='tinp' style='display:none'>"
    "<span class='lb'>TERMINAL</span>"
    "<span class='tv' id='tbuf'></span>"
    "</div>\n"
    "<div class='inp'>"
    "<span class='pr'>&#10095;</span>"
    "<input id='tx' type='text' placeholder='Type command and press Enter to send...' "
    "autocomplete='off' spellcheck='false'>"
    "<button class='go' onclick='snd()'>Send</button>"
    "</div>\n"
    "<div class='toast' id='toast'></div>\n"
    /* ---- JavaScript ---- */
    "<script>\n"
    "'use strict';\n"
    /* state */
    "var "
    "AS=true,PA=false,TS=false,ML=10000,RXB=0,TXB=0,LC=0,pq=[],raf=0,res='',lbuf='',lbT=0,"
    "txbuf='',txT=0;\n"
    "var hist=[],hIdx=-1;\n"
    "var L=document.getElementById('log'),"
    "dot=document.getElementById('dot'),"
    "stE=document.getElementById('st'),"
    "txI=document.getElementById('tx');\n"
    /* 256-color table */
    "var C16=['#000','#c00','#0a0','#c50','#00c','#c0c','#0cc','#ccc',"
    "'#555','#f55','#5f5','#ff5','#55f','#f5f','#5ff','#fff'];\n"
    "function c256(n){"
    "if(n<16)return C16[n];"
    "if(n<232){n-=16;var r=Math.floor(n/36)*51,g=Math.floor(n%36/6)*51,b=(n%6)*51;"
    "return'rgb('+r+','+g+','+b+')'}"
    "var v=8+(n-232)*10;return'rgb('+v+','+v+','+v+')'}\n"
    /* ANSI SGR state */
    "var fg='',bg='',bo=false,dm=false,it=false,ul=false;\n"
    "function aR(){fg='';bg='';bo=false;dm=false;it=false;ul=false}\n"
    "function aS(){"
    "var s='';"
    "if(fg)s+='color:'+fg+';';"
    "if(bg)s+='background:'+bg+';';"
    "if(bo)s+='font-weight:700;';"
    "if(dm)s+='opacity:.6;';"
    "if(it)s+='font-style:italic;';"
    "if(ul)s+='text-decoration:underline;';"
    "return s}\n"
    "function aA(p){"
    "for(var i=0;i<p.length;i++){var v=p[i];"
    "if(v===0)aR();"
    "else if(v===1){bo=true;dm=false}"
    "else if(v===2){dm=true;bo=false}"
    "else if(v===3)it=true;"
    "else if(v===4)ul=true;"
    "else if(v===22){bo=false;dm=false}"
    "else if(v===23)it=false;"
    "else if(v===24)ul=false;"
    "else if(v>=30&&v<=37)fg=C16[v-30+(bo?8:0)];"
    "else if(v===39)fg='';"
    "else if(v>=40&&v<=47)bg=C16[v-40];"
    "else if(v===49)bg='';"
    "else if(v>=90&&v<=97)fg=C16[v-82];"
    "else if(v>=100&&v<=107)bg=C16[v-92];"
    "else if(v===38&&i+1<p.length){"
    "if(p[i+1]===5&&i+2<p.length){fg=c256(p[i+2]);i+=2}"
    "else if(p[i+1]===2&&i+4<p.length){"
    "fg='rgb('+p[i+2]+','+p[i+3]+','+p[i+4]+')';i+=4}}"
    "else if(v===48&&i+1<p.length){"
    "if(p[i+1]===5&&i+2<p.length){bg=c256(p[i+2]);i+=2}"
    "else if(p[i+1]===2&&i+4<p.length){"
    "bg='rgb('+p[i+2]+','+p[i+3]+','+p[i+4]+')';i+=4}}"
    "}}\n"
    /* parse ANSI text to HTML spans */
    "function aP(t){"
    "var h='',i=0;"
    "while(i<t.length){"
    "if(t.charCodeAt(i)===0x1b&&t[i+1]==='['){"
    "i+=2;var ps=[],nm='';"
    "while(i<t.length){var ch=t[i];"
    "if(ch>='0'&&ch<='9'){nm+=ch;i++}"
    "else if(ch===';'){ps.push(nm?+nm:0);nm='';i++}"
    "else if(ch==='m'){ps.push(nm?+nm:0);i++;break}"
    "else{i++;break}}"
    "aA(ps)}"
    /* skip other ESC sequences (cursor moves, erase, etc.) */
    "else if(t.charCodeAt(i)===0x1b){"
    "i++;"
    "if(i<t.length&&t[i]==='['){i++;while(i<t.length&&t.charCodeAt(i)>=0x20&&t.charCodeAt(i)<="
    "0x3f)i++;if(i<t.length)i++}"
    "else if(i<t.length)i++}"
    "else{"
    "var r='';"
    "while(i<t.length&&t.charCodeAt(i)!==0x1b){"
    "var c=t[i];"
    "if(c==='<')r+='&lt;';"
    "else if(c==='>')r+='&gt;';"
    "else if(c==='&')r+='&amp;';"
    "else if(c!=='\\r')r+=c;i++}"
    "if(r){var s=aS();"
    "if(s)h+='<span style=\"'+s+'\">'+r+'</span>';"
    "else h+=r}}}"
    "return h}\n"
    /* HTML-escape plain text */
    "function he(t){"
    "return t.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}\n"
    /* formatting helpers */
    "function fB(n){"
    "if(n<1024)return n+' B';"
    "if(n<1048576)return(n/1024).toFixed(1)+' KB';"
    "return(n/1048576).toFixed(2)+' MB'}\n"
    "function tN(){"
    "var d=new Date();"
    "return('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2)+':'"
    "+('0'+d.getSeconds()).slice(-2)+'.'+('00'+d.getMilliseconds()).slice(-3)}\n"
    /* toast notification */
    "var toT=0;\n"
    "function toast(msg){"
    "var el=document.getElementById('toast');"
    "el.textContent=msg;el.classList.add('vis');"
    "clearTimeout(toT);toT=setTimeout(function(){el.classList.remove('vis')},2000)}\n"
    /* batch render via requestAnimationFrame */
    "function flush(){"
    "raf=0;if(!pq.length)return;\n"
    "var all=lbuf+pq.join('');pq=[];\n"
    /* split on \n — last element is the incomplete tail (may be empty) */
    "var parts=all.split('\\n');\n"
    "lbuf=parts[parts.length-1];\n" /* keep partial line for next flush */
    "var html='';\n"
    "for(var j=0;j<parts.length-1;j++){"
    "var ln=parts[j];\n"
    /* strip trailing \r from CRLF, then handle bare \r overwrites */
    "if(ln.charCodeAt(ln.length-1)===13)ln=ln.slice(0,-1);\n"
    "if(ln.indexOf('\\r')>=0){var segs=ln.split('\\r');ln=segs[segs.length-1]}\n"
    "if(!ln)continue;\n"
    "html+='<div class=\"ln\">';"
    "if(TS)html+='<span class=\"ts\">['+tN()+'] </span>';\n"
    "html+=aP(ln);"
    "html+='</div>';LC++}\n"
    "if(html)L.insertAdjacentHTML('beforeend',html);\n"
    "if(ML>0&&L.childElementCount>ML){"
    "var ex=L.childElementCount-ML;"
    "var range=document.createRange();"
    "range.setStartBefore(L.firstElementChild);"
    "range.setEndAfter(L.children[ex-1]);"
    "range.deleteContents()}\n"
    "document.getElementById('rx').textContent=fB(RXB);\n"
    "document.getElementById('txc').textContent=fB(TXB);\n"
    "document.getElementById('lc').textContent=LC.toLocaleString();\n"
    "if(AS)L.scrollTop=L.scrollHeight}\n"
    /* flush lbuf as incomplete line after idle timeout */
    "function lbFlush(){"
    "lbT=0;if(!lbuf)return;\n"
    "var ln=lbuf;lbuf='';\n"
    "if(ln.charCodeAt(ln.length-1)===13)ln=ln.slice(0,-1);\n"
    "if(ln.indexOf('\\r')>=0){var segs=ln.split('\\r');ln=segs[segs.length-1]}\n"
    "var html='<div class=\"ln\">';"
    "if(TS)html+='<span class=\"ts\">['+tN()+'] </span>';\n"
    "html+=aP(ln);html+='</div>';LC++;\n"
    "L.insertAdjacentHTML('beforeend',html);\n"
    "if(AS)L.scrollTop=L.scrollHeight}\n"
    /* add RX data with residual escape handling */
    "function addD(t){"
    "if(PA)return;"
    "t=res+t;res='';\n"
    "var le=t.lastIndexOf('\\x1b');\n"
    "if(le>=0&&le>t.length-20){"
    "var tail=t.substring(le);"
    "if(tail.length<3||(tail[1]==='['&&!/[a-zA-Z]/.test(tail.slice(2)))){"
    "res=tail;t=t.substring(0,le)}}\n"
    "if(t){pq.push(t);if(!raf)raf=requestAnimationFrame(flush);\n"
    "clearTimeout(lbT);lbT=setTimeout(lbFlush,150)}}\n"
    /* add TX echo line — buffer keystrokes, render on CR/LF or debounce */
    "function txFlush(){"
    "txT=0;if(!txbuf)return;\n"
    "var t=txbuf.replace(/[\\r\\n]/g,'');\n"
    "txbuf='';\n"
    "t=t.replace(/[\\x00-\\x08\\x0b\\x0c\\x0e-\\x1f]/g,'');\n" /* strip control chars (tab etc)
                                                                */
    "if(!t)return;\n"
    "var div=document.createElement('div');"
    "div.className='ln txl';\n"
    "var h='';"
    "if(TS)h+='<span class=\"ts\">['+tN()+'] </span>';\n"
    "h+='<span class=\"txp\">&#10095; </span>';"
    "h+='<span class=\"txd\">'+he(t)+'</span>';\n"
    "div.innerHTML=h;L.appendChild(div);LC++;\n"
    "if(AS)L.scrollTop=L.scrollHeight}\n"
    "function addTx(raw){"
    "if(PA)return;\n"
    "TXB+=raw.length;\n"
    "txbuf+=raw;\n"
    "if(/[\\r\\n]/.test(raw)){clearTimeout(txT);txFlush();return}\n"
    "clearTimeout(txT);txT=setTimeout(txFlush,150)}\n"
    /* SSE connection with auto-reconnect */
    "var es=null;\n"
    "function conn(){"
    "es=new EventSource('/stream');\n"
    "es.onopen=function(){"
    "dot.classList.remove('off');stE.textContent='connected';"
    "loadInfo()};\n"
    "es.onerror=function(){"
    "dot.classList.add('off');stE.textContent='reconnecting\\u2026';"
    "es.close();setTimeout(conn,2000)};\n"
    /* RX data (default unnamed event) */
    "es.onmessage=function(e){"
    "var r=atob(e.data);RXB+=r.length;addD(r)};\n"
    /* TX echo event */
    "es.addEventListener('tx',function(e){"
    "var r=atob(e.data);addTx(r)});\n"
    /* Terminal input sync event */
    "es.addEventListener('input',function(e){"
    "var tb=document.getElementById('tbuf'),"
    "tw=document.getElementById('tinp');\n"
    "if(!e.data){tw.style.display='none';return}\n"
    "var r=atob(e.data);"
    "tw.style.display='flex';tb.textContent=r})}\n"
    "conn();\n"
    /* Fetch device info for the header */
    "function loadInfo(){"
    "fetch('/api/state').then(function(r){return r.json()}).then(function(s){"
    "document.getElementById('dev').textContent=s.device+' @ '+s.baud+' baud';"
    "document.title='zyterm \\u2014 '+s.device"
    "}).catch(function(){})}\n"
    /* UI controls */
    "function tScr(){"
    "AS=!AS;document.getElementById('ab').classList.toggle('on',AS);"
    "if(AS)L.scrollTop=L.scrollHeight}\n"
    "function tPause(){"
    "PA=!PA;var b=document.getElementById('pb');"
    "b.classList.toggle('on',PA);"
    "b.innerHTML=PA?'&#9654; Resume':'&#9646;&#9646; Pause'}\n"
    "function tTs(){"
    "TS=!TS;document.getElementById('tb').classList.toggle('on',TS)}\n"
    "function clr(){"
    "L.innerHTML='';LC=0;RXB=0;TXB=0;lbuf='';clearTimeout(lbT);txbuf='';clearTimeout(txT);aR();"
    "document.getElementById('rx').textContent='0 B';"
    "document.getElementById('txc').textContent='0 B';"
    "document.getElementById('lc').textContent='0'}\n"
    "function cop(){"
    "navigator.clipboard.writeText(L.innerText).then(function(){"
    "toast('Copied to clipboard')})}\n"
    "function sav(){"
    "var b=new Blob([L.innerText],{type:'text/plain'});"
    "var a=document.createElement('a');"
    "a.href=URL.createObjectURL(b);"
    "a.download='zyterm_'+new Date().toISOString().replace(/[:.]/g,'-')+'.txt';"
    "a.click();URL.revokeObjectURL(a.href);"
    "toast('Saved')}\n"
    /* Send with command history */
    "function snd(){"
    "var v=txI.value;"
    "if(!v)return;"
    "hist.push(v);hIdx=-1;"
    "fetch('/tx',{method:'POST',body:v+'\\r\\n'}).catch(function(){});"
    "txI.value='';txI.focus()}\n"
    /* Keyboard: Enter to send, Up/Down for history */
    "txI.addEventListener('keydown',function(e){"
    "if(e.key==='Enter'){e.preventDefault();snd()}"
    "else if(e.key==='ArrowUp'){"
    "e.preventDefault();"
    "if(hist.length===0)return;"
    "if(hIdx<0)hIdx=hist.length;"
    "if(hIdx>0){hIdx--;txI.value=hist[hIdx]}}"
    "else if(e.key==='ArrowDown'){"
    "e.preventDefault();"
    "if(hIdx<0)return;"
    "hIdx++;"
    "if(hIdx>=hist.length){hIdx=-1;txI.value=''}"
    "else txI.value=hist[hIdx]}});\n"
    /* auto-detect manual scroll */
    "L.addEventListener('scroll',function(){"
    "if(L.scrollHeight-L.scrollTop-L.clientHeight>50){"
    "if(AS){AS=false;document.getElementById('ab').classList.remove('on')}}});\n"
    "</script></body></html>";
#pragma GCC diagnostic pop

/* ---- CORS / API helpers ---- */

/** Build the "Access-Control-Allow-*" block when CORS is enabled.
 *
 * ZT-004 (INVARIANTS §7): never advertise a wildcard origin for write methods.
 * Only GET/OPTIONS are offered cross-origin; POST (which mutates the device) is
 * intentionally omitted, and is additionally gated by request_origin_ok() +
 * request_token_ok() in classify_request(). */
static const char *cors_block(const zt_ctx *c) {
    if (!c || !c->net.http_cors) return "";
    return "Access-Control-Allow-Origin: *\r\n"
           "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
           "Access-Control-Max-Age: 86400\r\n";
}

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
static void send_text_c(zt_ctx *c, int fd, const char *status, const char *ctype,
                        const char *body, size_t n) {
    char hdr[384];
    int  h = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                       "Connection: close\r\nCache-Control: no-cache\r\n%s\r\n",
                      status, ctype, n, cors_block(c));
    (void)http_write_all(fd, hdr, snprintf_len(h, sizeof hdr));
    (void)http_write_all(fd, body, n);
}

static void handle_ws_upgrade(int fd, const char *key) {
    static const char kGUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char              concat[256];
    int               cn = snprintf(concat, sizeof concat, "%s%s", key, kGUID);
    SHA1_CTX          ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, (const unsigned char *)concat, snprintf_len(cn, sizeof concat));
    unsigned char digest[20];
    sha1_final(&ctx, digest);
    char accept[64];
    b64enc(digest, 20, accept);
    char resp[256];
    int  rn = snprintf(resp, sizeof resp,
                       "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: %s\r\n\r\n",
                       accept);
    (void)http_write_all(fd, resp, snprintf_len(rn, sizeof resp));
}

/* Stash a freshly-accepted fd into an HC_NEW slot. The actual request
 * read + classify happens in hc_pump_new() on subsequent http_tick()
 * passes — never inside a blocking loop. */
static int accept_one(zt_ctx *c, int lfd) {
    (void)c;
    int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd < 0) return -1;
    for (int i = 0; i < HC_MAX; i++) {
        if (g_conn[i].fd < 0) {
            g_conn[i].fd      = cfd;
            g_conn[i].type    = HC_NEW;
            g_conn[i].req_len = 0;
            now(&g_conn[i].accepted_at);
            return 0;
        }
    }
    /* No free slot: drop politely so the kernel TCP backlog can drain. */
    close(cfd);
    return 0;
}

/* Parse a Content-Length header (case-insensitive, line-anchored) from the
 * request header block [buf, buf+hdr_len). Returns the declared byte count,
 * 0 when the header is absent (no body), HC_REQ_CAP+1 when it exceeds what
 * our bounded buffer can hold, or -1 when malformed. Lets hc_pump_new() wait
 * for the whole POST body before dispatch and classify_request() send exactly
 * that many bytes (ZT-034). */
static long http_content_length(const char *buf, size_t hdr_len) {
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
static void classify_request(zt_ctx *c, int i) {
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
            send_text_c(c, cfd, "200 OK", "text/html; charset=utf-8", kIndex,
                        sizeof kIndex - 1);
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

/* Non-blocking header-read pump for an HC_NEW slot. Reads what's
 * available; on "\r\n\r\n" classifies and either upgrades the slot or
 * closes it. Idle slots past HC_HEADER_TIMEOUT_S are dropped — that's
 * the slowloris defence. */
static void hc_pump_new(zt_ctx *c, int i) {
    hc_t *h = &g_conn[i];
    for (;;) {
        if (h->req_len + 1 >= sizeof h->req_buf) {
            /* Headers oversized — refuse. */
            send_text_c(c, h->fd, "431 Request Header Fields Too Large", "text/plain", "", 0);
            hc_close(i);
            return;
        }
        ssize_t r = read(h->fd, h->req_buf + h->req_len, sizeof h->req_buf - 1 - h->req_len);
        if (r > 0) {
            h->req_len += (size_t)r;
            h->req_buf[h->req_len] = '\0';
            char *hdrend           = memmem(h->req_buf, h->req_len, "\r\n\r\n", 4);
            if (hdrend) {
                size_t header_len = (size_t)(hdrend - h->req_buf) + 4;
                /* ZT-034: for POST, don't dispatch until the whole declared
                 * body has arrived — otherwise a body split across TCP
                 * segments is truncated/dropped while we return 204. */
                if (h->req_len >= 5 && strncmp(h->req_buf, "POST ", 5) == 0) {
                    long clen = http_content_length(h->req_buf, header_len);
                    if (clen > (long)(sizeof h->req_buf - 1 - header_len)) {
                        /* declared body can't fit our bounded request buffer */
                        send_text_c(c, h->fd, "413 Payload Too Large", "text/plain", "", 0);
                        hc_close(i);
                        return;
                    }
                    if (clen > 0 && h->req_len - header_len < (size_t)clen)
                        continue; /* wait for the rest of the body */
                }
                classify_request(c, i);
                return;
            }
            continue;
        }
        if (r == 0) {
            hc_close(i);
            return;
        } /* peer closed */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        hc_close(i);
        return;
    }
    /* Drain timeout: don't let a stalled client tie up the slot. */
    struct timespec t;
    now(&t);
    if (ts_diff_sec(&t, &h->accepted_at) > HC_HEADER_TIMEOUT_S) {
        hc_close(i);
    }
}

void http_tick(zt_ctx *c) {
    if (!c || c->net.http_fd < 0) return;
    /* Drain new accepts (non-blocking). */
    while (accept_one(c, c->net.http_fd) == 0) {}
    /* Pump any slots still mid-request. */
    for (int i = 0; i < HC_MAX; i++) {
        if (g_conn[i].fd >= 0 && g_conn[i].type == HC_NEW) hc_pump_new(c, i);
    }
    /* ZT-043: reap established SSE/WS peers that have hung up. Otherwise a dead
     * peer's slot is freed only by a failing broadcast write, so on an idle
     * link (no RX/TX/input) 16 half-open clients exhaust HC_MAX and the bridge
     * stops accepting. Poll each stream fd non-blocking for hangup / readable
     * EOF and drop it. */
    for (int i = 0; i < HC_MAX; i++) {
        if (g_conn[i].fd < 0 || g_conn[i].type == HC_NEW) continue;
        struct pollfd pf = {.fd = g_conn[i].fd, .events = POLLIN};
        if (poll(&pf, 1, 0) <= 0) continue;
        if (pf.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            hc_close(i);
            continue;
        }
        if (pf.revents & POLLIN) {
            char    b[64];
            ssize_t r = recv(g_conn[i].fd, b, sizeof b, MSG_DONTWAIT);
            if (r == 0)
                hc_close(i); /* peer sent FIN (e.g. browser tab closed) */
            else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                hc_close(i);
            /* r > 0: unsolicited client bytes on a server→client stream — drop
             * them and keep the peer. */
        }
    }
}

/* Send one WS binary frame. Returns 0 on success, -1 if the peer's socket
 * can't take the whole frame (ZT-009): the caller must then close it, because
 * a partial frame desyncs the stream and a never-reaped dead peer exhausts the
 * 16 connection slots (ZT-017). Binary (opcode 0x2), not text: device RX is
 * arbitrary 8-bit data, and a TEXT frame carrying a non-UTF-8 byte makes every
 * conformant client fail the connection with close code 1007 (ZT-035). */
static int ws_frame_binary(int fd, const unsigned char *buf, size_t n) {
    unsigned char hdr[10];
    size_t        hl = 0;
    hdr[0]           = 0x82; /* FIN + binary */
    if (n < 126) {
        hdr[1] = (unsigned char)n;
        hl     = 2;
    } else if (n < 65536) {
        hdr[1] = 126;
        hdr[2] = (unsigned char)((n >> 8) & 0xFF);
        hdr[3] = (unsigned char)(n & 0xFF);
        hl     = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++)
            hdr[2 + i] = (unsigned char)((n >> (56 - i * 8)) & 0xFF);
        hl = 10;
    }
    if (http_stream_write(fd, hdr, hl) != 0) return -1;
    if (http_stream_write(fd, buf, n) != 0) return -1;
    return 0;
}

void http_broadcast(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (!c || !buf || n == 0) return;
    /* ZT-007: iterate the whole payload in <=4096-byte segments. The old code
     * encoded only the first 4096 bytes once, silently dropping the rest of any
     * RX burst from both the SSE and WS views. */
    for (size_t off = 0; off < n; off += 4096) {
        size_t               seglen = n - off > 4096 ? 4096 : n - off;
        const unsigned char *seg    = buf + off;
        char                 b64[8192];
        b64enc(seg, seglen, b64);
        char   ev[9000];
        int    en_raw = snprintf(ev, sizeof ev, "data: %s\n\n", b64);
        size_t en     = snprintf_len(en_raw, sizeof ev);
        for (int i = 0; i < HC_MAX; i++) {
            if (g_conn[i].fd < 0) continue;
            if (g_conn[i].type == HC_SSE) {
                ssize_t w = write(g_conn[i].fd, ev, en);
                /* EAGAIN: kernel buffer full — drop this event rather than
                 * block. Any other error, or a short write (can't queue the
                 * remainder here), means the peer is gone or stalled — close. */
                if (w < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) hc_close(i);
                } else if ((size_t)w != en)
                    hc_close(i);
            } else if (g_conn[i].type == HC_WS) {
                /* ZT-009/ZT-017: check the frame write and reap dead peers. */
                if (ws_frame_binary(g_conn[i].fd, seg, seglen) != 0) hc_close(i);
            }
        }
    }
}

void http_broadcast_tx(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (!c || !buf || n == 0 || c->net.http_fd < 0) return;
    /* ZT-007: segment the payload like http_broadcast() rather than capping the
     * TX echo at one 4096-byte chunk. */
    for (size_t off = 0; off < n; off += 4096) {
        size_t seglen = n - off > 4096 ? 4096 : n - off;
        char   b64[8192];
        b64enc(buf + off, seglen, b64);
        char   ev[9000];
        int    en_raw = snprintf(ev, sizeof ev, "event: tx\ndata: %s\n\n", b64);
        size_t en     = snprintf_len(en_raw, sizeof ev);
        for (int i = 0; i < HC_MAX; i++) {
            if (g_conn[i].fd < 0) continue;
            if (g_conn[i].type == HC_SSE) {
                ssize_t w = write(g_conn[i].fd, ev, en);
                if (w < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) hc_close(i);
                } else if ((size_t)w != en)
                    hc_close(i);
            }
        }
    }
}

void http_notify_input(zt_ctx *c) {
    if (!c || c->net.http_fd < 0) return;
    /* Send the unsent portion of the input buffer (what's visible in the bar). */
    size_t unsent = c->tui.input_len > c->tui.sent_len ? c->tui.input_len - c->tui.sent_len : 0;
    char   b64[ZT_INPUT_CAP * 2];
    if (unsent > 0)
        b64enc(c->tui.input_buf + c->tui.sent_len, unsent, b64);
    else
        b64[0] = '\0';
    char   ev[ZT_INPUT_CAP * 2 + 64];
    int    en_raw = snprintf(ev, sizeof ev, "event: input\ndata: %s\n\n", b64);
    size_t en     = snprintf_len(en_raw, sizeof ev);
    for (int i = 0; i < HC_MAX; i++) {
        if (g_conn[i].fd < 0) continue;
        if (g_conn[i].type == HC_SSE) {
            ssize_t w = write(g_conn[i].fd, ev, en);
            if (w < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) hc_close(i);
            } else if ((size_t)w != en)
                hc_close(i);
        }
    }
}
