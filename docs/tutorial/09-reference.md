# Chapter 9 — Reference

Every CLI flag, every key binding, in compact tabular form.

## Synopsis

```
zyterm [OPTIONS] <DEVICE>
zyterm --replay <LOGFILE>
zyterm --attach <NAME>
zyterm --diff <FILE_A> <FILE_B>
```

## Devices

| Form                      | Meaning                                    |
| ------------------------- | ------------------------------------------ |
| `/dev/ttyUSB0` (any path) | Open as a character device via `termios2`. |
| `tcp://host:port`         | Raw TCP — e.g. ser2net in raw mode.        |
| `telnet://host:port`      | TCP with Telnet IAC escaping.              |
| `rfc2217://host:port`     | **NYI**. Use ser2net raw + `tcp://`.       |
| _(omit when --port-glob)_ | Discovery resolves the device at startup.  |

## Connection flags

| Flag                             | Default  | Notes                                              |
| -------------------------------- | -------- | -------------------------------------------------- |
| `-b, --baud <rate>`              | `115200` | Any rate `termios2` allows (≥ 1, ≤ 20 000 000).    |
| `--data <5\|6\|7\|8>`            | `8`      | Data bits.                                         |
| `--parity <n\|e\|o>`             | `n`      | None / even / odd.                                 |
| `--stop <1\|2>`                  | `1`      | Stop bits.                                         |
| `--flow <n\|r\|x>`               | `n`      | None / RTS-CTS / XON-XOFF.                         |
| `--reconnect` / `--no-reconnect` | on       | Auto-reopen on hang-up.                            |
| `--port-glob <pat>`              |          | E.g. `/dev/ttyUSB*`. Re-resolves on reconnect.     |
| `--match-vid-pid <V:P>`          |          | Hex, e.g. `0403:6001`. Combine with `--port-glob`. |
| `--autobaud`                     | off      | Probe common rates after open.                     |

## Logging & capture

| Flag                 | Notes                                                    |
| -------------------- | -------------------------------------------------------- |
| `-l, --log <file>`   | Append RX with ms timestamps. `Ctrl+A l` toggles inline. |
| `--log-max-kb <N>`   | Rotate to `<file>.1` past N KiB.                         |
| `--tx-ts`            | Also log TX with `-> ` prefix.                           |
| `--log-format <fmt>` | `text` (default) \| `json` \| `raw`.                     |
| `--dump <sec>`       | Headless capture for N s. `0` = forever.                 |
| `--rec <file.cast>`  | Asciinema v2 cast.                                       |
| `--replay <file>`    | Replay log or cast through the live UI.                  |
| `--replay-speed <x>` | `0` = max, default `1.0`.                                |
| `--filter <cmd>`     | Pipe RX through external command.                        |

## Display & input

| Flag                     | Notes                                                         |
| ------------------------ | ------------------------------------------------------------- |
| `-x, --hex`              | RX as hex dump.                                               |
| `-e, --echo`             | Local echo on at startup.                                     |
| `--no-color`             | Disable RX log-level colouring.                               |
| `--ts`                   | Timestamps on at startup.                                     |
| `--watch <pat>`          | Highlight matching RX lines (max 8).                          |
| `--watch-beep`           | BEL on watch match.                                           |
| `--macro F<n>=<str>`     | `n` in 1..12. Escapes: `\r \n \t \xNN`.                       |
| `--map-out <mode>`       | EOL rewrite outbound: `none\|cr\|lf\|crlf\|cr-crlf\|lf-crlf`. |
| `--map-in <mode>`        | EOL rewrite inbound, same set.                                |
| `--osc52` / `--no-osc52` | OSC 52 clipboard. Default on.                                 |
| `--mute-dbg`             | Hide `<dbg>` log-level lines from the pane.                   |
| `--mute-inf`             | Hide `<inf>` log-level lines from the pane.                   |

## Framing & integrity

| Flag             | Modes                                   |
| ---------------- | --------------------------------------- |
| `--frame <mode>` | `raw \| cobs \| slip \| hdlc \| lenpfx` |
| `--crc <mode>`   | `none \| ccitt \| ibm \| crc32`         |

## Profiles & event hooks

| Flag                      | Notes                                            |
| ------------------------- | ------------------------------------------------ |
| `--profile <name>`        | Load `~/.config/zyterm/<name>.conf`. Hot-reload. |
| `--profile-save <name>`   | Snapshot at exit.                                |
| `--on-match '/RE/=ACT'`   | POSIX ERE on RX line. `send:` prefix to TX.      |
| `--on-connect '<cmd>'`    | Fires on each open / reopen.                     |
| `--on-disconnect '<cmd>'` | Fires on hang-up / clean shutdown.               |

Hook env: `ZYTERM_PORT`, `ZYTERM_BAUD`, `ZYTERM_LINE`, `ZYTERM_PATTERN`.
Hard limits: 16 hooks max, 100 ms per-hook rate-limit, 32 tracked PIDs.

## Networking & sessions

| Flag               | Notes                                      |
| ------------------ | ------------------------------------------ |
| `--http <port>`    | Embedded HTTP control server.              |
| `--webroot <dir>`  | Static webroot for the HTTP server.        |
| `--http-cors`      | Allow cross-origin requests.               |
| `--metrics <path>` | Prometheus-style metrics over UNIX socket. |
| `--detach <name>`  | Run in background under that name.         |
| `--attach <name>`  | Re-attach to a `--detach`ed session.       |

