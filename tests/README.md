# Tests

Comprehensive test suite across three tiers: 136 unit tests + 20
PTY-harness tests = 156 total assertions. All tiers run in CI.

## Layout

```
tests/
├── unit/         pure-function tests, no side effects (136 tests)
├── integration/  cross-module flows that need files or pipes
└── pty/          forkpty harness, full UI through a real TTY (20 tests)
```

The Makefile auto-discovers any `*.c` it finds under those three
subdirectories.

## Run

```sh
make test            # build and run all tiers (from repo root)
make -C tests unit   # only the unit tier (136 pass expected)
make -C tests pty    # only the pty harness (20 pass expected)
```

Or use the build helper:

```sh
./build.sh test      # build embed archive + run all test tiers
```

Expected: **136 unit + 20 pty = 156 / 156 pass**.

## Add a test

1. Drop `tests/<tier>/your_test.c`.
2. `#include "../test_macros.h"` if you want the assertion sugar
   (`ZT_OK`, `ZT_EQ`, ...).
3. Provide `int main(void)` returning 0 on success, non-zero on failure.
4. `make test` picks it up automatically.

For PTY tests you'll typically `forkpty` the zyterm binary and
script-drive it through the master fd. See `tests/pty/pty_harness.c`
for the established pattern.

## Conventions

- One concern per test file. If a file grows past about 300 LOC, split it.
- No global state. Each `main` cleans up after itself.
- No network calls.
- Wall-clock budget: the full suite must finish in under 10 seconds on
  a laptop.

See [docs/ARCHITECTURE.md, section 11](../docs/ARCHITECTURE.md#11-testing-strategy)
for the testing rationale.
