# Design: framing and CRC

How zyterm's frame decoders and CRC engine were designed, and why. This is the
rationale layer — for the line-by-line walk-through see
[../reference/INTERNALS.md](../reference/INTERNALS.md), for the don't-regress rules
see [../invariants/INVARIANTS.md](../invariants/INVARIANTS.md) §5, and for the
encode-time decisions behind length-prefix framing see
[../decisions/0006-in-memory-history-and-bookmarks.md](../decisions/0006-in-memory-history-and-bookmarks.md)
only where bookmarks intersect (they do not — framing is independent).

Source of truth: `src/proto/framing.c`, `src/core/crc.c`.

## Why a framing layer at all

A serial port is a byte stream with no record boundaries. Most embedded protocols
impose their own: COBS and SLIP and HDLC all carve the stream into delimited frames,
and length-prefix protocols announce each frame's size up front. zyterm's framing
module turns that raw byte stream back into *logical frames* so that everything
downstream — scrollback, search, JSON logging, the HTTP broadcast — sees one
completed frame as one entry, exactly as a raw line would arrive.

The design constraint that shaped everything else: framed output must be
**indistinguishable from raw output** once decoded. Rather than fork the pipeline,
`frame_dispatch()` calls the same `render_rx()` that a raw "line arrived from device"
triggers (`src/proto/framing.c:34,85`). One render path, one log path, one broadcast
path — framing is purely a front-end transform.

## The decoders: raw / COBS / SLIP / HDLC / LENPFX

`framing_feed()` (`src/proto/framing.c:225`) is the single RX entry point. It switches
on `c->proto.mode` and routes the chunk to the matching decoder; the `raw` default is
a straight pass-through to `render_rx()` with zero copying or buffering.

| Mode | Selector | Delimiter / shape | Decoder |
| --- | --- | --- | --- |
| `raw` | default | none — pass-through | `framing_feed` default case (`:232`) |
| `cobs` | `ZT_FRAME_COBS` | `0x00` terminator | `feed_cobs` (`:93`) |
| `slip` | `ZT_FRAME_SLIP` | `0xC0` END, `0xDB` ESC (RFC 1055) | `feed_slip` (`:142`) |
| `hdlc` | `ZT_FRAME_HDLC` | `0x7E` flag, `0x7D` escape (byte-async, no bit-stuffing) | `feed_hdlc` (`:171`) |
| `lenpfx` | `ZT_FRAME_LENPFX` | `<len16 LE><payload>` | `feed_len16` (`:194`) |

All four are the textbook RFC versions, chosen for interoperability rather than
cleverness — they decode frames produced by Zephyr MCUmgr, SLIP bridges, and
hand-rolled HDLC stacks without per-stack tweaks. HDLC here is the *byte-asynchronous*
variant (escape + flag, no bit-stuffing), which is what UART-attached HDLC actually
uses; full bit-stuffed HDLC belongs to synchronous links and is out of scope.

### The decoders are streaming, not message-at-a-time

A `read(2)` from the serial port returns whatever the kernel has buffered — it can
split a frame across two reads or pack several frames into one. Every decoder is
therefore a byte-at-a-time state machine that accumulates into `c->proto.buf`
(a fixed `ZT_LINEBUF_CAP` = 4096-byte buffer, `src/zt_ctx.h:349`) and fires
`frame_dispatch()` the instant a frame completes. State that must survive across reads
— the escape flag, the COBS pending count, the LENPFX header bytes and remaining
counter — all lives on `c->proto` so a partial frame at a read boundary resumes
correctly on the next chunk.

Every write into `c->proto.buf` is bounds-checked against `sizeof c->proto.buf` before
the store (e.g. `:116,:150,:157,:184,:211`). A frame longer than the buffer is
truncated rather than overflowed — see [INVARIANTS §5](../invariants/INVARIANTS.md).
The COBS decoder adds a second guard: it decodes *in place* (the encoded form is never
shorter than the decoded form, so the write cursor trails the read cursor), and an
explicit `if (wr > rd + j) break;` defends against a malformed code byte that would
push the write cursor past the read cursor and clobber not-yet-consumed input
(`src/proto/framing.c:115`).

## Why per-ctx accumulator state (not file statics)

The single most important structural decision in this module: **all decoder state
lives on `c->proto`, not in file-static variables.** The fields are
`len`, `escape`, `cobs_pending`, `len16_lenb[2]`, `len16_have`, `len16_need`
(`src/zt_ctx.h:349-365`), reset together by `framing_reset()`
(`src/proto/framing.c:46`).

Earlier revisions kept this state in file statics inside `framing.c`. That works for a
single, never-reset stream, but it breaks two things the rest of zyterm depends on:

- **Mid-stream reset.** Switching framing mode at runtime (`Ctrl+A F` cycles modes —
  see [../reference/KEYBINDINGS.md](../reference/KEYBINDINGS.md)), reconnecting, or
  toggling CRC must abandon any half-accumulated frame cleanly. `framing_reset()` zeroes
  one struct; a file static would have left a stale escape flag or partial COBS buffer
  to corrupt the first frame after the switch. The comments at `:96` and `:195` call
  this out explicitly as the reason the state was moved off statics.
