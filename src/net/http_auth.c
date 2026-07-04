/**
 * @file    http_auth.c
 * @brief   HTTP bridge trust boundary — Origin/Host pinning, bearer token, CORS.
 *
 * The bridge binds loopback only, but the operator's browser is itself on
 * loopback, so a cross-site page or a DNS-rebound host can still reach it.
 * Defences (INVARIANTS §7): pin Host to a loopback literal (rebind defence);
 * pin Origin — which browsers always send cross-origin and on POST — to
 * loopback (CSRF defence); and, when --http-token is set, require a matching
 * bearer token on state-changing routes. The router (http_routes.c) calls
 * request_origin_ok()/request_token_ok() before any streaming or write route,
 * and cors_block() when composing responses.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "http_internal.h"

#include <string.h>
#include <strings.h> /* strncasecmp */

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
bool request_origin_ok(const char *req) {
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
bool request_token_ok(const zt_ctx *c, const char *req) {
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

/** Build the "Access-Control-Allow-*" block when CORS is enabled.
 *
 * ZT-004 (INVARIANTS §7): never advertise a wildcard origin for write methods.
 * Only GET/OPTIONS are offered cross-origin; POST (which mutates the device) is
 * intentionally omitted, and is additionally gated by request_origin_ok() +
 * request_token_ok() in classify_request(). */
const char *cors_block(const zt_ctx *c) {
    if (!c || !c->net.http_cors) return "";
    return "Access-Control-Allow-Origin: *\r\n"
           "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
           "Access-Control-Max-Age: 86400\r\n";
}
