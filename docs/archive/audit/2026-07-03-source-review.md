# 2026-07-03 source review — zyterm v1.4.0 (wave 2)

**This is history/narrative, not current truth.** It records how the codebase was understood on
2026-07-03. For the *current* status of any finding, follow it through to the tracker — do not cite
this file as the present state.

- Current defect status → [tracking/KNOWN_ISSUES.md](../../tracking/KNOWN_ISSUES.md) (`ZT-030`…`ZT-049`)
- Fix order + non-defect (arch/testability/perf) work → [plans/HARDENING_2026-07.md](../../plans/HARDENING_2026-07.md)
- Net-new features → [plans/ROADMAP.md](../../plans/ROADMAP.md)
- Don't-regress rules → [invariants/INVARIANTS.md](../../invariants/INVARIANTS.md)

This is the sequel to [2026-06-03-source-review.md](./2026-06-03-source-review.md), which drove the
`ZT-001`…`ZT-029` set. It re-reviewed v1.4.0 *after* those fixes landed, so its job was to find
**new, current-code** defects — not to re-report what wave 1 closed.

## Method

A multi-agent review workflow ran 47 agents: 12 finders (7 per-module clusters covering the whole
`src/` tree + 5 cross-cutting lenses — concurrency/signals, memory/fd ownership, security/trust,
error-handling/resource, and parser robustness), **one adversarial skeptic per finding** whose job was
to *refute* it by re-reading the cited code, a 4-agent architecture/scalability/testability/features
panel, and a synthesis pass. Raw output: **30 findings → 7 refuted → 23 kept → 20 after dedup**
(three defects were filed by two agents each). Every `file.c:line` was read by both a finder and an
independent verifier; the four 🔴 defects were re-verified a third time when their detail files were
written. An interactive HTML report of the same results was also produced for the maintainer.

## Executive summary

zyterm is structurally sound and materially healthier than a typical hand-rolled C serial tool: the
single-`ctx` + poll-loop model and the wave-1 audit closed the reachable high-severity defects. This
pass found **no new whole-subsystem rot**. What it found instead is a consistent residual pattern —
**several wave-1 fixes were applied point-wise to one code path and their sibling paths were missed.**
Four genuinely new 🔴 defects survive, three of them one-line/one-guard fixes, that crash, hang, or
corrupt memory. The remaining ~16 are lower-severity residual polish clustered in the 1262-line
`net/http.c` and the under-tested `--threaded` SPSC path. The largest structural gap is **verification,
not code**: the hostile-input parsers (framing, xmodem, the HTTP request parser) and the lock-free
SPSC ring have essentially no unit/fuzz/TSan coverage, and two documented architecture guarantees —
compiler-enforced layering and all-state-in-one-`zt_ctx` — are demonstrably inaccurate.

## Cross-cutting themes

1. **Class-fixes applied to one path, siblings left unfixed** — the defining pattern. ZT-001 → still
   live on `--profile-save` (ZT-030); ZT-027 → gated `if (!threaded)` (ZT-036); ZT-007 → never reached
   the JSONL logger (ZT-040); ZT-006/015 → left the filter's stdin fd blocking (ZT-032); ZT-003 →
   doesn't cover C1 introducers (ZT-041). Fixing bugs *as a class* is the highest-leverage process
   change.
2. **Embedded (`siglongjmp`) mode + a long-lived host turns "harmless on `exit()`" into real crashes,
   leaks, and UAF.** `setup_serial` fd leak (ZT-047), replay device-string leak (ZT-048), clipboard
   wake-pipe leak (ZT-049), and — worst — the orphaned `--threaded` worker writing into the reclaimed
   host stack (ZT-033). The embedding needs one teardown that runs on *all* exit paths.
3. **`--threaded` is a second-class citizen:** final-burst RX discarded on hangup (ZT-036), silent
   byte-drop under backpressure with no counter, byte-by-byte ring copy, and zero concurrency testing
   (no TSan, no test moves a byte through the ring).
4. **Blocking calls inside the single-threaded loop violate INVARIANTS §3** in three places: the filter
   stdin write (ZT-032), the synchronous `connect()` (ZT-037), and `http_write_all`'s per-write
   deadline reset (ZT-044).
5. **`net/http.c` is the correctness weak spot** — one 1262-line TU carrying the trust boundary plus
   several protocol bugs (ZT-034, ZT-035, ZT-039, ZT-043, ZT-044). It should be split along its seams.
