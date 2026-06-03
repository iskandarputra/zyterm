# ZT-002: port rediscover frees the non-heap `argv` device pointer

- **Severity:** 🔴 high (heap corruption / abort on the first reconnect when discovery hints are set)
- **Area:** serial (port discovery) / ownership
- **Status:** open  (recorded 2026-06-03; not yet fixed)
- **Location:** `src/serial/port_discover.c:171` (free), `src/main.c:717` (the offending assignment)

## Root cause

Same root ownership defect as ZT-001, reached through a different path. `c->serial.device` may be a
borrowed `argv` pointer rather than a heap allocation:

```c
} else {
    c.serial.device = argv[optind];   /* src/main.c:717 — borrowed, not strdup'd */
}
```

`port_rediscover()` re-resolves the device path on reconnect when `--port-glob` / `--match-vid-pid`
hints are present. When the freshly discovered path differs from the current one, it frees the old
pointer and stores the new (heap) one:

```c
if (c->serial.device && !strcmp(c->serial.device, found)) {
    free(found);
    return 0;
}
free((void *)c->serial.device);   /* src/serial/port_discover.c:171 */
c->serial.device = found;
return 1;
```

If the program was launched with **both** a positional device *and* discovery hints, `serial.device`
is the borrowed `argv[optind]` pointer from `src/main.c:717`. The first reconnect that resolves a
*different* path therefore calls `free()` on `argv[optind]` → undefined behavior (heap corruption or
`free(): invalid pointer` abort). If the resolved path matches the current one, the early `return 0`
frees `found` instead and the bug stays dormant — so the crash is replug-timing dependent.

`port_rediscover()` is called from `reconnect_attempt()` (`src/ext/reconnect.c:45`) on **every**
reconnect tick, and `reconnect_attempt` runs from `run_reconnect_loop()` (the disconnect modal) and
from the `Ctrl+A r` manual-reconnect handler. So any unplug/replug — the headline use case for
`--port-glob` on USB-serial adapters — can land on this free.

## Trigger / repro

1. `zyterm --port-glob '/dev/ttyUSB*' /dev/ttyUSB0` (positional device → borrowed pointer at
   `src/main.c:717`, plus discovery hints).
2. Unplug the adapter; it re-enumerates as a different node, e.g. `/dev/ttyUSB1`.
3. The reconnect loop calls `reconnect_attempt` → `port_rediscover`; `found` (`/dev/ttyUSB1`) differs
   from the borrowed `argv` path, so `src/serial/port_discover.c:171` calls `free()` on
   `argv[optind]` → heap corruption / `SIGABRT`.

## Fix direction

Identical to ZT-001: enforce **single ownership** so `serial.device` is always heap-owned. The
minimal fix is at the assignment site —

```c
} else {
    c.serial.device = strdup(argv[optind]);   /* own it like the discovery path */
}
```

— which makes the `free()` at `src/serial/port_discover.c:171` correct without touching this file.
Codify the always-owned rule in `INVARIANTS §1`. ZT-001, ZT-002, ZT-016, and ZT-018 are the same
ownership tangle (cross-cutting theme A); fix them together and add the single free to the unified
cleanup label.

## Verify

- Repro above with a real adapter (or a `socat`/`tty0tty` pair renamed between reconnects). After the
  fix the reconnect must complete cleanly with the new path; under ASan, **no** "free on address
  which was not malloc"'d report.
- Add a test driving `port_rediscover()` directly: set `serial.device` to a heap string, point the
  glob at a different node, assert the function returns 1, swaps the pointer, and frees exactly once.
- Run the full reconnect pty/integration suite under the asan-ubsan CI config and confirm no
  heap-corruption diagnostics across repeated replug cycles.
