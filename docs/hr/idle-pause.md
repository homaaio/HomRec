# Record only when something happens (auto-pause on idle)

Added in HomRec 3.1.1. HomRec pauses the recording while the screen is still and continues by itself when the
picture changes again. The still time is simply left out of the file, exactly as with the **Pause** hotkey (`F10`).
Useful for long sessions where long stretches of nothing happen: a build, a render, a slow tutorial.

It is **off by default**. While it is off, nothing of it runs: no extra thread, no extra screen capture. The
only cost is one yes/no check twice a second during a recording.

## Turn it on

**Settings > Advanced > Record only when something happens**, then set *Pause after the screen is still for (sec)*
(default 5). Or from the console (`Ctrl+Shift+T`), or in `homrec.hrc`, section `[recording_extra]`:

```
auto_pause_on_idle = 1
idle_duration_sec  = 5
idle_threshold_pct = 0.02
```

`action_detection` and `record_on_motion` are accepted as other names for `auto_pause_on_idle`. The setting is read
during a recording too: switching it on or off takes effect within a second, and switching it off while HomRec has
the recording paused continues the recording.

| Key | Meaning | Default |
|---|---|---|
| `auto_pause_on_idle` | Pause while the screen is still | off |
| `idle_duration_sec` | How long the screen must stay still before the pause (1-600) | 5 |
| `idle_threshold_pct` | How much of the recorded area must change per second to count as activity, in percent (0.001-100) | 0.02 |

## What counts as "something happens"

HomRec asks Windows which parts of the screen were redrawn and adds up their area over the last second. The mouse
pointer moving on its own does **not** count. At the default `0.02` (about 400 pixels per second on a 1920x1080
screen):

| On screen | Counts as activity? |
|---|---|
| Video, scrolling, a window being dragged, an animation | yes |
| Typing | yes |
| A blinking text caret, a clock showing minutes | no |
| A clock showing seconds, a spinner in a corner | borderline: raise `idle_threshold_pct` if it keeps the recording from pausing |

Pausing on a wrong "idle" would lose footage for good, so every doubt is resolved towards recording: if HomRec cannot
tell (see *Limits*), it does not pause. If it paused and then loses the signal, it continues.

The first moments of new activity after a pause are lost (up to about half a second): HomRec can only start
recording after it has noticed the change.

## In the log

`logs\homrec.log` shows every decision with the measured value, which is the easiest way to tune the threshold:

```
Recording: auto-pause on idle is on - pauses after 5 s without screen changes (counts as activity: more than 0.020 % ...
Recording: the screen has been still for 5 s (0.003 % of the area changing per second, limit 0.020 %) - auto-paused.
Recording: the screen is changing again (4.812 % of the area per second) - auto-resumed.
```

If it pauses too often, lower `idle_threshold_pct`; if it never pauses, raise it above the value the log shows
for your "still" screen.

## Does it slow the PC?

Only while the option is on **and** a recording is running. Then HomRec holds a second Windows desktop duplication of
the same monitor and one thread that wakes up to ten times a second. It never copies or reads pixels. The extra cost
is a small amount of work for Windows' compositor for every screen update, and none while the screen is still. With
the option off the cost is zero.

To measure it on your PC, run `tools\perf_test.py` once with the option off and once on, with the same scene.

## Limits

- **Windows.Graphics.Capture window capture** and **full-GPU (ddagrab) capture** are not supported. HomRec writes one
  line to the log and records normally. (ddagrab is also switched off automatically while the option is on.)
- In **window** and **region** capture only the recorded part of the monitor is watched.
- Games in exclusive fullscreen that Windows does not show to screen capture look "still" - but HomRec cannot
  record them either.
- A UAC prompt, a display-mode change or a GPU reset interrupts the signal; the recording continues until the signal
  is back.
- If you also use *Auto-pause on silence* (see [audio.md](audio.md)), each of the two pauses on its own and only
  continues what it paused itself. Use one of them.
- A pause you make by hand is never undone by this feature.
- The `homrec.exe` terminal tool does not have this feature.

## Check that it works

1. Tick the option, set 5 s, start a recording and leave the screen alone. After about 5 s the log shows
   `auto-paused` and the timer in the window stops.
2. Move a window or scroll a page: `auto-resumed` appears at once.
3. Press `F10` to pause by hand, then move things around: it must stay paused.
4. Open the finished file: the still time is missing, the active parts are there.

If step 1 never happens, read the measured percentage in the log and see *What counts*.
