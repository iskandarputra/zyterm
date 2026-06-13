# zyterm Architecture

This is the **map** of zyterm: the modules, how they depend on each other, the runtime
model, and the build pipeline. It deliberately stays at altitude — the byte-level mechanics
(framing decoders, the SPSC ring protocol, the render pipeline, signal-safety proofs) live in
[INTERNALS.md](./INTERNALS.md). The don't-regress rules live in
[../invariants/INVARIANTS.md](../invariants/INVARIANTS.md). Known defects live in
[../tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md).

zyterm is a single Linux binary (~12.5K LOC of C, version `1.2.0` — `ZT_VERSION` in
`src/zt_ctx.h:70`). It is plain C11 + POSIX, links only libc (`-lpthread -ldl`, both
absorbed into glibc ≥ 2.34), and has no build-time graphical dependency — the X11 clipboard
is reached at runtime via `dlopen("libxcb.so.1")`.

---

## 1. Module layout

All code lives under `src/` as **9 semantic modules plus `main.c`**. Each module has exactly
one public internal header at `include/zyterm/internal/<module>.h` declaring its API; the
shared state struct lives in `src/zt_ctx.h`.

| Module     | Header              | One-line responsibility |
|------------|---------------------|-------------------------|
| `core/`    | `internal/core.h`   | Cross-cutting helpers: `zt_warn`/`zt_die`/`zt_trace`, the shared stdout output buffer (`ob_*`), signal & terminal management, monotonic time, CRC algorithms, the embed `siglongjmp` hook. |
| `serial/`  | `internal/serial.h` | Port open (`termios2`/`BOTHER`, macOS `IOSSIOSPEED`), flow control, reconnect, autobaud probe, USB port discovery (`--port-glob` / `--match-vid-pid`), `tcp://`+`telnet://` transport, kernel UART counters — and `fastio.c` (an **unwired** epoll/splice path, see §6). |
| `log/`     | `internal/log.h`    | Persistent log file + rotation, NDJSON emit (`log_json`), asciinema cast recording (`--rec`), the scrollback ring, and mouse-driven text selection. |
| `proto/`   | `internal/proto.h`  | Wire/escape protocols: frame decoders + CRC (`framing`), X/Y/ZMODEM transfer, F-key macros, OSC 52 clipboard + `osc8_rewrite` (**dead**, §6), native X11 clipboard, SGR pass-through, KGDB raw pass-through, line-ending translation. |
| `render/`  | `internal/render.h` | The RX byte-stream → screen pipeline (`render_rx`, `rx_ingest`, colorizing, hex view) and the throughput sparkline. |
| `tui/`     | `internal/tui.h`    | Terminal UI: HUD, input bar, dialogs, search/rename overlays, settings menu, the less-style pager, and the fuzzy finder. |
| `net/`     | `internal/net.h`    | Network-facing services: the HTTP/SSE/WS bridge + Prometheus `--metrics` exporter, and the detach/attach session multiplexer (local UNIX sockets). |
| `ext/`     | `internal/ext.h`    | Optional extensions: bookmarks, `--diff`, the `--filter` subprocess, log-level mute, profiles + inotify hot-reload, event hooks, the reconnect popup loop, and `multi.c` (a **stub**, §6). |
| `loop/`    | `internal/loop.h`   | Event-loop primitives: keyboard input parsing (`input.c`), the TX send pipeline (`send.c`), the optional `--threaded` reader (`rx_thread.c`), and the top-level run modes (`runtime.c`). |
| `main.c`   | (no header)         | CLI parsing (`getopt_long`), context init, and the `zyterm_main` entry point. The only translation unit allowed to call into `loop/`. |

Run `make modules` for a live per-module file/LOC summary.

---

## 2. The dependency chain (enforced by headers)

Modules form a strict linear chain. Each lower layer is usable by every layer above it and
**must not** reference anything above it. The chain is not just convention — it is mechanically
enforced because each module header `#include`s exactly the one beneath it:

```
core ← serial ← log ← proto ← render ← tui ← net ← ext ← loop
```

| Header                 | Includes (verified) | Line |
|------------------------|---------------------|------|
| `internal/core.h`      | `zt_ctx.h`          | `core.h:16` |
| `internal/serial.h`    | `core.h`            | `serial.h:15` |
| `internal/log.h`       | `serial.h`          | `log.h:14` |
| `internal/proto.h`     | `log.h`             | `proto.h:15` |
| `internal/render.h`    | `proto.h`           | `render.h:14` |
| `internal/tui.h`       | `render.h`          | `tui.h:15` |
| `internal/net.h`       | `tui.h`             | `net.h:15` |
| `internal/ext.h`       | `net.h`             | `ext.h:15` |
| `internal/loop.h`      | `ext.h`             | `loop.h:16` |

