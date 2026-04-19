# Embedding API

zyterm builds as both a standalone binary (`./zyterm`) and a static
archive (`build/zyterm_embed.a`) you can link into another program.
The host program drives zyterm via the public header `<zyterm.h>` and a
deliberately tiny surface of seven symbols. Optional X11 clipboard
support is compiled in if libxcb is detected; falling back to no-op
stubs on systems without it.

## 1. Linking

```sh
make zyterm_embed.a                 # produces build/zyterm_embed.a (~304K)
cc host.c -Iinclude \
    build/zyterm_embed.a \
    -lpthread -o host
```

The archive links libc and pthread. X11 clipboard support is
auto-detected and statically linked if libxcb is present.

## 2. Surface

| Symbol                 | Kind                | Purpose                                             |
| ---------------------- | ------------------- | --------------------------------------------------- |
| `zyterm_main`          | `int (int, char**)` | CLI entry point. Same contract as `main`.           |
| `zt_g_embedded`        | `bool`              | Set to `true` to suppress process-level exits.      |
| `zt_g_embed_jmp`       | `sigjmp_buf`        | Optional fatal-path landing pad.                    |
| `zt_g_embed_jmp_armed` | `bool`              | Set to `true` after `sigsetjmp`.                    |
| `zt_embed_disarm`      | `void (void)`       | Disarm the jmp_buf after a normal return.           |
| `zt_embed_reset`       | `void (void)`       | Restore signal and TTY state. Call before each run. |
| `zt_trace`             | printf-like         | Append a line to `$ZYTERM_TRACE` if set.            |

That is the entire API. Nothing else is exported.

## 3. Minimal embed

```c
#include <zyterm.h>
#include <stdbool.h>

int main(void) {
    char *argv[] = { "zyterm", "/dev/ttyUSB0", "-b", "115200", NULL };

    zt_embed_reset();
    zt_g_embedded = true;
    int rc = zyterm_main(4, argv);
    zt_embed_disarm();
    return rc;
}
```

That works. The host process keeps control after the user quits zyterm
with `Ctrl+A q`.

## 4. Crash-safe embed

If you need zyterm to never terminate the host, even on a fatal path
inside zyterm, wrap the call with `sigsetjmp`:

```c
#include <zyterm.h>
#include <setjmp.h>
#include <signal.h>

int run_zyterm(int argc, char **argv) {
    zt_embed_reset();
    zt_g_embedded = true;

    if (sigsetjmp(zt_g_embed_jmp, 1) == 0) {
        zt_g_embed_jmp_armed = true;
        int rc = zyterm_main(argc, argv);
        zt_embed_disarm();
        return rc;
    }

    /* Reached on a fatal path inside zyterm. */
    zt_embed_disarm();
    return 1;
}
```

`zt_die`, the SIGSEGV/SIGBUS/SIGFPE/SIGABRT handlers, and the alarm
watchdog all check `zt_g_embed_jmp_armed` and `siglongjmp` back to your
landing pad instead of calling `_exit`.

## 5. Side effects to know

While `zyterm_main` is running it will:

- put `STDIN_FILENO` into raw mode (restored on return),
- install handlers for SIGINT, SIGWINCH, SIGSEGV, SIGBUS, SIGFPE, SIGABRT,
  SIGPIPE, SIGCHLD, and SIGALRM (restored on return),
- register an `atexit` cleanup hook (one-shot, idempotent),
- spawn one reader thread per open serial port (joined on return).

`zt_embed_reset` undoes any of these that were left dangling by an
abnormal exit from a previous run, so you can call `zyterm_main`
repeatedly without leaks or signal-handler accumulation.

## 6. Thread safety

`zyterm_main` is not reentrant. Do not call it from two threads at the
same time. Sequential calls from the same thread are fine, as long as
`zt_embed_reset()` runs between them.

## 7. Tracing

Set `ZYTERM_TRACE` to a writable path. Every fatal path inside zyterm
appends a one-line record there:

```sh
ZYTERM_TRACE=/tmp/zt.log ./host
cat /tmp/zt.log
# 2025-11-19T14:03:11Z pid=12345 zt_die: serial open /dev/ttyUSB0: No such file
```

This is the supported way to diagnose embed-mode failures without
strace or attaching a debugger.

## 8. ABI stability

The seven symbols above are stable. Internal headers under
`include/zyterm/internal/` are not part of the API and may change in
any release. Do not include them from your host.

## 9. Archive size

| Component                    | Size                              |
| ---------------------------- | --------------------------------- |
| `build/zyterm_embed.a`       | ~304K                             |
| Stripped binary (`./zyterm`) | ~184K                             |
| Runtime dependencies         | libc, pthread                     |
| Optional X11 support         | xcb, xcb-xfixes (build-time only) |
