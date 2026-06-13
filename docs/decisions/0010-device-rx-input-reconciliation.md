# ADR-0010: Tab-completion mirrors via device prompt-line reconciliation

- **Status:** accepted
- **Date:** 2026-06-13

## Context

zyterm shows a local editable input line (`ZY ›`) distinct from the device's echo in scrollback.
Users want a device Tab-completion reflected in that local line (so history and further editing are
correct). The original mechanism captured whatever RX arrived after a Tab and appended it to the
input buffer. On a continuously-logging device (e.g. a Zephyr shell) the asynchronous log stream
interleaves with — or stands in for — the completion echo, so the capture injected log fragments
into the command line (`skycab … esp_` became `skycab … es RN_MSG : STATUS CHECK : SKYCAR NOT
ALIGNED`). That capture was removed as unsafe.

Removing it lost a feature users rely on. The question was how to mirror completions **robustly on a
device that emits asynchronous output**, where a naïve byte capture cannot distinguish a completion
suffix from a log fragment.

## Decision

**Model the device's *current* line and reconcile the input against it, gated to a short post-Tab
window; adopt only an append-only, single-token completion tail.**

- A bounded, pure parser (`devline_feed`, `src/proto/devline.c`) reconstructs the characters on the
  device's current line by interpreting a minimal line discipline — printable, `\r` (col 0), `\b`
  (col−1), `ESC[K` (erase), `ESC[nC`/`ESC[nD` (cursor), `ESC[2J`/`ESC[J` (clear) — and **resets on
  `\n`**. State lives in `c->proto.devline`; the parser is `zt_ctx`-free and unit-tested.
- On Tab (after `flush_unsent`, so `input_buf` holds the exact committed command), a window of
  `ZT_RECONCILE_WINDOW_MS` (500 ms) is armed. While armed, `devline_ingest` finds the command in the
  device line (rightmost/live occurrence) and adopts whatever the device appended after it as the
  completion **tail** — restricted to the **single completed token** (the maximal run of word
  characters: alphanumerics and `_-./`). Restricting the tail to one token is what stops an
  **inline async log line** from leaking in: this device prints logs on the prompt line with no
  preceding newline (`SkyCar:~$ skycab [00512770] <err> …`), so the tail must end at the space / `[`
  before the log — the `\n`-reset alone is not enough. Adoption is **append-only** (the typed prefix
  is never altered or deleted) and **length-capped** (`ZT_RECONCILE_TAIL_MAX` = 256). The window is
  cleared by any input edit or its deadline.

The reconciliation feeds only the **raw** RX stream (tapped in `rx_ingest` upstream of `render_rx`),
in raw mode only (framing/filter modes have no prompt-line concept).

## Consequences

- Tab-completion is mirrored into the local input line again, and an async log burst can no longer
  leak into it: the single-token tail (plus the `\n`-reset and append-only guards) means an inline
  log appended to the prompt line ends the tail at its leading space/`[`, while a newline-delimited
  log resets the model — either way the typed input is left untouched.
- **Trust ([INVARIANTS §6](../invariants/INVARIANTS.md)):** device RX now shapes `input_buf` within
  the post-Tab window. The blast radius is bounded: Enter does `flush_unsent` (a no-op, since
  `sent_len==input_len` after adoption) then sends `\r` — the device already holds the completed
  line it echoed, so reconciliation **never makes zyterm originate bytes to the device**. The
  adopted tail is display + history only. Worst case for a hostile device: ≤256 printable chars
  appended to the user's *exact* typed prefix, only within 500 ms of a Tab, visible before Enter.
  Whitelisting blocks control/escape bytes from ever entering `input_buf`.
- Accepted limitations (safe degradation, never corruption): a completion that *replaces* rather
  than extends the typed prefix is not mirrored (the prefix isn't found → no adoption); a multibyte
  UTF-8 edit can yield "no adoption" rather than a faithful mirror (the tail whitelist requires
  complete UTF-8); under non-default `--map-in` that suppresses `render_rx`'s own line flushing,
  reconciliation simply doesn't fire. In every case the device echo still shows in scrollback.
- Supersedes the interim removal of the echo capture on the same branch.
