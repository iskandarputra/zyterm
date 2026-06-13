# Security Policy

zyterm is a single-binary Linux serial terminal. This document states its threat model, the
surfaces an attacker can reach, how to reduce exposure, and how to report a vulnerability
privately.

The known instances below are tracked as `security` rows in
[docs/tracking/KNOWN_ISSUES.md](docs/tracking/KNOWN_ISSUES.md), with concrete fix directions in
[docs/plans/RELIABILITY_HARDENING.md](docs/plans/RELIABILITY_HARDENING.md). The trust-boundary
rule they all derive from is [INVARIANTS §7](docs/invariants/INVARIANTS.md) (network bridge &
local-IPC trust boundary).

---

## Supported version

| Version | Supported |
|---------|-----------|
| 1.4.0   | ✅ (current) |
| < 1.4.0 | ❌ |

Security fixes land on the current release line only. `ZT_VERSION` lives in `src/zt_ctx.h`.

---

## Threat model

**Trust boundary = the local machine.** zyterm trusts the user who runs it and other processes
running as that user. It does **not** defend against a malicious local user already on the box, and
it is not designed to be exposed to a network. The interesting surfaces are the ones that *widen*
that boundary without the operator realizing it: optional features that open a loopback HTTP port
or a UNIX socket, and the serial device itself, whose bytes flow back into the operator's terminal.

Two attacker classes matter:

1. **A web page in the operator's browser** — relevant whenever `--http` is enabled, because the
   bridge binds a loopback port that any local origin (including a browser visiting a hostile site)
   can reach.
2. **A hostile or compromised serial device** — relevant always, because device output is rendered
   to the terminal.

---

## Surfaces

### 1. The loopback HTTP bridge — `--http` (origin-pinned; optional token)

When `--http <port>` is set, zyterm serves a control/observability bridge on loopback. As of the
2026-06 hardening it is **origin-pinned**, and its write routes can additionally require a bearer
token. The CORS block advertises only read-only `GET, OPTIONS` (`src/net/http.c`).

- **`POST /tx` / `POST /api/send` writes the serial line.** Both routes now pin `Host`/`Origin` to a
  loopback literal — a cross-site simple request (foreign `Origin`) or a DNS-rebound host (non-loopback
  `Host`) is rejected with `403` before any byte reaches the device. When `--http-token <tok>` is set,
  the routes also require `Authorization: Bearer <tok>` (`401` otherwise). (**ZT-004 — fixed**,
  [detail](docs/tracking/issues/ZT-004-unauth-http-tx-csrf.md).)
  - *Defence in depth:* set `--http-token` if you tunnel/proxy the port beyond loopback. The built-in
    web UI is anonymous-but-same-origin, so it keeps working without a token.

- **The WebSocket upgrade and the SSE `/stream` validate `Origin`.** `GET /ws` and `GET /stream`
  reject a foreign origin, so a cross-origin site can no longer open the socket and read the live RX
  stream. (**ZT-013 — fixed**.)

### 2. Local IPC sockets — `--detach` / `--metrics` (0600 + peer-cred)

- **Detach session socket.** Now created under `$XDG_RUNTIME_DIR` (per-user, mode-0700; `/tmp` only
  if that is unset), mode `0600` via a scoped `umask`, and `session_tick()` rejects any attacher
  whose uid isn't yours via `SO_PEERCRED`. (**ZT-012 — fixed**, `src/net/session.c`.)

- **Metrics UNIX socket `--metrics <path>`.** Created `0600` via a scoped `umask`, and `metrics_tick`
  rejects non-self peers via `SO_PEERCRED`. (**ZT-028 — fixed**, `src/net/metrics.c`.)

### 3. A hostile device injecting terminal escapes via RX — default-denied

Device RX is no longer echoed verbatim. By default the render path runs a **bounded SGR-only
filter** (ADR-0009): well-formed `CSI … m` colour sequences pass, but ESC and every other
control/escape — **OSC 52 clipboard hijack**, **window-title injection**, cursor/erase/alt-screen
spoofs — are rewritten to inert `cat -v` caret notation (`^[`, `^G`, …) before reaching the
terminal. SGR is the one escape class that cannot drive the terminal, and the parser whitelists
only digit/`;`/`:` parameters (so `CSI ? 1 m` and friends are rejected) with a fixed, overflow-safe
buffer. `\t` and UTF-8 pass through. `--no-sgr` selects strict deny-all (colour neutralized too).
Full raw passthrough (`Ctrl+A G`) remains an explicit, off-by-default opt-in. (**ZT-003 / ZT-029 —
fixed**, `src/render/render.c`, `src/proto/sgr_passthrough.c`,
[detail](docs/tracking/issues/ZT-003-device-rx-escape-injection.md).)

- *Residual risk:* enabling full raw passthrough puts you back to trusting all of the device's
  escapes — only do so for devices you trust. 8-bit C1 controls are not filtered (that range
  carries UTF-8); see ADR-0009.

---

## What is *not* a vulnerability here

- Connecting to a serial device you own and trust. zyterm is a serial terminal; sending and
  receiving arbitrary bytes is its purpose.
- A local user with your privileges reading your own files or sockets. That is inside the trust
  boundary by design.
- The HTTP bridge being reachable when **you** enabled `--http`. It is origin-pinned and can be
  token-gated (`--http-token`); exposing it beyond loopback is your call to make deliberately.

---

## Reporting a vulnerability

Report security issues **privately**. Do **not** open a public GitHub issue or pull request for an
attack vector (HTTP bridge, detach/metrics sockets, RX escape injection, or anything else that
crosses the trust boundary).

- Email the maintainer privately, or use GitHub's **private vulnerability reporting** ("Report a
  vulnerability" under the repository's Security tab).
- Include: zyterm version (`./zyterm --version`), OS/kernel (`uname -a`), the exact CLI used, a
  minimal reproducer, and the impact you observed.

Please give a reasonable window to fix and release before any public disclosure. Confirmed surfaces
are recorded in [docs/tracking/KNOWN_ISSUES.md](docs/tracking/KNOWN_ISSUES.md); the hardening order
and design of the fixes live in
[docs/plans/RELIABILITY_HARDENING.md](docs/plans/RELIABILITY_HARDENING.md).
