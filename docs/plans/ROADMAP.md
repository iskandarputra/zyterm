# Roadmap

Planned and proposed **features** — ideas, not shipped. Everything below is forward-looking; if
you want to know what works *today*, read [reference/](../reference/) and
[tracking/STATUS.md](../tracking/STATUS.md), not this file. Reliability/fix work lives in
[RELIABILITY_HARDENING.md](./RELIABILITY_HARDENING.md); this doc is net-new capability.

Each item notes rough **impact** (how much it changes what zyterm can do) and **effort** (build
cost), and grounds itself in the code that already exists. Nothing here is a commitment.

The **2026-07-03 re-review** (v1.4.0) re-affirmed several items already here — DTR/RTS + auto-reset
recipes, the capture-group expect engine + `--extract`, `--send-file` pacing, `rfc2217://`, and
persistent history — and added four net-new ones: **interactive replay controls**, a
**`--plain`/accessible mode**, **expanded observability**, and an **auto-loaded base config with
device-keyed profiles**. Fix/hardening work from that pass is in
[HARDENING_2026-07.md](./HARDENING_2026-07.md).

_Last updated: 2026-07-03._

---

## Near-term: finish what's already half-built

### Expect/automation script engine
- **Impact: high · Effort: medium**

zyterm already fires per-line and per-event hooks (`--on-match /RE/=CMD`, `--on-connect`,
`--on-disconnect`) and a `send:` action injects bytes onto the wire (`src/ext/hooks.c`). The
missing piece is *state*: a stateful trigger→response engine that walks a small script
("wait for `login:`, send user, wait for `Password:`, send pass, on `# ` run a command"), with
timeouts and branches. This is the natural home for finishing the **capture-group TODO**: today
`hooks_on_line` calls `regexec(&h->regex, buf, 0, NULL, 0)` with zero capture slots and the
hook env sets `ZYTERM_MATCH` to the *full* line (`src/ext/hooks.c:115`, `:241`, commented
"full match; group 1 left for v2"). The expect engine should compile regexes with capture
groups and expose `\1..\N` to both `send:` substitution and the shell-action env.

Builds on: hooks (`src/ext/hooks.c`), `direct_send` (`src/loop/send.c:109`).

### Capture-group field extraction → CSV / JSONL
- **Impact: medium · Effort: low** (once capture groups exist)

Once `--on-match` captures groups, add a sink that writes extracted fields to a structured file
— e.g. `--extract '/temp=([0-9.]+) hum=([0-9.]+)/=sensors.csv'` appends a timestamped row per
match, or `.jsonl` for one object per line. This reuses the existing NDJSON log writer
(`log_json`) and the regex machinery; it turns zyterm into a lightweight serial→tabular logger
without a separate `awk` pipeline. Strictly downstream of the expect engine's capture-group
work above.

### DTR/RTS control + bootloader auto-reset recipes
- **Impact: high · Effort: medium**

zyterm **reads** the modem control lines (`TIOCMGET` in `src/serial/tty_stats.c:89`, surfaced in
the HUD and the settings menu) but never **sets** them — there is no `TIOCMSET`/`TIOCMBIC`/
`TIOCMBIS` anywhere. Adding line control unlocks the single most-requested embedded workflow:
the DTR/RTS reset/boot dance. Plan:
- `Ctrl+A` toggles for DTR and RTS, plus `--dtr on|off`, `--rts on|off` at startup;
- canned auto-reset recipes selectable by board — **ESP32** (the classic RTS=EN / DTR=GPIO0
  two-line sequence), **Arduino** (DTR pulse), and **NXP**-style boot-mode entry;
- expose the recipe as `--reset esp32` so a device can be put into bootloader without unplugging.

Builds on: `tty_stats` modem-line plumbing (`src/serial/tty_stats.c`), serial open
(`src/serial/serial.c`).

### `--send-file` with line/chunk pacing
- **Impact: medium · Effort: low**