6. **Interactive input/render memory-safety around the newly-wired fuzzy finder** (ZT-031, ZT-042),
   both reachable from documented keybindings against device-echoed history content.
7. **Architecture claims outrun reality, and the hostile-input parsers are effectively untested** — the
   layering is unenforced (three `extern` back-edges), ~20 file-statics escape the two-entry embed
   reset, dead surface (`fastio.c`, `osc8_rewrite`, `multi.c`) ships in every binary, and the byte-
   stream decoders have no round-trip/fuzz/coverage tests.

## Verified findings (23 → ZT-030 … ZT-049)

Grouped by severity as recorded on 2026-07-03. IDs link to the tracker for current status; the 🔴 four
have detail files under [`tracking/issues/`](../../tracking/issues/). Three findings were filed twice
(ZT-030, ZT-034, ZT-036) and are merged here.

### 🔴 High

- **ZT-030** `src/main.c:723` — `--profile-save <name> <DEVICE>` aliases the non-heap `argv[optind]`
  into `c.serial.device`; `cleanup_ctx` `free()`s it (`main.c:329`) → `free(): invalid pointer`. The
  ZT-001 twin, still live on the one path that was missed (replay guards it at `main.c:747`).
- **ZT-031** `src/tui/fuzzy.c:65` — the fuzzy finder's Enter branch injects a history line without
  resetting `sent_len` (its twin `load_history_into_buf` does, `input.c:40`); with local echo or after
  Tab, the next Backspace/Ctrl+W runs `memmove` with unsigned `input_len-(sent_len+cursor)` ≈
  `SIZE_MAX` over `input_buf`.
- **ZT-032** `src/ext/filter.c:70` — the `--filter` stdin write end is left blocking, so
  `filter_feed`'s EAGAIN drop-branch (`:127`) is dead and a slow filter blocks `write()` in the poll
  loop → the whole UI (incl. Ctrl+A x) hangs.
- **ZT-033** `src/core/core.c:312` — embedded fatal `siglongjmp` (`zt_die`/`sig_crash`) and
  `zt_embed_reset` skip `rx_thread_stop`, orphaning the `--threaded` worker still writing into
  `zyterm_main`'s reclaimed stack → UAF in the host + dup-fd/1 MiB-ring leak.

### 🟠 Medium

- **ZT-034** `src/net/http.c:1071` — `POST /tx`/`/api/send` ignore `Content-Length` and take the body
  only from bytes buffered when `\r\n\r\n` first appears → a segment-split body is truncated/dropped
  while returning 204 (a partial command can reach the wire).
- **ZT-035** `src/net/http.c:1161` — `/ws` ships raw 8-bit serial in a TEXT frame; conformant browsers
  fail the stream (close 1007) on the first non-UTF-8 byte.
- **ZT-036** `src/loop/runtime.c:189` — the threaded POLLHUP/POLLERR path frees the SPSC ring without
  draining it (the ZT-027 drain is gated `if (!threaded)`), losing the device's final RX burst.
- **ZT-037** `src/serial/transport.c:117` — synchronous `connect()` (socket made non-blocking only
  afterward) freezes the reconnect loop for the kernel connect timeout (~127 s) on a firewalled peer.
- **ZT-038** `src/serial/autobaud.c:52` — a silent/binary device scores 0.0 at every rate; the
  tie-break accepts monotonically higher rates, so autobaud always locks to the top rate and returns
  success, suppressing its own "no printable traffic" warning.

### ⚪ Low

- **ZT-039** `src/net/http.c:984` — unanchored `strstr` route dispatch over the whole request buffer
  before the POST branch → `GET /streamlit.html` served as SSE, `POST /tx` with `"GET /stream"` in the
  body hijacked into an upgrade.
- **ZT-040** `src/log/log_json.c:70` — JSONL escapes only the first 2048 payload bytes but writes the
  true `n` → silent, self-inconsistent records for bursts >2048 B (ZT-007 twin).
- **ZT-041** `src/render/render.c:247` — C1 `0x80–0x9F` introducers pass through `emit_inert_byte`
  verbatim, so a raw 8-bit OSC 52 bypasses the escape filter on C1-honoring terminals. _(plausible)_
- **ZT-042** `src/tui/hud.c:84` — `visible_len()` advances 2/3/4 bytes on a UTF-8 lead without checking
  the continuation bytes exist before NUL → OOB stack read on a mid-glyph-truncated history entry.
