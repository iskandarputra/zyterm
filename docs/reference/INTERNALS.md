# zyterm internals

Deep per-subsystem mechanics for zyterm 1.2.0. This is the level below
[ARCHITECTURE.md](ARCHITECTURE.md): each section walks one mechanism as it actually exists
in `src/`, with `file.c:line` citations you can open. For the higher-level module map and
dependency chain see ARCHITECTURE.md; for the don't-regress rules referenced throughout, see
[../invariants/INVARIANTS.md](../invariants/INVARIANTS.md); for known defects, see
[../tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md).

Several mechanisms here have sharp edges that are tracked as defects (`ZT-NNN`). Those are
called out inline so you don't mistake a documented hazard for safe behaviour.

---

## 1. Output buffering & terminal management

All rendering accumulates into one process-wide stdout buffer and is flushed once per frame,
so a full-screen repaint becomes a single `write(2)` instead of thousands of small ones.

The buffer is a fixed `256 KiB` static array `s_ob[OB_CAP]` with a running length `s_ob_len`
(`src/core/core.c:137-140`). Three entry points fill it:

- `ob_write(p, n)` (`core.c:150`) appends `n` bytes. If the append would overflow `OB_CAP`,
  it first `ob_flush()`es what's buffered; if the single write is itself larger than the
  buffer it bypasses the buffer entirely and goes straight to `zt_write_all(STDOUT_FILENO, …)`.
- `ob_cstr(s)` (`core.c:165`) is the `strlen` convenience wrapper.
- `ob_flush()` (`core.c:169`) writes the whole buffer with `zt_write_all` and resets the
  length to 0. It is a no-op when empty.

`zt_write_all` (`core.c:187`) is the only write primitive: it loops until every byte is out,
retrying **only** on `EINTR`. That EINTR-only retry is correct for blocking stdout but is the
root of [ZT-011](../tracking/KNOWN_ISSUES.md) when the same helper is
reused for the non-blocking HTTP/webroot fds (see §9).

A single optional observer hook, `s_ob_record_cb` (`core.c:144`), sees every byte just before
it hits stdout; the asciinema `--rec` recorder registers it via `ob_set_record_callback`.
Bypassed large writes feed the callback too (`core.c:156`), so recordings stay complete.

**Raw-mode entry.** `setup_stdin_raw` (`core.c:386`) snapshots the original `termios` and
fd flags into globals, sets `zt_g_stdin_saved = true`, then drops `ICRNL/IXON/OPOST/ECHO/
ICANON/ISIG/IEXTEN`, forces `CS8`, and sets `VMIN=1, VTIME=0`. It is a guarded no-op if
already saved, and silently returns if stdin is not a tty (`tcgetattr` fails) — that
non-tty-but-UI-active case is exactly what `zt_embed_reset` has to scrub (§2).

**Restore ordering is load-bearing.** `restore_terminal` (`core.c:410`) emits the
de-init escape string **before** the `TCSAFLUSH` `tcsetattr`. Reasons, from the source
comments:

- It runs when *either* `zt_g_stdin_saved` *or* `zt_g_ui_active` is set, so a non-tty
  embedded run that flipped `ui_active` still gets its alt-screen / mouse modes turned off.
- Mouse / bracketed-paste reporting is disabled first, then `TCSAFLUSH` drains and discards
  queued input — otherwise stray mouse-event bytes arriving mid-restore get injected into the
  parent shell's line editor as keystrokes.
- The escape order matters within the string: `\033[r` (reset scroll region) is sent *while
  still on the alt screen*, then `\033[?1049l` leaves it. Sending `\033[r` after leaving alt
  screen would home the cursor on the main buffer and clobber the restored prompt position.

The full reset inventory (scroll region, leave alt-screen, SGR/button/basic mouse off, focus
events off, bracketed paste off, autowrap on, cursor visible + blink, default cursor style,
keypad numeric, ASCII G0, SGR reset) is documented inline at `core.c:436-450`. The crash
handler `sig_crash` emits the *same* string from a static buffer (§2) so an abnormal exit
leaves the terminal in the same clean state.

> Device RX bytes are sanitized before they reach this terminal buffer: `render_rx` rewrites ESC
> and other C0/DEL controls to inert `cat -v` caret notation unless an explicit passthrough mode is
> on (closed [ZT-003](../tracking/issues/ZT-003-device-rx-escape-injection.md)); see §4 and
> [INVARIANTS §6](../invariants/INVARIANTS.md).

---

## 2. Signals & embed recovery

Signal installation is centralised, idempotent, and fully reversible — because zyterm can run
embedded inside the `zy` shell, where it must hand the terminal back rather than `exit()`.

