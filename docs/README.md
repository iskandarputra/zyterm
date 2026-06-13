# zyterm documentation

This tree is organized **by kind, not by topic**. A document's directory tells you what *kind*
of thing it is — and therefore how much to trust it, who maintains it, and when it changes.
"How does the reader thread work?" is reference; "why is reconnect on by default?" is a decision;
"the event loop must never block" is an invariant; "logging is silently lost after the first
rotation" is a known issue. Find the kind first, then the topic.

zyterm is a single-binary Linux serial-terminal emulator in C (~12.5K LOC), version **1.2.0**
(`ZT_VERSION`, `src/zt_ctx.h:70`).

---

## The directories

| Directory | Kind | Trust level |
|---|---|---|
| [`reference/`](reference/) | How it works **now** — architecture, internals, CLI, keys, embedding, FAQ | Authoritative. Trust it; if code disagrees, the doc is a bug. |
| [`invariants/`](invariants/) | Don't-regress **rules** — the contracts every change must preserve | Authoritative + binding. Read before touching the relevant subsystem. |
| [`decisions/`](decisions/) | **Why** — ADRs, dated, immutable | Historical record of intent. Append-only; superseded, never edited. |
| [`design/`](design/) | How a **shipped** subsystem was designed | Accurate as of its date; reference/ wins on current behavior. |
| [`plans/`](plans/) | **Open future work** — roadmap, hardening order | Aspirational. Nothing here is implemented unless reference/ says so. |
| [`tracking/`](tracking/) | **Live boards** — STATUS (non-defect work), KNOWN_ISSUES (defects) | Current snapshot. Re-stamped on change; verify against `src/`. |
| [`ops/`](ops/) | **Release** mechanics | Operational truth for cutting a build. |
| [`guide/`](guide/) | Task-oriented **user** learning | User-facing how-to; not an internals contract. |
| [`archive/`](archive/) | **History only** | Frozen. Never advertise as current. |

A standalone man page lives at [`zyterm.1`](zyterm.1).

---

## New-dev reading order

Read these in sequence the first time through. Each assumes the one before it.

1. [`../README.md`](../README.md) — what zyterm is and how to run it.
2. [`../CONTRIBUTING.md`](../CONTRIBUTING.md) — build, test, and the change workflow.
3. [`reference/ARCHITECTURE.md`](reference/ARCHITECTURE.md) — the module map and the enforced
   dependency chain (`core ← serial ← log ← proto ← render ← tui ← net ← ext ← loop`).
4. [`reference/INTERNALS.md`](reference/INTERNALS.md) — the single `zt_ctx`, the poll loop, the
   optional `--threaded` SPSC reader, signals.
5. [`invariants/INVARIANTS.md`](invariants/INVARIANTS.md) — the rules you must not break.
6. [`tracking/STATUS.md`](tracking/STATUS.md) — what is in flight right now.

---

## Where does this belong?

When you have something to write down, route it by *what it is*:

| You have… | It goes in | File |
|---|---|---|
| A bug / defect (something is wrong) | tracking | [`tracking/KNOWN_ISSUES.md`](tracking/KNOWN_ISSUES.md) |
| Non-defect work in progress | tracking | [`tracking/STATUS.md`](tracking/STATUS.md) |
| A reason a choice was made | decisions | [`decisions/`](decisions/) (new ADR) |
| A rule that must never regress | invariants | [`invariants/INVARIANTS.md`](invariants/INVARIANTS.md) |
| An explanation of how it works today | reference | [`reference/`](reference/) |
| A user-facing how-to | guide | [`guide/`](guide/) |
| Anything about cutting a release | ops | [`ops/RELEASE.md`](ops/RELEASE.md) |
| Future work that isn't built yet | plans | [`plans/`](plans/) |

Route, don't dump. A new top-level `.md` at the repo root or a loose file in `docs/` is almost
always one of the kinds above in disguise.

---

## Per-directory index

### `reference/` — current truth

- [`ARCHITECTURE.md`](reference/ARCHITECTURE.md) — 9 `src/` modules + `main.c`, the
  header-enforced dependency chain, and where each feature lives.
- [`INTERNALS.md`](reference/INTERNALS.md) — the single `zt_ctx`, the `poll(2)` event loop, the
  optional `--threaded` reader + 1 MiB SPSC ring, signal handling, embedding internals.
- [`CLI.md`](reference/CLI.md) — every flag, grounded in `src/main.c`.
- [`KEYBINDINGS.md`](reference/KEYBINDINGS.md) — `Ctrl+A` command mode and normal-input keys,
  grounded in `src/loop/input.c`.