Consequences:

- A translation unit includes only the highest module header it needs; everything beneath
  comes transitively. `src/zt_internal.h` is a compatibility umbrella that `#include`s only
  `internal/loop.h`, which transitively pulls every module.
- `loop/` is the topmost internal module. **`main.c` is the only TU allowed above `loop/`**
  (`loop.h:6-7`).
- There is no separate dependency-injection wiring: the linear chain plus the single shared
  `zt_ctx` (§3) is the whole structure. Adding a back-edge (a lower module calling up) is an
  invariant violation — see [INVARIANTS §8](../invariants/INVARIANTS.md) and §7 below.

---

## 3. State model: one `zt_ctx`, mostly one thread

All per-process runtime state lives in a single `zt_ctx` struct (`src/zt_ctx.h:169`), passed by
pointer to nearly every function. It is grouped into feature sub-structs (`serial`, `tui`, `log`,
`net`, `proto`, `ext`, `core`), each annotated with a feature *tier* (0–4) marking how core the
field is. Centralizing state in one header avoids circular includes and lets the compiler inline
member access.

**Thread ownership:**

- **Main thread** owns and mutates virtually all of `zt_ctx`. The default runtime is
  single-threaded: one `poll(2)` loop drives serial I/O, keyboard, rendering, and every
  ancillary subsystem.
- The **optional `--threaded` reader** (`src/loop/rx_thread.c`, enabled by setting
  `serial.spsc_enabled`) starts a worker that owns only a private `dup(2)` of the serial fd
  and a power-of-two **SPSC ring** of `ZT_SPSC_CAP` = 1 MiB (`zt_ctx.h:69`). Producer/consumer
  hand-off uses release/acquire atomics (`rx_thread.c:14`, `:70-88`); the worker wakes the main
  loop through a non-blocking pipe. The worker touches *nothing else* in `zt_ctx`. This is the
  only concurrency in the program — see [design/THREADING_AND_RECONNECT.md](../design/THREADING_AND_RECONNECT.md).
- **Signal handlers** touch only the two `volatile sig_atomic_t` globals `zt_g_quit` and
  `zt_g_winch` (`zt_ctx.h:140-141`). They never read or write `zt_ctx`.

**Signals** (installed in `core/core.c`, `install_signals`):

- `SIGINT` / `SIGTERM` / `SIGHUP` / `SIGQUIT` → set `zt_g_quit`.
- `SIGWINCH` → set `zt_g_winch`.
- `SIGPIPE` → **ignored** (write errors are handled explicitly at each call site).
- `SIGSEGV` / `SIGABRT` / `SIGBUS` / `SIGFPE` → an async-signal-safe crash handler
  (`SA_RESETHAND`, one-shot) that restores the terminal, then — for **embedded `SIGABRT`/`SIGFPE`
  only** — `siglongjmp`s back to the host (`core.c:312`); otherwise it re-raises for a core
  dump (`core.c:314`).
- There is **no `SIGCHLD` handler**. Child reaping is done synchronously in the main loop via
  `hooks_reap()` and `filter_stop()`.

See [INVARIANTS §2](../invariants/INVARIANTS.md) (async-signal-safety) and
[INVARIANTS §4](../invariants/INVARIANTS.md) (reader thread & fd lifecycle).

---

## 4. Runtime model: the poll loop

The interactive run mode is `run_interactive()` (`src/loop/runtime.c`). One `poll(2)` call drives
everything; there are three run modes total, all in `loop/runtime.c`:

| Mode             | Entry             | Triggered by |
|------------------|-------------------|--------------|
| Interactive      | `run_interactive` | normal startup |
| Dump             | `run_dump`        | `--dump <sec>` |
| Replay           | `run_replay`      | `--replay <file>` |

The interactive loop polls a set of **at most three fds** (`runtime.c:103`, `pfds[3]`):

1. `serial.fd` — the device. In default mode polled for `POLLIN`; in `--threaded` mode polled
   with `events=0` so the kernel still reports `POLLHUP`/`POLLERR` for hot-unplug without racing
   the worker on `POLLIN` (`runtime.c:117-120`).
2. `STDIN_FILENO` — keyboard.
3. The SPSC **wake-pipe** — only present in `--threaded` mode (`runtime.c:125-130`).