## Misc

| Flag             | Notes                                                    |
| ---------------- | -------------------------------------------------------- |
| `--threaded`     | Move RX read into a separate thread.                     |
| `--epoll`        | Use `epoll` instead of `poll` (faster at high fd count). |
| `--diff <a> <b>` | Diff two zyterm logs and exit.                           |
| `--mute-dbg`     | Suppress `<dbg>` lines.                                  |
| `--mute-inf`     | Suppress `<inf>` lines.                                  |
| `-h, --help`     | Show help and exit.                                      |
| `-V, --version`  | Print version and exit.                                  |

## Environment variables

| Var                 | Effect                                             |
| ------------------- | -------------------------------------------------- |
| `NO_COLOR`          | Forces help and UI to monochrome.                  |
| `TERM=dumb`         | Forces help to monochrome.                         |
| `ZYTERM_TRACE=path` | Append a trace record on fatal paths to that file. |

## Interactive keys

### Top-level

| Key              | Action                                    |
| ---------------- | ----------------------------------------- |
| `Ctrl+A`         | Open command menu (next key is the verb). |
| `F1`..`F12`      | Fire bound macro.                         |
| `PgUp` / `PgDn`  | Scroll back / forward.                    |
| `Up` / `Down`    | Line-edit history.                        |
| `Left` / `Right` | Cursor move within the line.              |
| `Tab`            | Send Tab byte (remote completion).        |
| `Ctrl+U`         | Clear the input line.                     |
| `Ctrl+W`         | Delete previous word.                     |
| `Ctrl+L`         | Clear screen.                             |
| `Ctrl+C`         | Send `0x03` (ETX).                        |

### `Ctrl+A` sub-commands

| Key | Action                                                         |
| --- | -------------------------------------------------------------- |
| `q` | Quit.                                                          |
| `?` | Pageable key reference.                                        |
| `e` | Toggle local echo.                                             |
| `c` | Toggle log-level colouring.                                    |
| `h` | Toggle hex view.                                               |
| `t` | Toggle timestamp display.                                      |
| `l` | Toggle log capture.                                            |
| `b` | Cycle common baud rates (no reconnect).                        |
| `r` | Force reconnect.                                               |
| `/` | Search scrollback (then `n`/`N` to step).                      |
| `f` | Cycle flow control (none / RTS-CTS / XON-XOFF).                |
| `a` | Toggle auto-baud probe.                                        |
| `m` | Toggle mouse capture.                                          |
| `s` | Session picker (multi-window split / attach).                  |
| `p` | Fuzzy command palette.                                         |
| `o` | Open settings dialog (4 pages: serial / display / keys / log). |
| `k` | Show key-binding popup.                                        |
| `j` | Cycle log format (text → json → raw).                          |
| `F` | Cycle framing mode (raw / cobs / slip / hdlc / lenpfx).        |
| `K` | Cycle CRC mode.                                                |
| `G` | Toggle raw passthrough.                                        |
| `D` | Mute / unmute `<dbg>`.                                         |
| `I` | Mute / unmute `<inf>`.                                         |
| `Y` | Quick-copy: selection → log line → flash.                      |
| `x` | Send a literal hex byte (e.g. `0x1B`).                         |
| `.` | Fuzzy finder over scrollback.                                  |
| `+` | Add a bookmark at current scrollback row.                      |
| `[` | Show bookmark list.                                            |

## Profile keys (recap)

| Key          | Range                                     |
| ------------ | ----------------------------------------- |
| `device`     | path or URL                               |
| `baud`       | 1..20 000 000                             |
| `data_bits`  | 5 / 6 / 7 / 8                             |
| `parity`     | n / e / o                                 |
| `stop_bits`  | 1 / 2                                     |
| `reconnect`  | true / false                              |
| `osc52`      | true / false                              |
| `frame`      | raw / cobs / slip / hdlc / lenpfx         |
| `crc`        | none / ccitt / ibm / crc32                |
| `log_format` | text / json / raw                         |
| `map_out`    | none / cr / lf / crlf / cr-crlf / lf-crlf |
| `map_in`     | (same set)                                |

## Files

| Path                        | Purpose                                     |
| --------------------------- | ------------------------------------------- |
| `~/.config/zyterm/*.conf`   | Profiles. Hot-reloaded.                     |
| `~/.zyterm/profiles`        | Saved baud / flow / macro presets.          |
| `~/.zyterm/bookmarks`       | Per-device bookmark store.                  |
| `~/.zyterm/history`         | Line-edit history.                          |
| `~/.cache/zyterm/clipboard` | Clipboard fallback (no X11/Wayland helper). |

## Limits & constants

| Symbol           | Value  | Meaning                            |
| ---------------- | ------ | ---------------------------------- |
| `ZT_WATCH_MAX`   | 8      | Max `--watch` patterns.            |
| `ZT_HOOK_MAX`    | 16     | Max hooks total.                   |
| `ZT_MACRO_COUNT` | 12     | F1..F12.                           |
| Hook PID pool    | 32     | Tracked child PIDs reaped on fire. |
| Hook rate-limit  | 100 ms | Per hook slot.                     |
| Profile debounce | 200 ms | Hot-reload coalescing window.      |

End of tutorial. Back to [00-index.md](00-index.md).
