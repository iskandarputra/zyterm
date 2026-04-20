# Tests

Test suite across three tiers. All tiers run in CI.
Run `make test` from the repo root to see the current counts.

## Layout

```
tests/
├── unit/         pure-function tests, no side effects
├── integration/  cross-module flows that need files or pipes
└── pty/          forkpty harness, full UI through a real TTY
```

The Makefile auto-discovers any `*.c` it finds under those three
subdirectories.

## Run

```sh
make test            # build and run all tiers (from repo root)
make -C tests unit   # only the unit tier
make -C tests pty    # only the pty harness
```

Or use the build helper:

```sh
./build.sh test      # build embed archive + run all test tiers
```

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
- Wall-clock budget: the full suite should finish in under 10 seconds
  on a laptop.

See [docs/ARCHITECTURE.md, section 11](../docs/ARCHITECTURE.md#11-testing-strategy)
for the testing rationale.