**Handler table** (`install_signals`, `core.c:328`):

| Signal(s) | Handler | Action |
|---|---|---|
| `SIGINT`, `SIGTERM`, `SIGHUP`, `SIGQUIT` | `sig_quit` (`core.c:267`) | set `zt_g_quit = 1` and return |
| `SIGWINCH` | `sig_winch` (`core.c:272`) | set `zt_g_winch = 1` and return |
| `SIGPIPE` | `SIG_IGN` | ignored; write errors handled explicitly |
| `SIGSEGV`, `SIGABRT`, `SIGBUS`, `SIGFPE` | `sig_crash` (`core.c:294`) | restore terminal, then recover-or-re-raise |

The two flags `zt_g_quit` / `zt_g_winch` are the **only** state the handlers touch, and both
are `volatile sig_atomic_t` (`core.c:39-40`). The handlers call no non-async-signal-safe
functions. This is the contract in [INVARIANTS §2](../invariants/INVARIANTS.md).

**There is no `SIGCHLD` handler.** Child reaping (filter subprocess, event hooks) is done
synchronously from the main loop via `hooks_reap` / `filter_stop`, not from a signal handler.
Do not add one — it would race the loop's `waitpid` bookkeeping.

**`install_signals` is idempotent** (`core.c:332`): it early-returns if `s_handlers_saved` is
already set. Without that guard, a second embedded invocation would capture zyterm's *own*
handlers as the "previous" ones and lose the host's originals. `uninstall_signals`
(`core.c:366`) restores all ten saved handlers and clears the flag; it is a no-op when nothing
is installed. `zt_embed_reset` (`core.c:93`) calls it on every embedded re-entry.

**`SA_RESETHAND` crash handling** (`core.c:351-359`). The crash signals are installed with
`SA_RESETHAND`, making the handler one-shot: after it runs, the disposition reverts to
`SIG_DFL`, so a later `raise(s)` produces a normal core dump. `sig_crash` itself
(`core.c:294`):

1. Computes `embed_recover = embedded && jmp_armed && (s == SIGABRT || s == SIGFPE)`.
2. Restores the terminal using only async-signal-safe calls (`tcsetattr` + a single `write`
   of the static cleanup string) — `restore_terminal`'s logic is duplicated here because it
   isn't signal-safe.
3. **If `embed_recover`**: clears `zt_g_embed_jmp_armed` and `siglongjmp`s back to the host
   with status `128 + s`.
4. **Otherwise**: `raise(s)` to take the (now-default) signal — re-raising on the corrupted
   state path so the OS dumps core.

**Why only `SIGABRT`/`SIGFPE` recover** (`core.c:286-293`): these can fire on otherwise
coherent heap/runtime state. `SIGSEGV`/`SIGBUS` imply memory corruption; `siglongjmp`ing out
of them and resuming arbitrary host code is undefined behaviour and a near-certain
double-fault on the next `malloc`/`free`. For those, restore + re-raise is strictly safer than
continuing.

**`zt_die`** (`core.c:215`) follows the same rule for non-signal fatal paths: if
`embedded && jmp_armed` it `siglongjmp`s with status 1 instead of `exit(1)`, so a fatal error
never kills the host shell.

**Embed re-entry scrub.** `zt_embed_reset` (`core.c:93`) zeroes the sticky globals
(`zt_g_quit`, `zt_g_winch`, `zt_g_ui_active`, `zt_g_stdin_saved`), discards any leftover output
buffer bytes via `zt_embed_reset_buffers` (so a previous run's pixels can't bleed onto the
host terminal), and calls `multi_embed_reset` / `session_embed_reset`. The non-tty case noted
in §1 is why `ui_active` must be force-cleared here. The `zt_trace` helper (`core.c:73`) logs
these transitions to `$ZYTERM_TRACE` or the `/tmp/zyterm.trace` sentinel; it is **not**
async-signal-safe, so `sig_crash` calls it only on the recover path it's about to leave anyway.

---

## 3. The SPSC reader thread (`--threaded`)

`--threaded` spawns one dedicated reader thread that drains the serial device into a
lock-free single-producer/single-consumer ring; the main thread consumes from the ring and
runs the render/log pipeline. This decouples UART interrupt latency from render latency at
high baud. At low baud it is a pessimization (one extra syscall per byte), so it is opt-in.
See [../design/THREADING_AND_RECONNECT.md](../design/THREADING_AND_RECONNECT.md) and
[INVARIANTS §4](../invariants/INVARIANTS.md).

