# ZT-031: fuzzy-finder Enter injects over a stale `sent_len`, underflowing the edit-key memmove (OOB)

- **Severity:** 🔴 high (an unsigned length underflow drives a ~`SIZE_MAX`-byte `memmove` over the
  4096-byte input buffer — segfault or corruption of the enclosing `zt_ctx`, reachable from documented
  keys alone)
- **Area:** tui/input / memsafety
- **Status:** **fixed** — merged to `main` 2026-07-04 (recorded 2026-07-03) — see [KNOWN_ISSUES Resolved](../KNOWN_ISSUES.md#resolved)
- **Location:** `src/tui/fuzzy.c:64` (the injection `memcpy` / length + cursor assignment in
  `fuzzy_handle`'s Enter branch), which fails to reset `c->tui.sent_len`; consumed by the edit-key
  handlers in `src/loop/input.c` (`delete_before_cursor` `:800`, Ctrl+W `:1025`).

## Root cause

The input editor maintains three cursor fields with a load-bearing invariant. `cursor` is documented
as an **offset from `sent_len`**, not an absolute index:

```c
size_t input_len; /**< Valid length in @ref input_buf.          */  /* src/zt_ctx.h:241 */
size_t sent_len;  /**< Prefix already committed to the device.  */  /* src/zt_ctx.h:242 */
size_t cursor;    /**< Offset from @c sent_len into unsent.     */  /* src/zt_ctx.h:243 */
```

Every edit-key handler forms the absolute index as `abs = sent_len + cursor` and relies on the
invariant `sent_len + cursor <= input_len` so that `input_len - abs` — an **unsigned** `size_t`
length — is non-negative.

`fuzzy_handle()`'s Enter branch injects the selected history string but overwrites only `input_len`
and `cursor`, leaving `sent_len` at whatever value it already held:

```c
memcpy(c->tui.input_buf, sel, n);   /* src/tui/fuzzy.c:64 */
c->tui.input_len = n;               /* src/tui/fuzzy.c:65 */
c->tui.cursor    = n;               /* src/tui/fuzzy.c:66 — sent_len NOT reset */
```

The correct twin, `load_history_into_buf()`, performs the *same* injection and explicitly zeroes
`sent_len` for exactly this reason:

```c
memcpy(c->tui.input_buf, s, n);     /* src/loop/input.c:38 */
c->tui.input_len = n;               /* src/loop/input.c:39 */
c->tui.sent_len  = 0;               /* src/loop/input.c:40 — the missing line */
c->tui.cursor    = n;               /* src/loop/input.c:41 */
```

`fuzzy_enter()` (`src/tui/fuzzy.c:19`) never touches `sent_len` either, so whatever prefix was
committed to the device before the finder opened survives the whole interaction. `sent_len` is
non-zero after any of the ordinary commit paths:

- **Local echo on:** every echoed character sets `sent_len = input_len`
  (`src/loop/input.c:1052`, in the block `:1049`–`:1053`).
- **Tab completion:** `flush_unsent()` sets `sent_len = input_len` (`src/loop/send.c:172`).

With `sent_len = 3`, `cursor = n`, `input_len = n` after injection, the invariant is broken:
`abs = sent_len + cursor = 3 + n`, which is **past** `input_len = n`. The next edit key underflows:

```c
size_t abs = c->tui.sent_len + c->tui.cursor;                          /* src/loop/input.c:799 → 3+n */
memmove(&c->tui.input_buf[abs - 1], &c->tui.input_buf[abs],
        c->tui.input_len - abs);                                       /* src/loop/input.c:800 */
```

`input_len - abs = n - (3 + n) = (size_t)(-3) ≈ SIZE_MAX`. `delete_before_cursor` enters this branch
whenever `cursor > 0` (`src/loop/input.c:798`), so a single Backspace issues a ~`SIZE_MAX`-byte
`memmove` reading/writing far past the 4096-byte `input_buf` (`ZT_INPUT_CAP`, `src/zt_ctx.h:55`,
`:240`) — a segfault or wholesale corruption of the surrounding `zt_ctx`. Ctrl+W walks the same
broken state, indexing `input_buf[sent_len + cursor - 1]` (out of bounds) at `src/loop/input.c:1027`
and `:1030` and then calling `delete_before_cursor` in the same loop (`:1025`–`:1031`).

## Trigger / repro

Reachable purely from documented keybindings — the fuzzy finder was only recently wired
(ZT-008), and this is a new interaction of that wiring.

1. Run `zyterm /dev/ttyUSB0`.
2. `Ctrl+A e` — enable local echo.
3. Type `abc` — each echoed char sets `sent_len = input_len`, so now `sent_len = 3`, `input_len = 3`.
4. `Ctrl+A .` — open the fuzzy finder (`fuzzy_enter` does **not** touch `sent_len`).
5. Type a query matching a prior history line of length `n`, press **Enter** — injects
   `input_len = n`, `cursor = n`, with `sent_len` still `3`.
6. Press **Backspace** (or Ctrl+W) → `abs = 3 + n > input_len`, the unsigned `input_len - abs`
   underflows, and the giant `memmove` faults.

A Tab completion before the finder (which routes through `flush_unsent`, `src/loop/send.c:172`)
sets up the identical stale `sent_len` and triggers the same crash without local echo.

## Fix direction

- In `fuzzy_handle()`'s Enter branch, reset `c->tui.sent_len = 0` immediately after the
  `memcpy` / length assignment (`src/tui/fuzzy.c:64`–`66`), mirroring `load_history_into_buf`
  (`src/loop/input.c:40`). Injecting a fresh buffer means nothing is committed to the device yet, so
  `sent_len = 0` is the correct state.
- Harden the shared invariant so a future injection path can't silently reintroduce this: add a
  cheap assertion `assert(c->tui.sent_len + c->tui.cursor <= c->tui.input_len)` at the top of the
  edit-key handlers (`delete_before_cursor`, `insert_char`, Ctrl+W), and/or clamp `abs` before the
  `memmove` so a broken state degrades to a no-op rather than an OOB.
- Record the rule in `INVARIANTS §1` (ownership & bounds): every path that replaces `input_buf`
  must re-establish `sent_len + cursor <= input_len`; `load_history_into_buf` (`src/loop/input.c:40`)
  is the reference implementation.

## Verify

- After the fix, run the repro under ASan: inject a history entry with local echo on, then press
  Backspace — the `memmove` length must be small and in-bounds, no ASan report, no crash.
- Add a regression test that sets `sent_len` non-zero, drives the fuzzy Enter injection, then issues
  Backspace and Ctrl+W, asserting `sent_len == 0` post-injection and that `input_len` / `cursor` stay
  within `[0, ZT_INPUT_CAP)`.
- Grep for other writers of `input_buf` / `input_len` and confirm each resets or preserves
  `sent_len` such that `sent_len + cursor <= input_len` holds.