The poll timeout is `ZT_HUD_REFRESH_MS` = 500 ms (`zt_ctx.h:60`), which paces the HUD repaint.
After the poll returns, the loop:

- drains serial RX (default: `read()` loop at `runtime.c:170`; threaded: `rx_thread_drain()` at
  `:192`), feeding bytes to `log_write()` + `rx_ingest()`;
- handles `POLLHUP`/`POLLERR` by entering `run_reconnect_loop()` when reconnect is on
  (`runtime.c:142-168`);
- processes keyboard input via `handle_stdin_chunk()` (`runtime.c:205`);
- paints at most one HUD+input frame per ~16 ms (a ~60 fps cap; RX bytes still hit
  log/scrollback immediately, `runtime.c:241-261`);
- services every ancillary subsystem with a cheap `*_tick()`/drain call — `filter_drain`,
  `http_tick`, `metrics_tick`, `session_tick`, `profile_watch_tick`, `hooks_reap`,
  `tty_stats_poll` — then `ob_flush()` (`runtime.c:262-270`).

Note that HTTP, metrics, session, filter, and profile-watch fds are **not** in the central
pollset; each subsystem owns its own non-blocking sockets and is serviced by its per-iteration
tick. This keeps the central loop tiny and makes "never block in the loop"
([INVARIANTS §3](../invariants/INVARIANTS.md)) tractable.

---

## 5. Hot paths

Two paths dominate runtime cost and define the latency/throughput behaviour:

- **RX → screen:** `read()`/`rx_thread_drain()` → `log_write()` → `rx_ingest()` →
  `framing_feed()` → `render_rx()` (`render/render.c`) → scrollback push + `ob_write()` →
  `ob_flush()`. At high baud this runs many times per HUD tick; the per-frame paint cap keeps
  the downstream terminal repaint from dominating. Each RX line also fans out to `watch_match`,
  `hooks_on_line`, the `--filter` pipe, and `http_broadcast`/cast recording where enabled.
- **Keyboard → TX:** `handle_stdin_chunk()` → input editing / command-mode dispatch
  (`loop/input.c`) → `trickle_send`/`direct_send` (`loop/send.c`) → line-ending translation +
  framing/CRC encode (`proto/`) → `write()` to the device.

The deep mechanics of both live in [INTERNALS.md](./INTERNALS.md).

---

## 6. Not wired / deferred (honest status)

Some code is present but **not reachable** in 1.2.0. Do not treat these as working features.

- **`serial/fastio.c` (epoll/splice fast path) — entirely unwired.** No call site exists; the
  runtime uses `poll(2)`. The `--epoll` flag was removed in 1.2.0 (only the `serial.epoll_fd = -1`
  init remains, `main.c:330`). Deferred — see
  [decisions/0003-epoll-splice-fastpath-deferred.md](../decisions/0003-epoll-splice-fastpath-deferred.md)
  and [tracking/STATUS.md](../tracking/STATUS.md).
- **`rfc2217://` transport — intentional stub.** `transport_open()` calls `zt_die` with
  "rfc2217:// is not yet implemented; use ser2net raw + tcp://" (`src/serial/transport.c:95`).
  Native `tcp://` and `telnet://` *do* work. See
  [decisions/0005-rfc2217-deferred.md](../decisions/0005-rfc2217-deferred.md).
- **`ext/multi.c` (multi-pane) — a stub.** `multi_render()` is a no-op `(void)c;`
  (`src/ext/multi.c:113-119`); it is not wired, not keybound, and not discoverable. Real
  multi-pane is future work — see [plans/ROADMAP.md](../plans/ROADMAP.md).
- **`osc8_rewrite()` (OSC 8 hyperlinks) — dead code.** Defined at `src/proto/osc.c:238` with
  **zero call sites**; the `Ctrl+A o` settings "OSC 8" toggle flips a flag nothing reads. It also
  carries a latent out-of-bounds write → [KNOWN_ISSUES ZT-019](../tracking/KNOWN_ISSUES.md).
- **Fuzzy finder (`Ctrl+A .`) — functional** as of the 2026-06 fix: `tui/fuzzy.c` scans history
  from index 1 and `input.c`'s `handle_stdin_chunk` routes keystrokes to it →
  [KNOWN_ISSUES ZT-008](../tracking/KNOWN_ISSUES.md).

For the full corrected feature truth (including in-memory-only history/bookmarks and the
real profile path), see [FAQ.md](./FAQ.md) and the relevant ADRs.

