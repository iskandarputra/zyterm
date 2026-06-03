# INVARIANTS — the don't-regress contract

This document records the properties that **must stay true** in zyterm. Each rule is one thing
that, if broken, reintroduces a defect we have already paid for. Most rules carry a `where`
source pointer and a link to the [ADR](../decisions/README.md) that decided it or the
[ZT-NNN defect](../tracking/KNOWN_ISSUES.md) it was learned from.

This is a contract, not a tutorial. For *how* a subsystem works today see
[`reference/`](../reference/ARCHITECTURE.md); for *why* a choice was made see
[`decisions/`](../decisions/README.md); for the live defect board see
[`tracking/KNOWN_ISSUES.md`](../tracking/KNOWN_ISSUES.md).

When you change code in an area below, re-read its section first. When you add a new invariant,
add it here with a `where` pointer and link the ADR or defect that motivated it.

Sections (cite as `INVARIANTS §N`):

1. [Resource & pointer ownership](#1-resource--pointer-ownership)
2. [Signals & async-signal-safety](#2-signals--async-signal-safety)
3. [The single-threaded event loop (never block in it)](#3-the-single-threaded-event-loop-never-block-in-it)
4. [Reader thread & fd lifecycle (the SPSC ring)](#4-reader-thread--fd-lifecycle-the-spsc-ring)
5. [Framing & protocol-decoder bounds](#5-framing--protocol-decoder-bounds)
6. [Terminal output & escape-sequence safety](#6-terminal-output--escape-sequence-safety)
7. [Network bridge & local-IPC trust boundary](#7-network-bridge--local-ipc-trust-boundary)
8. [Module dependency chain & `zt_ctx` ownership](#8-module-dependency-chain--zt_ctx-ownership)
9. [Build & release](#9-build--release)

---

## 1. Resource & pointer ownership

zyterm mixes heap-allocated strings with non-heap pointers (`argv`, string literals) in the same
`zt_ctx` fields, and that mix has been the single richest source of high-severity bugs. The
governing rule is **single ownership**: a pointer field is either always-heap (and freed exactly
once) or never-heap (and never freed) — never both depending on the code path.

- **`c->serial.device` must be single-owned.** It is read in `zt_ctx` as `const char *device`
  (`src/zt_ctx.h:171`) and is set on different paths from the positional `argv[optind]` (non-heap),
  from a profile `device=` key (heap), and from port discovery (heap). Any `free()` of this field
  is only safe if every writer `strdup`s. Until that holds, `free` paths corrupt the heap.
  - `where`: `src/main.c:717` (positional assignment), `src/ext/profile.c:93`
    (`free((void*)c->serial.device)` on inotify reload), `src/serial/port_discover.c:171`
    (free on first reconnect when a glob resolves a different path).
  - Paid for by [ZT-001](../tracking/issues/ZT-001-profile-load-frees-argv-device.md) and
    [ZT-002](../tracking/issues/ZT-002-port-rediscover-frees-argv-device.md) (free-of-`argv` → abort),
    [ZT-016](../tracking/KNOWN_ISSUES.md) (startup strdup leaked when a positional device is also
    given), and [ZT-018](../tracking/KNOWN_ISSUES.md) (early-return leaks of the same family of
    fields).

- **Every `malloc`/`calloc`/`strdup` is paired with a `free` on every exit path**, including error
  and early-return paths. Allocation that succeeds before a later failure must be unwound; a single
  cleanup label is preferred over scattered returns.
  - `where`: `src/loop/rx_thread.c:114-172` is the model — `rx_thread_start` frees `r->buf`, `r`,
    the dup fd, and the wake pipe on each of its failure branches.
  - The counter-example we must not repeat: `src/main.c:696`/`:717` early returns for
    `--replay`/`--attach`/`--diff`/`-h`/`-V` leak `filter_cmd` / `metrics_path` / `session_name` /
    `http_webroot` / hooks. Harmless on `exit()`, but a real leak under embedded reuse
    ([ZT-018](../tracking/KNOWN_ISSUES.md)).

- **Allocation results are NULL-checked before use** — including in worker threads, where an
  unchecked allocation crashes the whole process.
  - `where`: `src/proto/clipboard.c:333` (`memcpy(malloc(g.len), …)` with no NULL check in the X11
    worker). Paid for by [ZT-014](../tracking/KNOWN_ISSUES.md).

- **`zt_ctx` field ownership follows its tier comment.** Opaque `void *` impl fields
  (`spsc_impl`, `http_impl`, `hooks`) are owned solely by their defining module; ctx consumers
  must not free or reinterpret them. See [§8](#8-module-dependency-chain--zt_ctx-ownership).
  - `where`: `src/zt_ctx.h:210` (`spsc_impl`), `:337` (`http_impl`), `:413` (`hooks`).

---

## 2. Signals & async-signal-safety

Signal handlers run asynchronously and may interrupt the allocator, libc, or the render path.
The rule is **handlers do almost nothing**: they flip a flag or perform only async-signal-safe
work, and the main loop does everything else.

- **Quit/winch handlers touch only `volatile sig_atomic_t` globals.** `sig_quit` sets `zt_g_quit`;
  `sig_winch` sets `zt_g_winch`. No handler reads or writes `zt_ctx` or calls non-async-safe libc.
  - `where`: `src/core/core.c:267-275` (handlers), `src/zt_ctx.h:140-141` (the two globals).

- **SIGINT / SIGTERM / SIGHUP / SIGQUIT all route to the quit flag**, never to immediate teardown.
  The main loop observes the flag and tears down on its own schedule.
  - `where`: `src/core/core.c:337-341` (installation).

- **SIGPIPE stays ignored.** We handle write errors explicitly on every fd; a broken HTTP/SSE/WS
  peer or a hung-up serial device must not deliver SIGPIPE.
  - `where`: `src/core/core.c:346-348` (`sa.sa_handler = SIG_IGN; sigaction(SIGPIPE, …)`).

- **There is no SIGCHLD handler.** Child reaping (filter subprocess, event hooks) is the main
  loop's job via `hooks_reap` / `filter_stop` — never an async handler. Adding a SIGCHLD handler
  would reintroduce async non-safe reaping and race the loop's own `waitpid`.
  - `where`: absence is intentional; `src/core/core.c:328-362` installs no `SIGCHLD`.

- **The crash handler is async-signal-safe and one-shot.** `sig_crash` calls only `tcsetattr`,
  `write`, `raise`, `_exit` (plus `siglongjmp` on the embed path). `SA_RESETHAND` makes it fire
  once, so the re-raise hits `SIG_DFL` and produces a core dump.
  - `where`: `src/core/core.c:294-316` (handler), `:354-359` (`SA_RESETHAND` install for SEGV /
    ABRT / BUS / FPE).

- **Embed recovery via `siglongjmp` is restricted to SIGABRT and SIGFPE.** SIGSEGV / SIGBUS imply
  memory corruption; longjmping out and resuming host code is undefined behaviour and a near-certain
  double-fault on the next `malloc`/`free`. For those we restore the terminal and re-raise.
  - `where`: `src/core/core.c:295` (`embed_recover = … && (s == SIGABRT || s == SIGFPE)`).

- **The crash handler restores the operator's terminal before exiting.** It emits the same
  alt-screen / mouse / bracketed-paste reset inventory as `restore_terminal()`, in the same order
  (reset scroll region → leave alt-screen → reset main buffer). A regression here leaves the
  operator (or host shell) in a broken terminal after a crash.
  - `where`: `src/core/core.c:299-309`; mirror of `src/core/core.c:451-458`.

---

## 3. The single-threaded event loop (never block in it)

zyterm's runtime is one `poll(2)`-driven loop on a single thread (see
[ADR-0002](../decisions/0002-single-ctx-poll-driven-loop.md)). The loop owns the UI; **any
blocking call inside a loop tick freezes the entire UI**. The rule: every wait either has a bounded
deadline or is non-blocking, and never depends on a remote peer or child cooperating.

- **No blocking `waitpid` on a loop tick.** Reaping a child must use `WNOHANG` and escalate
  (SIGTERM → SIGKILL) rather than block on a child that ignores the signal.
  - `where`: `src/ext/filter.c:86` (`filter_stop`'s blocking `waitpid(…, 0)`).
    Paid for by [ZT-006](../tracking/issues/ZT-006-filter-stop-blocking-waitpid.md).

- **Every `poll()`-driven wait has an overall deadline.** TX trickle/flow-control loops must bound
  total time and surface a "TX stalled" flash rather than spin or hang forever under stuck flow
  control.
  - `where`: `src/loop/send.c:128` (TX `poll()` loop), `~:96` (trickle). Paid for by
    [ZT-026](../tracking/KNOWN_ISSUES.md).

- **No blocking client fds in the loop.** Sockets the loop services (metrics, HTTP) must be created
  `SOCK_NONBLOCK`; a non-draining client must be dropped on `EAGAIN`, never stall the tick.
  - `where`: `src/net/metrics.c:102` (`metrics_tick` accepts a blocking client fd — missing
    `SOCK_NONBLOCK`). Paid for by [ZT-024](../tracking/KNOWN_ISSUES.md).

- **`poll()` must watch a live fd.** When a serial reopen fails the loop must drive reconnect, not
  leave `serial.fd == -1` in the poll set where it is silently ignored while the HUD still shows
  "connected".
  - `where`: `src/loop/input.c:130` (failed `Ctrl+A A` autobaud leaves `fd == -1`).
    Paid for by [ZT-005](../tracking/KNOWN_ISSUES.md).

- **HUP paths drain like POLLIN paths.** A `POLLHUP` must read until empty before reconnecting, so
  buffered RX is not lost across a disconnect.
  - `where`: `src/loop/runtime.c:156` (non-threaded POLLHUP reads once).
    Paid for by [ZT-027](../tracking/KNOWN_ISSUES.md).

---

## 4. Reader thread & fd lifecycle (the SPSC ring)

The optional `--threaded` reader (`src/loop/rx_thread.c`) is the only second thread in the process.
Its correctness rests on a strict ownership split and the SPSC memory ordering. See
[`design/THREADING_AND_RECONNECT.md`](../design/THREADING_AND_RECONNECT.md).

- **The worker owns only its private `dup` fd and the SPSC ring — nothing else in `zt_ctx`.** It
  reads `c` for the ring and wake pipe but mutates no main-thread state. This lets the main thread
  freely close/reopen `c->serial.fd` (autobaud, reconnect) without racing the worker's in-flight
  `read()`.
  - `where`: `src/loop/rx_thread.c:14-24` (model), `:54` (`_Atomic int local_fd` "owned here"),
    `:136-144` (`F_DUPFD_CLOEXEC` of the serial fd). The struct comment in `src/zt_ctx.h:165-168`
    states the same: most fields are main-thread-only.

- **Producer↔consumer hand-off uses release/acquire atomics, with relaxed only on the owner's own
  index.** The producer publishes `head` with `release` after filling the ring; the consumer reads
  `head` with `acquire` before copying. Neither side may downgrade the cross-thread store/load to
  relaxed.
  - `where`: producer `src/loop/rx_thread.c:82-88` (load `tail` acquire, store `head` release;
    own `head` relaxed); consumer `:212-223` (load `head` acquire, store `tail` release; own
    `tail` relaxed).

- **`r->running` and `r->local_fd` are the shutdown handshake.** Stop flips `running` (release),
  then `atomic_exchange`s `local_fd` to `-1` and closes the old dup so any blocked `read()` returns
  `EBADF` promptly; only then does it `pthread_join`. Reordering these races the worker.
  - `where`: `src/loop/rx_thread.c:183-187`; worker exits cleanly on `EBADF` at `:97`.

- **fd swaps are bracketed by pause/unpause, never by a bare `fd` reassignment.** Any code that
  changes `c->serial.fd` (autobaud, `reconnect_attempt`, manual reopen) must call
  `rx_thread_pause()` before and `rx_thread_unpause()` after, so the worker re-dups the new fd.
  - `where`: `src/loop/rx_thread.c:247-258`. Pause is a no-op when stopped; unpause restarts only
    when the user actually asked for `--threaded`.

- **`spsc_enabled` is user intent and is never cleared by stop.** It records that `--threaded` was
  requested. `rx_thread_stop()` must not clear it, or a paired suspend/resume around an fd swap
  would silently fail to restart the reader.
  - `where`: `src/loop/rx_thread.c:200-202` (the explicit NOTE), `:257` (unpause gates on it).

- **`spsc_impl` is the authoritative "thread is up" marker** — not `spsc_enabled`. Idempotent
  start/stop/is-running all key off `spsc_impl`, because the CLI parser may set `spsc_enabled`
  before the thread is created.
  - `where`: `src/loop/rx_thread.c:117-120` (start), `:177` (stop), `:240` (is-running).

---

## 5. Framing & protocol-decoder bounds

The framing/CRC decoders (`src/proto/framing.c`) run over hostile-by-assumption device input. Every
accumulator is fixed-size, so **every write into a decoder buffer is bounded** and decoder state is
**reset on every mode change and reconnect**. See
[`design/FRAMING_AND_CRC.md`](../design/FRAMING_AND_CRC.md).

- **Every decoder/render/log write is bounded by its compile-time cap.** Line and frame buffers are
  `ZT_LINEBUF_CAP` (4096); the input line is `ZT_INPUT_CAP`; the read chunk is `ZT_READ_CHUNK`.
  No decoder may write past `buf[ZT_LINEBUF_CAP]`.
  - `where`: caps in `src/zt_ctx.h:55-57`; the framing accumulator `proto.buf[ZT_LINEBUF_CAP]` at
    `src/zt_ctx.h:349`.

- **Capacity checks must count the true worst case.** COBS overhead is `n + n/254 + 2`; a bound that
  undercounts it is a latent overflow even if the current caller oversizes its buffer.
  - `where`: `src/proto/framing.c:241` (`encode_cobs` cap check undercounts).
    Paid for by [ZT-021](../tracking/KNOWN_ISSUES.md).

- **Zero-length frames dispatch immediately.** A LENPFX frame with `len16_need == 0` must be
  emitted at once; consuming the next byte as payload desyncs the whole stream.
  - `where`: `src/proto/framing.c:199`; state fields `len16_*` at `src/zt_ctx.h:363-365`.
    Paid for by [ZT-022](../tracking/KNOWN_ISSUES.md).

- **All per-decoder accumulator state lives in `zt_ctx.proto`, not file-static, so it can be
  reset.** `framing_reset()` must clear `escape`, `cobs_pending`, `len16_lenb/have/need` on every
  framing-mode change and on reconnect. The fields were deliberately moved out of `framing.c`
  statics for exactly this reason.
  - `where`: `src/zt_ctx.h:351-365` and the rationale comment at `:355-361`.

- **OSC-8 rewrite must stay disabled until it is bounds-correct.** `osc8_rewrite` writes each URL
  twice past its bounds check (OOB when `url_len > ~18`). It currently has **zero call sites**, so
  it is latent — wiring it in without fixing the guard reintroduces an OOB write.
  - `where`: `src/proto/osc.c:256`. Tracked as [ZT-019](../tracking/KNOWN_ISSUES.md); see also the
    "dead code, do not advertise" note in
    [`tracking/KNOWN_ISSUES.md`](../tracking/KNOWN_ISSUES.md).

---

## 6. Terminal output & escape-sequence safety

The operator's terminal is a trust sink: bytes written to it can move the cursor, set the title,
or drive OSC 52 clipboard writes. Device RX is **untrusted**. The rule: **device RX must not be
able to inject escape sequences into the operator's terminal** unless the operator explicitly opted
into raw passthrough.

- **Device RX is filtered before it reaches the operator terminal.** Today the render path strips
  only `\r`, passing ESC/CSI/OSC through verbatim — that is the bug. The invariant we are moving to
  is default-deny of dangerous escapes (OSC 52 clipboard, title set, etc.) on the normal render
  path.
  - `where`: `src/render/render.c:93` (`render_rx`, ~248-288). Paid for by
    [ZT-003](../tracking/issues/ZT-003-device-rx-escape-injection.md).

- **Dangerous OSC sequences are default-deny.** A device must not silently write the operator's
  clipboard (OSC 52) or set the terminal title via emitted RX. Allowing them requires an explicit
  operator toggle, not a default.
  - `where`: render path `src/render/render.c:93`; clipboard module `src/proto/clipboard.c`.
    See [ZT-003](../tracking/issues/ZT-003-device-rx-escape-injection.md).

- **SGR and KGDB/raw passthrough are explicit, gated modes.** When the operator enables
  `sgr_passthrough` or `passthrough`, device escapes pass through *by request*. These flags are the
  only sanctioned way device RX reaches the terminal unfiltered, and they must not be on by default.
  - `where`: `src/zt_ctx.h:371` (`sgr_passthrough`), `:374` (`passthrough`).

- **All terminal output goes through the output buffer (`ob_*`), flushed once per frame.** Direct
  `write(STDOUT_FILENO, …)` is reserved for the bounded emergency cleanup string in the crash
  handler and the oversized-chunk fast path. This keeps rendering flicker-free and gives the cast
  recorder a single observation point.
  - `where`: `src/core/core.c:150-174` (`ob_write` / `ob_flush`), `:144-148` (record callback).

---

## 7. Network bridge & local-IPC trust boundary

The HTTP/SSE/WS bridge, the detach/attach session socket, and the metrics socket are all
**state-mutating or data-leaking endpoints**. The trust boundary is the rule here:
**loopback and the local user are not automatically trusted.** A localhost-only listener is still
reachable cross-site (CORS simple-request, DNS rebind) and a `/tmp` socket is still reachable by
any local user under a loose umask. See [`SECURITY.md`](../../SECURITY.md) and
[`design/HTTP_BRIDGE.md`](../design/HTTP_BRIDGE.md).

- **Loopback ≠ trusted: mutating HTTP endpoints require authentication.** `POST /tx` and
  `/api/send` write a line to the serial device and must require a bearer token plus
  Origin/Host validation — never reachable by an unauthenticated cross-site simple request.
  - `where`: `src/net/http.c:907`. Paid for by
    [ZT-004](../tracking/issues/ZT-004-unauth-http-tx-csrf.md).

- **No wildcard CORS on state-changing routes.** `--http-cors` must not emit `Access-Control-Allow-
  Origin: *` for routes that mutate device state; an allowlist is required.
  - `where`: `src/net/http.c` CORS emission (flag `http_cors`, `src/zt_ctx.h:339`).
    See [ZT-004](../tracking/issues/ZT-004-unauth-http-tx-csrf.md).

- **WebSocket upgrades validate Origin (and carry a token).** The live RX stream is sensitive; any
  origin must not be able to open a WS and read device output cross-origin.
  - `where`: `src/net/http.c:861` (WS upgrade does no Origin check).
    Paid for by [ZT-013](../tracking/KNOWN_ISSUES.md).

- **IPC sockets are created with restrictive permissions and peer-cred checks.** The detach session
  socket and the metrics socket must live in `$XDG_RUNTIME_DIR` (0700) with a 0600 socket, and
  verify `SO_PEERCRED` — not a default-umask socket under `/tmp`.
  - `where`: `src/net/session.c:48` (`/tmp/zyterm.<name>.sock`, default umask, no peer-cred) —
    [ZT-012](../tracking/KNOWN_ISSUES.md); `src/net/metrics.c:39` (metrics socket without
    restrictive perms) — [ZT-028](../tracking/KNOWN_ISSUES.md).

- **Bridge writes to network fds are EAGAIN-aware and bounded, and dead peers are closed.** The
  loop's `zt_write_all` retries only `EINTR`, so it must not be used to push large static files to
  a non-blocking HTTP fd (truncation on `EAGAIN`); broadcast must loop over the full payload, not
  cap at one 4096-byte segment; and a peer whose write fails must be closed, not leaked.
  - `where`: `zt_write_all` at `src/core/core.c:187-200` misused at `src/net/http.c`
    ([ZT-011](../tracking/KNOWN_ISSUES.md)); 4096-byte broadcast cap at `src/net/http.c:1020`
    ([ZT-007](../tracking/issues/ZT-007-http-broadcast-truncates-4k.md)); unclosed WS peers at
    `src/net/http.c:1036-1037` ([ZT-009](../tracking/issues/ZT-009-ws-broadcast-ignores-errors.md),
    [ZT-017](../tracking/KNOWN_ISSUES.md)).

- **The `--http` port is parsed and range-validated.** Use `strtol` with a 1–65535 range check, not
  `atoi` truncated to `uint16`.
  - `where`: `src/main.c:586`. Paid for by [ZT-020](../tracking/KNOWN_ISSUES.md).

---

## 8. Module dependency chain & `zt_ctx` ownership

zyterm is 9 modules under `src/` plus `main.c`. The architecture rule is a **strictly layered,
header-enforced dependency chain** with a single shared context object. See
[`reference/ARCHITECTURE.md`](../reference/ARCHITECTURE.md) and
[ADR-0001](../decisions/0001-linux-first-single-binary.md) /
[ADR-0002](../decisions/0002-single-ctx-poll-driven-loop.md).

- **The dependency chain only ever points down the layer stack:**
  `core ← serial ← log ← proto ← render ← tui ← net ← ext ← loop`. A module never includes a
  header above it in this chain.
  - `where`: enforced by `include/zyterm/internal/<m>.h`, each of which includes exactly the one
    beneath it (verified at line 15 of each header).

- **`main.c` is the only translation unit allowed into `loop/`.** No other module includes
  `loop.h`; the umbrella `src/zt_internal.h` includes only `loop.h`.
  - `where`: `src/zt_internal.h` (umbrella), `src/main.c`.

- **There is exactly one `zt_ctx` per process and it holds all per-process state.** Fields are
  grouped by feature tier (0–4). State is not duplicated into module-file statics where it would
  escape `framing_reset()` / embed reset (see [§5](#5-framing--protocol-decoder-bounds)).
  - `where`: `src/zt_ctx.h:169-427` (the struct), with the "owned by the process; most mutated only
    by the main thread" contract at `:165-168`.

- **Opaque `void *` ctx fields are owned solely by their defining module.** `spsc_impl`,
  `http_impl`, and `hooks` are private handles; only `rx_thread.c`, `http.c`, and `hooks.c`
  respectively may allocate, interpret, or free them.
  - `where`: `src/zt_ctx.h:210` (`spsc_impl` "defined in rx_thread.c"), `:337` (`http_impl`
    "defined in http.c"), `:413` (`hooks` "managed entirely by ext/hooks.c").

- **Embedded reuse must reset all sticky state to first-call baseline.** `zt_embed_reset()`
  uninstalls signal handlers, zeroes `zt_g_quit` / `zt_g_winch` / `zt_g_ui_active` /
  `zt_g_stdin_saved`, discards the output buffer, and calls each module's own embed-reset hook.
  Any new file-static or sticky global a feature adds must be scrubbed here too.
  - `where`: `src/core/core.c:93-127` (`zt_embed_reset`, including `multi_embed_reset()` /
    `session_embed_reset()`).

- **The embedding surface is exactly 7 exported symbols.** `zyterm_main`, `zt_g_embedded`,
  `zt_g_embed_jmp`, `zt_g_embed_jmp_armed`, `zt_embed_disarm`, `zt_embed_reset`, `zt_trace`. Adding
  to or removing from this set is an API change and must be reflected in
  [`reference/EMBEDDING.md`](../reference/EMBEDDING.md).
  - `where`: `src/zt_ctx.h:150-159` and `src/core/core.c:39-127`.

---

## 9. Build & release

The build is a plain Makefile that auto-discovers `src/**/*.c`; the release is a tag-gated `.deb`
pipeline. See [`ops/RELEASE.md`](../ops/RELEASE.md) and
[`plans/RELIABILITY_HARDENING.md`](../plans/RELIABILITY_HARDENING.md).

- **The version is single-sourced in `ZT_VERSION`.** No build script, doc, or CI step hard-codes the
  version string; everything derives from this one macro. The release workflow verifies the git tag
  equals `ZT_VERSION` and fails the release otherwise.
  - `where`: `src/zt_ctx.h:70` (`#define ZT_VERSION "1.2.0"`); tag check in
    `.github/workflows/release.yml`.

- **Runtime dependency is libc only.** No new hard link-time dependency may be added. The X11
  clipboard is loaded at runtime via `dlopen("libxcb.so.1")`, never linked; the release pins
  `strtol`/`strtoul` to a base glibc symbol via `.symver` (skipped under sanitizers) to keep the
  `.deb` portable.
  - `where`: `.symver` directives in `src/zt_internal.h`; clipboard `dlopen` in
    `src/proto/clipboard.c`.

- **The default sanitizer-clean and asan/ubsan build matrices stay green.** CI runs gcc+clang ×
  {none, asan-ubsan}, format-check, smoke, and the full test suite (`make test`: unit + pty +
  integration under `zyterm_embed.a`). A change must not regress any matrix cell.
  - `where`: `.github/workflows/ci.yml`; `make test` target in the `Makefile`.

- **OPEN ITEM — release builds must carry hardening flags and drop `-march=native`.** The current
  release build lacks hardening flags (`-D_FORTIFY_SOURCE`, `-fstack-protector-strong`, RELRO,
  etc.) and uses `-march=native`, which makes the `.deb` non-portable across CPUs. This is a known
  gap, **not** a satisfied invariant.
  - `where`: release flags in the `Makefile` release target. Tracked in
    [`plans/RELIABILITY_HARDENING.md`](../plans/RELIABILITY_HARDENING.md).

---

_Last updated: 2026-06-03._
