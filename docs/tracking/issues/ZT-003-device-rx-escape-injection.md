# ZT-003: device RX bytes echoed to the operator terminal verbatim (escape injection)

- **Severity:** 🔴 high (a hostile/compromised device can drive the operator's terminal — clipboard
  hijack, title injection, screen spoofing)
- **Area:** render (RX pipeline) / security
- **Status:** open  (recorded 2026-06-03; not yet fixed)
- **Location:** `src/render/render.c:93` (the verbatim `ob_write` in `emit_colored_line`),
  reached from `render_rx` (`src/render/render.c:224`, byte loop ~248–288)

## Root cause

Bytes read from the serial device are forwarded to the operator's terminal with **no escape-sequence
filtering**. The only byte ever dropped on the display path is carriage return.

In `render_rx()` the per-byte loop accumulates device output into `c->log.line` after stripping just
`\r` and treating `\n` as a line boundary:

```c
if (b == '\r') continue;            /* src/render/render.c:275 */
if (b == '\n') { flush_line(c); continue; }
if (c->log.line_len < ZT_LINEBUF_CAP)
    c->log.line[c->log.line_len++] = b;   /* ESC, OSC, CSI, … all pass through */
```

Every other byte — including `0x1B` (ESC) and the OSC introducer — is buffered as-is. `flush_line()`
then hands the line to `emit_colored_line()`, which writes the device content straight out:

```c
ob_write(content, clen);            /* src/render/render.c:93 — verbatim device bytes */
```

`ob_write()` is a thin buffered passthrough — it `memcpy`s into the stdout buffer and `ob_flush()`
does `zt_write_all(STDOUT_FILENO, …)` (`src/core/core.c:150`, `:172`). No sanitization happens
anywhere between the device read and the terminal. The "color" applied by `emit_colored_line` is
zyterm's own SGR wrapper around the payload; it does not neutralize escapes embedded *inside* the
payload.

Because the host terminal interprets those bytes, a device that emits attacker-controlled output can:

- **Hijack the clipboard** via OSC 52 (`ESC ] 52 ; c ; <base64> BEL`) — set the operator's clipboard
  to arbitrary content (e.g. a malicious command) for a later paste. This is the same OSC 52 surface
  zyterm itself uses for `Y`/yank, now driven by the remote side.
- **Inject the window/icon title** via OSC 0/1/2, and on some terminals trigger title-reporting
  read-back used for command injection.
- **Spoof the screen**: cursor moves, clears, and reverse-video let the device forge zyterm's own HUD
  / dialogs or hide output.

The data path is wide open: `rx_ingest` (`src/render/render.c:303`) delivers raw device bytes to
`render_rx`, and the framing/filter decoders also re-enter via `render_rx` with payload content, so
the gap applies to raw mode and to decoded-frame payloads alike.

## Trigger / repro

1. Run `zyterm /dev/ttyUSB0` against any device you can make emit bytes.
2. Have the device send an OSC 52 set-clipboard sequence, e.g.
   `printf '\033]52;c;%s\007' "$(printf 'rm -rf ~/important' | base64)"`.
3. zyterm relays it verbatim; the operator's terminal clipboard now holds the attacker string. A
   title-injection (`printf '\033]0;owned\007'`) or screen-spoof CSI demonstrates the same class.

No exotic firmware is needed — a `cat >/dev/ttyUSB0` from the device side, a malicious peer over
`tcp://`/`telnet://`, or compromised device firmware all suffice.

## Fix direction

Adopt a **default-deny** policy on the RX-to-terminal path: only emit bytes that are safe for an
untrusted source.

- Filter in the `render_rx` byte loop (and the framing/filter re-entry) before content reaches
  `emit_colored_line`: pass printable text and a small allowlist of formatting controls (`\n`, `\t`);
  replace or drop ESC/CSI/OSC/DCS/APC/PM and other C0/C1 controls with a visible placeholder so the
  line is still readable but inert. Hex mode already renders bytes non-executably and is unaffected.
- Keep zyterm's *own* SGR/decoration emission (it is trusted), but never let device-originated escape
  bytes through unmediated.
- Gate any "raw passthrough" of device escapes behind an explicit, off-by-default operator opt-in
  (mirror the existing KGDB/SGR passthrough gating), and surface in the HUD when it is on.
- Record the rule in `INVARIANTS §6` (terminal output & escape-sequence safety): hostile device RX is
  never echoed verbatim. Note this is distinct from the dead OSC 8 rewrite path (ZT-019).

## Verify

- After the fix, the OSC 52 / title / cursor-spoof repros above must leave the operator's clipboard,
  title, and cursor untouched; the sequence should appear as inert placeholder text in scrollback.
- Add a pty test that feeds known OSC 52 / OSC 0 / CSI sequences as device RX and asserts the bytes
  written to the slave terminal contain no raw `0x1B`-introduced control sequences from the device.
- Verify the raw-passthrough opt-in still allows full escapes when explicitly enabled, and that the
  HUD reflects the unsafe mode.