- **ZT-043** `src/net/http.c:1147` — established SSE/WS fds are never polled for hangup; on an idle link
  16 half-open clients exhaust `HC_MAX` and the bridge stops serving.
- **ZT-044** `src/net/http.c:195` — `http_write_all` resets its stall deadline on every partial write,
  so a trickle-reading client holds the poll loop inside a ~16.7 KB response for tens of seconds.
  _(plausible)_
- **ZT-045** `src/serial/tty_stats.c:72` — kernel error-counter deltas underflow on reconnect (unsigned
  arithmetic against baselines never reset on fd swap) → a fake ~4.3e9 fault flash.
- **ZT-046** `src/proto/clipboard.c:460` — the X11 worker's in-loop error breaks leave `g.running=true`
  → later copies report "native X11 owner" success while nothing owns the selection.
- **ZT-047** `src/serial/serial.c:160` — `setup_serial` calls `zt_die` on post-open error paths without
  `close(fd)` → fd leak per invocation under embedded `siglongjmp` (EMFILE over time).
- **ZT-048** `src/main.c:736` — the `--replay` branch overwrites `c.serial.device` without freeing a
  prior `--profile`-supplied `strdup`'d device (the ZT-016 free-before-assign was missed here).
- **ZT-049** `src/proto/clipboard.c:494` — the clipboard wake-pipe fds leak on X-worker init failure
  (`init_failed` latches, so later calls early-return and never close them). _(plausible)_

## Refuted (7 — killed by the verify pass)

Recorded to show the bar, and because a few are *real code defects gated behind dead call sites* — worth
knowing when that code is ever wired.

- **XMODEM malformed-block NAK loop** (`src/proto/xmodem.c:206`, filed twice) — the loop mechanics read
  as a potential infinite NAK, but the refuters traced the block refill and concluded it is guarded in
  the current flow. The 510-line engine nonetheless has **zero test coverage** (see the testability
  plan).
- **Passthrough `~.` escape triggers anywhere / drops the chunk** (`src/proto/passthrough.c:52`) — the
  state machine *is* broken, but `passthrough_handle` has **no call site** (dead code). Its file-static
  `state` also bleeds across embedded runs (folded into the arch/file-static work).
- **`loglevel_muted` substring match drops lines** (`src/ext/loglevel.c:21`) — real logic bug, but the
  function has **no production caller** (only tests reference it).
- **`sig_crash` calls non-async-signal-safe `zt_trace`** (`src/core/core.c:298`) — `zt_trace`
  short-circuits before any `fopen`/`malloc` unless `ZYTERM_TRACE`/`/tmp/zyterm.trace` is set, so the
  self-deadlock chain doesn't occur in the default configuration.
- **rx worker doesn't block signals** (`src/loop/rx_thread.c:155`) — true that it inherits an unblocked
  mask, but neither claimed failure (delayed quit / cross-thread `siglongjmp`) is reachable at the
  review bar.
- **`GET /api/state` / `/metrics` skip Origin pinning** (`src/net/http.c:1048`) — accurate code reading,
  but a **deliberate, documented** design decision ([ADR-0007](../../decisions/0007-http-bridge-auth-model.md)),
  not a defect.

## Dead / latent surface

Real code that ships but has no call site today (so it can't currently misbehave, but it is maintenance
and audit burden — and a landmine if wired): `serial/fastio.c` (epoll+splice), `proto/osc.c`
`osc8_rewrite`, and `ext/multi.c` (which reintroduces file-static session state + a blocking loop read
the invariants forbid). Removal/gating and the one real `fastio` win (`splice` on the raw-dump path)
are tracked in [HARDENING_2026-07.md](../../plans/HARDENING_2026-07.md) §7 and
[ROADMAP.md](../../plans/ROADMAP.md).

## Architecture, scalability, testability, features

The 29 forward-looking recommendations were routed into the plans rather than the tracker: the fix,
perf, testability, and architecture-accuracy work into
[HARDENING_2026-07.md](../../plans/HARDENING_2026-07.md) (Phases 2–7), and the four net-new features
into [ROADMAP.md](../../plans/ROADMAP.md). Headlines: make the layering claim real (narrow per-module
headers + a CI lint, resolving the three `extern` back-edges); add the test-injectable output sink that
unblocks framing/xmodem/ring unit tests and a TSan CI leg; batch `log_write` off its per-byte syscall
path; and split `net/http.c` along its seams so the trust boundary is small and independently testable.

_Archived 2026-07-03._