**Ring layout** (`spsc_ring_t`, `src/loop/rx_thread.c:45-55`). Power-of-two capacity
`ZT_SPSC_CAP = 1 MiB` (`src/zt_ctx.h:69`), a `head` (producer) and `tail` (consumer) as
`atomic_size_t`, a `running` flag, and a `local_fd` — the worker's *private* `dup` of the
serial fd. `head`/`tail` are free-running monotonic counters; the byte index is always
`counter & (cap - 1)`.

**Producer side** (`rx_thread_main`, `rx_thread.c:65`). Each iteration loads `local_fd`
(acquire), `read()`s up to `ZT_READ_CHUNK` (64 KiB) bytes into a scratch, then:

```c
size_t head    = load(head, relaxed);          /* we are the only writer */
size_t tail    = load(tail, acquire);           /* see consumer's progress */
size_t free_sp = r->cap - (head - tail);        /* monotonic-counter math */
size_t wr      = (n < free_sp) ? n : free_sp;   /* clamp to free space */
... copy wr bytes, each at (head + i) & (cap - 1) ...
store(head, head + wr, release);                /* publish */
wake_main(c);
```

(`rx_thread.c:82-89`.) The producer reads its own `head` `relaxed` (it is the sole writer) but
reads `tail` `acquire` and publishes `head` `release` — that pairing is what makes the written
bytes visible to the consumer before the new `head` is. `free_sp` works without modular
arithmetic precisely because both counters are unbounded and only masked at access time.

> **Silent byte-drop when full.** If `n > free_sp`, only `free_sp` bytes are written and the
> remainder is *dropped with no diagnostic* (`wr` is clamped, the rest of `tmp` is discarded).
> A render/log stall that lets the 1 MiB ring fill therefore loses RX data silently. This is
> tracked as backlog work in [../plans/ROADMAP.md](../plans/ROADMAP.md) (overflow accounting /
> backpressure); today the only signal is missing bytes.

**Consumer side** (`rx_thread_drain`, `rx_thread.c:208`). Loads `tail` `relaxed` and `head`
`acquire`; `avail = head - tail`. It copies up to `cap` bytes in **at most two contiguous
spans** (one up to the wrap, one from the base) so it never loops byte-by-byte, then publishes
`tail + rd` with `release`. It also drains the wake-pipe so `poll()` doesn't spin (`rx_thread.c:225`).

**The private dup is the whole trick** (`rx_thread.c:135-144`). On start the worker takes
`fcntl(c->serial.fd, F_DUPFD_CLOEXEC, 3)`. Because it reads *its own* fd, the main thread is
free to `close`/reopen `c->serial.fd` — for autobaud, reconnect, manual reopen — and even let
the OS recycle the fd number, without racing the worker's in-flight `read()`. To pick up a new
fd you must `rx_thread_stop()` then `rx_thread_start()`; the worker dups the *new* `c->serial.fd`
on the next start. Error handling inside the read loop distinguishes `EINTR` (retry), `EBADF`
(main closed the dup → exit cleanly), `EAGAIN`/`EWOULDBLOCK` (O_NONBLOCK serial, brief sleep),
and device-gone errors like `EIO`/`ENXIO` (wake main + back off so it sees `POLLHUP`)
(`rx_thread.c:96-108`).

**Shutdown** (`rx_thread_stop`, `rx_thread.c:174`). Set `running = 0` (release), then
`atomic_exchange` `local_fd → -1` and `close` the old dup so any blocked `read()` returns
`EBADF` promptly (rather than waiting out the 50 ms error back-off), then `pthread_join`.
The wake pipe is closed and the ring freed. Critically, `spsc_enabled` (user intent from
`--threaded`) is **not** cleared on stop — see pause/unpause below.

**Pause / unpause** (`rx_thread.c:252-258`). `rx_thread_pause` stops the worker iff running;
`rx_thread_unpause` restarts it iff `spsc_enabled` is set and it isn't already running. Both
are idempotent and nest safely. They bracket every `c->serial.fd` swap (autobaud, reconnect,
manual reopen) so the worker's dup is never left pointing at a closed fd. This dance is the
shared half of the reconnect machinery in §7.

---

## 4. RX ingestion pipeline (`rx_ingest`)

Every chunk read from the device — whether by the non-threaded `read()` in `runtime.c` or by
`rx_thread_drain` — funnels through one dispatcher, `rx_ingest` (`src/render/render.c:303`),
which normalises the stream once and then fans it out. Recursion is impossible: the framer
and the filter always deliver their output via `render_rx`, never back through `rx_ingest`.

The fixed order of operations:

