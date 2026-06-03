# ZT-001: profile reload frees the non-heap `argv` device pointer

- **Severity:** 🔴 critical (heap corruption / abort triggered by a routine config edit while running)
- **Area:** ext (profile hot-reload) / ownership
- **Status:** open  (recorded 2026-06-03; not yet fixed)
- **Location:** `src/ext/profile.c:93` (free), `src/main.c:717` (the offending assignment)

## Root cause

`c->serial.device` has **two different owners** depending on how the program was started, and
`profile_load()` assumes it is always heap-owned.

At startup, when a positional device argument is given, `main.c` stores a pointer that is **not on
the heap**:

```c
} else {
    c.serial.device = argv[optind];   /* src/main.c:717 — borrowed, not strdup'd */
}
```

`argv[optind]` points into the process's argument vector, owned by the C runtime. The discovery
path two lines up (`c.serial.device = port_discover(...)`, `src/main.c:715`) instead stores a
`strdup()`'d buffer. So `serial.device` may legitimately be either a borrowed `argv` pointer **or**
a heap allocation — the field carries no record of which.

`profile_load()` unconditionally frees it before replacing it when it sees a `device=` key:

```c
if (!strcmp(k, "device")) {
    free((void *)c->serial.device);   /* src/ext/profile.c:93 */
    c->serial.device = strdup(v);
}
```

When the profile was launched with a positional device (the common case), this calls `free()` on a
pointer the allocator never handed out → undefined behavior, typically heap-metadata corruption or
an immediate `free(): invalid pointer` abort. The startup `profile_load` (`src/main.c:602`) runs
*before* line 717, so the device field there is still NULL/heap and the bug is dormant at launch.
It becomes live for the **inotify hot-reload** path: `profile_watch_tick()` re-invokes
`profile_load()` (`src/ext/profile_watch.c:145`) every time the watched `~/.config/zyterm/<name>.conf`
is saved — by which point `serial.device` is the borrowed `argv` pointer from line 717.

## Trigger / repro

1. `zyterm --profile lab /dev/ttyUSB0` (positional device → `serial.device = argv[optind]`).
2. Edit `~/.config/zyterm/lab.conf` and add or change a `device = …` line; save.
3. inotify fires → `profile_watch_tick` → `profile_load` reaches `src/ext/profile.c:93` and calls
   `free()` on `argv[optind]` → heap corruption / `SIGABRT`.

A profile with **no** `device=` key never trips it; the crash is data-dependent, which is why it
survived casual testing.

## Fix direction

Make `serial.device` **single-owned: always heap, never borrowed.** At the assignment site,
duplicate the argument so every code path leaves an owned pointer:

```c
} else {
    c.serial.device = strdup(argv[optind]);   /* own it like the discovery path already does */
}
```

Then `free()`/`strdup()` in `profile_load` (`src/ext/profile.c:93`) and `port_rediscover`
(see ZT-002) are correct by construction. Add the matching free to the teardown/cleanup label so
the duplicate is released on exit (coordinate with ZT-016/ZT-018, which already track the leak side
of this ownership tangle). Codify the rule in `INVARIANTS §1` (resource & pointer ownership): a
pointer field is either always-borrowed or always-owned, never conditionally either.

## Verify

- Repro above: after the fix, editing the profile's `device=` while running must reload cleanly with
  no abort. Re-run under `make debug` and under ASan (`make` with the asan-ubsan CI config) — ASan
  must report **no** `attempting free on address which was not malloc`'d.
- Add a unit/pty test that launches with a positional device, programmatically rewrites the profile
  file, and asserts the process survives the reload and `serial.device` reflects the new value.
- Confirm the startup-leak side (ZT-016) does not reappear: launch with `--profile` + positional
  device, exit, and check the cleanup path frees `serial.device` exactly once under ASan.
