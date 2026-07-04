# ZT-050: `xmodem_receive` spins forever on a block with a bad block-number complement

- **Severity:** 🟠 medium (an infinite CPU loop that hangs the modal XMODEM-receive UI until SIGKILL;
  triggered by device-controlled input — line noise or a hostile/garbled sender — but only while a
  receive is in progress)
- **Area:** proto / transfer engine / hang (denial-of-service)
- **Status:** **fixed** 2026-07-04 on branch `test/coverage-and-xmodem-fuzz` — found by the new
  `xmodem_receive` fuzz target, fixed in the same change. See
  [KNOWN_ISSUES Resolved](../KNOWN_ISSUES.md#resolved).
- **Location:** the block loop in `xmodem_receive` (`src/proto/xmodem.c`, the
  `if ((bn_rcv ^ bn_inv) != 0xFF) { … continue; }` branch at the top of `while (1)`).
- **Found by:** `tests/fuzz/fuzz_xmodem.c` on its first runs (a socketpair-fed harness that drives the
  receiver with fuzz bytes as the device stream) — the run never terminated.

## Root cause

Each XMODEM data block is `SOH · blk# · ~blk# · 128 data · CRC16`. The receiver first checks that the
block number and its one's-complement agree, then checks the CRC. The two error paths were written
inconsistently:

```c
while (1) {
    int bn_rcv = blk[1];
    int bn_inv = blk[2];
    if ((bn_rcv ^ bn_inv) != 0xFF) {   /* bad complement */
        (void)write_all(c->serial.fd, &nak, 1);
        continue;                      /* <-- re-tests the SAME blk[] → never reads a new one */
    }
    uint32_t crc_rcv = …, crc_cmp = …;
    if (crc_rcv != crc_cmp) {          /* bad CRC */
        (void)write_all(c->serial.fd, &nak, 1);
    } else if (…) { … }                /* good block: ACK */
    int h = read_byte(…);              /* <-- the ONLY read of the next block */
    …
}
```

The CRC-mismatch branch falls through to the `read_byte` at the bottom of the loop, so it correctly
NAKs and then reads the sender's retransmission. The bad-complement branch instead `continue`s. Because
`blk[]` is only ever rewritten by that bottom `read_byte`, the `continue` re-evaluates the identical
`blk[1] ^ blk[2]` on the next iteration — still non-`0xFF` — and loops forever, writing a NAK each pass
and never consuming another byte from the line.

Any block whose third byte is not the exact complement of the second (a single bit of line noise, a
mis-framed byte, or a hostile peer) wedges the receiver. XMODEM receive is a **modal** operation that
drives its own read loop, so the whole UI / serial loop is stuck until the process is killed.

## Fix

Treat a bad complement the same as a bad CRC — a corrupt block to NAK and re-read — by merging the two
conditions and dropping the `continue`, so every path reaches the retransmission read:

```c
int      bn_rcv  = blk[1];
int      bn_inv  = blk[2];
uint32_t crc_rcv = ((uint32_t)blk[3 + XM_BLK] << 8) | blk[3 + XM_BLK + 1];
uint32_t crc_cmp = crc_compute(ZT_CRC_CCITT, blk + 3, XM_BLK);
if ((bn_rcv ^ bn_inv) != 0xFF || crc_rcv != crc_cmp) {
    (void)write_all(c->serial.fd, &nak, 1);      /* NAK, then read the retransmission below */
} else if (bn_rcv == (blknum & 0xFF)) {
    … accept, ACK …
} else {
    … duplicate, ACK …
}
int h = read_byte(c->serial.fd, XM_TIMEOUT_MS * 5);
…
```

## Regression

- `tests/fuzz/fuzz_xmodem.c` — the libFuzzer target that found it; runs on every PR via `make fuzz`
  and the `fuzz` CI job. Before the fix it hung; after, it explores the block parser freely.
- `tests/integration/test_xmodem.c` — "NAKs a bad block-number complement, no hang (ZT-050)": a
  scripted sender emits a block with a corrupt complement first, then the correct retransmission; the
  receiver must NAK the bad block, accept the retransmission, and reproduce the exact payload. A
  `SIGALRM` watchdog fails the test fast if the hang ever returns.