1. **Telnet IAC stripping** (`render.c:311-323`) — `telnet://` transport only. Runs *first*,
   on raw wire bytes, because the IAC parser must see the unfiltered stream. State is carried
   across chunks in `c->serial.telnet_rx_st`. The parser strips in place, so bytes are copied
   into a 4 KiB stack scratch (or a heap buffer for larger chunks) before `telnet_rx_filter`.
2. **`--map-in` line-ending translation** (`render.c:330-344`) — applied next so every
   downstream consumer (JSONL, HTTP, filter, framer, render) sees the same normalised stream.
   Output goes into a stack scratch or a `ZT_EOL_OUT_CAP(n)`-sized heap buffer. See §6.
3. **JSONL log** (`render.c:346`) — if `--log-format json`, `log_json_rx`.
4. **HTTP/SSE/WS broadcast** (`render.c:347`) — if the bridge is up, `http_broadcast` (§9).
5. **Filter subprocess** (`render.c:348`) — if `--filter` is running, bytes go to
   `filter_feed` and the function returns; transformed output re-enters later via
   `filter_drain` → `render_rx`.
6. **Framing** (`render.c:354`) — if `c->proto.mode != ZT_FRAME_RAW`, bytes go to
   `framing_feed`, which calls `render_rx` only with complete, CRC-verified payloads (§5).
7. **Raw** — otherwise straight to `render_rx`.

All heap scratch buffers (`telnet_heap`, `heap`) are freed on every exit path.

**`render_rx`** (`render.c:224`) is the terminal/scrollback writer. It counts bytes, honours
`paused`, and decides `live = (sb_offset == 0 && !popup_active)` — when not live it still
pushes to scrollback (so history and watches keep working) but suppresses on-screen writes so
a popup stays on top. In hex mode it accumulates 16-byte rows and emits colourised hexdump
lines via `hex_flush_row`. In text mode it walks bytes: `\r` is dropped, `\n` triggers
`flush_line`, everything else is appended to the line buffer (capped at `ZT_LINEBUF_CAP`).
There is the tab-completion-echo capture state machine at `render.c:255-273`.

> **No escape filtering.** Apart from dropping `\r`, device bytes — including `ESC`, CSI, and
> OSC sequences — are written to the operator's terminal verbatim (`render.c:280-281`, then
> `ob_write` in `emit_colored_line` / `flush_line`). A hostile or buggy device can therefore
> inject an OSC 52 clipboard write, a window-title change, or other terminal commands. This is
> [ZT-003](../tracking/issues/ZT-003-device-rx-escape-injection.md) (🔴) and the subject of
> [INVARIANTS §6](../invariants/INVARIANTS.md): the fix direction is default-deny dangerous
> escapes with raw passthrough gated behind an explicit toggle.

---

## 5. Framing decoders (`src/proto/framing.c`)

When a `--frame` mode other than `raw` is active, `framing_feed` (`framing.c:225`) routes the
post-`map-in` byte stream into one of four byte-at-a-time state machines. All decoder state
lives on `c->proto` (not file-static) so `framing_reset` (`framing.c:46`) and per-pane
isolation work. Each completed frame goes through `frame_dispatch`, then out via `render_rx`
so framed output is indistinguishable from raw output downstream (log, search, JSON,
broadcast). See [../design/FRAMING_AND_CRC.md](../design/FRAMING_AND_CRC.md).

**`frame_dispatch`** (`framing.c:61`) — the common tail. It increments `rx_count`, and if a
CRC mode is configured and the frame is longer than the CRC width (`crc_size`: 2 bytes for
ccitt/ibm, 4 for crc32), it reads the trailing CRC big-endian, recomputes over the payload,
and on mismatch increments `crc_err` and flashes a notice. Either way it then **strips the
trailing CRC** (`n -= csz`, `framing.c:82`) so only the payload is rendered. A zero-length
frame is dropped (`framing.c:63`).

**COBS** (`feed_cobs`, `framing.c:93`; RFC Cheshire & Baker). Accumulates raw bytes into
`c->proto.buf`, sized by `cobs_pending`; on the `0x00` delimiter it decodes in place. The
inner loop has explicit guards against malformed frames whose code byte would push the write
cursor past the read cursor (`framing.c:107-120`) and against running past the buffer.

**SLIP** (`feed_slip`, `framing.c:142`; RFC 1055). `0xC0` END flushes a non-empty frame;
`0xDB` ESC sets the escape latch; an escaped byte maps `0xDC→0xC0`, `0xDD→0xDB`, else passes
through.

