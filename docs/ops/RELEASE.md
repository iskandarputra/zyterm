# Release runbook

How a zyterm release is cut, what CI guards it, and how to build a `.deb` locally.
This is the operational companion to [INVARIANTS §9](../invariants/INVARIANTS.md) (build &
release) and [ADR-0001](../decisions/0001-linux-first-single-binary.md) (Linux-first single
binary).

## Version source of truth

There is exactly one place the version lives:

```c
#define ZT_VERSION "1.3.0"
```

`src/zt_ctx.h:70`. Everything else derives from it:

- `build.sh` reads it with `get_version()` (`build.sh:42`) — a `grep -oP` on that `#define`.
- The `.deb` `Version:` field and filename (`zyterm_<version>_<arch>.deb`) come from the same
  value (`build.sh:108`, `build.sh:131`).
- The release workflow refuses to publish unless the git tag matches it (see below).

To bump the version you edit `ZT_VERSION` and nothing else. Do not hand-edit a version string
anywhere downstream — if you find one, it is a bug.

## Cutting a release

A release is driven entirely by pushing a `v*` tag. The flow:

1. Land all release content on `main` (changelog, version bump in `src/zt_ctx.h`).
2. Tag it: `git tag v1.2.0 && git push origin v1.2.0`.
3. `.github/workflows/release.yml` fires on the `v*` tag push.

The workflow has two jobs:

**`package`** — runs once per architecture from a matrix (`release.yml:20-27`):

| arch    | runner               |
| ------- | -------------------- |
| `amd64` | `ubuntu-latest`      |
| `arm64` | `ubuntu-24.04-arm`   |

Each `package` job:

1. **Verifies the tag matches `ZT_VERSION`** (`release.yml:39-45`). It strips the leading `v`
   from the tag, greps `ZT_VERSION` out of `src/zt_ctx.h`, and aborts with `version mismatch`
   if they differ. This is the gate that keeps a `v1.3.0` tag from ever shipping a binary that
   still reports `1.2.0`.
2. Installs `build-essential dpkg-dev`, builds the release binary with `make -j`.
3. Smoke-tests the freshly built binary: `./zyterm --version` and `./zyterm --help | head -5`.
4. Builds the package with `./build.sh deb` and uploads `releases/*.deb` as an artifact named
   `zyterm-deb-<arch>`.

**`release`** — runs after both `package` jobs succeed (`release.yml:64`):

1. Downloads both arch artifacts (`merge-multiple: true`).
2. Creates the GitHub release if it does not already exist (`gh release create … --generate-notes`).
3. Uploads both `.deb` files to the release with `--clobber`.

You can re-run packaging for an existing tag without re-tagging via the `workflow_dispatch`
input (`release.yml:7-11`) — supply the tag (e.g. `v1.2.0`) and it rebuilds and re-uploads the
assets.

## CI matrix (every push to `main` and every PR)

`.github/workflows/ci.yml` builds and tests on a 2×2 matrix — `{gcc, clang}` × `{none,
asan-ubsan}` (`ci.yml:23-24`). All four jobs run with `fail-fast: false` so one failure does
not mask the others, and each job is capped at 15 minutes (`ci.yml:16`) because a clean
build+test is under 90 seconds — the ceiling exists to catch hangs (wedged child, blocked
stdin, infinite poll), not slow runners.

What runs in each job:

| Step             | When                              | Source                |
| ---------------- | --------------------------------- | --------------------- |
| Check formatting | `none` + `gcc` only               | `make format-check`   |
| Build (release)  | `sanitizer == none`               | `make`                |
| Build (asan+ubsan) | `sanitizer == asan-ubsan`       | `CFLAGS=-fsanitize=address,undefined … make` |
| Smoke test       | always                            | `./zyterm --version`, `./zyterm --help \| head -20` |
| Unit tests       | both variants                     | `make test`           |
| cppcheck         | `none` + `gcc` only               | `make lint` (non-fatal) |

The sanitizer build is `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`
(`ci.yml:60-63`). The smoke step and every test step wrap the binary in `timeout`, and the job
sets `ZT_CI_STEP_TIMEOUT=600` as a belt-and-braces kill for any subprocess that runs over ten
minutes (`ci.yml:27-29`).

CI is Linux-only by design: zyterm targets Linux serial surfaces (termios2 custom baud,
inotify, `/sys/class/tty` VID/PID discovery) and ships no macOS binaries
([ADR-0001](../decisions/0001-linux-first-single-binary.md)).

## Building a `.deb` locally

You do not need CI to produce a package. From the repo root:

```bash
./build.sh deb        # build release binary, then package to releases/
```

This (`build.sh:103-155`):

1. Reads the version from `src/zt_ctx.h` and the host architecture from
   `dpkg --print-architecture`.