- **Per-pane isolation.** The accumulator belongs to a `zt_ctx`, so any future
  multi-pane decode (each pane its own `zt_ctx`-like context) is isolated by
  construction. (Multi-pane itself is an unwired stub today — see
  [../plans/ROADMAP.md](../plans/ROADMAP.md) — but the framing layer is already correct
  for it, which is why the comments mention it.)

This also keeps the decoder reentrant with respect to the optional reader thread: the
worker only fills the SPSC ring; `framing_feed()` always runs on the main thread
against main-thread-owned state. See
[THREADING_AND_RECONNECT.md](THREADING_AND_RECONNECT.md).

## CRC: three algorithms, table-free, bit-exact

`src/core/crc.c` implements exactly the three CRCs that cover essentially every
embedded protocol in the field:

| Mode | Name | Poly | Init | Reflect | XOR-out | Width | Typical use |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `ZT_CRC_CCITT` | `ccitt` | `0x1021` | `0xFFFF` | no | none | 2 B | XMODEM-CRC, Zephyr, BT HCI |
| `ZT_CRC_IBM` | `modbus` | `0xA001` (refl `0x8005`) | `0xFFFF` | in/out | none | 2 B | Modbus, IBM CRC-16 |
| `ZT_CRC_CRC32` | `crc32` | `0xEDB88320` (refl `0x04C11DB7`) | `0xFFFFFFFF` | in/out | `0xFFFFFFFF` | 4 B | PKZIP, IEEE, Ethernet |

The implementations are deliberately **table-free** (`crc_ccitt`/`crc_modbus`/`crc_32`,
`src/core/crc.c:25-53`): a per-byte loop of eight shift-and-conditional-XOR steps, no
256-entry lookup table. The trade is a few cycles per byte for zero static data and a
tiny footprint, which fits the single-binary, libc-only ethos
([../reference/ARCHITECTURE.md](../reference/ARCHITECTURE.md)) — and at serial baud
rates the CRC is never the bottleneck. The public surface is three functions:
`crc_compute()` dispatches on mode (`:55`), `crc_size()` returns the trailer width in
bytes (2 or 4, or 0 for `none`, `:64`), and `crc_name()` for display (`:73`). Note the
display name for `ZT_CRC_IBM` is `"modbus"`, while the CLI flag is `--crc ibm` — see
[../reference/CLI.md](../reference/CLI.md).

## The strip-and-verify-on-RX / append-on-TX model

CRC handling is split cleanly by direction, and the asymmetry is intentional.

**RX — verify then strip.** `frame_dispatch()` (`src/proto/framing.c:61`) runs after a
decoder completes a frame. If a CRC mode is active and the frame is longer than the
trailer (`csz && n > csz`), it reads the trailer big-endian from the tail of the buffer,
recomputes the CRC over `n - csz` bytes, and compares (`:66-81`). On mismatch it bumps
`c->proto.crc_err` and flashes a diagnostic naming the frame number and the
want/got values; **the frame is still rendered** — zyterm is a diagnostic tool, so a
corrupt frame is shown (and counted) rather than silently dropped. Either way the
trailer bytes are stripped (`n -= csz`, `:82`) so the operator and the logs see only
the payload.

**TX — append then encode.** `framing_send()` (`:312`) does the inverse, in order:
if `crc_append` is set and a CRC mode is active, it copies the payload into a scratch
buffer and writes the CRC big-endian into the trailer (`:318-334`); the
payload-plus-CRC is then handed to the matching encoder
(`encode_cobs`/`encode_slip`/`encode_hdlc`/`encode_len16`, `:338-348`) and finally to
`direct_send()`. So on the wire the CRC covers the payload and the framing wraps both —
which is what every peer stack expects.

The trailer byte order is fixed big-endian on both sides (`:69-74` on RX, `:323-331`
on TX), so a frame round-trips through zyterm unchanged.

## Known edge cases

Two latent bugs in this module are recorded; neither is reachable in normal use today,
but both are tracked so they aren't reintroduced. See
[../tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md).

- **ZT-021** (⚪ low, `src/proto/framing.c:241`) — `encode_cobs()`'s capacity guard
  `if (cap < n + 2)` undercounts the true COBS worst case, which is
  `n + n/254 + 2` (one extra code byte every 254 non-zero bytes, plus the leading code
  byte and trailing delimiter). It is not currently reachable because the sole caller
  oversizes the `encoded` buffer to `ZT_LINEBUF_CAP * 2 + 8` (`:336`), which comfortably
  covers the real bound. Fix direction: correct the guard to the true bound so the
  function is safe for any caller.

- **ZT-022** (⚪ low, `src/proto/framing.c:199`) — a zero-length LENPFX frame
  (`len16 == 0`) is mishandled: after the two header bytes decode to a need of 0, the
  decoder falls into the payload branch on the *next* byte and consumes it before
  noticing `len == len16_need`, swallowing one byte and desyncing the stream. Fix
  direction: when `len16_need == 0`, dispatch the (empty) frame immediately on header
  completion instead of waiting for a payload byte. Note that `feed_len16` already
  rejects an over-cap length by resetting the header state (`:204-207`), so oversized
  frames are handled; only the zero-length case is wrong.

---

_Last updated: 2026-06-03._
