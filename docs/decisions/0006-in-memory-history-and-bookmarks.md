# ADR-0006: History & bookmarks in memory only

- **Status:** accepted
- **Date:** 2026-06-03

## Context

zyterm has two operator conveniences that *feel* like they might persist across runs:

- **Line-edit history** — the commands you typed and sent, recalled with `Up`/`Down`.
- **Scrollback bookmarks** — positions you marked in the scrollback ring with `Ctrl+A +` and
  jumped between via the `Ctrl+A [` list.

Both are session aids: scratch state for the current debugging session. A serial console is not a
shell — the "commands" are device-specific byte sequences for one board on one bench, and the
scrollback they bookmark is the live RX of *this* session. Persisting them across runs raises
questions with no clean answer (which device? which session? what about secrets typed at a login
prompt landing in a world-readable dotfile?) for a benefit that is marginal.

There is also a correctness argument: an on-disk history/bookmarks store is more state to own —
file format, location, growth, corruption, concurrent writers across multiple zyterm instances.
That is real surface for a feature whose value is "remembering what I typed five minutes ago."

The persistent surface that *does* earn its keep is **profiles**: named, reusable connection
setups. Those are deliberately written to disk at `~/.config/zyterm/<name>.conf`
(`src/ext/profile.c:44`), with inotify hot-reload.

## Decision

**Line-edit history and scrollback bookmarks are intentionally in-memory only** and are lost on
exit. The history ring (capacity `ZT_HISTORY_CAP = 128`, `src/zt_ctx.h:58`) and the bookmark
store (`ZT_BOOKMARK_MAX = 64`, `:67`, in `src/ext/bookmarks.c`) live entirely in the `zt_ctx`;
**neither touches disk.** There is no `~/.zyterm_history`, no `~/.zyterm/history`, and no
`~/.zyterm/bookmarks` file — any documentation claiming otherwise is wrong.

**Profiles are the persistent surface.** Durable, reusable state belongs in a `--profile`
(`~/.config/zyterm/<name>.conf`), saved with `--profile-save`.

## Consequences

- Predictable lifecycle: nothing the operator types or marks leaks into a file they did not ask
  for. No secret-in-dotfile hazard from history.
- Less state to own and corrupt; no cross-instance file contention.
- The history/bookmarks feature stays simple and bounded by its ring capacities.
- This corrects a recurring documentation myth — the [FAQ](../reference/FAQ.md) carries the
  correction so users do not go looking for files that do not exist.
- Persistence is a **possible future**, not a promise: if a real use case emerges, on-disk
  history/bookmarks would be designed (location, format, redaction) and recorded in a superseding
  ADR. The placeholder for that work is [`plans/ROADMAP.md`](../plans/ROADMAP.md).
- No new invariant is created; this decision narrows what state is durable, and the durable state
  (profiles) is owned and freed under the ownership rules in
  **[INVARIANTS §1](../invariants/INVARIANTS.md)**.
