# Architecture

zyterm is approximately 8.8k lines of C11, organised into ten small
modules under `src/`, plus `main.c`. Each module owns one concern,
lives in its own directory, and exposes exactly one public header at
`include/zyterm/internal/<module>.h`.

The whole runtime carries a single `zt_ctx` value (defined in
`src/zt_ctx.h`). zyterm is single-threaded by default, so a flat struct
gives the optimiser maximum inlining freedom.

## 1. Module map

| Module    | Files |  LOC | Responsibility                                                                                                                                                                                             |
| --------- | ----: | ---: | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `core/`   |     2 |  506 | Logging, output buffer, signals, terminal raw mode, time, CRC. No state beyond `zt_g_*` globals.                                                                                                           |
| `serial/` |     4 |  521 | Open and configure the serial port (`termios2` / `IOSSIOSPEED`), reconnect probe, `epoll(7)` + `splice(2)` fast path, `TIOCGICOUNT` / `TIOCMGET` polling, autobaud.                                        |
| `log/`    |     3 |  737 | Persistent log file with rotation, JSONL emit, scrollback ring buffer.                                                                                                                                     |
| `proto/`  |     7 | 1487 | COBS, SLIP, HDLC, and length-prefixed frame decoders, native XMODEM-CRC, YMODEM/ZMODEM via `lrzsz`, OSC 52 clipboard, OSC 8 hyperlinks, ANSI SGR pass-through, KGDB raw mode, F-key macro fire and expand. |
| `render/` |     2 |  370 | RX byte-stream parsing plus Zephyr/severity colouring plus scrollback push, throughput sparkline.                                                                                                          |
| `tui/`    |     4 |  990 | HUD row (neon status with activity LED), glassmorphism frosted-glass dialogs, command menu popup, settings menu (4-page serial/display/keyboard/logging), keybindings reference popup, search/rename overlays, less-style pager, subsequence fuzzy finder. |
| `net/`    |     3 | 1296 | HTTP + SSE + WebSocket remote view, Prometheus text-format metrics, UNIX-socket detach/attach.                                                                                                             |
| `ext/`    |     7 |  690 | Bookmarks, log-file diff, RX filter subprocess, log-level mute, multi-device panes, INI profiles, reconnect popup.                                                                                         |
| `loop/`   |     4 | 1629 | Keyboard input plus escape parsing, send pipeline (trickle, direct, flush_unsent), optional SPSC RX thread, top-level `run_interactive` / `run_dump` / `run_replay`.                                       |
| `main.c`  |     1 |  530 | CLI option parsing and entry point.                                                                                                                                                                        |

Run `make modules` for a live count.

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

| Path            | Optimisation                                                                                              |
| --------------- | --------------------------------------------------------------------------------------------------------- |
| RX to screen    | One `read()` (up to 64 KiB), one `framing_feed()`, one `render_rx()`.                                     |
| Stdout writes   | All UI output goes through a coalescing buffer (`ob_*`), with a single `writev` flush per loop iteration. |
| RAW log capture | On Linux, `splice(2)` from the serial fd straight into the log file fd. Zero copy through userspace.      |
| TX              | Bytes are spaced by `ZT_FLUSH_DELAY_US` (2 ms) to protect slow embedded UART RX buffers.                  |
| Frame decode    | Branch-free CRC tables, decoder driven by a small DFA.                                                    |
| Scrollback      | Ring buffer of `char *` pointers. Push is O(1).                                                           |

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

All user-facing dialogs share a unified glassmorphism aesthetic
implemented in `tui/hud.c::draw_dialog()`:

- **Frame**: Rounded thin borders (`╭ ╮ ╰ ╯`), frosted-glass
  background (RGB 42,42,42).
- **Title bar**: Slightly lighter strip (RGB 50,50,50) with warm gold
  accent icon and label.
- **Accents**: Warm amber (xterm color 214, 178) for interactive
  elements; dim separators (color 238, 239).
- **Shadow**: Single subtle drop shadow (RGB 28,28,28) behind the
  dialog.
- **Spacing**: Generous inner padding; clean typography using
  single-width glyphs only.

`draw_dialog()` is used by:

- Command menu (`Ctrl+A`): `draw_cmd_popup()`
- Settings menu (`Ctrl+A o`): `draw_settings_page()` -- four pages of
  live-update controls (serial, display, keyboard, logging)
- Keybindings reference (`Ctrl+A k`): `draw_keybind_popup()`
- Status flashes: transient feedback messages

The HUD row itself (`draw_hud()`) uses a separate neon cyberpunk
palette with an activity LED and throughput sparkline.

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
6. Bump the LOC table in section 1 if the module is new.
7. Run `make modules && make && make test`.

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