**HDLC-ish** (`feed_hdlc`, `framing.c:171`). Byte-async, *no* bit-stuffing. `0x7E` flag
flushes; `0x7D` escape latches; an escaped byte is `b ^ 0x20`.

**Length-prefix (`lenpfx` / "len16")** (`feed_len16`, `framing.c:194`). Reads a little-endian
16-bit length header into `len16_lenb`, computes `len16_need`, rejects lengths larger than the
frame buffer, then collects exactly `len16_need` payload bytes and dispatches.

> **Zero-length lenpfx desync.** When `len16_need == 0`, the decoder does not dispatch
> immediately at header completion; it falls into the payload branch and the *next* byte is
> swallowed by the `len < need` test before the `len == need` check re-triggers, desyncing the
> stream. This is [ZT-022](../tracking/KNOWN_ISSUES.md) (⚪). Fix direction:
> dispatch the moment `len16_need == 0`.

**Encoders** (`framing.c:240-352`) are the inverse algorithms, used by `framing_send` which
optionally appends a CRC into a temp buffer first. Note `encode_cobs`'s worst-case capacity
check undercounts (`framing.c:241`, [ZT-021](../tracking/KNOWN_ISSUES.md), ⚪) — not currently
reachable because the caller oversizes the output buffer.

---

## 6. Line-ending translation (`src/proto/line_endings.c`)

`--map-out` (host→device) and `--map-in` (device→host) select one of six rewrite modes:
`none`, `cr`, `lf`, `crlf`, `cr-crlf`, `lf-crlf` (`eol_parse`, `line_endings.c:43`). Both
directions are **streaming**: bytes can arrive split across reads (a `CR` at the end of one
chunk, the `LF` at the start of the next), so a one-byte `saw_cr` latch in a `zt_eol_state`
carries the "previous byte was CR" flag across calls. `ZT_EOL_NONE` is a fast `memcpy`
passthrough (`line_endings.c:76`).

**Outgoing** (`eol_translate_out`, `line_endings.c:74`). `cr` maps `LF→CR`; `lf` maps `CR→LF`;
`cr-crlf` and `lf-crlf` expand a single terminator to `CRLF`. The `saw_cr` latch matters only
for `crlf` mode (`line_endings.c:103-122`): a user-typed `CR` emits `CRLF` and sets `saw_cr`;
a following `LF` that was the second half of that typed `CRLF` is then *skipped* so the result
is one `CRLF`, not `CRLFLF`. Any non-terminator byte clears the latch.

**Incoming** (`eol_translate_in`, `line_endings.c:154`). `cr` maps `CR→LF` (old-Mac firmware);
`lf` maps `LF→CR`. The interesting cases are `crlf`/`lf-crlf` (collapse `CRLF→LF`) and
`cr-crlf` (collapse `CRLF→CR`), both at `line_endings.c:179-218`. A lone `CR` sets `saw_cr` and
emits nothing yet; the next byte decides:

- `LF` → emit the single collapsed terminator and clear the latch;
- another `CR` → emit `\r` and *keep* `saw_cr` set (so a run of `CR`s each resolve correctly);
- anything else → emit the held `\r` followed by the byte, clear the latch.

This held-CR design is why a `CR` arriving as the last byte of a chunk produces no output until
the next chunk — exactly the cross-read coalescing the latch exists to provide.

All output is bounded by the `EMIT` macro (`line_endings.c:25`), which refuses to write past
`out_cap` and returns the bytes produced so far — see the bounds requirement in
[INVARIANTS §5](../invariants/INVARIANTS.md). The caller sizes the buffer with
`ZT_EOL_OUT_CAP(n)` (§4).

---

## 7. Reconnect & fd lifecycle (`src/ext/reconnect.c`, `src/serial/port_discover.c`)

Reconnect is on by default (`--reconnect`; ADR-0004) and is built around the §3 pause/unpause
dance so the optional reader thread never reads a stale fd. See
[../design/THREADING_AND_RECONNECT.md](../design/THREADING_AND_RECONNECT.md).

**The wait loop** (`run_reconnect_loop`, `reconnect.c:107`). On entry it:

1. `rx_thread_pause(c)` for the whole disconnect window — the worker's dup of the old fd would
   otherwise keep returning `EIO` and spinning (`reconnect.c:108-110`).
2. `close`s `c->serial.fd` and sets it to `-1`, fires the `disconnect` hook, marks
   `c->tui.disconnected`, and drops in the modal popup.
3. Loops at `ZT_RECONNECT_MS` (1000 ms) granularity, keeping the UI live: `poll`ing stdin so
   PgUp/PgDn scrollback, in-app selection + OSC 52 copy, scrollback search, `Ctrl+A r` force
   retry, and `Ctrl+A x`/`q` quit all keep working while disconnected. Other `Ctrl+A` keys are
   gated off by `c->tui.disconnected` so a stray keystroke can't spawn a log file mid-outage.
