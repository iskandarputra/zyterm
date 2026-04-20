/**
 * @file transport.c
 * @brief Network transports: tcp://, telnet://, rfc2217://
 *
 * Lets zyterm talk to a remote serial server (ser2net, esp-link, USB-IP,
 * SSH-tunneled tty) the same way it talks to a local /dev/ttyUSB0:
 *
 *   zyterm tcp://lab-pi.local:23000
 *   zyterm telnet://192.168.1.50:2000
 *   zyterm --baud 115200 rfc2217://server:2217   (NYI — see below)
 *
 * The trick is that zyterm's runtime already operates on @c c->serial.fd
 * via poll(2) / read(2) / write(2), all of which work on connected
 * sockets unchanged. So we only need to:
 *
 *   1. Detect URL-shaped device strings up front;
 *   2. Open a TCP socket (with sane keep-alive defaults) instead of open(2);
 *   3. Tell the rest of the code "this is a socket, skip TTY ioctls"
 *      via @c c->serial.is_socket; and
 *   4. For telnet:// only — escape outgoing 0xFF and strip incoming
 *      Telnet IAC sequences so a real telnet server doesn't get
 *      confused.
 *
 * RFC 2217 (Telnet COM-PORT-OPTION) carries baud/data-bits/parity over
 * the wire; that's a larger negotiation surface so it's deliberately
 * stubbed here with an actionable error. The hook points are in place
 * for a future pass to wire it.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── URL parsing ──────────────────────────────────────────────────────── */

bool transport_is_url(const char *device) {
    if (!device) return false;
    return strncmp(device, "tcp://",      6) == 0
        || strncmp(device, "telnet://",   9) == 0
        || strncmp(device, "rfc2217://", 10) == 0;
}

/** Split a URL into scheme / host / port. Caller-owned char buffers. */
static int parse_url(const char *url, char *scheme, size_t scs,
                     char *host, size_t hcs, char *port, size_t pcs) {
    const char *colon = strstr(url, "://");
    if (!colon) return -1;
    size_t schl = (size_t) (colon - url);
    if (schl + 1 > scs) return -1;
    memcpy(scheme, url, schl);
    scheme[schl] = '\0';

    const char *rest = colon + 3;
    /* Bracketed IPv6 literal: [fe80::1]:23 */
    if (*rest == '[') {
        const char *end = strchr(rest, ']');
        if (!end || end[1] != ':') return -1;
        size_t hl = (size_t) (end - rest - 1);
        if (hl + 1 > hcs) return -1;
        memcpy(host, rest + 1, hl);
        host[hl] = '\0';
        snprintf(port, pcs, "%s", end + 2);
        return 0;
    }
    const char *p = strrchr(rest, ':');
    if (!p) return -1;
    size_t hl = (size_t) (p - rest);
    if (hl == 0 || hl + 1 > hcs) return -1;
    memcpy(host, rest, hl);
    host[hl] = '\0';
    snprintf(port, pcs, "%s", p + 1);
    return 0;
}

/* ── socket open ──────────────────────────────────────────────────────── */

int transport_open(const char *url, bool *out_telnet) {
    char scheme[16], host[256], port[16];
    if (parse_url(url, scheme, sizeof scheme, host, sizeof host, port, sizeof port) < 0)
        zt_die("zyterm: bad transport URL: %s (expected scheme://host:port)", url);

    if (!strcmp(scheme, "rfc2217")) {
        zt_die("zyterm: rfc2217:// is not yet implemented; "
               "for now use 'ser2net' in raw mode and connect with tcp://%s:%s",
               host, port);
    }

    bool telnet = (strcmp(scheme, "telnet") == 0);
    if (out_telnet) *out_telnet = telnet;

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0)
        zt_die("zyterm: %s://%s:%s: %s", scheme, host, port, gai_strerror(rc));

    int fd = -1;
    int last_errno = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) { last_errno = errno; continue; }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        last_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        errno = last_errno ? last_errno : ECONNREFUSED;
        return -1;
    }

    /* Sane defaults for an interactive serial bridge:
     *   - TCP_NODELAY: characters go out as the user types them
     *   - SO_KEEPALIVE: detect dead peers (lab-pi yanked from the wall)
     *   - O_NONBLOCK: matches what zyterm expects from setup_serial(). */
    int yes = 1;
    (void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,  &yes, sizeof yes);
    (void) setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE, &yes, sizeof yes);

    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    return fd;
}

/* ── Telnet IAC handling ──────────────────────────────────────────────── */

/* Parser states for telnet_rx_filter. */
enum {
    TS_DATA = 0,    /* normal data flow                                  */
    TS_IAC,         /* saw 0xFF, awaiting next byte                      */
    TS_OPT,         /* saw IAC + WILL/WONT/DO/DONT, awaiting option byte */
    TS_SB,          /* inside sub-negotiation, scanning for IAC SE       */
    TS_SB_IAC,      /* inside SB and just saw IAC                        */
};

/* Telnet command bytes we care about. */
#define IAC  0xFF
#define SB   0xFA
#define SE   0xF0
#define WILL 0xFB
#define WONT 0xFC
#define DO   0xFD
#define DONT 0xFE

size_t telnet_rx_filter(uint8_t *state, unsigned char *buf, size_t n) {
    uint8_t st = state ? *state : TS_DATA;
    size_t  w  = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = buf[i];
        switch (st) {
        case TS_DATA:
            if (b == IAC) { st = TS_IAC; }
            else          { buf[w++] = b; }
            break;
        case TS_IAC:
            if      (b == IAC)                            { buf[w++] = IAC; st = TS_DATA; }
            else if (b == WILL || b == WONT || b == DO || b == DONT) { st = TS_OPT; }
            else if (b == SB)                             { st = TS_SB; }
            else                                          { st = TS_DATA; /* drop 2-byte cmd */ }
            break;
        case TS_OPT:
            /* Drop the option byte. We are deliberately silent — most
             * ser2net configurations don't actually negotiate, and a
             * passive client that ignores everything reaches the
             * ser2net "data" state by default. */
            st = TS_DATA;
            break;
        case TS_SB:
            if (b == IAC) st = TS_SB_IAC;
            /* else: drop sub-negotiation payload. */
            break;
        case TS_SB_IAC:
            if (b == SE)  { st = TS_DATA; }
            else if (b == IAC) { st = TS_SB; /* escaped 0xFF inside SB; drop */ }
            else          { st = TS_SB; }
            break;
        }
    }
    if (state) *state = st;
    return w;
}

size_t telnet_tx_escape(const unsigned char *in, size_t n,
                        unsigned char *out, size_t out_cap) {
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (w >= out_cap) break;
        out[w++] = in[i];
        if (in[i] == IAC) {
            if (w >= out_cap) { w--; break; } /* don't split the pair */
            out[w++] = IAC;
        }
    }
    return w;
}
