# ADR-0008: Device RX escape sequences are default-denied on the render path

- **Status:** accepted
- **Date:** 2026-06-13

## Context

zyterm renders bytes read from the serial device straight into the operator's terminal. The
terminal is a *trust sink*: the bytes it receives can move the cursor, clear the screen, set the
window title (OSC 0/1/2), and write the system clipboard (OSC 52). Device RX is **untrusted** — it
comes from firmware that may be buggy, compromised, or hostile, or over a `tcp://` / `telnet://`
transport from a peer the operator does not control.

The 2026-06-03 audit (ZT-003) found the render path stripped only `\r` and passed everything else —
including `0x1B` (ESC) and the OSC introducer — through to the terminal verbatim. A device that
emits `ESC ] 52 ; c ; <base64> BEL` silently rewrites the operator's clipboard for a later paste;
`ESC ] 0 ; … BEL` injects the window title; cursor/erase/reverse-video sequences let the device
forge zyterm's own HUD or hide output. No exotic firmware is required — a `cat >/dev/ttyUSB0` from
the device side suffices.

The question was the *policy* for device-originated control bytes. Options considered:

1. **Strip them entirely.** Safe, but loses the signal that a control sequence was present and
   complicates byte accounting in the fixed-size line buffer.
2. **Parse and allow a curated subset (e.g. SGR colour).** More faithful, but a partial escape
   parser over hostile input is exactly the kind of surface that grows bugs; and zyterm already
   re-colours lines with its own palette.
3. **Default-deny with a visible, inert rendering, plus an explicit opt-in for raw passthrough.**
   Neutralize the dangerous bytes but keep the line readable, and preserve a sanctioned way for the
   operator to *choose* unfiltered output for a device they trust.

## Decision

**Device RX escapes are default-denied on the normal render path; raw passthrough is an explicit,
off-by-default opt-in.**

- In `render_rx` (`src/render/render.c`), ESC (`0x1B`), DEL (`0x7F`), and other C0 controls (except
  `\t`, with `\r`/`\n` handled separately) are rewritten to inert `cat -v` caret notation (`^[`,
  `^G`, …) before they enter the line buffer, via `rx_line_putc()`. Printable ASCII and UTF-8 high
  bytes pass through, so international text and ordinary output render normally; a sequence that
  *was* an escape shows as readable, non-executable text.
- The only sanctioned way device escapes reach the terminal unmediated is an explicit operator
  toggle — `c->proto.passthrough` (KGDB/raw) or `c->proto.sgr_passthrough` — both off by default.
  The filter is gated on `raw_ok = passthrough || sgr_passthrough`.

## Consequences

- A hostile or buggy device can no longer drive the operator's clipboard, title, or screen through
  RX. This realizes **[INVARIANTS §6](../invariants/INVARIANTS.md)** (terminal output &
  escape-sequence safety) and closes
  [ZT-003](../tracking/issues/ZT-003-device-rx-escape-injection.md).
- zyterm's *own* SGR/decoration emission is unaffected — it is trusted and is applied around the
  (now-inert) payload, not embedded in it. Hex mode already renders bytes non-executably and is
  unchanged.
- Operators who genuinely want a device's colour/cursor escapes (e.g. a trusted board running a
  full-screen TUI) opt in with the passthrough toggles and thereby re-accept the device's escapes —
  a deliberate, surfaced choice rather than a silent default.
- Trade-off: legitimate device colour output is shown as caret notation unless passthrough is on.
  This is the intended default-deny posture; the escape hatch exists for the trusted case. The dead
  OSC 8 rewrite path is a separate concern (ZT-019) and remains uncalled.