4. Each tick calls `reconnect_attempt`; on success it `rx_thread_unpause(c)` (the worker dups
   the *new* fd), clears `disconnected`, flashes "reconnected", and fires the `connect` hook
   (`reconnect.c:204-218`).

**`reconnect_attempt`** (`reconnect.c:40`) re-runs discovery (`port_rediscover`) then a single
`try_reopen_serial`; the loop is the retry mechanism. Its **precondition** is that the RX
worker is already suspended — both `run_reconnect_loop` and the `Ctrl+A r` handler in
`input.c` honour that (`reconnect.c:33-39`).

**Discovery** (`port_discover.c`). `port_rediscover` (`port_discover.c:159`) is a no-op unless
`--port-glob` or `--match-vid-pid` was given. It calls `port_discover`, which globs candidate
paths (the explicit glob, or the `/dev/ttyUSB*`,`/dev/ttyACM*`,`/dev/serial/by-id/*` defaults
when only VID/PID is set) and VID/PID-matches each by walking `/sys/class/tty/<name>/device`
upward for `idVendor`/`idProduct` (`find_usb_ancestor`, `port_discover.c:54`). This is the
hot-plug story: a USB-serial adapter that re-enumerates as a different `/dev/ttyUSBn` after a
replug is transparently re-attached.

> **Two ownership hazards live in this path.**
>
> - [ZT-002](../tracking/issues/ZT-002-port-rediscover-frees-argv-device.md) (🔴): when discovery
>   resolves a *new* path, `port_rediscover` does `free((void*)c->serial.device)`
>   (`port_discover.c:171`). If `c->serial.device` is still the non-heap `argv[optind]`
>   positional, that frees a non-heap pointer → heap corruption/abort on the first reconnect.
>   This is one instance of the cross-cutting mixed-ownership theme with
>   [ZT-001](../tracking/issues/ZT-001-profile-load-frees-argv-device.md); see [INVARIANTS §1](../invariants/INVARIANTS.md).
>   Fix direction: make `c->serial.device` single-owned (always `strdup`).
> - [ZT-005](../tracking/issues/ZT-005-autobaud-strands-fd.md) (🟠): a *failed* `Ctrl+A A`
>   autobaud leaves `c->serial.fd == -1` (the `A` handler closes the fd up front,
>   `src/loop/input.c:130`). The main `poll()` then ignores the dead fd, so the reconnect path
>   in §7 never fires while the HUD still shows "connected" — the link is stranded. Fix
>   direction: drive reconnect/reopen on autobaud failure.

A related drain bug lives in the non-threaded `POLLHUP` path: it reads the device exactly
*once* before entering `run_reconnect_loop` (`src/loop/runtime.c:154-162`), so buffered RX can
be lost at unplug — [ZT-027](../tracking/KNOWN_ISSUES.md) (⚪); fix direction is to drain in a
loop like the `POLLIN` path does.

---

## 8. Scrollback ring + selection (`src/log/scrollback.c`)

**Ring.** Scrollback is a fixed-size array of `char*` lines, `ZT_SCROLLBACK_CAP = 10000`
(`src/zt_ctx.h:61`), with `sb_head` and `sb_count`. `scrollback_push` (`scrollback.c:170`)
`malloc`s a NUL-terminated copy of the current line; while not full it appends at
`(sb_head + sb_count) % CAP` and bumps `sb_count`; once full it overwrites `sb_head`, frees the
evicted line, and advances `sb_head` — a classic overwrite ring. History is in-memory only and
is freed at exit (`scrollback_free`, `scrollback.c:189`); nothing is persisted to disk
(ADR-0006).

The viewport is addressed by `line_from_bottom` (0 = newest). `redraw_scrollback`
(`scrollback.c:199`) walks the visible rows mapping each to a ring index
`(sb_head + sb_count - 1 - line_from_bottom) % CAP`, draws each via `emit_colored_line`,
overlays the selection highlight, and paints the right-edge scrollbar thumb (geometry at
`scrollback.c:287-315`).

**Selection** is anchored to `line_from_bottom` indices, not screen rows, so the highlight
stays glued to the underlying text as the user scrolls (`scroll_up` comment, `scrollback.c:325`).
Column↔byte mapping (`col_to_byte`, `utf8_cols`, `scrollback.c:77/105`) counts one column per
UTF-8 codepoint and uses `skip_esc` (`scrollback.c:46`) to step over embedded SGR/OSC escapes
so colours don't shift the mapping. When timestamps are hidden, both the overlay and the copy
path skip the embedded 15-char `[HH:MM:SS.mmm] ` prefix so columns align with what is actually
drawn.

