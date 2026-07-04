/**
 * @file    http_internal.h
 * @brief   Net-module-private glue shared across the split HTTP bridge units.
 *
 * The HTTP bridge is one logical unit spread over several translation units so
 * each seam stays small and testable:
 *
 *   http.c         — connection table, socket lifecycle, accept + header pump,
 *                    http_tick(), the two bounded write helpers.
 *   http_routes.c  — request-line parse, the route table, response builders,
 *                    the WebSocket upgrade handshake.
 *   http_auth.c    — Origin/Host pinning, bearer-token gate, CORS block
 *                    (the INVARIANTS §7 trust boundary).
 *   http_sha1.c    — SHA-1 + base64 primitives and the WS accept-key.
 *   http_stream.c  — SSE/WS egress: framing, broadcast, input notify.
 *   http_asset.c   — the built-in single-page UI, a pure data blob.
 *
 * This header is private to the src/net directory — it is deliberately not one
 * of the enforced umbrella headers in include/zyterm/internal/ (INVARIANTS §8). It
 * only declares symbols the split units share with each other; the public HTTP
 * surface lives in net.h. The connection table @c g_conn is defined in http.c
 * and is the single owner of per-connection state.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_NET_HTTP_INTERNAL_H_
#define ZYTERM_NET_HTTP_INTERNAL_H_

#include "zt_ctx.h"
#include "zyterm/internal/net.h"

/* ── Connection table (owned by http.c) ─────────────────────────────────── */
#define HC_REQ_CAP 4096
#define HC_MAX     16

typedef enum { HC_NEW, HC_SSE, HC_WS } hc_type;
typedef struct {
    int     fd;
    hc_type type;
    /* HC_NEW only: accumulating request bytes until "\r\n\r\n". */
    char            req_buf[HC_REQ_CAP];
    size_t          req_len;
    struct timespec accepted_at;
} hc_t;

/** The 16-slot connection table. Defined in http.c; the router and the egress
 *  path read it directly, so it is the module's single shared mutable state. */
extern hc_t g_conn[HC_MAX];

/** Close and free connection slot @p i (no-op if already closed). */
void hc_close(int i);

/* ── Bounded write helpers (defined in http.c) ──────────────────────────── */
/** Full write for one-shot responses; waits for writability but bounds total
 *  wall time so a stalled peer can't hang the loop (ZT-044, INVARIANTS §3). */
int http_write_all(int fd, const void *buf, size_t n);
/** Non-blocking "write every byte or fail" for streaming frames — a partial
 *  write corrupts SSE/WS framing, so the caller drops the peer. */
int http_stream_write(int fd, const void *buf, size_t n);

/* ── Router (defined in http_routes.c) ──────────────────────────────────── */
/** Dispatch a fully-buffered request on slot @p i: promote to HC_SSE/HC_WS or
 *  answer and close. */
void classify_request(zt_ctx *c, int i);
/** Declared Content-Length in the header block [buf, buf+hdr_len): the byte
 *  count, 0 when absent, HC_REQ_CAP+1 when too large to buffer, -1 malformed.
 *  Shared with http.c's header pump so it waits for the whole POST body. */
long http_content_length(const char *buf, size_t hdr_len);
/** Small text response with Content-Length/Connection: close and optional CORS.
 *  Used by the router and by http.c's header pump for 4xx errors. */
void send_text_c(zt_ctx *c, int fd, const char *status, const char *ctype, const char *body,
                 size_t n);

/* ── Trust boundary (defined in http_auth.c, INVARIANTS §7) ─────────────── */
/** Reject cross-origin / DNS-rebound requests (Host + Origin pinned loopback).*/
bool request_origin_ok(const char *req);
/** When --http-token is set, require a matching "Authorization: Bearer". */
bool request_token_ok(const zt_ctx *c, const char *req);
/** CORS header block when --http-cors is set, else "". */
const char *cors_block(const zt_ctx *c);

/* ── Crypto primitives (defined in http_sha1.c) ─────────────────────────── */
/** Base64-encode @p n bytes of @p in into NUL-terminated @p out. */
void zt_base64(const unsigned char *in, size_t n, char *out);
/** Compute the RFC 6455 Sec-WebSocket-Accept value for client @p key into
 *  @p out (needs >= 32 bytes). */
void zt_ws_accept_key(const char *key, char *out, size_t cap);

/* ── Built-in UI asset (defined in http_asset.c) ────────────────────────── */
extern const char   zt_http_index[];   /**< NUL-terminated built-in SPA. */
extern const size_t zt_http_index_len; /**< strlen(zt_http_index). */

/* Clamp snprintf's return to bytes actually present in @p cap: snprintf
 * returns the would-be length on truncation (>= cap), which used directly as a
 * write count reads past the stack buffer and leaks bytes to the network. */
static inline size_t snprintf_len(int n, size_t cap) {
    if (n < 0 || cap == 0) return 0;
    return ((size_t)n >= cap) ? (cap - 1) : (size_t)n;
}

#endif /* ZYTERM_NET_HTTP_INTERNAL_H_ */
