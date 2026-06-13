# Archive — history only

**Everything under `archive/` is history, not current truth.** It is kept for provenance: to
show how the documentation and the codebase were understood at a point in time. Do not cite it
as the present state of zyterm, and do not link to it from current docs as a source of fact.

If you need to know how something works *now*, go to the live docs instead:

- How it works today → [`docs/reference/`](../reference/ARCHITECTURE.md)
- Rules that must not regress → [`docs/invariants/INVARIANTS.md`](../invariants/INVARIANTS.md)
- Why a decision was made → [`docs/decisions/`](../decisions/README.md)
- Known defects → [`docs/tracking/KNOWN_ISSUES.md`](../tracking/KNOWN_ISSUES.md)
- Non-defect work in flight → [`docs/tracking/STATUS.md`](../tracking/STATUS.md)

## Why these documents were archived

The pre-rebuild docs mixed three things that should never live in one file: durable rules,
live defect status, and time-stamped narrative. Before anything was moved here, its still-useful
content was lifted into the right kind of doc:

- **Durable, don't-regress rules** were promoted into
  [`invariants/INVARIANTS.md`](../invariants/INVARIANTS.md). If an archived document asserts a
  rule and the live invariants disagree, the invariants win.
- **Defects** were recorded as numbered issues in
  [`tracking/KNOWN_ISSUES.md`](../tracking/KNOWN_ISSUES.md) (IDs `ZT-001`…`ZT-028`), with detail
  files under [`tracking/issues/`](../tracking/issues/) for the high-severity ones. If an
  archived doc describes a bug, its live status lives in the tracker, not here.

Only after that extraction was a document moved into `archive/`. So an archived file may still
*describe* a behaviour that has since been corrected, contradicted, or filed as a defect — that
is expected. The archive is a snapshot; the live docs are the record.

A note on the rebuild's correction work: several features the old docs advertised as working are
in fact dead, broken, or stubbed (OSC 8 hyperlinks, the epoll/splice fast path, the fuzzy
finder, multi-pane, `rfc2217://`, and on-disk history/bookmarks). Those corrections are reflected
in the live reference, tracking, and plans docs — never trust an archived file's optimistic
claim over them.

## Contents

- [`audit/2026-06-03-source-review.md`](audit/2026-06-03-source-review.md) — the full
  source-review narrative behind the v1.2.0 documentation rebuild. It walks the codebase module
  by module and is the origin story for the `ZT-001`…`ZT-028` defect set and the corrected
  feature truth. Read it for *context and reasoning*; for the *current* status of any finding,
  follow it through to [`tracking/KNOWN_ISSUES.md`](../tracking/KNOWN_ISSUES.md).

---

_Last updated: 2026-06-03._
