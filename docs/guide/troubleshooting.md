# Troubleshooting

Practical fixes for the problems that bite at a serial bench. Each entry is symptom →
cause → fix. If your question is more conceptual, check the
[FAQ](../reference/FAQ.md) first.

See also: [Getting started](getting-started.md) · [Recipes](recipes.md) ·
[Automation](automation.md) · [CLI reference](../reference/CLI.md) ·
[Keybindings](../reference/KEYBINDINGS.md).

---

## "Permission denied" opening the port

**Symptom:** `zyterm: open(/dev/ttyUSB0): Permission denied`
(the open fails fatally, `src/serial/serial.c:132`).

**Cause:** on Linux, serial devices belong to the `dialout` group and your user isn't in
it.

**Fix:** add yourself to `dialout`, then log out and back in (group membership is applied
at login):

```sh
sudo usermod -aG dialout "$USER"
# log out / log back in, or start a fresh login shell:
newgrp dialout
```

Confirm with `groups` (you should see `dialout`) and check the device's group with
`ls -l /dev/ttyUSB0`. A `sudo zyterm …` will work too, but running a terminal as root is
not recommended.

---

## Garbage / mojibake output

**Symptom:** the device is clearly talking, but you see random bytes, boxes, or
`�`-style garbage instead of readable text.

**Cause:** almost always a **baud-rate mismatch** (also data bits / parity / stop bits,
but baud is the usual culprit).

**Fix:** set the right baud explicitly:

```sh
zyterm --baud 115200 /dev/ttyUSB0      # the default; try 9600, 57600, 921600, …
```

If you don't know the rate, let zyterm probe it:

```sh
zyterm --autobaud /dev/ttyUSB0
```

`--autobaud` sets a "discover" sentinel and probes a fixed list of common rates
(9600, 19200, 38400, 57600, 115200, 230400, 460800, … `src/serial/autobaud.c:22`),
scoring each and picking the cleanest; ties break toward the higher rate. You can also
trigger a probe at runtime with `Ctrl+A A`.

