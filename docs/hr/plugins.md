# Plugins (Lua)

Plugins extend HomRec with Lua scripts: new console commands and settings, overlays, reactions to recording events.
They are optional. HomRec runs fine without any. This page is the full reference: you can write a plugin from it
without reading the HomRec source.

> **Security.** A plugin runs with the same rights as HomRec: it can read and write files, run programs and use the
> network. Only install plugins from sources you trust. The `permissions` block below switches off only a few
> `homrec.*` functions (see [permissions](#permissions)); it is the plugin's own manifest, so it is not a sandbox
> against a hostile plugin.

Plugins work in the Windows build only. In the Linux build **File > Import Plugin** shows a "not now" message, and
that build is not linked against Lua.

## Install a plugin

Pick one way:

| Way | Steps | Restart? |
|---|---|---|
| In the app | **File > Import Plugin (.hrp)...**, choose the file. | No |
| By hand | Copy the `.hrp` (or an unpacked plugin folder) into `plugins\` next to `hr.exe`. | Yes |
| Package manager | `hom install <name>`; see [../hom/usage.md](../hom/usage.md). | Yes |

A `.hrp` is a renamed `.zip`. HomRec unpacks it into `plugins\.installed\<file name without .hrp>\` and loads it from
there; it re-extracts when the `.hrp` is newer than the unpacked `plugin.json`. Unpacking uses `tar.exe` from
Windows, and `powershell Expand-Archive` if that fails; each step is stopped after two minutes.

Things that are easy to miss:

- **Updating:** importing a newer `.hrp` of a plugin that is already running does not replace the running code. The
  window says "installed and loaded", but the old version keeps running until you restart HomRec. A plugin whose `id`
  is already loaded is skipped.
- **Where `plugins\` is:** HomRec opens `plugins` relative to the working directory it was started in; it does not
  change that directory itself. Starting HomRec from its own folder (double-click, or a shortcut with "Start in" set
  to that folder) is what the rest of this page assumes. If HomRec is started from another directory, it finds no
  plugins and prints no error.
- **Folder and `.hrp` with the same `id`:** whichever is loaded first wins; the other is skipped silently.
- Loading happens once, at start. There is no reload command and no way to unload one plugin while HomRec runs.

**Check:** open `logs\plugins.log` (enable **Settings > Security > Plugin logging**). You should see
`[<id>] loaded (version <x>)`.

<!-- IMG: hr-plugins-import-menu -->

## Write a plugin

### Quick start

A plugin is a folder:

```
plugins\
  my_plugin\
    plugin.json
    main.lua
```

`plugin.json`:

```json
{
  "id": "my_plugin",
  "name": "My Plugin",
  "version": "1.0",
  "author": "you",
  "entry": "main.lua",
  "permissions": { "network": true, "filesystem": true, "store": true }
}
```

`main.lua`:

```lua
function on_load()
  homrec.log("hello from my_plugin")
  homrec.show_toast("My plugin loaded")
end

function on_recording_start()
  homrec.log("recording started")
end
```

Restart HomRec, open `logs\plugins.log`, and you should see `[my_plugin] loaded (version 1.0)` and
`[my_plugin] hello from my_plugin`. A complete example with a command and a setting is at the
[end of this page](#complete-example).

### plugin.json

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | Unique name of the plugin. It appears in `plugins.log` lines. A plugin without an `id` is skipped without any message. |
| `name` | no | Display name. Returned by `homrec.plugin_info()`. |
| `version` | no | Text, for example `"1.0"`. Written to `plugins.log` (`?` when missing). |
| `author` | no | Text. |
| `entry` | no | The Lua file to run, relative to the plugin folder. Default `main.lua`. |
| `permissions` | no | Object with the booleans `network`, `filesystem`, `store`. Each defaults to `true`. |

HomRec reads this file with a very small text search, not with a real JSON parser. Follow these rules or a field
silently reads as empty or as the wrong text:

- Put `id`, `name`, `version`, `author` and `entry` as **double-quoted strings**. `"version": 1.0` (a bare number) does
  not work; write `"version": "1.0"`.
- Do not put a backslash-escaped quote (`\"`) inside a value; the value ends at the escaped quote.
- The first place where the quoted key name appears in the file wins, wherever that is. Do not repeat `"id"`,
  `"name"` and so on inside other fields or nested objects.
- In `permissions` use bare `true` / `false`.
- Other fields are ignored.

#### permissions

`false` turns off the following, and only these:

| Flag | What `false` does |
|---|---|
| `network` | `homrec.http_get` / `homrec.http_post` return `nil, "network access disabled for this plugin (see permissions in plugin.json)"`. |
| `filesystem` | `homrec.log_to` returns `false` and writes nothing. |
| `store` | `homrec.store_get` / `homrec.store_set` raise a Lua error `store access disabled for this plugin (see permissions in plugin.json)`. |

`filesystem = false` does **not** stop `io.open`, `os.remove`, `os.execute` and the rest of the Lua standard library,
which are always available. Disabled permissions are listed in `plugins.log` when the plugin loads.

### How a plugin runs

- **One Lua 5.4 state per plugin.** Global variables and functions are not shared between plugins. Plugins talk to
  each other only through `homrec.emit` and `on_custom_event`, with plain strings.
- **The whole standard library is open:** `string`, `table`, `math`, `utf8`, `coroutine`, `io`, `os`, `package`,
  `debug`. Lua's own `print()` shows nowhere in the app; use `homrec.print` or `homrec.log`.
- **Load steps:** HomRec reads `plugin.json`, runs the entry script once, then calls `on_load()` if you defined it.
  A Lua error while the script runs aborts the load (`script error: ...` in `plugins.log`, the plugin does not
  appear). An error inside `on_load()` is logged (`on_load() error: ...`) but the plugin stays loaded, together with
  everything it registered before the error.
- **Load order is not defined.** Do not depend on another plugin being loaded before yours. An event you `emit`
  inside `on_load()` reaches only the plugins that already finished loading.
- **Everything runs on HomRec's main thread.** Hooks, commands and setting handlers run one after another and block
  the window while they work. A slow network call, a long loop or `os.execute` freezes HomRec. There are no timers and
  no background tasks: your code runs only when a hook, a console command or a setting handler is called.
- **Hooks never receive data about the event.** For example `on_recording_stop` gets no file name. Read what you need
  with `homrec.settings_get` (for example `"output_folder"`).
- **Text is UTF-8.** Save `main.lua` as **UTF-8 without BOM**. A BOM at the start of the file is a syntax error
  (`unexpected symbol near '<\239>'`) because HomRec passes the file text straight to Lua.
- **No API returns the plugin's own folder**, and `require` / `dofile` / `io.open` with a relative path use the working
  directory (the HomRec folder, see [Install a plugin](#install-a-plugin)), not the plugin folder. The simplest
  plugin is one `main.lua`. If you ship extra files, their path from the HomRec folder is
  `plugins\<folder>\<file>` for a folder plugin and `plugins\.installed\<hrp file name>\<file>` for a `.hrp`.

### Hooks

Define a **global** function with one of these names. A `local function` is not found.

| Hook | Called when | Arguments |
|---|---|---|
| `on_load` | The plugin has been loaded (after the entry script ran). | none |
| `on_unload` | HomRec shuts down normally. Best effort; not called after a crash or kill. | none |
| `on_recording_start` / `on_recording_stop` | A recording starts / stops. | none |
| `on_replay_saved` | An Instant Replay clip is saved. | none |
| `on_overlay_added` / `on_overlay_removed` | A layer is added / removed, by the user in the Layers panel **or by a plugin** (including yours). | none |
| `on_custom_event` | Another plugin called `homrec.emit`. | `event` (string), `arg` (string) |

Rules:

- Return values are ignored.
- An error in a hook is written to `plugins.log` as `<hook>() error: ...` and HomRec carries on; the plugin stays
  loaded. Errors inside `on_custom_event` are dropped without any message.
- The order in which different plugins receive the same hook is not defined.
- **Recursion.** `homrec.add_overlay` and `homrec.remove_overlay` call `on_overlay_added` / `on_overlay_removed`
  themselves. If you call `add_overlay` from inside `on_overlay_added`, the hook calls itself again without end. Guard
  it with a flag.

### The `homrec.*` API

All functions are in the global table `homrec`. "string" in an argument list means a Lua string or a number (numbers
are converted); anything else (`nil`, boolean, table) raises a Lua error such as
`bad argument #1 to 'log' (string expected, got table)`.

#### Logging and console output

| Function | What it does |
|---|---|
| `homrec.log(text [, level])` | Appends `[date time] [LEVEL] [<id>] text` to `logs\plugins.log`. `level` is any string; use `INFO` (default), `WARN` or `ERROR`. Writes nothing when **Plugin logging** is off. |
| `homrec.log_to(filename, text)` | Appends `[date time] [<id>] text` to a log file of your own in the `logs` folder. Returns `true` on success, `false` when it failed or `permissions.filesystem` is `false`. Not affected by the **Plugin logging** switch. |
| `homrec.print(text)` | Shows a line in the console **while one of your command or setting handlers is running**. Called from anywhere else (`on_load`, a hook) it reaches `plugins.log` only. It is always also written to `plugins.log` as `INFO`. |

Details:

- `logs\plugins.log` and custom logs are cut to their newer half when they pass 5 MB.
- For `log_to`, only the last part of the name is used (a path is dropped), the characters `: < > " | ? *` are removed,
  leading and trailing dots and spaces are trimmed, `.log` is added unless the name ends in `.log` or `.txt`, and an
  empty name becomes `plugin.log`. The names `homrec.log`, `pc.log` and `plugins.log` become `homrec_custom.log`,
  `pc_custom.log` and `plugins_custom.log`.

#### Notices and information

| Function | What it does |
|---|---|
| `homrec.show_toast(message, _, duration_ms)` | A small dark pop-up in the bottom-right corner of the main screen, always on top, closed by a timer. Pass `nil` as the second argument (it is ignored). `duration_ms` is a whole number of milliseconds, default `3000`. The box is about 280 x 80 pixels; long text is wrapped and cut. Example: `homrec.show_toast("Saved", nil, 5000)`. |
| `homrec.get_colors()` | Table of the current theme colours as `"#rrggbb"` strings: `bg`, `fg`, `accent`, `success`, `warning`, `error`, `surface`, `text`, `text_secondary`. |
| `homrec.get_ffmpeg()` | The path of the ffmpeg in use (a string), or `nil` when none is resolved yet. |
| `homrec.plugin_info()` | Table `{id=, name=, version=, author=}` of the calling plugin (empty strings for fields missing in `plugin.json`). |

#### Storage

| Function | What it does |
|---|---|
| `homrec.store_set(key, value)` | Saves `value` under `key` in the plugin's own store. |
| `homrec.store_get(key [, default])` | Returns the saved text, or `default` (default `""`). |

Details:

- Needs `permissions.store`; otherwise both raise a Lua error.
- The store is the file `.store` inside the plugin's own folder: one line `key<TAB>value` per entry.
- `key` and `value` must be strings or numbers. A **boolean is an error** in `store_set`. Numbers are saved as text.
- `store_get` **always returns a string**. Convert it yourself: `tonumber(homrec.store_get("count", "0"))`; for
  booleans save `"1"` / `"0"`.
- A key must not contain a tab or a line break; a value must not contain a line break (it would cut the entry).
- The whole file is rewritten on every `store_set`. Use it for small settings, not for logs or many writes in a loop.

#### Reading and changing HomRec settings

| Function | What it does |
|---|---|
| `homrec.settings_get(key)` | Value of the built-in setting `key`. |
| `homrec.settings_set(key, value)` | Sets a built-in setting and saves it to the settings file at once. Returns `true` when the key exists and the value was accepted, otherwise `false`. |

`key` is case-insensitive and the alternative names in the [list below](#settings-keys) work too.

- `settings_get` returns a boolean for `boolean` settings, a number for `number` settings and a string for the rest.
  Numbers are Lua floats (`30.0`, not `30`); use `math.tointeger(x)` or `string.format("%d", x)` if you need an integer.
  An **unknown key returns `false`** (no error), so for a string setting check `type(v) == "string"`.
- `settings_set` takes a boolean, a number or a string. For a boolean setting, only `true`, `1` and `"true"`, `"1"`,
  `"yes"` mean on; any other text means off and is not reported as an error.
- The console's protection does not apply to plugins: `settings_set` ignores the `sec` lock and the `inwid`
  confirmation, so a plugin can change even the two protected keys (`custom_ffmpeg_args`, `post_record_hook_path`).
  Another reason to read a plugin before installing it.
- `homrec.settings_get` reads HomRec's own settings only, not the ones registered by plugins with `register_setting`.

#### Events between plugins

| Function | What it does |
|---|---|
| `homrec.emit(event [, arg])` | Calls `on_custom_event(event, arg)` in **every other** loaded plugin, one after another, before `emit` returns. `arg` is `""` when omitted. |

Both values are strings; you cannot pass a table. The sender does not receive its own event, and there is no reply
value. Every plugin gets every event, so give events a prefix: `"my_plugin:done"`.

#### Network

| Function | What it does |
|---|---|
| `homrec.http_get(url)` | Returns the response body as a string, or `nil, "error text"`. |
| `homrec.http_post(url, body [, content_type])` | Sends `body` and returns the response body, or `nil, "error text"`. `content_type` defaults to `application/x-www-form-urlencoded`. |

Both need `permissions.network` (otherwise `nil, "network access disabled ..."`). Details:

- They use WinINet with the system proxy settings and the user agent `HomRecPlugin/1.0`.
- They block HomRec until the answer arrives; there is no timeout argument.
- You get the body only: no HTTP status code, no response headers.
- You cannot set request headers other than the POST `Content-Type`. An API that wants an `Authorization` header cannot be
  called; use one that takes the key in the URL or in the body.
- Bodies are passed as Lua strings that end at the first zero byte, so binary data is cut. Use text (JSON, form data).
- For `http_post` the host name must be shorter than 256 characters and the path shorter than 1024, or the result is
  `nil, "invalid URL"`. Other error texts: `InternetOpen failed`, `InternetOpenUrl failed`, `InternetConnect failed`,
  `HttpOpenRequest failed`, `HttpSendRequest failed`.
- JSON example: `homrec.http_post(url, '{"text":"hi"}', "application/json")`.

#### Console commands and console settings

| Function | What it does |
|---|---|
| `homrec.register_command(name, description, handler)` | Adds the console command `name`. |
| `homrec.register_setting(name, description, getter, setter)` | Adds a console setting `name`. |

Call them from `on_load()`. `name` is one word without spaces and is not case-sensitive.

**Command.** `handler(raw)` is called with the full line as typed, including the command word (`"rc reset"` for
`rc reset`). The return value is ignored. Show output with `homrec.print`. An error in the handler is shown in the
console as `plugin command error: ...`. To read the arguments: `local arg = raw:match("^%S+%s*(.-)%s*$")`.

**Setting.**

- `name` alone calls `getter()` and shows `name = <value>`. The value must be a **string or a number**; a boolean
  or `nil` is shown as `?`.
- `name value` or `name = value` calls `setter("value")`. The argument is always a string. If the setter prints nothing,
  the console shows `name = value`. The return value is ignored: to refuse a bad value, call `homrec.print("...")`
  or `error("...")` (shown as `plugin setting error: ...`).
- An empty value means "query", so you cannot set an empty string.
- HomRec does not save plugin settings. Save them yourself with `store_set` in the setter and read them in `on_load`.

**Where they work and which name wins:**

- Typing in the console, `cfg\` scripts (they run after plugins are loaded, so `on_load` registrations are available)
  and custom hotkey actions can all call them.
- Built-in commands always win over a plugin command with the same name. A plugin command is tried before built-in
  settings, so a plugin command named like a built-in setting hides that setting. Built-in settings are tried before
  plugin settings.
- If two plugins register the same name, one of them wins and which one is not defined. Prefix your names.
- The description is stored but this build shows it nowhere (no `help` listing), so document your commands in
  your plugin's own README.

#### Overlays (layers)

| Function | What it does |
|---|---|
| `homrec.add_overlay(props)` | Adds a layer from the table `props`. Returns its `id` string, or `false, "reason"`. |
| `homrec.remove_overlay(id)` | Removes the layer with this `id`. Returns `true` when it was found. |
| `homrec.move_overlay(id, x, y [, w, h])` | Moves, and optionally resizes, a layer. `x` and `y` are required. Returns `true` when found. |
| `homrec.list_overlays()` | List (Lua array) of tables `{id, type, x, y, w, h, visible}`. |

**Fields of `props`** (all but `type` are optional):

| Field | Default | Used for | Meaning |
|---|---|---|---|
| `type` | required | all | `"text"`, `"image"`, `"gif"` or `"webcam"`. Anything else returns `false, 'type must be one of "text"/"image"/"gif"/"webcam"'`. |
| `x`, `y` | `40`, `40` | all | Position in pixels. |
| `w`, `h` | `200`, `60` for text; `200`, `150` for the others | all | Size in pixels. |
| `text` | `""` | text | The text. The tokens `{fps}` (live capture rate), `{time}` (`HH:MM:SS`) and `{date}` (`YYYY-MM-DD`) are replaced while recording. |
| `text_color` | `"#FFFFFF"` | text | `"#RRGGBB"`. |
| `font_family` | `"Segoe UI"` | text | Font name. |
| `opacity` | `100` | all | 0 to 100. |
| `image_path` | `""` | image, gif | Path of the picture or `.gif` file. |
| `visible` | `true` | all | Boolean. |
| `webcam_index` | `0` | webcam | Camera number. |
| `webcam_name` | `""` | webcam | Camera name. |

Details:

- The new layer goes to the top of the Layers list, which means in front of the others.
- The `id` looks like `plugin_ov_0`. The counter starts again at `0` every time HomRec starts, so do not keep ids
  between runs; find a layer again with `list_overlays()`.
- `remove_overlay`, `move_overlay` and `list_overlays` work on **every** layer: also the ones the user made and the
  window and region capture layers (`type` is `"window"` / `"region"`; input overlays are `"input_overlay"`). The main
  Desktop layer is not in the list.
- Number fields are returned as floats (`40.0`).
- You cannot set a layer's name or lock through this API, and you cannot create an `input_overlay` layer with it; use
  `register_input_overlay` below so the user can add one.
- `add_overlay` and `remove_overlay` fire `on_overlay_added` / `on_overlay_removed`; `move_overlay` fires nothing.

```lua
local id, err = homrec.add_overlay({
  type = "text", text = "FPS {fps}   {time}",
  x = 20, y = 20, w = 260, h = 40, text_color = "#00FF88", opacity = 90,
})
if not id then homrec.log("add_overlay failed: " .. tostring(err), "ERROR") end
```

#### Input-overlay presets

| Function | What it does |
|---|---|
| `homrec.register_input_overlay(category, label, json_path, png_path)` | Adds a preset to the **Select Input-Overlay...** list. |

Call it from `on_load()`. `category` is free text that classifies the preset (the bundled presets use `keyboard`,
`gamepad`, `mouse`); `label` is the name the user sees, for example `"WASD (Keyboard)"`. The two paths are relative to
the plugin folder (absolute paths also work) and are not checked when you register. The entry **Select
Input-Overlay...** appears in the Layers panel's Add menu once at least one preset is registered. When the user picks
one, HomRec reads the `.json` layout; if it cannot be read or holds no elements, the user sees "That preset's layout
file couldn't be read." Otherwise an `input_overlay` layer is added.

**Layout file (`.json`)** and spritesheet (`.png`). The picture holds the button images; the layout says where
each one is drawn:

```json
{
  "overlay_width": 300,
  "overlay_height": 120,
  "space_v": 2,
  "elements": [
    { "type": 0, "mapping": [0, 0, 300, 120], "pos": [0, 0] },
    { "type": 1, "code": 17, "mapping": [10, 130, 40, 40], "pos": [20, 20] }
  ]
}
```

| Key | Meaning |
|---|---|
| `overlay_width`, `overlay_height` | Size of the overlay canvas. When missing, the size is taken from the elements; `default_width` / `default_height` raise that minimum. |
| `space_v` | Gap in pixels between a button's "released" picture and its "pressed" picture in the `.png`. |
| `elements[].mapping` | `[x, y, w, h]`: the rectangle of the "released" picture in the `.png`. |
| `elements[].pos` | `[x, y]`: where it is drawn on the canvas. |
| `elements[].type` | `0` (or anything not listed): always drawn, never changes. `1`: a keyboard key, `code` is its scan code. `3` or `4`: `code` is a Windows virtual-key code. |
| `elements[].code` | Key code for types `1`, `3`, `4`. |

The "pressed" picture is in the same column of the `.png`, directly under the released one: at
`mapping y + mapping h + space_v`. If that is outside the picture, the released image stays. Unknown keys are ignored;
a UTF-8 BOM at the start of the file is allowed; at least one element is required.

### Settings keys

For `homrec.settings_get` / `homrec.settings_set`. These are the same names the console and the `.hrc` file use.
Keys marked "protected" are blocked in the console without `sec 0` / `inwid`, but not through `settings_set`.
For what each one does, see [settings-reference.md](settings-reference.md) (it is organised by dialog tab, not
by key).

| Key | Type in Lua | Other accepted names | Notes |
|---|---|---|---|
| `language` | string |  |  |
| `theme` | string |  |  |
| `ui_font` | string |  |  |
| `ui_scale` | number |  |  |
| `settings_path` | string |  |  |
| `window_w` | number |  |  |
| `window_h` | number |  |  |
| `output_folder` | string |  |  |
| `quality` | number |  |  |
| `target_fps` | number |  |  |
| `scale_factor` | number |  |  |
| `resolution_mode` | string (enum) |  | "0" or "1" ("1" = absolute resolution) |
| `resolution_w` | number |  |  |
| `resolution_h` | number |  |  |
| `recording_mode` | string (enum) |  | `ultra`, `turbo`, `eco`, `balanced` |
| `show_summary` | boolean |  |  |
| `monitor_id` | number |  |  |
| `capture_mode` | string (enum) |  | `desktop`, `window`, `region` |
| `desktop_visible` | boolean |  |  |
| `desktop_present` | boolean |  |  |
| `desktop_x` | number |  |  |
| `desktop_y` | number |  |  |
| `desktop_w` | number |  |  |
| `desktop_h` | number |  |  |
| `desktop_locked` | boolean |  |  |
| `desktop_lock_aspect` | boolean |  |  |
| `desktop_name` | string |  |  |
| `capture_window_title` | string |  |  |
| `window_capture_method` | string |  |  |
| `window_capture_area` | string |  |  |
| `capture_window_exe` | string |  |  |
| `capture_window_class` | string |  |  |
| `window_match_exe` | boolean |  |  |
| `window_match_class` | boolean |  |  |
| `window_match_title` | boolean |  |  |
| `window_title_mode` | string |  |  |
| `window_follow_hwnd` | boolean |  |  |
| `window_capture_smart` | boolean |  |  |
| `window_capture_fps_cap` | number |  |  |
| `region_x` | number |  |  |
| `region_y` | number |  |  |
| `region_w` | number |  |  |
| `region_h` | number |  |  |
| `preview_width` | number |  |  |
| `preview_height` | number |  |  |
| `preview_quality_pct` | number |  |  |
| `preview_fps` | number |  |  |
| `disable_preview` | boolean |  |  |
| `game_mode` | boolean |  |  |
| `video_codec` | string |  |  |
| `hw_accel` | string |  |  |
| `enc_preset` | string |  |  |
| `enc_crf` | number |  |  |
| `custom_ffmpeg_args` | string |  | protected (see below) |
| `pix_fmt` | string |  |  |
| `gpu_convert` | boolean |  |  |
| `gpu_overlay` | boolean | `gpu_overlays` |  |
| `skip_static_frames` | boolean | `vfr_pipe` |  |
| `gpu_capture_ddagrab` | boolean |  |  |
| `ddagrab_pause` | boolean |  |  |
| `ddagrab_overlays` | boolean |  |  |
| `mf_encoder` | boolean | `media_foundation` |  |
| `mf_gpu_texture` | boolean |  |  |
| `game_friendly_priority` | boolean | `game_priority` |  |
| `qsv_low_power` | boolean | `low_power` |  |
| `gpu_low_priority` | boolean | `low_gpu_priority` |  |
| `crash_safe` | boolean |  |  |
| `flush_sec` | number |  |  |
| `video_format` | string (enum) |  | `mp4`, `mkv`, `both` |
| `inproc_audio` | boolean | `live_audio` |  |
| `audio_sample_rate` | number |  |  |
| `audio_aac_bitrate` | string |  |  |
| `audio_out_channels` | number |  |  |
| `separate_audio_mp3` | boolean |  |  |
| `level_meter_fps` | number |  |  |
| `mic_device_id` | string |  |  |
| `audio_meter_style` | string |  |  |
| `audio_mic_removed` | boolean |  |  |
| `audio_sys_removed` | boolean |  |  |
| `hotkey_start_stop` | string |  |  |
| `hotkey_pause` | string |  |  |
| `hotkey_fullscreen` | string |  |  |
| `hotkey_save_replay` | string |  |  |
| `filename_template` | string |  |  |
| `auto_stop_min` | number |  |  |
| `replay_buffer_sec` | number |  |  |
| `replay_in_ram` | boolean | `replay_ram` |  |
| `replay_ram_mb` | number |  |  |
| `instant_replay_enabled` | boolean |  |  |
| `active_preset_name` | string |  |  |
| `auto_capture_exe` | string |  |  |
| `scheduled_start_enabled` | boolean |  |  |
| `scheduled_start_time` | string |  |  |
| `auto_pause_on_silence` | boolean |  |  |
| `silence_threshold_db` | number |  |  |
| `silence_duration_sec` | number |  |  |
| `auto_pause_on_idle` | boolean | `action_detection`, `record_on_motion` |  |
| `idle_duration_sec` | number |  |  |
| `idle_threshold_pct` | number |  |  |
| `post_record_hook_enabled` | boolean |  |  |
| `post_record_hook_type` | string (enum) |  | `none`, `script`, `move`, `open_folder` |
| `post_record_hook_path` | string |  | protected (see below) |
| `always_on_top` | boolean |  |  |
| `minimize_to_tray` | boolean | `minimize_tray` |  |
| `countdown_enabled` | boolean | `countdown` |  |
| `timestamp_enabled` | boolean | `timestamp` |  |
| `cursor_enabled` | boolean | `cursor` |  |
| `show_audio_panel` | boolean |  |  |
| `show_overlays_panel` | boolean |  |  |
| `show_scenes_panel` | boolean |  |  |
| `notify_sound` | boolean |  |  |
| `notify_flash` | boolean |  |  |
| `hint_no_overlay` | boolean |  |  |
| `console_hom_log` | boolean | `hom_log` |  |
| `console_history_save` | boolean | `history_save` |  |
| `cmd_sound` | boolean | `cmd-sound` |  |
| `ui_locked` | boolean | `lock_ui`, `ui_lock` |  |
| `ui_layout` | string |  |  |
| `system_logging_enabled` | boolean |  |  |
| `plugin_logging_enabled` | boolean |  |  |
| `wgc_logging_enabled` | boolean |  |  |
| `desktop_shortcut_enabled` | boolean |  |  |
| `desktop_shortcut_path` | string |  |  |
| `autostart_enabled` | boolean |  |  |

### Complete example

`plugin.json`:

```json
{
  "id": "rec_counter",
  "name": "Recording counter",
  "version": "1.2.3",
  "author": "you",
  "permissions": { "network": false, "filesystem": false, "store": true }
}
```

`main.lua`:

```lua
-- Counts started recordings. Adds the command "rc" and the setting "rc_prefix".
local prefix = "Recording"

local function count()
  return tonumber(homrec.store_get("count", "0")) or 0
end

function on_load()
  prefix = homrec.store_get("prefix", prefix)

  homrec.register_command("rc", "Show or reset the recording counter", function(raw)
    local arg = raw:match("^%S+%s*(.-)%s*$")
    if arg == "reset" then
      homrec.store_set("count", 0)
      homrec.print("counter reset")
    else
      homrec.print(prefix .. " count: " .. count())
    end
  end)

  homrec.register_setting("rc_prefix", "Text shown in the pop-up",
    function() return prefix end,
    function(value)
      prefix = value
      homrec.store_set("prefix", value)
    end)

  homrec.log("loaded, count = " .. count())
end

function on_recording_start()
  local n = count() + 1
  homrec.store_set("count", n)
  homrec.show_toast(prefix .. " #" .. n, nil, 2500)
end
```

In the console: `rc` shows the count, `rc reset` clears it, `rc_prefix` shows the text and `rc_prefix Clip` changes it.

### Package it

Zip the **contents** of the plugin folder (so `plugin.json` is at the top of the zip, not inside a sub-folder), then
rename `.zip` to `.hrp`. The file name decides the unpacked folder (`plugins\.installed\<name>\`); the `id` inside
`plugin.json` decides the plugin's name. Keep the file name the same between versions so an update replaces the old
unpacked copy.

### Test while writing

1. Turn on **Settings > Security > Plugin logging**.
2. Put the folder into `plugins\` and restart HomRec. A folder plugin cannot be reloaded without a restart.
3. Open `logs\plugins.log`. Errors are written as `script error: ...`, `on_load() error: ...` and `<hook>() error: ...`
   with the Lua line number.
4. Open the console (`Ctrl+Shift+T`) and run your command.

## Safety switches

`secp 0` / `secp 1` in the console is described as the plugin version check and RAM watchdog switch
([Commands.md](../commands/Commands.md#security-switches)). In this build the console only stores the value and shows
it when you type `secp`; nothing in the program reads it, so it does not change how plugins behave.

## What can fail

| Symptom | Cause | Fix |
|---|---|---|
| Nothing in `plugins.log` | Plugin logging is off. | Settings > Security > Plugin logging. |
| No plugins at all, no error | HomRec was started from another working directory, so `plugins\` was not found. | Start HomRec from its own folder. |
| `script error: ...` | A Lua syntax or runtime error in the entry script. Loading is aborted. | Fix the line shown. |
| `script error: ... unexpected symbol near '<\239>'` | `main.lua` is saved with a UTF-8 BOM. | Save as UTF-8 without BOM. |
| `on_load() error: ...` | `on_load` raised an error. The plugin stays loaded. | Fix the code. |
| Plugin does not appear, no message | `plugin.json` missing or unreadable, empty `id`, or the entry script is missing or empty. | Check the three files and the quoting rules in [plugin.json](#pluginjson). |
| Fields such as the version look wrong or empty | A value in `plugin.json` is not a double-quoted string, or the key name appears earlier in the file. | Quote the value; do not repeat key names. |
| `archive extraction failed` | A damaged `.hrp`, or neither `tar.exe` nor PowerShell could unpack it. | Re-download; check that it opens as a zip. |
| `store access disabled ...` (error) / `network access disabled ...` (`nil, text`) / `log_to` returns `false` | `permissions` says `false`. | Edit `plugin.json` if you trust the plugin. |
| `bad argument #2 to 'store_set' (string expected, got boolean)` | Booleans cannot be stored. | Save `"1"` / `"0"` and compare when reading. |
| A hook is never called | Misspelled name, or the function is `local`. | The function must be a global with the exact name. |
| `on_overlay_added` runs without end | The hook calls `add_overlay`. | Guard it with a flag. |
| A console command from the plugin is unknown | The plugin did not load, registered another name, or a built-in command has the same name. | Check `plugins.log`; rename the command. |
| An old version still runs after importing a new `.hrp` | A loaded plugin is not replaced while HomRec runs. | Restart HomRec. |
| HomRec freezes for a moment | A handler is doing slow work (network, loop, `os.execute`) on the main thread. | Keep handlers short. |