**`selection_copy` saturation cap** (`scrollback.c:425`). The assembled selection is hard-capped
at `kMaxSel = 256 KiB` (`scrollback.c:481`). The cap arithmetic is deliberately written with
**saturation**: `room = (len < kMaxSel) ? (kMaxSel - len) : 0` (`scrollback.c:482`). The inline
comment records why — a previous version computed `256*1024 - len - 1`, which *underflows to
`SIZE_MAX`* once `len + 1 >= 256 KiB`, producing a huge `be - bs` and a heap overflow inside the
`memcpy`. The current code clamps `be` against `room`, stops growing the buffer at `kMaxSel`,
and breaks out when `room == 0`. After assembly, `strip_escapes` (`scrollback.c:389`) removes
CSI/OSC runs so the clipboard gets pure visible text, then the result goes to `osc52_copy` when
OSC 52 is enabled (else a "selection: N bytes" flash). This bounds-safety is part of
[INVARIANTS §6](../invariants/INVARIANTS.md).

---

## 9. HTTP/SSE/WS bridge (`src/net/http.c`)

`--http <port>` starts a self-contained HTTP server bound to **loopback only**
(`INADDR_LOOPBACK`, `http.c:189`) with an embedded HTML live-view, SSE and WebSocket RX
streams, `/metrics`, `/api/state`, and a `POST /tx` send route. It is fully non-blocking and
runs entirely from the main loop's `http_tick` — never its own thread. See
[../design/HTTP_BRIDGE.md](../design/HTTP_BRIDGE.md) and the trust-boundary rules in
[INVARIANTS §7](../invariants/INVARIANTS.md).

> **`--http <port>` is parsed with `atoi`, unvalidated** — truncated to a `uint16_t` with no
> range check (`src/main.c:586`, [ZT-020](../tracking/KNOWN_ISSUES.md), ⚪).

**Connection table.** A fixed `g_conn[HC_MAX]` with `HC_MAX = 16` slots (`http.c:159-160`),
each tagged `HC_NEW` / `HC_SSE` / `HC_WS`. The listen socket is `O_NONBLOCK`.

**Non-blocking accept + the HC_NEW pump.** `http_tick` (`http.c:985`) first drains all pending
accepts via `accept4(..., SOCK_CLOEXEC | SOCK_NONBLOCK)` into free `HC_NEW` slots (no free slot
→ politely `close` so the kernel backlog drains, `http.c:831`). It then pumps every `HC_NEW`
slot with `hc_pump_new` (`http.c:949`), which reads what's available into a per-connection
`req_buf` (4 KiB cap) and classifies once it sees `\r\n\r\n`. The header-read is fully
non-blocking and idle slots are dropped after `HC_HEADER_TIMEOUT_S = 5.0` s — this replaced an
earlier synchronous read that let a slowloris client freeze the whole UI/serial loop for up to
200 ms per connection (`http.c:137-147`). Keeping all network work non-blocking and off the
loop's critical path is [INVARIANTS §3](../invariants/INVARIANTS.md).

**`classify_request`** (`http.c:838`) dispatches by request line: `OPTIONS` → CORS preflight;
`GET /stream` → SSE (promote slot to `HC_SSE`); `GET /ws` → WebSocket upgrade (RFC 6455 SHA-1 +
base64 handshake, `http.c:795`, promote to `HC_WS`); `GET /metrics`, `GET /api/state`,
`POST /tx`/`POST /api/send`, and webroot/`kIndex` static serving each respond and close.

**SSE vs WS broadcast.** `http_broadcast` (`http.c:1017`) is called from `rx_ingest` (§4) for
every RX chunk and fans out to both stream types — but they are handled asymmetrically:

- **SSE** (`HC_SSE`): the bytes are base64'd into a `data: …\n\n` event and `write()`n.
  `EAGAIN`/`EWOULDBLOCK` → drop this event rather than block; any other error or a short write
  → `hc_close` the peer (`http.c:1028-1035`).
- **WS** (`HC_WS`): `ws_frame_text` (`http.c:995`) frames and writes — but its `zt_write_all`
  return value is **ignored** (`http.c:1037`). Dead WS peers are therefore never detected or
  closed, unlike SSE. This is [ZT-009](../tracking/issues/ZT-009-ws-broadcast-ignores-errors.md)
  (🟠) and [ZT-017](../tracking/KNOWN_ISSUES.md) (⚪, fd-leak / 16-disconnect DoS): partial
  frames also corrupt the stream. Fix direction: check the write and close on error, mirroring
  SSE.

