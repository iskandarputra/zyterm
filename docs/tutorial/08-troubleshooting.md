# Chapter 8 — Troubleshooting & FAQ

## Connection problems

### `serial open /dev/ttyUSB0: Permission denied`

You're not in the `dialout` (Debian/Ubuntu) or `uucp` (Arch/Fedora)
group.

```sh
ls -l /dev/ttyUSB0                       # see which group owns it
sudo usermod -aG dialout "$USER"         # or  -aG uucp
newgrp dialout                           # apply without logout
```

Don't `chmod 666` the device — it un-fixes itself on every replug.

### `serial open /dev/ttyUSB0: No such file or directory`

The device isn't enumerated. Check:

```sh
dmesg | tail -20 | grep -iE 'tty|usb'    # look for the latest enumeration
lsusb                                    # is the adapter seen at all?
```

If `lsusb` shows the adapter but no `tty*` appeared, the kernel
driver isn't loaded — either install the right module
(`cdc-acm`, `ftdi_sio`, `cp210x`, `ch341`) or check for a USB cable
that's charge-only (no data lines).

### `Device or resource busy`

Something else has the port open. Common culprits:

```sh
sudo fuser /dev/ttyUSB0                  # who owns it
sudo lsof /dev/ttyUSB0
```

ModemManager is the usual offender on desktops:

```sh
sudo systemctl mask ModemManager.service
sudo systemctl stop ModemManager.service
```

### Garbage characters at high baud

Order to try (in order):

1. **Cable.** USB-A→A or unbranded $1 cables can't sustain 921600+.
2. **Adapter.** CH340 caps near 921600 reliably. Use FT232H/FT4232H
   for 1 Mbaud+.
3. **Flow control.** If the device supports it, `--flow r` (RTS/CTS).
4. **Threaded RX.** `--threaded` moves the read off the main loop.
5. **Map line endings.** A `--map-in crlf-lf` mismatch shows as
   double-spaced output, not garbage.

## UI problems

### "It looks frozen after my laptop wakes from suspend"

Press any key. zyterm only re-arms the terminal on first input post-
resume. The first keystroke is consumed for that re-arm and may not
reach the device.

### My shell prompt is wrecked after `zyterm` exits

zyterm restores the terminal **before** all other shutdown work, so
this should be very rare. If it still happens, run `reset` in your
shell. Then file a bug report with `ZYTERM_TRACE=/tmp/zt.log
zyterm ...` and attach `/tmp/zt.log`.

### Mouse selection doesn't work

Two things to check:

- `Ctrl+A m` toggles mouse capture; verify it's **on** (default).
- The terminal emulator must support either OSC 52 or you must have
  `xclip`/`xsel`/`wl-copy` on PATH. Inside tmux, OSC 52 needs
  `set -g set-clipboard on` in `~/.tmux.conf`.

## Profile problems

### My profile changes aren't applied

Check the file path: it must be exactly
`~/.config/zyterm/<name>.conf` (no `.profile`, no other suffix).
Run with `ZYTERM_TRACE=/tmp/zt.log` and grep for `profile` in the
trace.

### "I added `log-dir = ...` and nothing happens"

Because `log-dir` is **not** a profile key. The supported keys are
listed in [chapter 4](04-profiles.md#every-supported-key). Anything
else is silently ignored.

### Hot-reload doesn't trigger

The watcher needs `inotify`. Inside an unprivileged container,
`inotify` may not work — try the change with the container exposing
`/proc/sys/fs/inotify/`.

For network filesystems (NFS, SMB), inotify on remote files is
unreliable. Keep profiles on local disk.

## Hooks problems

### My `--on-match` regex doesn't fire

1. Test with `grep -E` first:
   ```sh
   echo 'login: ' | grep -E 'login: $'    # must match
   ```
2. Remember matching is **per completed line**. Mid-line bytes don't
   trigger.
3. Trailing whitespace matters. Many shells emit `prompt$ ` (with a
   space).
4. The 100 ms per-hook rate-limit collapses bursts. To verify firing
   at all, append to a counter file:
   ```sh
   --on-match '/x/=printf . >> /tmp/c'
   ```

### `send:` doesn't seem to write to the device

- The action must start **literally** with `send:` (no leading space).
- Escapes are `\r`, `\n`, `\t`, `\xNN`. Backslash-quote handling
  depends on your shell — single-quote the whole `--on-match` value
  to keep escapes intact:
  ```sh
  --on-match '/login: $/=send:root\r'      # correct
  --on-match  /login:\ \$/=send:root\r     # broken
  ```

### Hooks fire for old logged data on replay

Hooks are wired to live RX, **not** to replay playback. They will not
fire from `--replay`.

## Logging problems

### My log file is empty after a crash

zyterm doesn't `fsync` on every line — the kernel decides when to
flush. After a hard crash you can lose the last few hundred bytes.
For at-most-once-loss-of-one-line guarantees, use
`--log-format raw` and stream to a separate consumer (`tail -F`,
`syslog-ng`, etc.).

### Log rotation overwrites my old `.1`

By design — there's only one rotation slot. If you need more, watch
the directory and move `.1` aside (see recipe 7 in chapter 7).

## CLI problems

### `--list` doesn't exist

Older docs mention `zyterm --list`. There is **no `--list` flag**. To
discover devices, use the OS:

```sh
ls /dev/ttyUSB* /dev/ttyACM*
dmesg | tail -20 | grep tty
lsusb
```

Then use `--port-glob` and/or `--match-vid-pid` to pick automatically.

### `zyterm: invalid option -- '...'`

The flag isn't recognised. Run `zyterm -h` for the full list.

## Frequently asked

**Q: Does zyterm run on macOS?**
A: No. It uses Linux-only system calls (inotify, termios2, sysfs).

**Q: Does zyterm run on Windows / WSL?**
A: WSL2 with `usbipd` exposes USB-serial as `/dev/ttyUSB*` and works
fine. WSL1 doesn't expose serial at all.

**Q: Why isn't zyterm packaged for my distro?**
A: It's a single-file binary; build it once and copy it. There's no
distro packaging upstream right now (this may change).

**Q: Can I use zyterm as a library?**
A: Yes — `make embed` produces `libzyterm_embed.a`. See
[API.md](../API.md).

**Q: Is there a config file that's separate from a profile?**
A: No. Profiles **are** the config files. Use multiple if you have
multiple boards, and a wrapper script for things profiles don't
cover.

**Q: How do I report a bug?**
A: Run with `ZYTERM_TRACE=/tmp/zt.log zyterm ...`, reproduce the
issue, then attach `/tmp/zt.log` plus the exact command line and
your `zyterm --version` output.

## Next

See [chapter 9 — Reference](09-reference.md).
