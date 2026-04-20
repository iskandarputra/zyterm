# Chapter 5 — Event hooks & automation

Hooks let zyterm fire shell commands (or inject TX bytes) in response
to three events:

| Event        | Flag              | Fires when                                       |
| ------------ | ----------------- | ------------------------------------------------ |
| `match`      | `--on-match`      | An RX line matches a POSIX ERE pattern           |
| `connect`    | `--on-connect`    | The device opens (initial open and every reopen) |
| `disconnect` | `--on-disconnect` | The device closes / hangs up                     |

All three flags are **repeatable** (up to 16 hooks total). Each fired
hook is **rate-limited** to one execution per **100 ms** per hook
slot.

## Match hooks

Format: `--on-match '/REGEX/=ACTION'`.

```sh
zyterm /dev/ttyUSB0 \
    --on-match '/PANIC|BUG/=echo "$(date) $ZYTERM_LINE" >> panics.log'
```

- `REGEX` is a **POSIX extended regex** (`regcomp(REG_EXTENDED|REG_NEWLINE)`).
- The action is run as `/bin/sh -c "<ACTION>"` via `fork()` + `execve()`.
- Up to **32 child PIDs** are tracked; older zombies are reaped on each fire.
- The match is line-oriented — zyterm tests each completed line of RX,
  not partial bytes.

> The `=` sign is the separator. The parser scans **forward** from
> the leading `/` for the closing `/=`, so paths in the action (e.g.
> `/tmp/foo`) are safe — they won't be mistaken for the separator.

### Inject TX with `send:`

If the action begins with `send:`, the rest of the action is parsed
for escapes (`\r`, `\n`, `\t`, `\xNN`) and written **back to the
serial device** instead of being shell-executed:

```sh
zyterm /dev/ttyUSB0 --on-match '/login: $/=send:root\r'
zyterm /dev/ttyUSB0 --on-match '/Password: $/=send:hunter2\r'
zyterm /dev/ttyUSB0 --on-match '/\$ $/=send:uname -a\r'
```

This is how you build cheap auto-login or canned-response automation.

### Environment variables passed to the action

Every shell-executed hook gets these in its environment:

| Var              | Contains                                                        |
| ---------------- | --------------------------------------------------------------- |
| `ZYTERM_PORT`    | Device path or URL (e.g. `/dev/ttyUSB0`)                        |
| `ZYTERM_BAUD`    | Current baud rate as a decimal string (`"115200"`)              |
| `ZYTERM_LINE`    | The full RX line that triggered the match (no trailing newline) |
| `ZYTERM_PATTERN` | The regex source string from your `--on-match`                  |

So you can write actions that don't need to embed those values:

```sh
zyterm /dev/ttyUSB0 \
    --on-match '/CRASH/=logger -t zyterm "[$ZYTERM_PORT] $ZYTERM_LINE"'
```

## Connect / disconnect hooks

Format: `--on-connect '<cmd>'`, `--on-disconnect '<cmd>'`. No regex.
Runs as a shell command. `ZYTERM_PORT` and `ZYTERM_BAUD` are set;
`ZYTERM_LINE` and `ZYTERM_PATTERN` are empty.

```sh
zyterm /dev/ttyUSB0 \
    --on-connect    'notify-send "zyterm" "$ZYTERM_PORT up @ ${ZYTERM_BAUD}b"' \
    --on-disconnect 'notify-send "zyterm" "$ZYTERM_PORT down"'
```

`--on-connect` fires:

- Once at startup, after the device opens and subsystems boot.
- Again on every successful reconnect (when `--reconnect` is on).

`--on-disconnect` fires:

- On every hang-up the reconnect loop sees.
- Once at clean shutdown.

## Hard limits

| Limit           | Value                            | Source            |
| --------------- | -------------------------------- | ----------------- |
| Max hooks total | 16                               | `ZT_HOOK_MAX`     |
| Tracked PIDs    | 32                               | reap-on-fire pool |
| Per-hook rate   | 1 / 100 ms                       | clamps log spam   |
| Action shell    | `/bin/sh -c`                     | `execve` directly |
| Match anchoring | per RX line, after EOL stripping |                   |

## What hooks **cannot** do

- They can't see TX bytes you typed. Patterns match RX only.
- They don't see partial lines. The match runs on each completed line
  (terminator stripped before regex).
- Action stdout/stderr go to **zyterm's** stderr — you'll see it
  scroll inline. Redirect inside the action if you want quiet output:

  ```sh
  --on-match '/PANIC/=cmd >>~/panics.log 2>&1'
  ```

- There is no priority / ordering between hooks; they fire in
  registration order on the same RX line.

## Debugging hooks

If a hook isn't firing:

1. Double-check the regex with a quick `grep -E`:

   ```sh
   echo 'login: ' | grep -E 'login: $'
   ```

   If `grep -E` doesn't match, neither will zyterm.

2. Make sure the regex matches a **complete line** as the device
   sends it. Many shell prompts end in a space (`uart:~$ `) and many
   bootloaders end in `>`; trailing whitespace matters.

3. Look at zyterm's stderr. Hook process stdout/stderr land there.

4. The 100 ms rate-limit means rapid-fire matches collapse — that's
   not a bug, that's the brake. Test with `--on-match
'/x/=date >> /tmp/c.log'` and watch the line count.

## Next

See [chapter 6 — Recording & replay](06-recording.md).
