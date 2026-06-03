# ADR-0003: epoll+splice fast path deferred

- **Status:** accepted
- **Date:** 2026-06-03

## Context

[ADR-0002](0002-single-ctx-poll-driven-loop.md) commits to a `poll(2)` event loop. At very high
baud rates with many watched fds, `poll(2)` has known costs: it rescans the whole fd set each call,
and copying RX bytes through userspace into a raw log is pure overhead when the kernel could move
them with `splice(2)`.

An experimental Linux fast path was written to address this: `src/serial/fastio.c` wraps
`epoll_create1`/`epoll_ctl` (edge-triggered) plus `splice(2)`/`writev(2)`, with a `poll()`-based
fallback for non-Linux. It was wired to a `--epoll` flag for a time.

The problem is that the scaffolding was never finished. The functions exist and compile
(`fastio_init` at `src/serial/fastio.c:39`, `fastio_shutdown`, `fastio_add_fd`, …) but they have
**zero call sites** anywhere in the runtime — `grep -rn 'fastio_' src/` returns matches only inside
`fastio.c` itself. The event loop in `src/loop/runtime.c` calls `poll(2)` directly. The only
residue elsewhere is `c.serial.epoll_fd = -1` initialization in `src/main.c:330`. Shipping a
`--epoll` flag that toggled a code path nobody had integrated or tested was a promise we could not
keep.

## Decision

- **`poll(2)` is the shipping I/O path.** It is what the loop actually uses, and it is correct.
- **The `--epoll` flag was removed in 1.2.0.** We do not expose an option whose backend is unwired.
- **The `fastio.c` scaffolding is kept, not deleted.** The epoll/splice design is sound and worth
  finishing; throwing it away would mean rewriting it later. It stays in the tree as deferred work.

## Consequences

- **Dead code is retained on purpose.** `src/serial/fastio.c` has no callers and is not part of
  any shipping path. That is a deliberate, documented state, not an oversight — recorded in
  [`tracking/STATUS.md`](../tracking/STATUS.md) as deferred (non-defect) work.
- No user-visible feature is lost: `poll(2)` already handles the bench-realistic fd counts and baud
  rates, and high-baud draining has its own answer in the opt-in `--threaded` reader
  ([ADR-0002](0002-single-ctx-poll-driven-loop.md)).
- Because the fast path is unwired, it carries no runtime risk today — but it is one of the
  "advertised-but-dead code" items the audit calls out as a class to clean up. When we revisit it,
  the bar is: **wired into the loop *and* tested** (unit + integration), or removed. Until then it
  must not be advertised as a working feature.
- Re-evaluation is tracked in [`plans/ROADMAP.md`](../plans/ROADMAP.md) and
  [`plans/RELIABILITY_HARDENING.md`](../plans/RELIABILITY_HARDENING.md). A future ADR will
  supersede this one if the fast path is wired and shipped.