A first-class file sender: stream a file to the device with configurable pacing —
`--send-file fw.txt --pace-line 20ms` or `--pace-chunk 64`. The trickle path already paces
byte-by-byte with an inter-byte delay (`trickle_send`, `src/loop/send.c:79`, using
`ZT_FLUSH_DELAY_US`); `--send-file` generalizes that to line/chunk granularity with a progress
indicator, for hardware that can't keep up with line-rate paste. Distinct from the binary
transfer protocols (XMODEM/YMODEM/ZMODEM) which already work for framed transfers.

### Interactive replay controls (pause / step / seek)
- **Impact: medium · Effort: medium**

Replay is a linear playback with only a global speed knob today (`--replay <file>`,
`--replay-speed <x>`). The recorder (`--rec`, `src/log/record_cast.c`) already captures per-event
timestamps, so the timing data for scrubbing exists. Give `--replay` an interactive transport:
Space to pause/resume, arrow/`j`-`k` to step one event or one second, and a "go to +NNs" prompt that
fast-forwards to a timestamp (reusing the scrollback/search UI in `src/tui/`). Replay opens no device,
so all the serial-fd guards are irrelevant — this is a self-contained render-loop addition that turns
replay from "watch it again" into a post-mortem debugger for captured sessions.

### `--plain` accessible mode + consistent `NO_COLOR`
- **Impact: medium · Effort: small**

The interactive UI is inherently visual (colour-coded HUD, Unicode sparkline, cursor-addressed
regions) and `NO_COLOR`/`TERM=dumb` are honoured only for `--help`, not the running UI. Add a
`--plain`/accessible mode that renders RX as a flat, timestamp-prefixed line stream with no
cursor-addressed HUD, no sparkline, and no SGR — screen-reader and pipe friendly while keeping input,
logging, and hooks live. Honour `NO_COLOR` across the whole runtime, and offer a monochrome/
high-contrast HUD that drops the dim-grey line-state escapes in `tty_stats_modem_str`
(`src/serial/tty_stats.c`). Small, self-contained, and widens the audience.

### Expanded observability: richer metrics + NDJSON event stream
- **Impact: medium · Effort: medium**

The Prometheus exporter emits only 6 counters (`src/net/metrics.c`) even though
`kern_parity_err`/`kern_brk`/`kern_buf_overrun` are already tracked and never exported, and there is
no uptime, reconnect count, `build_info`, or SPSC-drop gauge. Extend the snapshot with those counters
(plus the `rx_dropped_bytes_total` the hardening plan adds) and a `zyterm_build_info{version=…}`
gauge, growing/guarding `buf[2048]` so lines can't truncate. Complement it with an opt-in
`--events <fd|file>` that emits one NDJSON object per lifecycle event (connect, disconnect, reconnect,
match, crc_err, tx_stall) so a supervising script reacts to structured events instead of screen-
scraping. Both reuse existing counters and the NDJSON writer — libc-only, no new deps.

---

## Mid-term: new subsystems

### Real multi-pane
- **Impact: high · Effort: high**

This is **greenfield** — the earlier non-functional `src/ext/multi.c` stub (a no-op `multi_render`,
file-static pane state, a blocking pane read) was removed in the 2026-07 architecture cleanup, so
there is no code to mislead. Real multi-pane needs row-addressed rendering that the current
single-pane render path doesn't provide, independent scrollback per pane, focus routing for
keystrokes, and a split layout that survives `SIGWINCH`. This is a renderer refactor first, a
feature second. Do **not** advertise multi-pane until this lands.

### Modbus-RTU / NMEA-0183 decode views
- **Impact: medium · Effort: medium**

Protocol-aware decode views layered on the existing framing pipeline
(`src/proto/framing.c`): a **Modbus-RTU** view that parses function code / address / CRC-16 and
renders register reads/writes in a table, and an **NMEA-0183** view that parses `$GP…*hh`
sentences and validates the checksum. These slot in next to the current frame decoders as
display modes (like hex view), reusing the CRC engine for validation. Read-only decode first;
request injection can follow.

### REST/WS control plane over the existing bridge
- **Impact: high · Effort: medium · Blocked on security**

