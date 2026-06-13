# ADR-0009: Device RX SGR colour is allowed by default through a bounded SGR-only filter

- **Status:** accepted
- **Date:** 2026-06-13
- **Supersedes:** [ADR-0008](0008-device-rx-escape-default-deny.md) (in part — the default policy)

## Context

[ADR-0008](0008-device-rx-escape-default-deny.md) established that device RX escapes are
default-denied: ESC and other controls are rewritten to inert caret notation, and the only way
device escapes reach the terminal is the `passthrough` / `sgr_passthrough` opt-ins. ADR-0008
explicitly *rejected* its option 2 — "parse and allow a curated subset (e.g. SGR colour)" — on the
grounds that a partial escape parser over hostile input grows bugs, and that zyterm already
re-colours lines with its own palette.

Two things have since changed the calculus:

1. **The `sgr_passthrough` flag never did what its name said.** `render_rx` gated on
   `raw_ok = passthrough || sgr_passthrough`, which disabled neutralization for *all* escapes — so
   "SGR Passthrough = On" actually forwarded OSC 52, title, cursor and screen control, identical to
   raw passthrough. The function meant to isolate SGR (`sgr_filter`) was dead, no-op code. The
   feature was mislabeled and gave no SGR-specific safety. (Filed as ZT-029.)
2. **Operators reasonably expect coloured device logs to render.** The most common real device
   output is level-coloured log lines (`<wrn>` yellow, `<err>` red). Rendering those as `^[[1;33m…`
   by default reads as a bug, and zyterm's own palette only colours formats it recognizes.

Crucially, ADR-0008's objection was about the *parser surface*, not about SGR being dangerous. SGR
(`CSI … m`) is in fact the one escape class that cannot drive the terminal: it sets
colour/weight/underline only — it cannot move the cursor, clear the screen, write the clipboard,
set the title, or report state back. The risk lived in the parser, so the right answer is to make
the parser small, bounded, pure, and exhaustively tested — not to forgo SGR entirely.

## Decision

**Device SGR colour renders by default through a bounded SGR-only filter; every other escape stays
neutralized. Full raw passthrough remains a separate, off-by-default opt-in.**

`render_rx` (`src/render/render.c`) now selects one of three modes per chunk:

| Mode | Selector | Behaviour |
|------|----------|-----------|
| RAW | `proto.passthrough` (default off) | All escapes verbatim — KGDB / full-screen TUI escape hatch. |
| SGR_FILTER | `proto.sgr_passthrough` (**default ON**) | Only well-formed SGR passes; everything else neutralized. |
| STRICT | neither | Neutralize every escape — the ADR-0008 posture, reachable via `--no-sgr` or toggling `E` off. |

`passthrough` wins over `sgr_passthrough`. The filter (`sgr_feed`, `src/proto/sgr_passthrough.c`)
is a pure, caller-owned-state CSI state machine (NONE → ESC → CSI). It admits a sequence *only* if
it is `ESC [ <params> m` where every parameter byte is a digit, `;`, or `:`. Any private-parameter
marker (`< = > ?`) or intermediate (0x20–0x2F) — even with an `m` final, e.g. `CSI ? 1 m` — is
neutralized. The parameter buffer is fixed (`ZT_SGR_PARAM_CAP` = 64) and overflow aborts to inert,
so the parser can neither overrun nor buffer unbounded hostile input. State lives in `proto.sgr`,
so a sequence split across `read()` chunks resumes correctly. Allowed SGR is stored verbatim in the
line buffer (so it renders in-position and colours scrollback); a `\033[0m` is appended at flush to
contain colour bleed into the HUD or the next line.

## Consequences

- Coloured device logs render out of the box, while OSC 52 clipboard writes, title injection,
  cursor / erase / alt-screen and DCS remain neutralized to inert caret text. This **narrows** —
  does not remove — [INVARIANTS §6](../invariants/INVARIANTS.md): the default is now "SGR-only
  filter", not "deny all".
- The mislabeled feature is fixed (ZT-029): `sgr_passthrough` is a real SGR filter and the dead
  `sgr_filter` body is replaced by the working `sgr_feed`. The settings row is relabeled
  "Device SGR Filter".
- Scrollback, selection-copy and log files store the line *text* (with the embedded trailing reset;
  `strip_escapes` already removes SGR from copied/searched text), so device colour is a
  live + replay rendering effect and cannot become a re-injection vector — only whitelisted SGR is
  ever stored.
- The strict ADR-0008 posture is preserved, not lost: `--no-sgr` (or toggling `E` off) restores
  deny-all for the security-conscious or for untrusted/unknown devices.
- Accepted residual: 8-bit C1 controls (0x9B CSI, 0x9D OSC, …) are not filtered — that byte range
  carries UTF-8, and modern terminals run UTF-8 where those bytes are invalid continuations, not
  control introducers. This matches STRICT mode's pre-existing behaviour, so it is not a regression.
- Watch patterns and line hooks see raw SGR bytes in the matched line in SGR_FILTER mode (as they
  already did under passthrough); device *text* content is unchanged, so substring patterns still
  match.