2. Builds the release binary with `make -j`.
3. Lays out `build/deb/zyterm_<version>_<arch>/` with the binary at `usr/local/bin/zyterm`.
4. Generates a `DEBIAN/control` file (depends only on `libc6`).
5. Runs `dpkg-deb --build --root-owner-group` → `releases/zyterm_<version>_<arch>.deb`.

Inspect what you built:

```bash
dpkg-deb -I releases/zyterm_1.2.0_amd64.deb     # control metadata
dpkg-deb -c releases/zyterm_1.2.0_amd64.deb     # file contents
```

`./build.sh install` does the same and then `sudo dpkg -i` the result. `./build.sh all` runs the
full `format → lint → build → test → deb` pipeline (`build.sh:176-182`).

The local `.deb` is architecture-tagged to whatever machine you build on. CI is what produces
the canonical `amd64` + `arm64` pair for a tagged release.

## The glibc `.symver` pin

zyterm's only runtime dependency is libc. To keep a binary built on a recent toolchain runnable
on an older glibc, `src/zt_internal.h:66-84` pins `strtol`/`strtoul` back to a base glibc symbol
version.

The problem it solves (`zt_internal.h:26-37`): GCC ≥ 13 on glibc ≥ 2.38 silently rewrites
`strtol`/`strtoul` to the C23 variants `__isoc23_strtol`/`__isoc23_strtoul` at the header level.
That stamps the binary with a `GLIBC_2.38` floor, so a build from Ubuntu 24.04 fails to start on
Ubuntu 22.04 with `version 'GLIBC_2.38' not found`. The fix targets the already-redirected names
and maps them back to a classic base symbol:

```c
__asm__(".symver __isoc23_strtol,strtol@" ZT_GLIBC_BASE_VER);
__asm__(".symver __isoc23_strtoul,strtoul@" ZT_GLIBC_BASE_VER);
```

Two details that the v1.2.0 release shook out:

- **Per-architecture base version** (`zt_internal.h:66-75`). x86_64 has carried every libc symbol
  since `GLIBC_2.2.5`, but aarch64 only entered glibc at `2.17`. Pinning to a version that does
  not exist for the target arch produces a link error
  (`undefined reference to strtoul@GLIBC_2.2.5`) — exactly what first broke the arm64 release
  build. So the macro is `GLIBC_2.2.5` on `__x86_64__` and `GLIBC_2.17` on `__aarch64__`/`__riscv`.
- **Skipped under sanitizers** (`zt_internal.h:39-64`, gated by `ZT_SANITIZER_ACTIVE`). ASan/TSan/
  MSan install libc interceptors keyed on the modern `@GLIBC_2.38` symbol; the `.symver` back to
  the base symbol bypasses the interceptor's argument-marshalling thunk and turns a harmless
  `strtol("230400")` into a SEGV inside libc. Sanitizer builds are dev-only and do not need
  old-glibc portability, so the pin is compiled out when any sanitizer is active. The detection
  guards `__has_feature` behind a nested `#if` so GCC (which lacks `__has_feature`) never tries to
  evaluate it.

The pin is also a no-op on non-Linux / non-glibc targets (musl, macOS, FreeBSD) — it is wrapped
in `#if defined(__linux__) && defined(__GLIBC__)` and only emits the `.symver` when glibc is
≥ 2.38, the version where the redirect appears.

**What it buys you:** a release binary that runs on materially older glibc (down to the per-arch
base) with no recompilation, while keeping the one-binary, libc-only deploy story intact.

## Open gaps

These are tracked release hardening items, not implemented. Do not describe them as done. See
[plans/RELIABILITY_HARDENING.md](../plans/RELIABILITY_HARDENING.md).

- **No supply-chain integrity yet.** The release publishes bare `.deb` files: no detached
  signatures, no published SHA-256 checksums, and no SBOM. A consumer cannot today verify a
  download against a signed manifest.
- **Release build lacks hardening flags.** The shipped binary is built with plain `make` (`-O3
  -Wall -Wextra`); it does not add `-D_FORTIFY_SOURCE=2`, `-fstack-protector-strong`,
  `-fstack-clash-protection`, `-fPIE -pie`, or `RELRO`/`-z now` link flags.
- **`-march=native` in the `release` make target.** `make release` (`Makefile:85`) adds
  `-flto -march=native -DNDEBUG`. `-march=native` tunes for the *builder's* CPU and can emit
  instructions the consumer's CPU lacks (SIGILL on older hardware). It is unsuitable for a
  distributable artifact. (The CI/packaging path uses plain `make`, not `make release`, but the
  target exists and the flag needs to go before `release` is used for distribution.)

---

_Last updated: 2026-06-03._
