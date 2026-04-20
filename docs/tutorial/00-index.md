# zyterm tutorial

A deep, chapter-by-chapter walkthrough of zyterm — what it does, how
to drive it, and how to bend it to real embedded-development workflows.

If you just want a one-page overview, see [USER_GUIDE.md](../USER_GUIDE.md).
If you want internals, see [ARCHITECTURE.md](../ARCHITECTURE.md).

This tutorial is the **deep version** — every flag, every key,
every "why does it work that way", with copy-pasteable examples
grounded in the actual source code.

## Chapters

| #   | Topic                                          | Read when                                                              |
| --- | ---------------------------------------------- | ---------------------------------------------------------------------- |
| 01  | [Installing & building](01-install.md)         | First time setup, packagers, distro porting.                           |
| 02  | [Your first session](02-first-session.md)      | New to zyterm, want a guided first connection.                         |
| 03  | [Logging & capture](03-logging.md)             | You need to keep evidence — boot logs, RX/TX traces, rotated archives. |
| 04  | [Profiles & hot-reload](04-profiles.md)        | You have more than one board and want fast context switching.          |
| 05  | [Event hooks & automation](05-hooks.md)        | You want zyterm to react — fire scripts on PANIC, auto-login, etc.     |
| 06  | [Recording & replay](06-recording.md)          | Sharing demos, regression-replaying captures, asciinema casts.         |
| 07  | [Recipes & real workflows](07-recipes.md)      | Firmware bring-up, panic-watch, SSH-tunnelled serial, multi-board, …   |
| 08  | [Troubleshooting & FAQ](08-troubleshooting.md) | Something's broken, weird, or surprising.                              |
| 09  | [Reference](09-reference.md)                   | Every CLI flag and every `Ctrl+A` key with one-liner examples.         |

## Conventions

Throughout this tutorial:

- **`$`** at the start of a line means a shell prompt.
- **`Ctrl+A k`** means hold `Ctrl`, press `A`, release both, then press `k`.
- File paths in monospace are absolute (or `~/`-prefixed) unless noted.
- Code blocks marked `# zyterm <name>.conf` are profile files.
- Examples assume Linux. zyterm is Linux-only by design (uses inotify,
  termios2, and Linux serial ioctls). macOS/BSD/Windows are not supported.

## Versions

This tutorial targets **zyterm 1.1.x**. Run `zyterm --version` to
confirm. If the flag tables in chapter 09 don't match your build,
either you have an older binary or this doc is ahead of release.
