# Contributing

Thanks for your interest. zyterm is small (around 8.8k LOC), focused,
and friendly to drive-by patches.

## 1. Build and run

```sh
git clone https://github.com/iskandarputra/zyterm.git
cd zyterm
make            # ./zyterm  (release, -O3)
make debug      # ./zyterm  (-O0 -g3 for gdb / valgrind)
make test       # 156 pass: 136 unit + 20 pty
make modules    # per-module LOC summary
```

You need: `cc` (gcc or clang), GNU make, and `pthread`. That's it.

### Using build.sh

A convenience script wraps common workflows:

```sh
./build.sh all            # format → lint → build → test → deb
./build.sh format         # auto-format all source files
./build.sh format-check   # dry-run format check (CI uses this)
./build.sh deb            # produce a .deb package in releases/
```

## 2. Style

| Topic        | Rule                                                                                      |
| ------------ | ----------------------------------------------------------------------------------------- |
| Standard     | C11 (`-std=gnu11`). No C23-only constructs.                                               |
| Indentation  | 4 spaces, never tabs. K&R braces.                                                         |
| Width        | 96 cols hard, 80 preferred.                                                               |
| Naming       | `lower_snake_case` for functions and locals. `ZT_UPPER_SNAKE` for macros and enum values. |
| Globals      | Prefix every globally visible symbol with `zt_`. This is non-negotiable.                  |
| `static`     | Anything not declared in `include/zyterm/internal/<module>.h` must be `static`.           |
| Headers      | Include guard `ZYTERM_<NAME>_H_` with trailing underscore.                                |
| Doc comments | `/** ... */` Doxygen blocks on every exported function and every header.                  |
| Format       | `make format` runs clang-format. Run it before every commit.                              |

### Formatting enforcement

CI runs `make format-check` on every PR. If your code isn't formatted,
the check will fail. Run `make format` (or `./build.sh format`) before
committing to avoid this.

The formatting rules are defined in [`.clang-format`](.clang-format) at
the project root.

## 3. The dependency rule

The internal modules form a chain:

```
core ← serial ← log ← proto ← render ← tui ← net ← ext ← loop
```

A module to the left may not call into a module to its right. The chain
is enforced by `#include` order: each module header includes only the
one beneath it. If your change requires a back-edge, the function is in
the wrong module. Move it down.

## 4. Adding a module

The cookbook lives in [ARCHITECTURE.md, section 8](ARCHITECTURE.md#8-adding-a-module).
Two-line summary: drop the `.c` under `src/<module>/`, add prototypes to
`include/zyterm/internal/<module>.h`, and ship a unit test under
`tests/unit/`.

## 5. Tests

Every change that touches a module should come with at least one
assertion. Existing tests live in:

- `tests/unit/test_subsystems.c`: 136 assertions across 20+ subsystems.
- `tests/pty/pty_harness.c`: 20 assertions, end-to-end via `forkpty`.

Place new files in:

- `tests/unit/` for pure-function tests,
- `tests/integration/` for cross-module flows,
- `tests/pty/` if a real TTY is required.

The Makefile auto-discovers any `*.c` under those three subdirs.

```sh
make test           # build and run everything
make -C tests unit  # only unit tests
make -C tests pty   # only the pty harness
```

## 6. Packaging

You can test `.deb` packaging locally:

```sh
./build.sh deb            # produces releases/zyterm_<version>_<arch>.deb
dpkg-deb -I releases/zyterm_*.deb   # inspect metadata
dpkg-deb -c releases/zyterm_*.deb   # inspect contents
```

The version is extracted automatically from `ZT_VERSION` in
`src/zt_ctx.h`.

## 7. Commits

- One logical change per commit.
- Imperative subject line, 72 chars or fewer: `add COBS frame decoder`,
  not `added COBS frame decoder`.
- Reference docs updated in the body where relevant:
  `Updates docs/ARCHITECTURE.md section 1 (LOC table).`

## 8. Pull requests

- Branch off `main`. PRs against feature branches are fine but rebased
  before merge.
- CI must pass (build + format check + `make test`).
- Squash trivial fixup commits.

## 9. Bug reports

Open an issue with:

1. zyterm version (`./zyterm --version`),
2. OS and kernel (`uname -a`),
3. the exact CLI you used,
4. a minimal reproducer,
5. an excerpt of `$ZYTERM_TRACE` output if non-empty.

## 10. Security

Report security issues privately to the maintainer. Please do not file
public issues for remote-attack vectors (HTTP bridge, sessions, metrics
exporter).