Promote the HTTP bridge (`src/net/http.c`) from a read-mostly view + raw `/tx` into a real
control plane: structured endpoints to set baud/framing, start/stop logging, fire macros, run
expect scripts, and subscribe to typed events over WebSocket. **Hard prerequisite:** the bridge
must authenticate first — this depends on **ZT-004** (token + Origin/Host on state-changing
routes) and **ZT-013** (WS Origin check) from
[RELIABILITY_HARDENING.md](./RELIABILITY_HARDENING.md) Phase 2. Shipping a richer write API
before auth lands would widen an already-open hole. See
[INVARIANTS §7](../invariants/INVARIANTS.md).

---

## Persistence & deferred work

### Persistent history + saved snippets
- **Impact: medium · Effort: low**

Command history and bookmarks are **in-memory only** today and are lost on exit — there is no
`~/.zyterm_history`, `~/.zyterm/history`, or bookmarks file; `history_*` and `bookmarks.c` never
touch disk. This was a deliberate v1 scope decision (see ADR
[0006-in-memory-history-and-bookmarks](../decisions/0006-in-memory-history-and-bookmarks.md)).
The planned feature: opt-in persistence (`~/.config/zyterm/history`, alongside the existing
profile dir) plus **saved snippets** — named, reusable command strings the operator can recall,
distinct from F-key macros. Superseding ADR-0006 is the right way to record the reversal if/when
this ships.

### Auto-loaded base config + device-keyed profiles
- **Impact: medium · Effort: medium**

Config only loads via an explicit `--profile <name>` (`src/main.c`); there is no auto-loaded
`~/.config/zyterm/config.conf` applied to every run. `profile_save`/`profile_load` are also lossy and
asymmetric — `flow` is documented in the `profile.c` header but is neither written nor parsed, so flow
control silently fails to round-trip, and watches/macros/hooks aren't persisted at all. The plan: (1)
auto-load `config.conf` as base defaults with CLI flags overriding, reusing the existing INI parser;
(2) allow a profile to be keyed to a device path so a known adapter auto-selects its settings on
connect; (3) close the round-trip gaps (persist `flow`, watches, macros). All of it stays libc-only
INI under the dir `profile.c` already manages. Pairs with the persistent-history work above.

### Finish `rfc2217://`
- **Impact: medium · Effort: medium**

`rfc2217://` is an intentional stub: `transport_open()` calls `zt_die` with
"rfc2217:// is not yet implemented; for now use ser2net in raw mode and connect with tcp://…"
(`src/serial/transport.c:95`). The deferral is recorded in ADR
[0005-rfc2217-deferred](../decisions/0005-rfc2217-deferred.md). Finishing it means implementing
the RFC 2217 Telnet COM-Port-Control option negotiation (baud/parity/data/stop/flow over the
control channel) on top of the existing telnet transport and IAC filter
(`telnet_rx_filter`, `src/serial/transport.c:163`), so a remote port can be configured from
zyterm's own CLI flags instead of pre-configuring ser2net.

### (Maybe) a `splice`-to-logfile fast path
- **Impact: low · Effort: medium**

The unwired `src/serial/fastio.c` (epoll + splice) was **deleted** in the 2026-07 architecture
cleanup — it was dead code masquerading as a feature. The `epoll` half was never worth it over the
existing `--threaded` SPSC reader for zyterm's tiny fd set. The one idea worth keeping is
`fastio_splice_log()`: on the raw-`--dump`/log-only path (no rendering) a `splice(2)` could move
serial→logfile with zero userspace copies. If that's ever built, do it as a small, benchmarked,
self-contained addition on the dump path — not a general epoll runtime. Rationale + old code:
[ADR-0003](../decisions/0003-epoll-splice-fastpath-deferred.md) and git history.

---

## Explicitly not planned (here)

Dead/broken code is **not** a roadmap item to "advertise" — the fuzzy finder
(`Ctrl+A .`, now functional, ZT-008) is repaired rather than advertised; the OSC 8 hyperlink
rewriter + its inert toggle (`osc8_rewrite`, ZT-019) and the `fastio.c` / `multi.c` dead units were
all deleted in the 2026-07 cleanup. Their status
lives in [tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md) and
[tracking/STATUS.md](../tracking/STATUS.md), never in feature copy.
