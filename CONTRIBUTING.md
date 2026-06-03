# Contributing to zyterm

zyterm is a single-binary Linux serial terminal in C (~12.5K LOC, version 1.2.0). It is small,
the build is plain, and the rules that protect it are few. This document is the change loop: how
to build and run, how code is styled, the one structural rule you cannot break, how to add code
and tests, how to commit, and — the part that keeps the project honest — how to work with `docs/`.

For security reports, do **not** open a public issue: see [SECURITY.md](SECURITY.md).

---

## 1. Build and run

You need a C compiler (`gcc` or `clang`) and GNU `make`. Runtime dependencies are libc only; the
X11 clipboard is loaded at runtime via `dlopen("libxcb.so.1")`, so there is no build-time X11
dependency.

```sh
git clone https://github.com/iskandarputra/zyterm.git
cd zyterm
make            # ./zyterm — release, -O3
make debug      # ./zyterm — -O0 -g3 -DZT_DEBUG, for gdb / valgrind / sanitizers
make test       # unit + pty + integration suites (links against zyterm_embed.a)
make modules    # per-module file/LOC summary
```

The Makefile **auto-discovers** sources with `find src -name '*.c'` (`Makefile:52`), so adding a
file never requires a Makefile edit. Each `src/.../<file>.c` compiles to `build/obj/.../<file>.o`
and links into `./zyterm`.

| Target                | Produces |
|-----------------------|----------|
| `make` / `make all`   | `./zyterm` (release, `-O3`). |
| `make debug`          | `-O0 -g3 -DZT_DEBUG`. |
| `make release`        | adds `-flto -march=native -DNDEBUG`. |
| `make zyterm_embed.a` | static archive for embedders — all objects **except** `main.o` (`Makefile:89`). |
| `make test`           | builds the archive, then runs unit + pty + integration suites. |
| `make format`         | `clang-format -i` across `src/` + `include/`. |
| `make format-check`   | dry-run format check (what CI runs). |
| `make lint`           | `cppcheck --enable=all` across `src/`. |
| `make modules`        | per-module file/LOC summary. |

### build.sh

A convenience wrapper around the same workflows, plus `.deb` packaging:

```sh
./build.sh build          # release binary
./build.sh debug          # debug binary
./build.sh format         # auto-format all sources
./build.sh format-check   # dry-run format check (CI gate)
./build.sh lint           # cppcheck
./build.sh test           # build embed archive + run the full suite
./build.sh deb            # produce releases/zyterm_<version>_<arch>.deb
./build.sh all            # format → lint → build → test → deb
```

The packaged version is read from `ZT_VERSION` in `src/zt_ctx.h` — never hand-edit it in two
places.

---

## 2. Code style

Style is enforced by [`.clang-format`](.clang-format); CI runs `make format-check` on every PR and
fails on any drift. Run `make format` (or `./build.sh format`) before you commit. The rules:

| Topic        | Rule |
|--------------|------|
| Standard     | C11 (`-std=gnu11`). No C23-only constructs. |
| Indentation  | 4 spaces, never tabs. K&R / attached braces. |
| Width        | 96 columns hard, 80 preferred. |
| Pointers     | Bind to the variable: `char *p`, not `char* p`. |
| Naming       | `lower_snake_case` for functions and locals; `ZT_UPPER_SNAKE` for macros and enum values. |
| Globals      | Every globally visible symbol is prefixed `zt_`. Non-negotiable (e.g. `zt_g_quit`, `zt_write_all`). |
| `static`     | Anything **not** declared in `include/zyterm/internal/<module>.h` must be `static`. |
| Header guards| `ZYTERM_<NAME>_H_` with a trailing underscore. |
| Doc comments | A `/** ... */` Doxygen block on every exported function and every header. |

Tabular alignment in `case` arms and field declarations is intentional and preserved by the
formatter — don't fight it.

---

## 3. The dependency-chain rule

This is the one structural invariant you cannot break. The modules form a one-way chain, enforced
by `#include` order — each `include/zyterm/internal/<m>.h` includes exactly the one beneath it
(verified at line 15 of each header):

```
core ← serial ← log ← proto ← render ← tui ← net ← ext ← loop
```

A module may only call **down** the chain (toward `core`), never up. If your change needs a
back-edge — `core` reaching into `tui`, say — the function is in the wrong module; move it down to
where its dependencies already live. `main.c` is the only translation unit allowed to include
`loop/`, and `src/zt_internal.h` is the umbrella header that includes only `loop.h`.

All per-process state lives in a single `zt_ctx` (`src/zt_ctx.h`), grouped by feature tier. Most
fields are main-thread-only; do not add cross-thread sharing without reading
[INVARIANTS §8](docs/invariants/INVARIANTS.md) (module dependency chain & `zt_ctx` ownership) and
[INVARIANTS §4](docs/invariants/INVARIANTS.md) (the reader-thread SPSC ring). The full picture is
in [docs/reference/ARCHITECTURE.md](docs/reference/ARCHITECTURE.md).

---

## 4. Adding a module or a file

Because sources are auto-discovered, adding code is mechanical:

1. Drop the `.c` under the right `src/<module>/` — pick the module by *kind*, respecting §3.
2. Declare anything other modules need in `include/zyterm/internal/<module>.h`; everything else is
   `static`. The module header includes exactly the one beneath it in the chain — keep that line.
