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
| 1.2.0   | ✅ (current) |
| < 1.2.0 | ❌ |

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

### 1. The loopback HTTP bridge is unauthenticated — `--http`

When `--http <port>` is set, zyterm serves a control/observability bridge. It has **no
authentication of any kind**, and the CORS block is `Access-Control-Allow-Origin: *` on every
route, including state-changing ones (`src/net/http.c:604`).

- **`POST /tx` / `POST /api/send` writes the serial line** (`src/net/http.c:907`). The body is sent
  verbatim to the device with no auth and no Origin/Host check. As a CORS "simple request" it is
  reachable cross-site, and via DNS rebinding a hostile page can target the loopback port — so any
  web page the operator visits can **execute commands on the attached device**. (**ZT-004**,
  [detail](docs/tracking/issues/ZT-004-unauth-http-tx-csrf.md).)
  - *Reduce exposure:* do not pass `--http` on a shared or untrusted machine, or while browsing the
    web. If you must, treat the port as fully trusted by every local process and browser tab. There
    is no token or Origin gate to rely on yet.

- **The WebSocket upgrade does no Origin check** (`src/net/http.c:861`). `GET /ws` upgrades without
  validating `Origin`, so any site can open the socket cross-origin and **read the live RX stream**
  — everything the device prints. (**ZT-013**.)
  - *Reduce exposure:* same as above — keep `--http` off except on a single-user machine you fully
    control.

### 2. Local IPC sockets have default permissions — `--detach` / `--metrics`

- **Detach session socket `/tmp/zyterm.<name>.sock`** (`src/net/session.c:48`, path built at
  `session.c:37`). It is created under the process's default umask with no `SO_PEERCRED` peer-cred
  check, so any local user who can reach the path can connect and **inject bytes onto the serial
  line**. (**ZT-012**.)

- **Metrics UNIX socket `--metrics <path>`** (`src/net/metrics.c:39`). Created without restrictive
  permissions, so under a loose umask another local user can connect and read exported metrics —
  an **information leak**. (**ZT-028**.)
  - *Reduce exposure for both:* run zyterm under a tight umask (`umask 077`), and place the metrics
    socket under a directory only you can read — ideally `$XDG_RUNTIME_DIR`, which is already
    mode-0700 and per-user. Avoid world-readable locations like `/tmp` for the socket path. The
    planned fix is an `$XDG_RUNTIME_DIR` 0700 dir + 0600 socket + peer-cred check.

### 3. A hostile device can inject terminal escape sequences via RX — always on

Device output is written to the operator's terminal **verbatim** — only `\r` is stripped, with no
ESC/OSC filtering (`src/render/render.c:93`; the RX render path strips `\r` and `\n` but passes
everything else through, `render.c` ~248–288). A malicious or compromised device can therefore emit
escape sequences that the operator's terminal acts on: **OSC 52 clipboard hijack** (writing the
system clipboard), **window-title injection**, cursor and screen manipulation, and similar tricks.
(**ZT-003**, [detail](docs/tracking/issues/ZT-003-device-rx-escape-injection.md).)

- *Reduce exposure:* only connect to devices you trust. Do not pipe untrusted device output to
  another terminal or scrollback you later paste from. The planned fix is to default-deny dangerous
  escapes and gate raw passthrough explicitly.

---

## What is *not* a vulnerability here

- Connecting to a serial device you own and trust. zyterm is a serial terminal; sending and
  receiving arbitrary bytes is its purpose.
- A local user with your privileges reading your own files or sockets. That is inside the trust
  boundary by design.
- The HTTP bridge being reachable when **you** enabled `--http`. The issue is the *lack of auth*,
  not the feature existing; the mitigation today is not to enable it where it can be reached.

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
