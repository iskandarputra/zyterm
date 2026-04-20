# Architecture

zyterm is organised into ten modules under `src/`, plus `main.c`.
Each module owns one concern, lives in its own directory, and exposes
one public header at `include/zyterm/internal/<module>.h`.

The whole runtime carries a single `zt_ctx` value (defined in
`src/zt_ctx.h`). zyterm is single-threaded by default; a flat struct
keeps things simple and gives the compiler room to inline.

## 1. Module map

| Module    | Responsibility                                                                                                                                                                    |
| --------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `core/`   | Logging, output buffer, signals, terminal raw mode, time, CRC.                                                                                                                    |
| `serial/` | Open and configure the serial port (`termios2` / `IOSSIOSPEED`), reconnect probe, `epoll(7)` + `splice(2)` fast path on Linux, `TIOCGICOUNT` / `TIOCMGET` polling, autobaud.      |
| `log/`    | Persistent log file with rotation, JSONL emit, scrollback ring buffer.                                                                                                            |
| `proto/`  | COBS, SLIP, HDLC, and length-prefixed frame decoders, XMODEM-CRC, YMODEM/ZMODEM via `lrzsz`, clipboard (dlopen xcb at runtime), OSC 52, OSC 8 hyperlinks, ANSI SGR pass-through, KGDB raw mode, F-key macros. |
| `render/` | RX byte-stream parsing, Zephyr/severity colouring, scrollback push, throughput sparkline.                                                                                         |
| `tui/`    | HUD row, dialogs, command menu popup, settings menu (4-page serial/display/keyboard/logging), keybindings reference popup, search overlays, pager, fuzzy finder.                  |
| `net/`    | HTTP + SSE + WebSocket remote view, Prometheus text-format metrics, UNIX-socket detach/attach.                                                                                    |
| `ext/`    | Bookmarks, log-file diff, RX filter subprocess, log-level mute, multi-device panes, INI profiles, reconnect popup.                                                                |
| `loop/`   | Keyboard input plus escape parsing, send pipeline, optional SPSC RX thread, top-level `run_interactive` / `run_dump` / `run_replay`.                                              |
| `main.c`  | CLI option parsing and entry point.                                                                                                                                               |

Run `make modules` for a live per-module breakdown.

## 2. Dependency rule

Every internal header includes exactly the one beneath it:

```
core   ← serial   ← log   ← proto   ← render   ← tui   ← net   ← ext   ← loop
```

A `.c` file in module N can call into any module to its left in the
chain, but not to its right. The headers physically prevent it. This
enforces the design without a build-time linter.

`main.c` is the only translation unit allowed to call into `loop/`
directly. It owns the option parser and the lifetime of `zt_ctx`.

## 3. Runtime model

zyterm is single-threaded by default:

- One thread reads stdin, one event loop, one writer.
- All state lives in a single `zt_ctx` value.
- All polling is done with `poll(2)`, or `epoll(7)` on Linux when
  `--epoll` is set.
- Signal handlers only flip the `zt_g_quit` and `zt_g_winch`
  `sig_atomic_t` flags. They never touch `zt_ctx`.

There is an optional reader thread (`--threaded`):

- It drains the serial fd into a 1 MiB SPSC ring (`loop/rx_thread.c`).
- It wakes the main thread via a tiny pipe used as an eventfd.
- It is meant for high baud rates (above 1 Mbps), where `read(2)`
  latency in the main loop starts to lose bytes.
- The main thread still owns all rendering and writing.

## 4. Hot paths

These are the performance-sensitive code paths. We've tried to keep
them tight, though there's always room for improvement.

| Path            | Approach                                                                                             |
| --------------- | ---------------------------------------------------------------------------------------------------- |
| RX to screen    | One `read()` (up to 64 KiB), one `framing_feed()`, one `render_rx()`.                                |
| Stdout writes   | UI output goes through a coalescing buffer (`ob_*`), flushed once per loop iteration via `writev`.   |
| RAW log capture | On Linux, `splice(2)` from the serial fd into the log file fd, avoiding a copy through userspace.    |
| TX              | Bytes are spaced by `ZT_FLUSH_DELAY_US` (2 ms) to avoid overrunning slow embedded UART RX buffers.   |
| Frame decode    | CRC tables are precomputed; decoder is a small state machine.                                        |
| Scrollback      | Ring buffer of `char *` pointers. Push is O(1).                                                      |

## 5. The `zt_ctx` struct

`src/zt_ctx.h` defines one struct holding every piece of per-process
state. It is grouped by feature tier:

