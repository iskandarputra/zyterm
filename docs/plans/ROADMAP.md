# Roadmap

Planned and proposed **features** — ideas, not shipped. Everything below is forward-looking; if
you want to know what works *today*, read [reference/](../reference/) and
[tracking/STATUS.md](../tracking/STATUS.md), not this file. Reliability/fix work lives in
[RELIABILITY_HARDENING.md](./RELIABILITY_HARDENING.md); this doc is net-new capability.

Each item notes rough **impact** (how much it changes what zyterm can do) and **effort** (build
cost), and grounds itself in the code that already exists. Nothing here is a commitment.

_Last updated: 2026-06-03._

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

---

## Mid-term: new subsystems

### Real multi-pane
- **Impact: high · Effort: high**

`src/ext/multi.c` is a **stub today**: `multi_render()` is a no-op (`src/ext/multi.c:113`), it is
not keybound, not discoverable, and `multi_tick` only prefix-tags extra devices into the shared
log (`src/ext/multi.c:96`). Real multi-pane needs row-addressed rendering that the current
single-pane render path doesn't provide (the stub's own comment says so), independent
scrollback per pane, focus routing for keystrokes, and a split layout that survives `SIGWINCH`.
This is a renderer refactor first, a feature second. Do **not** advertise multi-pane until this
lands; the stub is tracked in [STATUS.md](../tracking/STATUS.md).

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

### Wire up — or delete — the epoll/splice fast path
- **Impact: low · Effort: low (decision), medium (if built)**

`src/serial/fastio.c` is **entirely unwired**: no call site, the runtime uses `poll(2)`, and the
`--epoll` flag was removed in 1.2.0. The deferral rationale is ADR
[0003-epoll-splice-fastpath-deferred](../decisions/0003-epoll-splice-fastpath-deferred.md).
This needs a decision, not drift: either wire `epoll` + `splice` into the runtime behind a flag
*with a benchmark that justifies it over the existing `--threaded` SPSC reader*, or delete the
module so it stops reading as a shipped feature. Leaving dead code that looks like a feature is
exactly the failure mode the docs are trying to end. Tracked as a stub in
[STATUS.md](../tracking/STATUS.md).

---

## Explicitly not planned (here)

Dead/broken code is **not** a roadmap item to "advertise" — the fuzzy finder
(`Ctrl+A .`, non-functional, ZT-008), OSC 8 hyperlinks (dead `osc8_rewrite`, ZT-019), and the
multi-pane stub are either repaired through the work above or removed. Their current broken
status lives in [tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md) and
[tracking/STATUS.md](../tracking/STATUS.md), never in feature copy.
