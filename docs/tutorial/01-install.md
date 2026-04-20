# Chapter 1 — Installing & building

zyterm is a single C binary with **no runtime dependencies**. Building
it means running `make`. Installing it means copying one file.

## Prerequisites

| Need         | Version                       | Notes                                 |
| ------------ | ----------------------------- | ------------------------------------- |
| Linux kernel | ≥ 3.17 (for `termios2` IOCTL) | Anything from 2015+ is fine.          |
| C compiler   | GCC ≥ 7 or Clang ≥ 10         | C11 + a few POSIX/glibc extensions.   |
| `make`       | Any                           | GNU make assumed.                     |
| `pkg-config` | optional                      | Only used if present, never required. |

zyterm does **not** depend on ncurses, libtermkey, libserialport,
libreadline, libuv, GTK, Qt, glib, libxcb (loaded at runtime if found),
or anything else. The whole binary is self-contained.

> macOS, BSD, and Windows are **not supported** and won't compile.
> zyterm uses inotify for profile hot-reload, `termios2` for arbitrary
> baud, and `/sys/class/tty` for USB device discovery.

## Building from source

```sh
git clone https://github.com/<your-fork>/zyterm.git
cd zyterm
make -j
./zyterm --version          # → zyterm 1.1.1
```

The build produces a single binary at `./zyterm` (~250 KB stripped).
Object files land under `build/obj/`; clean with `make clean`.

### Build variants

| Command      | Produces                                                           |
| ------------ | ------------------------------------------------------------------ |
| `make`       | Optimized release build (`-O2 -g`).                                |
| `make debug` | Debug build (`-O0 -g3 -fsanitize=address,undefined` if available). |
| `make tests` | Compiles and runs the full test suite (unit + pty + e2e).          |
| `make embed` | Builds `libzyterm_embed.a` for linking into other programs.        |
| `make clean` | Removes `build/` and `./zyterm`.                                   |

A successful test run looks like:

```
[unit]        208/208 pass
[pty]          20/20  pass
[e2e/hooks]    11/11  pass
```

## Installing

There is no `make install` in this repo by design — install policy is
your distro's / your call. The standard Linux placement is:

```sh
sudo install -m 0755 ./zyterm /usr/local/bin/zyterm
sudo install -m 0644 docs/zyterm.1 /usr/local/share/man/man1/zyterm.1
```

Then:

```sh
which zyterm                # → /usr/local/bin/zyterm
man zyterm
```

### Per-user install (no sudo)

```sh
mkdir -p ~/.local/bin
install -m 0755 ./zyterm ~/.local/bin/zyterm
# make sure ~/.local/bin is on PATH
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
```

## Serial port permissions

The single biggest first-time gotcha. By default, `/dev/ttyUSB*` and
`/dev/ttyACM*` are owned by `root:dialout` (Debian/Ubuntu) or
`root:uucp` (Arch/Fedora). Until your user is in that group, every
open call fails with **`Permission denied`**.

```sh
# Find the right group for your distro:
ls -l /dev/ttyUSB0
# crw-rw---- 1 root dialout ... /dev/ttyUSB0       ← Debian-family
# crw-rw---- 1 root uucp ... /dev/ttyUSB0          ← Arch/Fedora

# Add yourself:
sudo usermod -aG dialout "$USER"           # or  -aG uucp $USER
# Log out and back in, OR run a fresh shell:
newgrp dialout

groups | grep -E 'dialout|uucp'            # verify
```

**Don't `chmod 666` the device.** It works, but it lasts only until
the next plug/unplug, and it widens permissions for every user on the
machine. Use the group.

### udev rule for a specific adapter

If you want a board to always appear at the same path regardless of
plug order, add a udev rule keyed on USB vendor:product:

```sh
# /etc/udev/rules.d/99-myboard.rules
SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", \
    SYMLINK+="ttyMYBOARD", GROUP="dialout", MODE="0660"
```

```sh
sudo udevadm control --reload
sudo udevadm trigger
```

Now `/dev/ttyMYBOARD` is a stable symlink to whichever `ttyUSB*` got
allocated.

## Uninstalling

```sh
sudo rm -f /usr/local/bin/zyterm /usr/local/share/man/man1/zyterm.1
rm -rf ~/.config/zyterm  ~/.zyterm  ~/.cache/zyterm   # optional, wipes profiles + state
```

## Next

See [chapter 2 — Your first session](02-first-session.md).