> **Broadcast (fixed):** `http_broadcast` / `http_broadcast_tx` loop over the whole payload in
> ≤4096-byte segments for both SSE and WS, so RX bursts larger than 4 KiB are no longer truncated
> — closed [ZT-007](../tracking/issues/ZT-007-http-broadcast-truncates-4k.md).

**Origin-pinned trust boundary.** The bridge validates `Host`/`Origin` and can require a token:

- `POST /tx` / `POST /api/send` (`classify_request`) pin `Host`/`Origin` to a loopback literal
  (`403` on a foreign origin / rebound host) and, with `--http-token`, require
  `Authorization: Bearer <token>` (`401` otherwise) →
  closed [ZT-004](../tracking/issues/ZT-004-unauth-http-tx-csrf.md).
- The `/ws` upgrade and `/stream` SSE validate `Origin` via the same `request_origin_ok` helper →
  closed [ZT-013](../tracking/KNOWN_ISSUES.md).
- `--http-cors` advertises only `GET, OPTIONS` — never `POST` to `*` (`cors_block`).
- One-shot responses use the bounded, EAGAIN-aware `http_write_all` (not the EINTR-only
  `zt_write_all`), so a large `--webroot` file isn't truncated on `EAGAIN` →
  closed [ZT-011](../tracking/KNOWN_ISSUES.md).

These realize the local-IPC trust-boundary rules in [INVARIANTS §7](../invariants/INVARIANTS.md).

---

## 10. Clipboard worker (`src/proto/clipboard.c`)

zyterm can own the X11 `CLIPBOARD` selection natively, with **zero build dependency** on
libxcb. Two layers make that work: runtime `dlopen` and a background selection-owner thread.

**Runtime `dlopen`** (`xcb_load`, `clipboard.c:212`). On first use the module
`dlopen`s `libxcb.so.1` (falling back to `libxcb.so`) and resolves the ~15 functions it needs
via `dlsym` into a function-pointer table (`xcb_api_t`, `clipboard.c:180`). If the library is
absent (headless, pure Wayland without XWayland, BSD), or **any** symbol fails to resolve,
`xcb_load` returns false, `clipboard_native_set` marks `init_failed`, and the caller falls
through to OSC 52 / external helpers. The module hand-defines just enough of the xcb/X11 wire
ABI (32-byte event structs, `intern_atom` reply, screen layout) to drive a selection owner
(`clipboard.c:55-176`) — no headers, no `-lxcb`, no pkg-config.

**Why a live thread is required** (`clipboard.c:5-23`). X11's `CLIPBOARD` is owned by a *live*
process: when another app pastes, the X server forwards a `SelectionRequest` to the current
owner, which must reply with the bytes. If the owner exits, the clipboard goes empty. So
`clipboard_native_set` (`clipboard.c:458`), guarded by `DISPLAY` being set, lazily spawns one
detached `worker_main` thread on first copy.

**Worker thread** (`worker_main`, `clipboard.c:361`). Opens a *second* X connection (separate
from any host-TTY X code), interns all atoms in one round-trip batch, creates a 1×1 off-screen
`InputOnly` window to act as owner, then `poll()`s on the X fd plus a self-pipe wake fd
(`clipboard.c:417`). On each wake it claims `CLIPBOARD` ownership if `need_claim` is set, then
drains X events: `SelectionRequest` → `handle_selection_request`; `SelectionClear` (someone
stole the selection) → free the buffer under the mutex.

**Mutex-guarded handoff.** Shared state lives in a single `g` struct with a
`PTHREAD_MUTEX_INITIALIZER` mutex (`clipboard.c:275-294`). `clipboard_native_set` copies the
caller's bytes, swaps them into `g.buf`/`g.len` under the lock, sets `need_claim`, unlocks, and
`wake_worker()`s via the self-pipe. The worker snapshots the buffer under the lock before
replying so it never serves X requests while holding it for the duration of a network round
trip.

> **Unchecked `malloc` in the X worker.** The snapshot in `handle_selection_request`
> (`clipboard.c:333`) is `memcpy(malloc(g.len), g.buf, g.len)` with no NULL check — an OOM in
> the worker crashes via a NULL `memcpy`. This is [ZT-014](../tracking/KNOWN_ISSUES.md) (⚪);
> fix direction: split the allocation out and NULL-check it. It is part of
> [INVARIANTS §1](../invariants/INVARIANTS.md) (allocation-failure handling).
