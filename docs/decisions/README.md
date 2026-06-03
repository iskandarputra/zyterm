# Architecture Decision Records (ADRs)

This directory records the **why** behind zyterm's foundational design choices. Each ADR
captures one decision, the context that forced it, and the consequences we accepted — including
the rules it later imposed on the code (the [invariants](../invariants/INVARIANTS.md)).

## What an ADR is here

- **Immutable.** An ADR is a dated snapshot of a decision at a point in time. We never rewrite
  history. If a decision changes, we add a **new** ADR that supersedes the old one and update the
  index below; the original stays as written.
- **Narrow.** One decision per record. Cross-cutting context lives in
  [`reference/ARCHITECTURE.md`](../reference/ARCHITECTURE.md); open future work lives in
  [`plans/`](../plans/ROADMAP.md).
- **Grounded.** Every claim is checkable against `src/`. ADRs cite `file.c:line` where it helps.

## Format

Each record follows a fixed shape:

```
# ADR-NNNN: <title>
- **Status:** accepted
- **Date:** <ISO date>
## Context        — the forces and constraints in play
## Decision       — what we chose
## Consequences    — what that buys and costs us (links the resulting INVARIANTS §N)
```

## Index

| ADR | Title | Status | Summary |
| --- | --- | --- | --- |
| [0001](0001-linux-first-single-binary.md) | Linux-first, single self-contained binary | accepted | Linux is the supported/CI target; ship one libc-only C binary. |
| [0002](0002-single-ctx-poll-driven-loop.md) | One `zt_ctx`, single-threaded poll-driven loop | accepted | All state in one context; one `poll(2)` loop; opt-in `--threaded` reader. |
| [0003](0003-epoll-splice-fastpath-deferred.md) | epoll+splice fast path deferred | accepted | `fastio.c` scaffolding kept but unwired; `poll(2)` ships. |
| [0004](0004-reconnect-on-by-default.md) | Auto-reconnect on by default | accepted | Hardware gets unplugged; reconnect defaults ON, `--no-reconnect` opts out. |
| [0005](0005-rfc2217-deferred.md) | `rfc2217://` deferred to a stub | accepted | `rfc2217://` is a deliberate NYI stub; use ser2net raw + `tcp://`. |
| [0006](0006-in-memory-history-and-bookmarks.md) | History & bookmarks in memory only | accepted | Line-edit history and bookmarks are session-scoped; profiles persist. |

## Related

- [`reference/ARCHITECTURE.md`](../reference/ARCHITECTURE.md) — how the system is shaped now.
- [`invariants/INVARIANTS.md`](../invariants/INVARIANTS.md) — the don't-regress rules these
  decisions produced.
- [`plans/ROADMAP.md`](../plans/ROADMAP.md) — what a future ADR might revisit.