If the rate is right but text is still wrong, check framing: `--data 8 --parity n
--stop 1` (commonly written "8N1") is the default; some devices use 7E1. And if lines are
double-spaced or run together, it's a line-ending issue — see
[line-ending fixes](recipes.md#line-ending-fixes).

> If a runtime `Ctrl+A A` autobaud *fails*, the connection can be left stranded (the fd
> goes to `-1` but reconnect doesn't re-fire and the HUD may still read "connected").
> Tracked as [ZT-005](../tracking/issues/ZT-005-autobaud-strands-fd.md); if it happens,
> press `Ctrl+A r` to reconnect manually.

---

## Device not found / disappears on re-plug

**Symptom:** `zyterm: open(/dev/ttyUSB0): No such file or directory`, or the device
vanishes when you unplug/replug a USB adapter and zyterm doesn't come back.

**Cause:** USB serial adapters get a new `/dev/ttyUSB*` (or `/dev/ttyACM*`) index each
time they re-enumerate, so a fixed path goes stale.

**Fix:** let zyterm re-discover the device on each reconnect (reconnect is on by default):

```sh
zyterm --port-glob '/dev/ttyUSB*'         # re-resolve the glob each reconnect
zyterm --match-vid-pid 0403:6001          # or pin by USB VID:PID (hex; see lsusb)
```

If nothing matches at startup you'll get `no device matched --port-glob /
--match-vid-pid`. See the full recipe in
[Hot-plug reconnect](recipes.md#hot-plug-reconnect-survive-a-usb-re-plug). To reconnect
by hand at any time, press `Ctrl+A r`.

Also sanity-check the kernel actually saw the device:

```sh
ls -l /dev/ttyUSB* /dev/ttyACM*   # what exists now
sudo dmesg | tail                 # what the kernel logged on plug-in
lsusb                             # find the VID:PID
```

---

## I type but nothing reaches the device

**Symptom:** keystrokes echo (or don't) but the device never responds; sends seem to go
nowhere.

**Causes & fixes, in order:**

1. **Flow control.** If the device asserts hardware (RTS/CTS) or software (XON/XOFF) flow
   control and zyterm isn't matched to it, transmits stall. Set it:

   ```sh
   zyterm --flow r /dev/ttyUSB0      # hardware RTS/CTS  (CRTSCTS)
   zyterm --flow x /dev/ttyUSB0      # software XON/XOFF (IXON|IXOFF)
   zyterm --flow n /dev/ttyUSB0      # none (default)
   ```

   You can also cycle flow control live with `Ctrl+A f`. A device holding CTS low (or
   never sending XON) will block TX until it's ready — this is the device's choice, not a
   zyterm bug.

2. **Line endings.** The device may be waiting for a specific terminator. If pressing
   Enter does nothing, try `--map-out crlf` (send CRLF) — see
   [line-ending fixes](recipes.md#line-ending-fixes).

3. **Wrong end of the cable.** TX/RX swapped, no common ground, or a null-modem where you
   need straight-through. Check wiring.

4. **Paused.** If you pressed `Ctrl+A p`, the session is paused. Press it again to
   resume.

---

## Terminal is left messed up after a crash

**Symptom:** after zyterm exits abnormally your shell prompt is invisible, in the wrong
colour, no echo, or the cursor is gone.

**Cause:** zyterm puts the terminal into raw mode; a normal exit restores it, and the
crash handler restores it on SIGSEGV/ABRT/BUS/FPE too. A `kill -9`, a closed terminal, or
a parent process dying can still leave a stale terminal state.

**Fix:** reset the terminal from a fresh shell:

```sh
reset       # full terminal reset (best)
# or, if "reset" isn't available:
stty sane; printf '\033c'
```

If echo is off but the screen otherwise looks fine, `stty sane` alone usually fixes it.

---

## The browser bridge isn't reachable / isn't safe to expose

The `--http` bridge binds to **loopback only** (`127.0.0.1`), so it's not reachable from
another machine by design — use `http://127.0.0.1:<port>/`. The write routes pin
`Host`/`Origin` to loopback (so a cross-site page or DNS-rebound host can't drive the device),
and you can require a bearer token with `--http-token <tok>` before forwarding the port. See the
full [security note](recipes.md) and [SECURITY.md](../../SECURITY.md).

---

## Features that look like they should work but don't

A few capabilities are present in the code but **not functional in the shipping binary**.
If you're hitting a dead end, it may be one of these rather than a misconfiguration:

| You tried…                                  | Status                                                      |
|---------------------------------------------|-------------------------------------------------------------|
| `Ctrl+A .` fuzzy finder                     | **works** as of the 2026-06 fix ([ZT-008](../tracking/KNOWN_ISSUES.md)) |
| The `Ctrl+A o` "OSC 8 hyperlinks" toggle    | dead — the flag is read by nothing ([ZT-019](../tracking/KNOWN_ISSUES.md)) |
| `rfc2217://` URLs                           | not implemented; use ser2net raw + `tcp://` ([ADR-0005](../decisions/0005-rfc2217-deferred.md)) |
| Multi-pane / split view                     | stubbed; not wired or keybound                              |
| XMODEM/YMODEM/ZMODEM from the TUI           | engines exist but have no interactive trigger — embedding-API only (see [Recipes](recipes.md#file-transfer-xmodem--ymodem--zmodem)) |
| A saved command history after restart       | history and bookmarks are **in-memory only**, lost on exit ([ADR-0006](../decisions/0006-in-memory-history-and-bookmarks.md), [FAQ](../reference/FAQ.md)) |

These are tracked honestly in [`tracking/KNOWN_ISSUES.md`](../tracking/KNOWN_ISSUES.md)
and [`tracking/STATUS.md`](../tracking/STATUS.md); planned work is in
[`plans/ROADMAP.md`](../plans/ROADMAP.md).

---

## Where to file bugs

zyterm is developed at **https://github.com/iskandarputra/zyterm**. File issues and pull
requests on the
[issue tracker](https://github.com/iskandarputra/zyterm/issues). A good report includes:

- the exact command line you ran,
- `zyterm --version` (this is **1.3.0**),
- the device / adapter and its USB VID:PID (`lsusb`),
- what you expected vs. what happened, and any on-screen error.

For security-sensitive reports (the bridge / IPC trust boundary), follow the disclosure
process in [SECURITY.md](../../SECURITY.md) rather than opening a public issue.

If a behaviour is already in [`tracking/KNOWN_ISSUES.md`](../tracking/KNOWN_ISSUES.md),
referencing the `ZT-NNN` id in your report helps a lot.
