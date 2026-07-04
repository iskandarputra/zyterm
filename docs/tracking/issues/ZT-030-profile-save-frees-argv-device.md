# ZT-030: `--profile-save` frees the non-heap `argv` device pointer on exit

- **Severity:** 🔴 high (heap corruption / abort on a documented, routine invocation; in the
  embedded `zy` builtin it can corrupt the long-lived host allocator)
- **Area:** ownership / memsafety
- **Status:** **fixed** — merged to `main` 2026-07-04 (recorded 2026-07-03) — see [KNOWN_ISSUES Resolved](../KNOWN_ISSUES.md#resolved)
- **Location:** `src/main.c:723` (the borrowed `argv` assignment), `src/main.c:730` (the
  `cleanup_ctx` call), freed at `src/main.c:329` (`free((void *)c->serial.device)`)

## Root cause

This is the **same ownership trap as ZT-001** (a borrowed `argv` pointer reaching a `free()`),
still live on one path that the ZT-001 fix missed: `--profile-save`.

In the `--profile-save` early-return branch of `zyterm_main`, an optional positional `<DEVICE>` is
stored **by borrowing** the argument vector — no `strdup`:

```c
if (profile_save_name) {
    if (optind < argc) c.serial.device = argv[optind];   /* src/main.c:723 — borrowed, not strdup'd */
    int rc = profile_save(&c, profile_save_name);
    ...
    cleanup_ctx(&c);                                      /* src/main.c:730 */
    return rc == 0 ? 0 : 1;
}
```

`argv[optind]` points into the process's argument vector, owned by the C runtime — the allocator
never handed it out. `profile_save()` only **reads** the field (`if (c->serial.device) fprintf(fp,
"device = %s\n", c->serial.device);`, `src/ext/profile.c:153`, function `src/ext/profile.c:145`);
it never `strdup`s, reassigns, or nulls it. So `c.serial.device` still holds the raw `argv` pointer
when control falls into `cleanup_ctx`, whose final statement frees it:

```c
free((void *)c->serial.device);   /* src/main.c:329 — free() on a pointer malloc never returned */
```

Calling `free()` on a non-heap pointer is undefined behavior: typically `free(): invalid pointer` /
`SIGABRT`, or silent heap-metadata corruption.

Every **other** path that leaves a device in `c.serial.device` before `cleanup_ctx` already keeps
the free discipline — which is exactly why this one stands out:

- the main path owns it (`c.serial.device = strdup(argv[optind]);`, `src/main.c:770`);
- the discovery path frees any prior heap copy then stores a `strdup`'d buffer
  (`src/main.c:762`, `:763`), and the `else` branch frees before assigning (`src/main.c:769`);
- the `--replay` path explicitly **nulls** the aliased non-heap pointer before teardown
  (`c.serial.device = NULL;`, `src/main.c:747`) with a comment naming the ZT-001 trap.

The `--profile-save` block did none of these. It was added/kept without the guard the other early
returns carry.

## Trigger / repro

1. `zyterm --profile-save myprof /dev/ttyUSB0` — documented usage: the positional `<DEVICE>` is
   optional and, if given, is saved into the profile.
2. `profile_save()` writes `~/.config/zyterm/myprof.conf` successfully (including
   `device = /dev/ttyUSB0`) and returns 0.
3. `cleanup_ctx(&c)` (`src/main.c:730`) runs and reaches `free((void *)c->serial.device)`
   (`src/main.c:329`), freeing `argv[optind]` → `free(): invalid pointer` / heap corruption on exit.

Running `--profile-save myprof` with **no** positional device leaves `serial.device` NULL and never
trips it — the crash is input-dependent, which is how it survived.

In the embedded case (`zy` builtin: `zt_g_embedded` set, teardown jmp armed), `zt_die`/cleanup runs
inside a long-lived host process, so the corrupted allocator can crash the host **later**, far from
the `--profile-save` call.

## Fix direction

Mirror the replay-path guard so the field is never a borrowed `argv` pointer at teardown. Either:

- `c.serial.device = strdup(argv[optind]);` in the `--profile-save` block (with an OOM check like
  `src/main.c:771`), so `cleanup_ctx` frees a heap copy; or
- set `c.serial.device = NULL;` after `profile_save()` and before `cleanup_ctx` (`src/main.c:730`),
  matching the `--replay` guard at `src/main.c:747`.

Then grep every `c.serial.device = argv[...]` site and assert the free/null discipline holds on each
path into `cleanup_ctx`, and codify it in `INVARIANTS §1` (resource & pointer ownership): a pointer
field is either always-owned or always-borrowed, never conditionally either — see the ZT-001 detail
file ([`ZT-001-profile-load-frees-argv-device.md`](ZT-001-profile-load-frees-argv-device.md)).

## Verify

- Repro above: after the fix, `zyterm --profile-save myprof /dev/ttyUSB0` writes the profile and
  exits 0 with no abort. Re-run under `make debug` and under ASan (`make` with the asan-ubsan CI
  config) — ASan must report **no** `attempting free on address which was not malloc`'d.
- Add an ASan regression test that runs `--profile-save` **with** a positional device and asserts a
  clean exit, plus one with no device (the currently-safe path) to lock both in.
- Confirm the profile is still written correctly (the `device =` line reflects the positional
  argument) and that `serial.device` is freed exactly once on exit under ASan.