---

## 7. Embedding contract (summary)

zyterm can be linked into a host program (e.g. the `zy` shell as a builtin). The public surface
is the single header `include/zyterm.h`, exporting **7 symbols total**:

| Symbol                  | Purpose |
|-------------------------|---------|
| `zyterm_main`           | Entry point; behaves like `main` for one run. |
| `zt_g_embedded`         | Set `true` to suppress host-killing behaviour (alarm watchdog, `_exit`). |
| `zt_g_embed_jmp`        | `sigjmp_buf` the host arms with `sigsetjmp` before the call. |
| `zt_g_embed_jmp_armed`  | Flag guarding the jump buffer. |
| `zt_embed_disarm`       | Disarm the jump after a normal return. |
| `zt_embed_reset`        | Reset process-wide state to a clean slate before each in-process run. |
| `zt_trace`              | Append a trace line to `$ZYTERM_TRACE` (embedded-mode diagnostics). |

The full contract — calling sequence, the `siglongjmp` recovery path, and reset guarantees —
is in [EMBEDDING.md](./EMBEDDING.md).

---

## 8. Build pipeline

A plain Makefile **auto-discovers** sources — `find src -name '*.c'` (`Makefile:52`) — so adding
a file to a module needs no Makefile edit. Each `src/.../<file>.c` compiles to
`build/obj/.../<file>.o` and links into `./zyterm`.

| Target               | Produces |
|----------------------|----------|
| `make` / `make all`  | `./zyterm` (release, `-O3`). |
| `make debug`         | `-O0 -g3 -DZT_DEBUG`. |
| `make release`       | adds `-flto -march=native -DNDEBUG`. |
| `make zyterm_embed.a`| static archive for embedders (all objects **except** `main.o`, `Makefile:89`). |
| `make test`          | unit + pty + integration suites, linked against `zyterm_embed.a`. |
| `make modules`       | per-module file/LOC summary. |

`build.sh` wraps format → lint → build → test → `.deb`. CI (`.github/workflows/ci.yml`) runs
gcc+clang × {none, asan-ubsan}, format-check, smoke, and tests. The release workflow
(`release.yml`) builds amd64+arm64 `.deb`s on a `v*` tag and verifies the tag matches
`ZT_VERSION`.

For old-glibc portability, `src/zt_internal.h` pins `strtol`/`strtoul` back to a per-arch base
glibc symbol via `.symver` (skipped under sanitizers, which install interceptors keyed on the
modern symbol). The release build currently lacks hardening flags and uses `-march=native`;
fixing that is tracked in [plans/RELIABILITY_HARDENING.md](../plans/RELIABILITY_HARDENING.md).

---

## 9. Anti-patterns (don't do these)

These follow directly from the structure above. Each is enforced as an invariant; cite
[../invariants/INVARIANTS.md](../invariants/INVARIANTS.md) in review.

- **No back-edge in the dependency chain.** A lower module must never call up the chain
  (e.g. `core/` calling `loop/`). The header includes only flow downward; keep it that way
  ([INVARIANTS §8](../invariants/INVARIANTS.md)).
- **Never block in the event loop.** Everything reachable from `run_interactive` must return
  promptly. Blocking `waitpid`/`read`/`write`/`poll` without a bounded deadline starves
  rendering and input ([INVARIANTS §3](../invariants/INVARIANTS.md); cf. the open hangs
  [ZT-006](../tracking/KNOWN_ISSUES.md), [ZT-026](../tracking/KNOWN_ISSUES.md)).
- **No `exit()` outside `loop/` (and `main.c`).** Use `zt_die` for fatal errors — in embedded
  mode it `siglongjmp`s back to the host instead of killing it (`core.c:49-52`). Bare `exit`/
  `_exit` is acceptable only in a just-forked child immediately after a failed `exec`.
- **Assume `serial.fd` may be `-1`.** It is `-1` whenever the device is disconnected
  (`zt_ctx.h:173`). Device-touching code must tolerate that; never assume a valid fd.
- **Never touch `zt_ctx` from a signal handler.** Handlers may write only `zt_g_quit` /
  `zt_g_winch` and must stay async-signal-safe ([INVARIANTS §2](../invariants/INVARIANTS.md)).

---

_See also: [INTERNALS.md](./INTERNALS.md) · [../invariants/INVARIANTS.md](../invariants/INVARIANTS.md)
· [../tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md) · [EMBEDDING.md](./EMBEDDING.md)._
