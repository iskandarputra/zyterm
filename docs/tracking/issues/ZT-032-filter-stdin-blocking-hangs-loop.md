# ZT-032: `--filter` child stdin fd is blocking, so a slow filter freezes the whole event loop

- **Severity:** 🔴 high (any `--filter` child that drains its stdin slower than the device streams
  hard-hangs the UI — no serial reads, no repaint, no quit)
- **Area:** ext (filter) / blocking-in-loop
- **Status:** **open** — recorded 2026-07-03 (2026-07 re-review of v1.4.0). Not fixed.
- **Location:** `src/ext/filter.c:39` (pipe created `O_CLOEXEC`-only), `:70` (blocking write end stored
  into `c->ext.filter_stdin_fd`), and the dead drop-branch at `src/ext/filter.c:127` in `filter_feed`

## Root cause

`filter_feed()` is written as a best-effort, non-blocking relay — it explicitly promises to drop bytes
rather than stall the TTY — but the descriptor it writes to is **blocking**, so the promise is a lie
and the drop-branch is dead code.

In `filter_start()` the stdin pipe is created with `O_CLOEXEC` only (no `O_NONBLOCK`):

```c
if (pipe2(in_pipe, O_CLOEXEC) != 0) return -1;   /* src/ext/filter.c:39 — no O_NONBLOCK */
```

After the fork, `filter_start()` sets `O_NONBLOCK` on **only** the read end `out_pipe[0]` (our drain
side), and never touches the write end before storing it:

```c
/* Non-blocking on our read end for clean draining. */
int fl = fcntl(out_pipe[0], F_GETFL, 0);         /* src/ext/filter.c:67 */
fcntl(out_pipe[0], F_SETFL, fl | O_NONBLOCK);    /* src/ext/filter.c:68 — only the read end */

c->ext.filter_stdin_fd  = in_pipe[1];            /* src/ext/filter.c:70 — stored still-blocking */
```

So `c->ext.filter_stdin_fd` is a plain blocking pipe write end. `filter_feed()` then loops on it
assuming it can get `EAGAIN`:

```c
ssize_t w = write(c->ext.filter_stdin_fd, p, left);   /* src/ext/filter.c:121 */
if (w < 0) {
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;  /* src/ext/filter.c:127 — DEAD: fd is blocking */
    filter_stop(c);
    return;
}
```

Because the fd is blocking, `write()` on a full pipe never returns `EAGAIN` — it **blocks** in the
kernel until the pipe drains. The `EAGAIN`/`EWOULDBLOCK` drop-branch at line 127 can therefore never
be taken, and the header comment "drop bytes if the pipe is full rather than blocking the TTY"
(`src/ext/filter.c:117`) is contradicted by the actual descriptor state.

`filter_feed()` runs on the RX-ingest path inside the single-threaded `poll(2)` loop. A blocking
`write()` there is a hard stall of the entire runtime: no serial reads, no HUD repaint, no keyboard
handling. This is exactly the failure class **INVARIANTS §3 (the single-threaded event loop — never
block in it)** forbids. It is the same family as the filter-blocking defects ZT-006 (`filter_stop`
blocking `waitpid`) and ZT-015 (`filter_feed` EINTR truncation); those hardened `filter_stop` and the
EINTR retry but **left the stdin fd blocking**, so the backpressure path was never actually made
non-blocking.

## Trigger / repro

1. Run a filter that consumes stdin slower than the device produces, e.g.
   `zyterm --filter 'stdbuf -oL jq -c .'` — or, worst case, a filter that never reads its stdin at all.
2. Have the device stream a fast, sustained dump (high-baud firmware hex/log flood) so RX exceeds the
   filter's stdin drain rate.
3. Once more than one kernel pipe buffer (~64 KiB) is outstanding, `write(filter_stdin_fd, …)` in
   `filter_feed()` fills the pipe and **blocks indefinitely**. The UI freezes: no serial reads, no
   repaint, and `Ctrl+A x` (quit) is not processed. It stays hung until the filter drains its stdin —
   possibly never.

No hostile device is required; any filter slower than the byte rate reproduces it.

## Fix direction

Make our write end non-blocking so the existing `EAGAIN` drop-path in `filter_feed()` becomes live.

- In `filter_start()`, after the fork and after `close(in_pipe[0])`, fetch flags on `in_pipe[1]` and
  set `O_NONBLOCK` — the same `fcntl(fd, F_GETFL) | O_NONBLOCK` pattern already applied to
  `out_pipe[0]` at `src/ext/filter.c:67–68` — **before** assigning it to `c->ext.filter_stdin_fd`
  (line 70).
- This is safe for the child: the child's stdin is `in_pipe[0]` (dup2'd onto `STDIN_FILENO`), a
  **separate open file description**, so setting `O_NONBLOCK` on our `in_pipe[1]` does not make the
  child's stdin non-blocking — the filter keeps blocking reads.
- With the write end non-blocking, a full pipe returns `EAGAIN`, `filter_feed()` takes its `break`
  (line 127) and drops the overflow, and the loop tick returns immediately. No new code paths are
  needed — the drop logic already exists; it just needs a non-blocking fd to fire.

## Verify

- A `--filter` child that never reads its stdin (e.g. `--filter 'sleep 100000'`) under a fast RX flood
  must **not** freeze the UI: `Ctrl+A x` still quits promptly and the HUD keeps repainting.
- Under sustained backpressure, confirm the drop path at `src/ext/filter.c:127` actually engages
  (bytes are dropped, not queued) instead of the loop stalling.
- Regression-check that a normal, keeping-up filter still receives its full byte stream (no spurious
  drops when the pipe is not full).