3. Add at least one test (see §5). Run `make format` and `make test`.

A brand-new module is the same plus a new `include/zyterm/internal/<newmod>.h` whose first include
is the header of the module immediately below it; nothing above may include it unless the chain
allows. See [docs/reference/ARCHITECTURE.md §1–2](docs/reference/ARCHITECTURE.md) before
introducing one.

---

## 5. Tests

Every change that touches a module should ship at least one assertion. The suites live under
`tests/`, and `tests/Makefile` auto-discovers any `*.c` in these three tiers:

- `tests/unit/` — pure-function and per-module tests (`tests/unit/test_subsystems.c` covers 20+
  subsystems).
- `tests/integration/` — cross-module flows.
- `tests/pty/` — anything that needs a real TTY, via `forkpty` (`tests/pty/pty_harness.c`).

Tests link against `zyterm_embed.a`, so they exercise the same objects that ship (minus `main.o`).

```sh
make test               # build the archive + run all three tiers
make -C tests unit      # unit only
make -C tests pty       # pty harness only
make -C tests integration
```

---

## 6. Commits and pull requests

- One logical change per commit. Imperative subject line, ≤72 chars: `add COBS frame decoder`, not
  `added COBS frame decoder`.
- When a change touches `docs/`, say so in the body: `Updates docs/reference/ARCHITECTURE.md §1`.
- Branch off `main`. CI must pass: gcc+clang × {none, asan-ubsan}, format-check, smoke, and
  `make test`.
- Squash trivial fixup commits before merge.

A bug report (not a security report — see §8) should carry: `./zyterm --version`, `uname -a`, the
exact CLI you ran, a minimal reproducer, and any non-empty `$ZYTERM_TRACE` output.

---

## 7. Working with `docs/`

zyterm's docs are organized by **kind**, not topic, and the discipline is **route, don't dump**:
new knowledge goes to the one place its kind belongs, and the relevant doc is updated **in the same
change** that creates the fact. A code change with a stale doc is an incomplete change.

The router is [docs/README.md](docs/README.md). Where things go:

| You have…                                          | It goes to… |
|----------------------------------------------------|-------------|
| A **defect** (something is wrong in `src/`)        | A row in [docs/tracking/KNOWN_ISSUES.md](docs/tracking/KNOWN_ISSUES.md); if high/critical, also a detail file [docs/tracking/issues/ZT-NNN-*.md](docs/tracking/issues/). |
| **Non-defect** work (planned features, refactors)  | [docs/tracking/STATUS.md](docs/tracking/STATUS.md) or [docs/plans/ROADMAP.md](docs/plans/ROADMAP.md). |
| A **decision** you made (and why)                  | A new, immutable ADR in [docs/decisions/](docs/decisions/) — append-only; supersede, never edit. |
| A **don't-regress rule** the change establishes    | The matching section of [docs/invariants/INVARIANTS.md](docs/invariants/INVARIANTS.md). |
| **How a subsystem works now**                      | [docs/reference/](docs/reference/) (ARCHITECTURE, INTERNALS, CLI, KEYBINDINGS, EMBEDDING, FAQ). |
| A **user-visible change**                           | The `[Unreleased]` section of [CHANGELOG.md](CHANGELOG.md). |
| A **security surface**                              | [SECURITY.md](SECURITY.md) + a `security` row in KNOWN_ISSUES. |

Rules of the road:

- **Defects get an ID.** Take the next monotonic `ZT-NNN` (never reuse a retired number), add a row
  to the "Open" table of KNOWN_ISSUES in severity order, and — if it is 🔴 high/critical — write a
  detail file from the template in the [shared brief / KNOWN_ISSUES header](docs/tracking/KNOWN_ISSUES.md).
  When it is fixed, **move** the row to "Resolved" with the fixing commit — never delete it. IDs
  are permanent; the board is the historical record.
- **Never advertise a dead feature.** If something doesn't work, it is a KNOWN_ISSUES row or a
  `plans/` entry, not a README bullet. (Today: OSC 8 hyperlinks, the epoll/splice fast path, the
  fuzzy finder, and multi-pane are not wired — see KNOWN_ISSUES and
  [docs/plans/ROADMAP.md](docs/plans/ROADMAP.md). History and bookmarks are in-memory only.)
- **ADRs are immutable.** To change a decision, add a superseding ADR that links the old one; don't
  rewrite history.
- **Keep docs synced in the same PR.** If you change a CLI flag, update
  [docs/reference/CLI.md](docs/reference/CLI.md) and the man page. If you change a keybinding,
  update [docs/reference/KEYBINDINGS.md](docs/reference/KEYBINDINGS.md). If you change the embedding
  surface, update [docs/reference/EMBEDDING.md](docs/reference/EMBEDDING.md). Reviewers will ask.

Board and tracker docs carry a one-line purpose and a `_Last updated: …_` stamp; keep it current
when you touch them.

---

## 8. Security

Local-machine surfaces — the unauthenticated loopback HTTP bridge, the detach socket, and the
`--metrics` socket — are real attack surface. Do **not** file public issues for them. Read and
follow [SECURITY.md](SECURITY.md); the known instances are tracked as `security` rows in
[docs/tracking/KNOWN_ISSUES.md](docs/tracking/KNOWN_ISSUES.md) with fix directions in
[docs/plans/RELIABILITY_HARDENING.md](docs/plans/RELIABILITY_HARDENING.md).
