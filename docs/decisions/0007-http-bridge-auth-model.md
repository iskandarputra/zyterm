# ADR-0007: HTTP bridge auth model — origin-pinned by default, optional bearer token

- **Status:** accepted
- **Date:** 2026-06-13

## Context

The optional `--http` bridge serves a control/observability surface on a loopback TCP port:
a live RX view, an SSE stream (`/stream`), a WebSocket stream (`/ws`), read-only JSON
(`/api/state`, `/metrics`), and — critically — **write** routes (`POST /tx`, `POST /api/send`)
that put bytes on the serial line.

Binding to `INADDR_LOOPBACK` is necessary but not sufficient. The operator's own web browser
runs on loopback, so any web page they visit can issue requests to `127.0.0.1:<port>`. The
2026-06-03 audit found this was exploitable two ways (ZT-004): a cross-site `fetch` to
`POST /tx` is a CORS *simple request* (no preflight, the server acts regardless of whether the
response is readable), and DNS rebinding lets an attacker page present itself as same-origin to
the loopback port. Either drives **arbitrary commands onto the attached device**. The `/ws` and
`/stream` reads had the mirror problem (ZT-013): any origin could open them and read device output.

The design question was *how* to authenticate. Options considered:

1. **Always require a token.** Strongest, but breaks the built-in web UI out of the box — `zyterm
   --http 8080` then clicking *Send* would 401, because the served page is a static asset that
   carries no secret. A bad default for the common single-user-on-their-own-box case.
2. **Localhost-only, no auth.** Already the status quo, and already broken — loopback is reachable
   cross-site and via rebinding.
3. **Origin/Host pinning, always on; token, optional.** Browsers send an `Origin` header on every
   cross-origin request and on POST, and the `Host` header reflects the name the client connected
   to. Pinning both to a loopback literal rejects cross-site requests and rebound hosts *without a
   secret*, so the same-origin built-in UI keeps working, while a token can layer on top for
   operators who deliberately expose the port.

## Decision

**The bridge is origin-pinned by default; a bearer token is opt-in via `--http-token`.**

- **Origin/Host pinning (always on)** on the state-changing routes (`POST /tx`, `/api/send`) and
  the streaming reads (`/ws`, `/stream`): `Host` must be a loopback literal
  (`127.0.0.1` / `localhost` / `::1`), and `Origin`, when present, must resolve to the same — else
  `403`. This is the CSRF + DNS-rebind defence and needs no configuration. Helpers
  `request_origin_ok()` / `host_is_loopback()` / `origin_is_loopback()` in `src/net/http.c`.
- **Bearer token (opt-in)**: when `--http-token <tok>` is set, the write routes additionally
  require `Authorization: Bearer <tok>` (`401` otherwise) via `request_token_ok()`. With no token
  configured the bridge is anonymous-but-origin-pinned, so the built-in same-origin UI works
  unchanged.
- **CORS is read-only**: `cors_block` advertises only `GET, OPTIONS`; `POST` is never granted to
  the `*` origin.

## Consequences

- The common case (`--http` on your own machine, using the built-in UI) works with zero config and
  is no longer drivable by a random web page. Exposing the port beyond loopback (an SSH tunnel, a
  reverse proxy) is a deliberate act for which `--http-token` provides authentication.
- This realizes the network-bridge half of **[INVARIANTS §7](../invariants/INVARIANTS.md)** and
  closes [ZT-004](../tracking/issues/ZT-004-unauth-http-tx-csrf.md) and
  [ZT-013](../tracking/KNOWN_ISSUES.md). The local-IPC sockets (detach, metrics) are the *other*
  half of that boundary; they are hardened with `0600` perms + `SO_PEERCRED` (ZT-012, ZT-028)
  rather than a token, because they are not browser-reachable.
- A token passed on the command line is visible in `ps`; operators who care can keep the port on
  loopback and tunnel it, relying on origin-pinning alone. The flag is the discoverable mechanism,
  not the only line of defence.
- Limitation: the token is a single shared secret with no rotation or per-client identity. That is
  proportionate to a single-operator local tool; anything richer (per-client tokens, TLS) would be
  a larger design that this ADR does not pre-commit to.
