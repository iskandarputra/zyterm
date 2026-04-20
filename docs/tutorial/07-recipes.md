# Chapter 7 — Recipes & real workflows

Pragmatic, copy-pasteable patterns. Each recipe is short and
explains _why_, not just _what_.

## 1. Bring up an unknown board

You just got a board, no datasheet handy. You know it's USB-serial
but not the baud:

```sh
# 1. Find the port:
dmesg | tail -10 | grep tty
ls /dev/ttyUSB* /dev/ttyACM*

# 2. Connect with auto-baud probe:
zyterm /dev/ttyUSB0 --autobaud

# 3. Reset the board. zyterm cycles through 9600, 19200, 38400,
#    57600, 115200, 230400, 460800, 921600, ... and locks on
#    when it sees clean ASCII frames.
```

Once locked, capture so you remember:

```sh
zyterm /dev/ttyUSB0 -b 115200 --profile-save mystery-board
```

## 2. Pin a USB-serial adapter to a stable name

When two adapters fight for `ttyUSB0`/`ttyUSB1` on each plug:

```sh
lsusb
# Bus 003 Device 014: ID 0403:6001 Future Technology Devices ... FT232
```

Match by VID:PID, no udev rule needed:

```sh
zyterm --match-vid-pid 0403:6001 -b 115200
```

Combined with `--port-glob` for filtered discovery:

```sh
zyterm --port-glob '/dev/ttyUSB*' --match-vid-pid 1a86:7523 -b 9600
```

The device path is **re-resolved on every reconnect**, so unplug /
replug into a different port keeps working.

## 3. Boot-log capture for CI

```sh
zyterm /dev/ttyACM0 \
    --dump 60 \
    -l ci/boot.log \
    --tx-ts \
    --no-color \
    --watch 'PANIC|BUG|ASSERT' \
    --watch-beep \
    --on-match '/PANIC|BUG|ASSERT/=touch ci/.failed'

[ -f ci/.failed ] && { echo "boot failed"; exit 1; }
```

- Headless (no UI), bounded duration.
- TX is also logged with `-> ` prefix.
- A `--on-match` hook leaves a sentinel file for the surrounding
  test harness.

## 4. Auto-login on boot

```sh
zyterm /dev/ttyUSB0 \
    --on-match '/login: $/=send:root\r' \
    --on-match '/Password: $/=send:hunter2\r'
```

Each hook fires per RX line. The 100 ms rate-limit prevents
re-sending if the prompt scrolls.

## 5. Panic-watch with desktop notification

```sh
zyterm /dev/ttyUSB0 \
    --watch 'PANIC|FAULT' --watch-beep \
    --on-match '/PANIC|FAULT/=notify-send -u critical "Board panic" "$ZYTERM_LINE"'
```

You can leave the terminal minimised and still get a popup.

## 6. SSH-tunnelled serial (poor person's serial-over-IP)

On the lab machine that has the device:

```sh
# install ser2net, or just netcat:
sudo socat TCP-LISTEN:23000,reuseaddr,fork \
    /dev/ttyUSB0,raw,echo=0,b115200
```

On your laptop (over SSH):

```sh
ssh -L 23000:localhost:23000 lab-host
zyterm tcp://localhost:23000 -l remote.log
```

For Telnet IAC handling (when ser2net is in `telnet` mode):

```sh
zyterm telnet://lab-host:23000
```

> `rfc2217://` (Telnet COM Port Control) is **not yet implemented**.
> Use `tcp://` against ser2net in raw mode instead, and set the baud
> on the ser2net side.

## 7. Rotate huge logs without losing data

```sh
zyterm /dev/ttyUSB0 -l rx.log --log-max-kb 10240
```

When `rx.log` exceeds 10 MiB it's renamed to `rx.log.1` and a fresh
`rx.log` starts. There's only **one** `.1` slot — if you need multi-
generation history, copy `rx.log.1` aside in a watcher:

```sh
inotifywait -m -e moved_to . | while read -r dir _ file; do
    [ "$file" = "rx.log.1" ] && mv rx.log.1 "rx-$(date +%s).log.1"
done
```

## 8. Fast cycle through several boards

Wrap each board as its own profile + alias:

```sh
# ~/.config/zyterm/board-a.conf
device = /dev/ttyUSB0
baud   = 115200

# ~/.config/zyterm/board-b.conf
device = /dev/ttyACM0
baud   = 921600

# ~/.bashrc
alias za='zyterm --profile board-a'
alias zb='zyterm --profile board-b'
```

Hot-reload means you can `:wq` profile changes from another terminal
without restarting.

## 9. Save typed history, replay it later

Line-edit history is auto-stored in `~/.zyterm/history`. To re-send
the last command without retyping, just press **Up**.

To bulk-pre-stuff a board:

```sh
printf 'version\r' | zyterm /dev/ttyUSB0 --dump 5 -l reply.log
cat reply.log
```

## 10. Map line endings between Windows-flavoured & Unix devices

Some devices want CRLF; others choke on the CR. Two flags:

| Flag                | What it does                                            |
| ------------------- | ------------------------------------------------------- |
| `--map-out lf`      | Send LF only (the default).                             |
| `--map-out crlf`    | Send CR + LF (Windows-style).                           |
| `--map-out cr`      | Send CR only (classic Mac / many bootloaders).          |
| `--map-out cr-crlf` | If user typed CR, send CRLF; pass others through.       |
| `--map-out lf-crlf` | Replace bare LF with CRLF; pass others through.         |
| `--map-in <mode>`   | Same set, applied to incoming bytes before display/log. |

```sh
# Talk to a U-Boot prompt that wants CRLF:
zyterm /dev/ttyUSB0 -b 115200 --map-out crlf

# Strip CR from a chatty Windows device on the way in:
zyterm /dev/ttyUSB0 --map-in lf
```

## 11. Macro keys for repetitive prompts

```sh
zyterm /dev/ttyUSB0 \
    --macro F1='version\r' \
    --macro F2='reboot\r' \
    --macro F3='help\r' \
    --macro F4='\x1b[A'           # send literal Up-arrow
```

Press F1..F12 to fire. Escapes: `\r`, `\n`, `\t`, `\xNN`.

## 12. Quick `grep` of a live session

zyterm's `--filter` runs your stdout through an external command:

```sh
zyterm /dev/ttyUSB0 --filter 'grep --line-buffered ERROR'
```

Only lines matching `ERROR` reach the renderer. Useful when a device
is _very_ chatty and you only care about a slice. Combine with `-l`
to keep the full log on disk:

```sh
zyterm /dev/ttyUSB0 -l full.log --filter 'grep --line-buffered ERROR'
```

## Next

See [chapter 8 — Troubleshooting & FAQ](08-troubleshooting.md).
