# UI script format (`xcat-ui-script`), version 1

**Status:** Normative. Read by `xcat/ui_script.h` (`core/src/ui_script.c`, M8, fuzzed) and played by `app/ui/script.c`. The reasons behind it are in [ADR 0015](../adr/0015-ui-sdl2-own-widgets.md) decision 6.

A UI script presses the controller's buttons for the app, a command at a time, and checks which screen each press leads to. It exists for the M8 exit test's scripted walkthrough ([testing.md](../testing.md#m8-scripted-walkthrough)), so the same walk through the screens can be repeated in xemu or on a console without a person at the controller. It is a test tool: nothing in a release uses it.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Example

```
# xcat-ui-script v1

note Search
expect games 60000
press y
expect keyboard-search
press down
press right
press a
press start
expect games
```

The walkthrough itself is [`tools/ui/walkthrough.txt`](../../tools/ui/walkthrough.txt).

## 2. Location and loading

- The app reads `ui-script.txt` **next to its XBE** (`D:\ui-script.txt` as the app sees it) once, at startup. On a console that's the XBE's folder, `E:\Apps\xcat\ui-script.txt` for example; in xemu it's the disc, so `make docker-xbe` builds `app/xiso-cartographer-walkthrough.iso`, the app's disc with the walkthrough on it, and nothing has to be written to the emulated drive.
- Without the file, the app behaves as it always does. That isn't an error.
- A script starts when the library first shows. It runs once.
- **The controller keeps working** while a script plays: its presses go first, and the script's come between them. So a script left in the folder can't lock anyone out, and the controller can always leave for the dashboard.

## 3. Syntax

1. UTF-8 text, optional leading BOM ignored, LF or CRLF line endings, at most **64 KiB** in total, **256 bytes** per line (without the line ending) and **512** commands. A NUL or any other byte that isn't UTF-8 text is an error.
2. The first line that isn't blank **must** be the header `# xcat-ui-script v<MAJOR>[.<MINOR>]`. Versioning follows [map.md §3](map.md#3-header-and-versioning): a newer major version is rejected.
3. After it, leading and trailing whitespace is ignored on every line. Blank lines and lines starting with `#` are skipped. Every other line is a command (§4): a command word, then its arguments, separated by spaces or tabs. Command words and button names may be in either case. Control characters (other than tab) aren't allowed.
4. Numbers are decimal, with no sign and no leading zeros.

## 4. Commands

The commands run in order, one at a time.

### 4.1 `press <button> [<count>]`

Presses a button, `count` times (1 to 100; default 1). The buttons are `up`, `down`, `left`, `right` (the D-pad), `a`, `b`, `x`, `y`, `start`, `back`, `lt`, `rt` (the triggers), `white` and `black`.

Each press comes **300 ms** after the command before it ended, or after the press before it, so the screen has drawn the last one. A press is one button going down, as a person's quick press; nothing is held, so nothing repeats, and LT + RT + Back + Black can't be pressed together.

### 4.2 `wait <ms>`

Does nothing for `ms` milliseconds (0 to 600,000: ten minutes).

### 4.3 `expect <screen> [<ms>]`

Waits until the screen shows `screen` (§6), for at most `ms` milliseconds (1 to 3,600,000: an hour, for an install; default 10,000). A screen's name is 1 to 31 of `a`–`z`, `0`–`9` and `-`.

- If the screen shows it in time, the check passes and the script goes on. It's logged.
- If not, **the script stops there**: it fails, with the line, the screen expected and the one shown. Nothing after it is pressed.

### 4.4 `note <text>`

Writes `Script: <text>` to the log, to mark where a part of the walk begins. The text is the rest of the line, trimmed: 1 to 95 bytes, any UTF-8, `#` included.

## 5. Errors

The file is **all-or-nothing**. Any problem rejects the whole file with its line number and a reason, and nothing is played. Problems include an unreadable or oversized file, a missing or malformed header, a newer major version, a line too long or not text, too many commands, an unknown command or button, a missing or extra argument, a number out of range, and a screen name with other characters.

The app also checks every `expect` against its own screens (§6) before it starts, so a mistyped name is found at once, not after its wait. A rejected script is shown in the library (UI SCRIPT) and logged, with what to do: fix the line, or delete the file.

## 6. Screens

What the app shows, by name. An open keyboard comes first, then an open dialog, then the section or page.

| Name | Shown |
|---|---|
| `games`, `installed`, `settings` | The library's section, its list or its sections highlighted |
| `folders`, `logins`, `sources`, `nas` | SETTINGS' pages: Install location, Authentication, Map sources, NAS |
| `nas-browse` | SETTINGS > NAS, browsing a NAS's shares and folders |
| `keyboard-search`, `keyboard-email`, `keyboard-password`, `keyboard-folder`, `keyboard-source`, `keyboard-title-id`, `keyboard-nas-address`, `keyboard-nas-user`, `keyboard-nas-password` | The keyboard, open for that field |
| `ask-install`, `ask-exit`, `ask-logout`, `ask-forget`, `ask-remove`, `ask-kind`, `ask-play`, `ask-nas-use`, `ask-delete` | The dialog asking before installing (or, for one that isn't finished, CARRY ON or DELETE), leaving, logging out, removing a login, removing a source, what kind of source to add, playing an installed game, adding a NAS folder, or deleting an install |
| `message` | A message with OK (UI SCRIPT, SETTINGS, NOT ADDED, …) |
| `installing`, `ask-stop` | The install screen, and its "stop?" dialog |
| `install-ok`, `install-failed` | The install's end: INSTALLED, or NOT INSTALLED (whatever its buttons) |

While a script runs, the app logs each screen as it reaches it (`Screen: <name>`), and the library's footer says where the script is (`Script: line 16, command 8 of 63`), then how it ended: `Script passed: 63 commands, 19 screens checked`, or `Script FAILED at line 30: expected install-ok, on install-failed after 600000 ms`. Both go to `E:\xcat\install.log`.

## 7. Changes

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-10-28 | First version. |
| 1.0 | 2026-10-29 | §6: the NAS page's screens and `ask-play` (ADR 0018). The format is unchanged. |
| 1.0 | 2026-11-01 | §6: `ask-delete`. The format is unchanged. |