- **Tier 0**: fds, sizes, flags, counters (always present).
- **Tier 1**: kernel UART counters, sparkline, SPSC ring, epoll fd.
- **Tier 2**: framing, CRC, SGR pass-through, KGDB, filter subprocess.
- **Tier 3**: JSONL log format, Prometheus exporter, HTTP bridge,
  detach/attach, bookmarks, log-level mute.
- **Tier 4**: fuzzy finder overlay, pager, OSC 52, OSC 8.

Compile-time tunables (buffer sizes, refresh rates) live in the same
header under the `ZT_*_CAP` / `ZT_*_MS` block.

## 6. Embedding contract

zyterm exports four symbols beyond `zyterm_main`:

- `zt_g_embedded`: set true by the host before calling `zyterm_main()`.
  Suppresses host-fatal behaviour (alarm watchdog, `_exit` in crash
  handlers).
- `zt_g_embed_jmp` and `zt_g_embed_jmp_armed`: an optional `sigjmp_buf`
  the host arms with `sigsetjmp`. Fatal paths inside zyterm
  `siglongjmp` back instead of calling `exit`.
- `zt_embed_disarm()`: host calls after a clean return.
- `zt_embed_reset()`: host calls before each invocation to wipe the
  process-wide signal/terminal state left by the previous run.

Full pattern in [API.md](API.md).

## 7. UI design language

The user-facing dialogs share a consistent dark-theme look implemented
in `tui/hud.c::draw_dialog()`:

- **Frame**: Rounded thin borders (`╭ ╮ ╰ ╯`), dark background.
- **Title bar**: Slightly lighter strip with a warm gold accent.
- **Accents**: Amber tones for interactive elements; dim separators.
- **Shadow**: Subtle drop shadow behind the dialog.
- **Spacing**: Generous inner padding; single-width glyphs.

`draw_dialog()` is shared by:

- Command menu (`Ctrl+A`): `draw_cmd_popup()`
- Settings menu (`Ctrl+A o`): `draw_settings_page()` -- four pages of
  live-update controls (serial, display, keyboard, logging)
- Keybindings reference (`Ctrl+A k`): `draw_keybind_popup()`
- Status flashes: transient feedback messages

The HUD row itself (`draw_hud()`) uses a different colour scheme with
an activity LED and throughput sparkline.

## 8. Build pipeline

```
src/<mod>/<file>.c   ─┐
                      ├──► build/obj/<mod>/<file>.o ──► ./zyterm
include/**.h          ┘                                  zyterm_embed.a
```

The Makefile auto-discovers any `.c` file dropped under `src/`, so
adding a file to a module needs no Makefile edit. Object directories
mirror `src/` under `build/obj/`.

A convenience script (`build.sh`) wraps the build, formatting, linting,
testing, and `.deb` packaging workflows. See the
[README](../README.md#build-helper) for the full command list.

## 9. Adding a module

1. Create `src/<module>/<file>.c`.
2. If it exports symbols, add the prototypes to
   `include/zyterm/internal/<module>.h`. Otherwise mark functions
   `static`.
3. If it owns state, add fields to the appropriate tier in `zt_ctx`
   (`src/zt_ctx.h`).
4. Add a unit test under `tests/unit/test_<thing>.c`.
5. Document the module in `docs/USER_GUIDE.md` if it has a user-facing
   surface (CLI flag or keybinding).
6. Run `make modules && make && make test`.

## 10. Anti-patterns

- A back-edge in the include chain. If `serial/` needs `tui/`, the
  function lives in the wrong module. Move it down.
- `extern` in a `.c` file. All cross-TU declarations belong in the
  matching module header.
- `sleep(3)` inside the event loop. Use the `poll(2)` timeout.
- `exit(1)` from non-`loop/` code. Use `zt_die()`. It cooperates with
  the embedding `sigjmp_buf`.
- Assuming `c->serial_fd >= 0`. It can be `-1` mid-reconnect. Always
  check before `read`/`write`.
- Touching `zt_ctx` from a signal handler. Only the `zt_g_*`
  `sig_atomic_t` flags are signal-safe.

## 11. Testing strategy

| Tier        | Path                 | Scope                                         |
| ----------- | -------------------- | --------------------------------------------- |
| Unit        | `tests/unit/`        | Pure-function tests against `zyterm_embed.a`. |
| Integration | `tests/integration/` | Cross-module flows (placeholder).             |
| PTY         | `tests/pty/`         | `forkpty`-based end-to-end roundtrips.        |

Tests link against the embedding archive, so `main.c` is excluded from
coverage and replaced by harness `main()` functions.

See [tests/README.md](../tests/README.md) for runner instructions and
conventions.
