# Embedding zyterm

zyterm runs either as the standalone `./zyterm` binary or linked **in-process** into a host
program — for example, as a builtin of the `zy` shell. This document is the canonical embedding
contract: the exact public surface, how to link it, how to make an in-process run crash-safe, the
side effects each run leaves behind, and what is (and is not) ABI-stable.

> The public header is [`include/zyterm.h`](../../include/zyterm.h). Everything not declared there
> is internal and may change at any time — see [ABI stability](#abi-stability) below.

---

## The public surface: exactly 7 symbols

The entire embedding API is **7 symbols**, all declared in `include/zyterm.h` and defined in
`src/core/core.c`. (An earlier `ARCHITECTURE.md` claimed "four symbols" — that was wrong; this
document is canonical.)

| Symbol | Kind | Source | Purpose |
| --- | --- | --- | --- |
| `int zyterm_main(int, char **)` | function | runs full CLI | The entry point. Same behaviour as the standalone binary's `main`. |
| `bool zt_g_embedded` | variable | `core.c:46` | Set `true` before calling to suppress host-killing behaviour (no `alarm()` watchdog, no `_exit()` in crash paths). |
| `sigjmp_buf zt_g_embed_jmp` | variable | `core.c:53` | Jump buffer the host arms with `sigsetjmp()` so fatal paths return to the host instead of `exit()`-ing. |
| `bool zt_g_embed_jmp_armed` | variable | `core.c:54` | Guards `zt_g_embed_jmp`. Set `true` after arming; cleared automatically when a longjmp fires. |
| `void zt_embed_disarm(void)` | function | `core.c:56` | Clears `zt_g_embed_jmp_armed` after a normal return, so a stale buffer can't be jumped into later. |
| `void zt_embed_reset(void)` | function | `core.c:93` | Resets process-wide state to a clean baseline. **Call before every** `zyterm_main`. |
| `void zt_trace(const char *, ...)` | function | `core.c:73` | Appends a diagnostic line to `$ZYTERM_TRACE`; no-op when unset. |

That is the complete surface. Anything else you find in `src/` or the `include/zyterm/internal/`
headers is private.

---

## Linking

Build the embeddable archive and link it with the threads and dynamic-loader libraries:

```sh
make zyterm_embed.a          # produces build/zyterm_embed.a
cc host.c build/zyterm_embed.a -lpthread -ldl -o host
```

- **`-lpthread`** is required because the optional `--threaded` reader spawns a worker
  (`src/loop/rx_thread.c`).
- **`-ldl`** is required because the native X11 clipboard path resolves `libxcb.so.1` at runtime via
  `dlopen()` (`src/proto/clipboard.c:213`) — there is no compile-time dependency on libxcb.

On glibc ≥ 2.34 both `-lpthread` and `-ldl` are absorbed into libc and the flags are harmless
stubs; on older glibc they name real shared objects. On non-Linux toolchains drop `-ldl`
(`dlopen` lives in libc / libSystem) — see the OS notes in the `Makefile` `LDLIBS` block.

There are no other link-time dependencies. See [runtime dependencies](#runtime-dependencies).

---

## Minimal embed example

The smallest correct in-process run sets `zt_g_embedded`, resets state, and calls `zyterm_main`:

```c
#include <zyterm.h>

int run_zyterm(int argc, char **argv) {
    zt_g_embedded = true;   /* don't kill the host on a fatal path */
    zt_embed_reset();       /* clean baseline — mandatory before each call */
    return zyterm_main(argc, argv);
}
```

This is enough for the common case, but a fatal error inside zyterm (`zt_die`, see `core.c:215`)
will still call `exit(1)` and take the host down, because no jump buffer is armed. For a host
process you care about, use the crash-safe pattern below.

---

## Crash-safe embedding with `sigsetjmp`

To keep the host alive when zyterm hits a fatal path, arm `zt_g_embed_jmp` with `sigsetjmp()`
before the call and set `zt_g_embed_jmp_armed`. Fatal paths then `siglongjmp` back to the host
instead of terminating the process.

```c
#include <setjmp.h>
#include <zyterm.h>

int run_zyterm_safe(int argc, char **argv) {
    zt_g_embedded = true;
    zt_embed_reset();                          /* clean slate, every time */

    int rc = sigsetjmp(zt_g_embed_jmp, 1);     /* save signal mask (arg 1) */
    if (rc != 0) {
        /* We longjmped back here from a fatal path.
         * rc carries the status: 1 from zt_die(), or 128+signo from a crash. */
        zt_embed_reset();                      /* re-baseline after the abort */
        return rc;
    }
    zt_g_embed_jmp_armed = true;               /* arm only after sigsetjmp */

    rc = zyterm_main(argc, argv);              /* normal run */
    zt_embed_disarm();                         /* clear the armed flag */
    return rc;
}
```

Two distinct fatal paths can jump back:

- **`zt_die()`** — the controlled fatal-error exit. When embedded and armed it does
  `siglongjmp(zt_g_embed_jmp, 1)` instead of `exit(1)` (`core.c:224`).
- **The crash handler `sig_crash()`** — but **only for `SIGABRT` and `SIGFPE`**. These are signals
  where the heap and runtime state are still likely coherent, so unwinding to the host is
  defensible; it jumps with status `128 + signo` (`core.c:295`, `core.c:312`).

`SIGSEGV` and `SIGBUS` are deliberately **not** recovered. A memory fault implies corruption;
`siglongjmp`-ing out of it and resuming arbitrary host code is undefined behaviour and a
near-certain double-fault on the next `malloc`/`free`. For those signals zyterm restores the
terminal and then **re-raises the signal** under `SA_RESETHAND`, so the OS produces a core dump and
the host dies cleanly with a known exit status (`core.c:314`). That is strictly safer than
continuing on corrupted state — design your host accordingly.

`sigsetjmp` is used (not plain `setjmp`) with a non-zero second argument so the signal mask is
saved and restored across the jump; the crash handler runs with the faulting signal blocked, and a
plain `setjmp`/`longjmp` would leave it blocked in the host.

---

## State each run leaves behind, and `zt_embed_reset`

`zyterm_main` is built for a process that lives until exit, so it leaves global, sticky state. When
embedded you must scrub that between runs. `zt_embed_reset()` (`core.c:93`) is the single function
that does so; the host **must** call it immediately before each `zyterm_main`.

What a run touches and what `zt_embed_reset` restores:

| Side effect | Installed / set by | Reset behaviour |
| --- | --- | --- |
| **Raw terminal mode** | `setup_stdin_raw` snapshots `termios` + flags into `zt_g_orig_stdin` | `restore_terminal` (registered via `atexit`) undoes it; `zt_embed_reset` zeroes `zt_g_stdin_saved` so the next run re-snapshots cleanly. |
| **Signal handlers** | `install_signals` installs INT/TERM/HUP/QUIT/WINCH/PIPE + crash handlers, saving the host's originals | `zt_embed_reset` calls `uninstall_signals()` (`core.c:99`), restoring the host's previous handlers. Both are idempotent. |
| **`atexit(restore_terminal)`** | registered once in the runtime (`src/loop/runtime.c:39`) | Stays registered for the process lifetime — it is a safety net for the standalone binary. Harmless in a host because `restore_terminal` early-returns when nothing is saved. |
| **`alarm()` watchdog** | the standalone shutdown path arms `alarm(3)` | **Skipped entirely when `zt_g_embedded`** (`src/main.c:789`) — it would otherwise kill the host. |
| **Quit / winch flags** | `zt_g_quit`, `zt_g_winch` (`sig_atomic_t`) | Cleared to `0` by `zt_embed_reset`. |
| **UI-active flag** | `zt_g_ui_active` (set even on non-tty stdin) | Forced `false` by `zt_embed_reset` so a prior non-tty run can't poison the next one. |
| **Output buffer** | the 256 KiB render buffer in `core.c` | Discarded (length zeroed) so leftover bytes can't bleed onto the host terminal at the next flush. |
| **`--threaded` reader** | `pthread` worker in `src/loop/rx_thread.c` owning a `dup()`ed fd + SPSC ring | Joined as part of the normal run teardown. |
| **Other subsystem statics** | session, multi | Scrubbed via `session_embed_reset()` / `multi_embed_reset()` called from `zt_embed_reset`. |

`zt_embed_reset` guarantees the second (and every subsequent) embedded run starts identically to
the first, **regardless of how the previous one terminated** — normal exit, `siglongjmp` from
`zt_die`, or a recovered crash. After a longjmp you should call it again before retrying (as the
crash-safe example does).

### Not reentrant

zyterm relies on process-wide globals (the output buffer, the cached stdin snapshot, the quit/winch
flags, file-static state across modules). **Do not** call `zyterm_main` concurrently from two
threads, and do not nest calls. One embedded run at a time, fully torn down (or reset) before the
next.

---

## Diagnostics: `ZYTERM_TRACE`

`zt_trace()` appends a timestamped, pid-tagged line to a file, used to diagnose embedded fatal
paths (`zt_die`, `sig_crash`, signal install/uninstall) without `strace` or a debugger attach. It
is enabled by **either**:

1. setting `ZYTERM_TRACE=/path/to/file` in the environment, or
2. the sentinel file `/tmp/zyterm.trace` existing (`core.c:72`) — useful under hosts like `zy`
   whose `export` does not propagate to builtins.

When neither is present, `zt_trace` is a no-op. It is best-effort and never fails. Note that
`zt_trace` is **not** async-signal-safe, so the crash handler only calls it on the embed-recovery
path (where a `siglongjmp` is imminent anyway), never before re-raising `SIGSEGV`/`SIGBUS`.

---

## ABI stability

- **Stable surface = the 7 symbols above, and only those.** Their signatures in
  `include/zyterm.h` are the contract.
- **Everything under `include/zyterm/internal/` is unstable.** The module headers
  (`core.h … loop.h`) and `src/zt_ctx.h` exist to enforce the internal dependency chain (see
  [ARCHITECTURE.md](./ARCHITECTURE.md)); they are not an embedding interface and change freely.
- Do not link against internal symbols, include internal headers from host code, or depend on the
  layout of `struct zt_ctx`.

---

## Runtime dependencies

- **libc only.** No required shared libraries beyond the C runtime.
- The X11 clipboard path is loaded lazily with `dlopen("libxcb.so.1")` at first use
  (`src/proto/clipboard.c:213`); if libxcb is absent, the native clipboard is simply unavailable
  and OSC 52 still works. No build-time or hard runtime dependency on libxcb.
- The release build pins `strtol`/`strtoul` back to a base glibc symbol via an arch-aware
  `.symver` directive in `src/zt_internal.h:80` (`GLIBC_2.2.5` on x86_64, `GLIBC_2.17` on
  aarch64/riscv). This stops GCC ≥ 13 / glibc ≥ 2.38 from stamping a `GLIBC_2.38` floor onto the
  binary, so a build made on a newer distro still starts on an older one. The pin is **skipped
  under sanitizers** (`__SANITIZE_ADDRESS__` / `__has_feature`) because their libc interceptors key
  on the modern symbol; bypassing them would turn a benign `strtoul` into a fault.

---

## See also

- [ARCHITECTURE.md](./ARCHITECTURE.md) — module layout, the single `zt_ctx`, signals, and the build.
- [INVARIANTS.md §2](../invariants/INVARIANTS.md#2-signals--async-signal-safety) — signals and
  async-signal-safety rules that the crash/embed paths above must uphold.
