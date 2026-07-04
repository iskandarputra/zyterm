# ZT-033: `--threaded` reader orphaned on embedded `siglongjmp` — use-after-free of a stack-local `zt_ctx`

- **Severity:** 🔴 high (a live worker thread keeps writing through a stale pointer into a reused
  host stack — use-after-free / memory corruption in the long-lived `zy` builtin)
- **Area:** loop/concurrency (embedded) / memsafety
- **Status:** **fixed** — merged to `main` 2026-07-04 (recorded 2026-07-03) — see [KNOWN_ISSUES Resolved](../KNOWN_ISSUES.md#resolved)
- **Location:** worker `rx_thread_main` (`src/loop/rx_thread.c:65`, ring write ~`:86–89`), created
  in `rx_thread_start` (`src/loop/rx_thread.c:155`) with `arg = &c`, the stack-local `zt_ctx`
  declared in `zyterm_main` (`src/main.c:352`). The only teardown, `rx_thread_stop(&c)`
  (`src/main.c:881`), is skipped by the `siglongjmp` in `zt_die` (`src/core/core.c:226`) and
  `sig_crash` (`src/core/core.c:312`), and `zt_embed_reset` (`src/core/core.c:93`) never stops it.

## Root cause

The optional `--threaded` reader worker is created with a pointer to the **stack-local** `zt_ctx`
of `zyterm_main`. `zyterm_main` declares its context on the stack:

```c
zt_ctx c;                              /* src/main.c:352 */
memset(&c, 0, sizeof c);
```

and, once a live serial fd exists, hands `&c` straight to `pthread_create`:

```c
if (c.serial.spsc_enabled && c.serial.fd >= 0) rx_thread_start(&c); /* src/main.c:832 */
```

```c
c->serial.spsc_impl = r;
if (pthread_create(&r->thread, NULL, rx_thread_main, c) != 0) { ... } /* src/loop/rx_thread.c:155 */
```

The worker loops on `r->running`, `read()`s the private dup fd, and writes the bytes into the heap
ring reached **through `c`**, then wakes main through a pipe fd read out of `c`:

```c
zt_ctx      *c = (zt_ctx *)arg;                               /* :66 */
spsc_ring_t *r = (spsc_ring_t *)c->serial.spsc_impl;          /* :67 */
...
while (atomic_load_explicit(&r->running, memory_order_acquire)) {   /* :70 */
    ssize_t n = read(fd, tmp, sizeof tmp);                         /* :80 */
    if (n > 0) {
        ...
        for (size_t i = 0; i < wr; i++)
            r->buf[(head + i) & (r->cap - 1)] = tmp[i];            /* :86–87 wild write once r is stale */
        atomic_store_explicit(&r->head, head + wr, ...);           /* :88 */
        wake_main(c);                                              /* :89 → write(c->serial.spsc_wake_pipe[1],…) :62 */
    }
}
```

The worker is stopped, joined, and its ring/dup freed **only** on the normal-return cleanup path via
`rx_thread_stop(&c)` (`src/main.c:881`), which clears `r->running` and closes the dup fd
(`src/loop/rx_thread.c:183–185`).

Both embedded FATAL exits bypass that path. When zyterm runs as a `zy` builtin
(`zt_g_embedded = 1`) with the host jump buffer armed, `zt_die` and `sig_crash` `siglongjmp` back to
the host **instead of returning**, unwinding `zyterm_main`'s frame without running any of the
`rx_thread_stop` … cleanup below `run_interactive`:

```c
if (zt_g_embedded && zt_g_embed_jmp_armed) {   /* src/core/core.c:224 */
    zt_g_embed_jmp_armed = false;
    siglongjmp(zt_g_embed_jmp, 1);             /* :226 — skips rx_thread_stop */
}
exit(1);
```

```c
if (embed_recover) {                           /* src/core/core.c:310  (SIGABRT/SIGFPE only) */
    zt_g_embed_jmp_armed = false;
    siglongjmp(zt_g_embed_jmp, 128 + s);       /* :312 — skips rx_thread_stop */
}
```

After the jump `r->running` is still `1` and the dup fd is still open, so the worker keeps looping:
`read(dupfd)` → byte-copy into `r->buf` → `atomic_store` head → `wake_main(c)`. Its `arg` `c` now
points at a **reclaimed stack region** in the long-lived host.

`zt_embed_reset` — the per-run scrub the host calls between embedded invocations — does **not** stop
the worker either:

```c
void zt_embed_reset(void) {                    /* src/core/core.c:93 */
    ...
    uninstall_signals();
    ...
    multi_embed_reset();
    session_embed_reset();
}                                              /* no rx_thread_stop() */
```

So the next `zyterm_main` reuses the **same stack** for a fresh `zt_ctx c` while the orphaned worker
is mid-flight: `c->serial.spsc_impl` now reads whatever the new frame put there, the ring write at
`:86–87` becomes a wild write through a stale/garbage `r`, and `wake_main` writes stray bytes into a
possibly-reused wake-pipe fd number — a use-after-free / memory corruption in the host process. The
malloc'd 1 MiB ring (`ZT_SPSC_CAP`, `src/zt_ctx.h:69`) and the dup fd also leak permanently.

This is **benign in standalone mode**: there `zt_die` falls through to `exit(1)` and `sig_crash`
re-raises to a core dump, so the OS reclaims the thread, ring, and fd. It is only live under
embedding, where `siglongjmp` deliberately keeps the process running to protect the host shell.

Several other embedded leaks share this one root — nothing between the `siglongjmp` and the next run
tears down per-run state (the `setup_serial` fd, the replay device string, the clipboard/wake-pipe
fds, and the http/metrics/session/log fds stopped at `src/main.c:881–889`). The embedding needs
**one** teardown that runs on **all** exit paths, not just the normal return.

## Trigger / repro

1. Host (`zy`) runs zyterm embedded: `zt_g_embedded = 1`, jump buffer armed via `sigsetjmp` before
   calling `zyterm_main`, launched with `--threaded` against a live device.
2. The worker starts (`src/main.c:832`) and begins draining the dup fd into the ring.
3. During interactive operation a **SIGABRT or SIGFPE** occurs (an `assert`/`abort`, or an FP
   exception). `sig_crash` sets `embed_recover` and `siglongjmp`s to the host (`src/core/core.c:312`),
   skipping `rx_thread_stop`. (A `zt_die` fatal takes the equivalent path at `:226`.)
4. The worker keeps `read()`ing and copying into `r->buf`. The host calls `zt_embed_reset` (which
   does not stop it) and re-enters `zyterm_main`, reusing the same stack for a fresh `c`.
5. The worker's `c->serial.spsc_impl` now reads garbage; `r->buf[(head+i) & (r->cap-1)] = …` becomes
   a wild write, and `wake_main` writes into a stale fd — heap/stack corruption in the host.

## Fix direction

Ensure the worker is stopped and joined on **every** exit path, never left referencing a stack-local
`zt_ctx` across a `siglongjmp`:

- Give the embedding a single teardown that runs on all exits. At minimum call `rx_thread_stop()`,
  then close/stop the http/metrics/session/log fds and the setup_serial / replay / clipboard fds —
  the same set the normal path stops at `src/main.c:881–889`. Run it either as an unwind step before
  returning to the host, or at the **top of `zt_embed_reset`** and **immediately after the host's
  `sigsetjmp` returns non-zero**.
- In `sig_crash` keep only async-signal-safe work (it already does — terminal restore + `write`); the
  full teardown, including `pthread_join`, belongs on the host side **after** the jump, not in the
  handler.
- Do not leave a live pthread referencing a stack-local ctx across a `siglongjmp`. Pair this with a
  **registry-based embed reset** so every module's per-run state (ring, fds, device strings) is torn
  down uniformly on every re-entry, closing the sibling leaks noted above in the same mechanism.

## Verify

- Reproduce the sequence above under ASan/TSan: embedded run with `--threaded`, force a SIGABRT
  mid-transfer, re-enter `zyterm_main`. Before the fix ASan reports a heap-use-after-free / wild write
  from `rx_thread_main`; after the fix the worker is already joined, so the second run is clean.
- Assert no fd or 1 MiB allocation leaks across repeated embedded runs that each hit the fatal path
  (`/proc/self/fd` count and RSS stay flat over N iterations).
- Confirm the standalone path is unchanged: `zt_die` → `exit(1)` and `sig_crash` → re-raise still
  reclaim everything without the new teardown running twice.
- Cross-check against `INVARIANTS §4` (reader thread & fd lifecycle / SPSC ring — the worker must be
  stopped and joined before its ctx dies) and `INVARIANTS §2`/`§8` (signals / embedding — no
  long-lived thread may outlive the frame it was handed). See also
  [`docs/reference/EMBEDDING.md`](../../reference/EMBEDDING.md).
