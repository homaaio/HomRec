# HomRec Developer Console v1.2.0 — Command Reference

> **Open console:** `Ctrl+Shift+T` · **Close:** `Esc` or × button  
> **Quick help inside the console:** `!help`

---

## Table of Contents

1. [General Conventions](#1-general-conventions)
2. [Global Flags](#2-global-flags)
3. [Math in Arguments](#3-math-in-arguments)
4. [Recording — `!rec`, `!start --rec`](#4-recording)
5. [Windows — `!create --window`, `!start --window`, `!edit --window`, `$rm --window`](#5-windows)
6. [Rules — `!create --rule`, `!rule`, `!edit --rule`](#6-rules)
7. [AE Objects — `!create --ae`, `!disconnect --ae`](#7-ae-objects)
8. [Connect & Disconnect — `!connect`, `!disconnect`](#8-connect--disconnect)
9. [Hotkeys — `!connect --function`, `!disconnect --function`](#9-hotkeys)
10. [Registry Inspection — `!ls`, `!status`, `!info`](#10-registry-inspection)
11. [History & Aliases — `!history`, `!alias`](#11-history--aliases)
12. [Command Execution — `!repeat`, `!delay`, `!batch`, `!run`](#12-command-execution)
13. [Timers & Watchers — `!timer`, `!watch`](#13-timers--watchers)
14. [Console Utilities — `!echo`, `!clear`, `!clip`, `!env`](#14-console-utilities)
15. [Log — `!log`, `!open --log`, `!connect --log`, `!disconnect --log`](#15-log)
16. [System Commands — `!ping`, `!version`, `!exit`, `!date`, `!help`](#16-system-commands)
17. [Deleting Objects — `$rm`](#17-deleting-objects)
18. [Architecture: DLL ↔ Python](#18-architecture-dll--python)

---

## 1. General Conventions

### Named Parameters

Named parameters are passed as `#key="value"` or `#key=value`.  
If the value contains spaces, **always** wrap it in quotes:

```
!create --window #name="My Dashboard" #bg=black #size=(800x600)
```

Common keys used across many commands:

| Parameter | Type | Description |
|---|---|---|
| `#name="..."` | string | Object name (window, rule, AE, alias, etc.) |
| `#ms=N` | int | Time in milliseconds |
| `#count=N` | int | Number of repetitions |
| `#file="..."` | string | File path |
| `#val="..."` | string | Generic value (context-dependent) |

### Rule Step Syntax

Steps inside a rule body are separated by `;`. The keyword `then` is a harmless separator and is ignored during execution:

```
!create --rule #name="auto"; !start --rec 1 then; !echo --ok "done"
```

---

## 2. Global Flags

These flags can be appended to **any** command:

| Flag | Description |
|---|---|
| `-s` / `--silent` | Suppress all console output for this command |
| `-q` | Skip confirmation prompts (where applicable) |
| `-return` / `-ret` | Instead of text output, print only `1` (success) or `0` (failure) |

**Examples:**
```
!connect --rule #name="logger" -s
!create --window #name="debug" -return
```

---

## 3. Math in Arguments

The expression `{int.random(a, b)}` can be used **anywhere** in any command — it is replaced by a random integer in the range `[a, b]` inclusive, before the command is parsed.

```
!echo Random number: {int.random(1, 100)}
!create --window #name="win_{int.random(1,999)}"
!delay #ms={int.random(500, 2000)} !rec
```

---

## 4. Recording

### `!rec`

Toggle recording — starts if not active, stops if active.

```
!rec
```

---

### `!start --rec 1|0`

Explicitly start (`1`) or stop (`0`) recording.

```
!start --rec 1     # start recording
!start --rec 0     # stop recording
```

If recording is already in the requested state, the command does nothing and prints an informational message.

---

## 5. Windows

Windows are stored in the registry file `create/windows.json`. Each window has a name, style properties, and an `enabled` status.

### `!create --window` — create a window

```
!create --window #name="..." [#bg=COLOR] [#fg=COLOR] [#size=(WxH)]
                 [-o] [-s] [-n] [-c] [-d]
                 [--topmost] [--borderless] [--resizable] [--minimized]
                 [--center] [--opacity #val=N] [--icon #path="..."]
                 [--title #val="..."]
```

| Parameter / Flag | Description |
|---|---|
| `#name="..."` | **Required.** Window name |
| `#bg=COLOR` | Background color (CSS color name or hex, e.g. `#1e1e2e`) |
| `#fg=COLOR` | Text / foreground color |
| `#size=(WxH)` | Window dimensions, e.g. `#size=(1024x768)` |
| `-o` | Register only — do not open immediately |
| `-s` | Silent mode |
| `-n` | Do not register in the registry |
| `-c` | Auto-connect after creation |
| `-d` | Create as disconnected (`enabled=false`, window is not opened) |
| `--topmost` / `-t` | Always on top |
| `--borderless` / `-b` | Borderless window |
| `--resizable` / `-r` | Resizable window |
| `--minimized` / `-m` | Open minimized |
| `--center` | Center on screen when opened |
| `--opacity #val=N` | Opacity level (0–100) |
| `--icon #path="..."` | Window icon path |
| `--title #val="..."` | Window title (if different from `#name`) |

**Examples:**
```
!create --window #name="Dashboard" #bg=#1e1e2e #fg=#cdd6f4 #size=(800x500)
!create --window #name="debug" -o -d
!create --window #name="top" --topmost --center #size=(400x300)
```

---

### `!create --window --notepad` — create a text file

Creates a file inside `./create/` and opens it in the system editor.

```
!create --window --notepad [as .EXT] #name="..."
```

| Parameter | Description |
|---|---|
| `as .EXT` | File extension (default `.txt`). Examples: `as .md`, `as .log` |
| `#name="..."` | File name without extension |

**Examples:**
```
!create --window --notepad #name="notes"
!create --window --notepad as .md #name="readme"
!create --window --notepad as .py #name="script"
```

---

### `!start --window` — open a previously created window

Re-opens a window or file previously registered via `!create --window`.

```
!start --window #name="..."
```

```
!start --window #name="Dashboard"
```

---

### `!edit --window` — re-open a window for editing

Re-opens the window (semantically "edit"). Behaves identically to `!start --window`.

```
!edit --window #name="..."
```

---

### `!edit --file` — open a notepad file in the system editor

```
!edit --file #name="..."
```

Opens the file linked to the notepad window with the given name.

---

### `$rm --window` — remove a window from the registry

```
$rm --window #name="..." [-q] [--purge] [--if-disconnected]
```

| Flag | Description |
|---|---|
| `-q` | No confirmation prompt |
| `--purge` | Also delete the physical file (for notepad windows) and any associated hotkeys |
| `--if-disconnected` | Only delete if the window is currently in a disabled state |

```
$rm --window #name="debug" -q
$rm --window #name="notes" --purge
$rm --all --window -y
```

---

## 6. Rules

Rules are stored in `create/rules.json`. Each rule contains a body (steps separated by `;`) and a `connected` status.

### `!create --rule` — create a rule

```
!create --rule #name="..." ; <step1> ; <step2> ; ... [-c] [-d]
                [--once] [--loop #count=N] [--delay #ms=N] [--on-fail #cmd="..."]
```

| Flag / Parameter | Description |
|---|---|
| `#name="..."` | **Required.** Rule name |
| `; step1; step2` | Rule body. Steps separated by `;` |
| `-c` | Execute steps immediately after creation (connect) |
| `-d` | Save as disconnected (do not execute) |
| `--once` | Rule executes only once |
| `--loop #count=N` | Repeat steps N times when connected |
| `--delay #ms=N` | Delay between steps in milliseconds |
| `--on-fail #cmd="..."` | Command to run on step failure |

**Examples:**
```
!create --rule #name="start"; !start --rec 1; !echo --ok "recording started"
!create --rule #name="auto"; !start --rec 1 then; $rm --window #name="tmp" -q  -c
!create --rule #name="once-only"; !echo "runs once" --once
```

---

### `!rule --check` — inspect rule state

```
!rule --check #name="..."
```

Prints the rule status (active / disconnected) and its body.

---

### `!rule --get from connect` — fetch rule from registry

```
!rule --get from connect #name="..."
```

Prints the rule's `connected` flag and body as stored in the registry.

---

### `!edit --rule` — replace the rule body

```
!edit --rule #name="..."; <new step1>; <new step2>
```

If no steps are provided, the current body is printed without modification.

```
!edit --rule #name="auto"; !start --rec 1; !echo "updated"
```

---

### `!connect --rule` / `!disconnect --rule`

Connect or disconnect a rule (toggles the `connected` flag).  
When a rule is connected, its body **executes immediately**.

```
!connect    --rule #name="auto"
!disconnect --rule #name="auto"
!connect    --rule #name="auto" --toggle   # flip current state
!connect    --rule --all                   # connect all rules
!disconnect --rule --all                   # disconnect all rules
```

---

## 7. AE Objects

AE (Anything Else) — extensible objects of arbitrary type. Currently the `color` type is supported. Stored in `create/ae.json`.

### `!create --ae` — create an AE object

```
!create --ae #type=color{rgb=(R,G,B)} #name="..."
!create --ae #type=color{hex=(#RRGGBB)} #name="..."
```

**Examples:**
```
!create --ae #type=color{rgb=(255,100,50)} #name="accent"
!create --ae #type=color{hex=(#FF6432)}    #name="orange"
```

Saved with both `hex` and `rgb` fields.

---

### `!disconnect --ae` — remove an AE object

```
!disconnect --ae #type=color #name="accent"
```

---

## 8. Connect & Disconnect

### `!connect --window` — enable / disable a window

```
!connect --window #name="..." 1   [-s] [-q] [--toggle] [--all] [-f]
!connect --window #name="..." 0
```

| Value / Flag | Description |
|---|---|
| `1` | Enable window (`enabled=true`) and open it |
| `0` | Disable window |
| `--toggle` | Flip the current state |
| `--all` | Apply to all windows in the registry |
| `-f` / `--force` | Apply even if already in the target state |

---

### `!disconnect --window` — disable a window

```
!disconnect --window #name="..." [-s] [-q] [--toggle] [--all] [-f] [--if-disconnected]
```

---

## 9. Hotkeys

Bindings are stored in `create/hotkeys.json` and restored on startup.

### `!connect --function` — bind a command to a key

```
!connect --function <command> to <key> [#name="alias"] [-s] [-q]
!connect --function <command> ; <key>  [#name="alias"]
```

| Part | Description |
|---|---|
| `<command>` | Any console command |
| `to` or `;` | Separator keyword |
| `<key>` | Key combination, e.g. `ctrl+shift+r` |
| `#name="..."` | Optional alias for the binding (used to unbind by name later) |

**Examples:**
```
!connect --function !rec to ctrl+shift+r
!connect --function "!start --rec 1" to ctrl+F9 #name="start-rec"
!connect --function "$rm --window #name=\"tmp\" -q" ; ctrl+alt+d
```

---

### `!disconnect --function` — unbind a key

```
!disconnect --function <command> to <key>
!disconnect --function <command> ; <key>
!disconnect #name="start-rec"    # unbind by alias
```

---

## 10. Registry Inspection

### `!ls` — list registry objects

```
!ls [--windows] [--rules] [--ae] [--hotkeys] [--all]
    [-v] [--json] [--connected] [--disconnected] [--count]
    [--sort #val=name]
```

| Flag | Description |
|---|---|
| `--windows` | Windows only |
| `--rules` | Rules only |
| `--ae` | AE objects only |
| `--hotkeys` | Hotkeys only |
| *(no flag)* | Show everything |
| `-v` / `--verbose` | Full data for each object |
| `--json` | Output as JSON |
| `--connected` | Only connected / enabled objects |
| `--disconnected` | Only disconnected / disabled objects |
| `--count` | Print only the count per category |
| `--sort #val=name` | Sort results (e.g. by name) |

**Examples:**
```
!ls
!ls --rules --connected
!ls --windows -v
!ls --json
!ls --count
```

---

### `!status` — system state snapshot

```
!status [--json]
```

Prints in a single block:
- whether recording is active
- whether the log is connected
- active / total windows
- connected / total rules
- number of hotkey bindings

---

### `!info` — detailed object card

```
!info --window  #name="..."  [--json]
!info --rule    #name="..."  [--json]
!info --ae      #name="..."  [--json]
!info --hotkey  #key="..."   [--json]
```

**Examples:**
```
!info --window #name="Dashboard"
!info --rule   #name="auto"
!info --hotkey #key="ctrl+shift+r"
!info --ae     #name="accent" --json
```

---

## 11. History & Aliases

### `!history` — command history

```
!history [#count=N] [--clear] [--search "text"]
```

| Parameter | Description |
|---|---|
| `#count=N` | Show last N commands (default 20) |
| `--clear` | Clear the history |
| `--search "text"` | Filter by substring |

```
!history #count=50
!history --search "create"
!history --clear
```

The input field also supports keyboard navigation: **↑** / **↓** to cycle through history.

---

### `!alias` — command aliases

Aliases are persisted in `create/aliases.json`.

```
!alias #name="name" #cmd="command"   # create
!alias --list                         # list all
!alias --remove #name="name"          # delete
```

**Examples:**
```
!alias #name="sr"  #cmd="!start --rec 1"
!alias #name="sp"  #cmd="!start --rec 0"
!alias #name="cls" #cmd="!clear"
!alias --list
!alias --remove #name="sr"
```

Once an alias is created, just type its name as a command:
```
sr     # executes !start --rec 1
```

---

## 12. Command Execution

### `!repeat` — repeat a command N times

```
!repeat #count=N <command>
```

```
!repeat #count=5 !echo "hello"
!repeat #count=3 !start --rec 1
```

---

### `!delay` — execute a command after N milliseconds

```
!delay #ms=N <command>
```

Non-blocking — the command runs in a background thread; the console remains interactive.

```
!delay #ms=3000 !rec
!delay #ms=500  !echo --ok "done"
```

---

### `!batch` — run multiple commands in sequence

```
!batch <cmd1> && <cmd2> && <cmd3> [-x | --stop-on-error]
```

| Flag | Description |
|---|---|
| `-x` / `--stop-on-error` | Abort on the first error |

```
!batch !start --rec 1 && !echo --ok "recording started" && !timer #name="t1" #ms=5000 !start --rec 0
!batch !ls && !status -x
```

---

### `!run` — execute a script file line by line

```
!run #file="script.hrc" [--encoding utf8|cp1251] [--ignore-errors]
                         [--echo-each] [-x | --stop-on-error]
```

Each line in the file is one console command. Lines starting with `#` are treated as comments and skipped.

| Parameter / Flag | Description |
|---|---|
| `#file="..."` | File path (absolute or relative to the base directory) |
| `--encoding` | File encoding (default `utf-8`) |
| `--ignore-errors` | Continue execution on errors |
| `--echo-each` | Print each line before executing it |
| `-x` | Stop on the first error |

```
!run #file="setup.hrc"
!run #file="scripts/init.hrc" --echo-each --encoding cp1251
```

**Example `setup.hrc`:**
```
# Initialise workspace
!create --window #name="main" #size=(800x600)
!create --rule #name="logger"; !echo "step executed"
!connect --rule #name="logger"
```

---

## 13. Timers & Watchers

### `!timer` — one-shot timer

```
!timer #name="name" #ms=N <command>
!timer --cancel #name="name"
!timer --list
```

The timer is removed from the list after it fires.

```
!timer #name="stop-rec" #ms=10000 !start --rec 0
!timer --cancel #name="stop-rec"
!timer --list
```

---

### `!watch` — periodic trigger

```
!watch #name="name" #ms=N <command>
       [--max-runs #count=N] [--jitter #ms=N]
!watch --stop #name="name"
!watch --list
```

| Parameter / Flag | Description |
|---|---|
| `#ms=N` | Interval between runs in milliseconds |
| `<command>` | Command executed each interval |
| `--max-runs #count=N` | Maximum number of executions before auto-stop |
| `--jitter #ms=N` | Random deviation of the interval ±N ms |
| `--stop #name` | Stop the watcher |
| `--list` | List all active watchers |

```
!watch #name="ping-loop" #ms=5000 !ping
!watch #name="screenshot" #ms=60000 !echo "tick" --max-runs #count=10
!watch --stop #name="ping-loop"
!watch --list
```

---

## 14. Console Utilities

### `!echo` — print text to the console

```
!echo [--ok | --warn | --err] <text>
```

| Flag | Icon | Description |
|---|---|---|
| *(no flag)* | — | Plain text |
| `--ok` | ✔ | Success (green) |
| `--warn` | ⚠ | Warning (yellow) |
| `--err` | ✖ | Error (red) |

`{int.random(a,b)}` and environment variables `$name` are expanded inside the text.

```
!echo --ok "task completed"
!echo --warn "caution: {int.random(1,100)}"
!echo --err "critical failure"
!echo $USER started the console
```

---

### `!clear` — clear the console output

```
!clear
```

---

### `!clip` — clipboard operations

```
!clip --copy "text"    # copy text to clipboard
!clip --paste          # read clipboard and print to console
!clip --clear          # clear the clipboard
```

```
!clip --copy "HomRec v3.0"
!clip --paste
!clip --clear
```

---

### `!env` — console environment variables

Variables are stored in memory only (not persisted between sessions). Available as `$name` in any command.

```
!env --set  #name="name" #val="value"   # set a variable
!env --get  #name="name"                # print a variable
!env --list                             # list all variables
!env --unset #name="name"               # delete a variable
```

```
!env --set #name="mode" #val="debug"
!echo Mode: $mode
!env --list
!env --unset #name="mode"
```

---

## 15. Log

### `!log` — view and filter `homrec.log`

```
!log --tail [#count=N]
!log --search "text" [--invert]
!log --level info|warn|err
!log --since #time="HH:MM"
!log --clear
```

| Flag / Parameter | Description |
|---|---|
| `--tail` | Show the last N lines (default 20) |
| `--search "text"` | Filter lines by substring |
| `--invert` | Invert the search filter |
| `--level info|warn|err` | Filter by log level |
| `--since #time="HH:MM"` | Show lines no earlier than the given time |
| `--clear` | Truncate the log file |

```
!log --tail #count=50
!log --search "error"
!log --search "connect" --invert
!log --level err
!log --clear
```

---

### `!open --log` — open the log in the system editor

```
!open --log
```

Opens `homrec.log` in the default system editor.

---

### `!disconnect --log` — pause log writing

```
!disconnect --log
```

While the log is disconnected, new log entries are not written to `homrec.log`.

---

### `!connect --log` — resume log writing

```
!connect --log
```

---

## 16. System Commands

### `!ping` — check the DLL ↔ Python bridge

```
!ping
```

Prints `pong` and the round-trip time in milliseconds. Useful for diagnosing bridge connectivity.

---

### `!homrec --version` — component versions

```
!homrec --version
```

Prints the console version (C++ DLL), Python bridge version, and Python interpreter version.

---

### `!exit` — force-quit the application

```
!exit
```

Stops recording, kills the ffmpeg process (if active), stops the tray icon, and destroys the main window.

---

### `!date` — run one or two commands sequentially

```
!date [command_a] [command_b]
```

Commands are passed without `!` (prefix added automatically), or with it.

```
!date rec
!date "start --rec 1" "echo --ok done"
```

---

### `!help` — show help

```
!help [-w]
```

| Flag | Description |
|---|---|
| *(no flag)* | Print command list and open documentation in the browser |
| `-w` | Print command list without opening the browser |

---

## 17. Deleting Objects

### `$rm` — remove an object from the registry

```
$rm --window #name="..." [-q] [--purge] [--if-disconnected]
$rm --rule   #name="..." [-q] [--purge] [--if-disconnected]
$rm --ae     #name="..." [-q]
$rm --all --window|--rule|--ae [-y]
```

| Flag | Description |
|---|---|
| `-q` / `-y` | No confirmation prompt |
| `--purge` | Windows: delete the physical file (notepad) and linked hotkeys. Rules: delete linked hotkeys |
| `--if-disconnected` | Delete only if the object is currently in a disconnected/disabled state |
| `--all` | Delete all objects of the specified type |

**Examples:**
```
$rm --window #name="debug" -q
$rm --rule   #name="auto"
$rm --ae     #name="accent" -q
$rm --all --window -y
$rm --all --rule --if-disconnected -y
$rm --window #name="notes" --purge -q
```

---

## 18. Architecture: DLL ↔ Python

The console is split into two layers:

```
┌──────────────────────────────────┐
│   hr_console.dll  (Win32 / GDI)  │  ← user types a command
│                                  │
│  Built-in dispatcher (C++):      │
│  !help, !rec, !open, !exit,      │
│  !date, !homrec — handled        │
│  entirely inside the DLL.        │
│                                  │
│  Everything else →               │
│  forward_to_python(raw)          │
└──────────┬───────────────────────┘
           │  CB_COMMAND callback
           ▼
┌──────────────────────────────────┐
│  hr_console_bridge.py (Python)   │
│                                  │
│  NativeConsole._dispatch_extended│
│  handles all extended commands:  │
│  !create, !connect, !disconnect, │
│  $rm, !ls, !alias, !timer,       │
│  !watch, !batch, …               │
└──────────────────────────────────┘
```

### Public API (DLL)

| Function | Description |
|---|---|
| `hr_con_init(...)` | Initialise: register callbacks and paths |
| `hr_con_set_command_cb(cb)` | Register the Python handler for extended commands |
| `hr_con_toggle()` | Show / hide the console window |
| `hr_con_set_recording(int)` | Sync recording state with the UI |
| `hr_con_log_connected()` | Returns `1` if log writing is enabled, `0` if paused |

### Storage Files (JSON)

| File | Contents |
|---|---|
| `create/windows.json` | Window registry (`WindowRegistry`) |
| `create/rules.json` | Rule registry (`RuleRegistry`) |
| `create/ae.json` | AE objects (`AERegistry`) |
| `create/aliases.json` | Command aliases (`AliasRegistry`) |
| `create/hotkeys.json` | Hotkey bindings (`HotkeyManager`) |

---

## Usage Examples

### Auto-stop recording after 10 minutes

```
!create --rule #name="rec-10min"; !start --rec 1; !timer #name="stop" #ms=600000 !start --rec 0  -c
```

### Set up a workspace from a script

```
# workspace.hrc
!create --window #name="main" #bg=#1e1e2e #fg=#cdd6f4 #size=(1024x768)
!create --window --notepad as .md #name="notes"
!alias #name="rec" #cmd="!start --rec 1"
!alias #name="stop" #cmd="!start --rec 0"
!connect --function !rec to ctrl+F9 #name="start recording"
!connect --function !stop to ctrl+F10 #name="stop recording"
!echo --ok "workspace ready"
```

Run with: `!run #file="workspace.hrc" --echo-each`

### Periodic status monitoring

```
!watch #name="monitor" #ms=30000 !status
```

A system snapshot is logged every 30 seconds.

### Command chain with error handling

```
!batch !connect --rule #name="export" && !echo --ok "export started" && !delay #ms=2000 !status -x
```

### Bind a toggle shortcut for recording

```
!alias #name="tr" #cmd="!rec"
!connect --function !tr to ctrl+shift+space #name="toggle-rec"
```

Now `Ctrl+Shift+Space` starts or stops recording from anywhere.