- [`EMBEDDING.md`](reference/EMBEDDING.md) — the 7-symbol embedding surface and `zyterm_embed.a`.
- [`FAQ.md`](reference/FAQ.md) — including the corrections (history/bookmarks are in-memory only;
  profiles persist at `~/.config/zyterm/<name>.conf`).

### `invariants/` — binding rules

- [`INVARIANTS.md`](invariants/INVARIANTS.md) — nine numbered sections (cite as `INVARIANTS §N`):
  ownership, signals/async-safety, the single-threaded loop, the reader thread/SPSC ring, framing
  bounds, terminal-output safety, the network/IPC trust boundary, the module dependency chain, and
  build/release.

### `decisions/` — ADRs (append-only)

- [`README.md`](decisions/README.md) — index and conventions.
- [`0001-linux-first-single-binary.md`](decisions/0001-linux-first-single-binary.md)
- [`0002-single-ctx-poll-driven-loop.md`](decisions/0002-single-ctx-poll-driven-loop.md)
- [`0003-epoll-splice-fastpath-deferred.md`](decisions/0003-epoll-splice-fastpath-deferred.md)
- [`0004-reconnect-on-by-default.md`](decisions/0004-reconnect-on-by-default.md)
- [`0005-rfc2217-deferred.md`](decisions/0005-rfc2217-deferred.md)
- [`0006-in-memory-history-and-bookmarks.md`](decisions/0006-in-memory-history-and-bookmarks.md)

### `design/` — shipped-subsystem design notes

- [`FRAMING_AND_CRC.md`](design/FRAMING_AND_CRC.md) — raw/cobs/slip/hdlc/lenpfx + ccitt/ibm/crc32.
- [`HTTP_BRIDGE.md`](design/HTTP_BRIDGE.md) — the HTTP/SSE/WS bridge and Prometheus metrics.
- [`THREADING_AND_RECONNECT.md`](design/THREADING_AND_RECONNECT.md) — the `--threaded` reader and
  the reconnect/discovery path.

### `plans/` — open future work

- [`ROADMAP.md`](plans/ROADMAP.md) — feature direction (e.g. real multi-pane).
- [`RELIABILITY_HARDENING.md`](plans/RELIABILITY_HARDENING.md) — fix order, perf, testing, and
  security posture.

### `tracking/` — live boards

- [`STATUS.md`](tracking/STATUS.md) — non-defect work in flight.
- [`KNOWN_ISSUES.md`](tracking/KNOWN_ISSUES.md) — defects `ZT-001 … ZT-028`; high-severity rows
  link into [`issues/`](tracking/issues/).

### `ops/` — release

- [`RELEASE.md`](ops/RELEASE.md) — tagging, the `.deb` build, and the `tag == ZT_VERSION` check.

### `guide/` — user learning

- [`README.md`](guide/README.md) — guide index.
- [`getting-started.md`](guide/getting-started.md)
- [`logging-and-capture.md`](guide/logging-and-capture.md)
- [`automation.md`](guide/automation.md)
- [`recipes.md`](guide/recipes.md)
- [`troubleshooting.md`](guide/troubleshooting.md)

### `archive/` — history only

- [`README.md`](archive/README.md) — what is frozen here and why.
- [`audit/2026-06-03-source-review.md`](archive/audit/2026-06-03-source-review.md) — the source
  review that produced the `ZT-NNN` defect set.

---

## Conventions

- **Kind, not topic** decides the directory. If you can't place a doc, you haven't decided what
  kind it is.
- **Current truth only.** No hype, no "coming soon", no unverified numbers. Every code claim must
  be checkable in `src/`; prefer `file.c:line` citations.
- **Honesty about dead code.** Stubbed, unwired, or broken features are never described as working.
  They live in `tracking/KNOWN_ISSUES.md` or `plans/`. (For the current list of these — OSC 8
  hyperlinks, the epoll/splice fast path, the fuzzy finder, multi-pane, `rfc2217://` — see
  [`tracking/KNOWN_ISSUES.md`](tracking/KNOWN_ISSUES.md) and [`plans/ROADMAP.md`](plans/ROADMAP.md).)
- **Boards carry a stamp.** STATUS and KNOWN_ISSUES open with a one-line purpose, a
  `_Last updated:_` date, and their emoji legend.
- **ADRs are immutable.** To change a decision, add a superseding ADR; never rewrite an old one.

_Last updated: 2026-06-03._
