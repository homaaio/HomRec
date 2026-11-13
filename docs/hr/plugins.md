# Plugins (Lua)

Plugins extend HomRec with Lua scripts: new console commands, overlays, reactions to recording events. They are
optional. HomRec runs fine without any.

> **Security.** A plugin runs with the same rights as HomRec: it can read and write files and use the network. Only
> install plugins from sources you trust. The `permissions` block below can switch these off per plugin, but it is the
> plugin's own manifest, so it is not a sandbox against a hostile plugin.

## Install a plugin

Pick one way:

| Way | Steps | Restart? |
|---|---|---|
| In the app | **File > Import Plugin (.hrp)...**, choose the file. | No |
| By hand | Copy the `.hrp` (or an unpacked plugin folder) into `plugins\` next to `hr.exe`. | Yes |
| Package manager | `hom install <name>`; see [../hom/usage.md](../hom/usage.md). | Yes |

A `.hrp` is a renamed `.zip`. HomRec unpacks it into `plugins\.installed\<name>\` and loads it from there; it
re-extracts when the `.hrp` is newer.

**Check:** open `logs\plugins.log` (enable **Settings > Security > Plugin logging**). You should see
`<id>: loaded (version <x>)`.

<!-- IMG: hr-plugins-import-menu -->

## Write a plugin

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

- `id` is required and must be unique; a plugin without an `id` is skipped.
- `entry` is the script to run; it defaults to `main.lua`.
- `permissions` is optional. Each flag defaults to `true`. Setting one to `false` makes the matching API return an
  error such as `network access disabled for this plugin (see permissions in plugin.json)`.

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

### Hooks

Define a global function with one of these names:

| Hook | Called when |
|---|---|
| `on_load` / `on_unload` | The plugin is loaded / unloaded |
| `on_recording_start` / `on_recording_stop` | A recording starts / stops |
| `on_replay_saved` | An Instant Replay clip is saved |
| `on_overlay_added` / `on_overlay_removed` | An overlay is added / removed |
| `on_custom_event` | Another plugin raised an event with `homrec.emit` |

### The `homrec.*` API

| Function | Purpose |
|---|---|
| `homrec.log(text [, level])` | Write to `plugins.log` (level defaults to `INFO`). |
| `homrec.log_to(...)` | Write to a log file of your own. |
| `homrec.print(text)` | Print to the console. |
| `homrec.show_toast(message, ..., duration_ms)` | Pop-up notice; the third argument is the duration (default 3000). |
| `homrec.store_set(key, value)` / `homrec.store_get(key [, default])` | A small per-plugin key/value store. Values are strings, numbers or booleans. Needs `permissions.store`. |
| `homrec.settings_get(key)` / `homrec.settings_set(key, value)` | Read or change HomRec settings by key. An unknown key returns `false` instead of raising an error. |
| `homrec.get_colors()` | The current theme colours. |
| `homrec.get_ffmpeg()` | The path of the ffmpeg in use. |
| `homrec.plugin_info()` | `{id, name, version, author}` of the calling plugin. |
| `homrec.emit(event [, arg])` | Raise `on_custom_event` in other plugins. |
| `homrec.http_get(url)` / `homrec.http_post(url, body [, content_type])` | Network access (WinINet). Needs `permissions.network`. |
| `homrec.register_command(name, description, handler)` | Add a console command. |
| `homrec.register_setting(name, description, getter, setter)` | Add a console setting. |
| `homrec.register_input_overlay(...)` | Add an input-overlay preset. |
| `homrec.add_overlay(table)` | Add an overlay: `type` is `text`, `image`, `gif` or `webcam`; other fields as in the overlay editor. |
| `homrec.remove_overlay(id)` | Returns `true` when removed. |
| `homrec.move_overlay(id, x, y [, w, h])` | Move or resize. |
| `homrec.list_overlays()` | A list of tables with `id`, `type`, `x`, `y`, `w`, `h`, `visible`. |

Exact fields of `add_overlay` and `register_input_overlay` are defined in `src/plugins/lua_api.cpp`; read it for the
full list.

### Package it

Zip the **contents** of the plugin folder (so `plugin.json` is at the top of the zip, not inside a sub-folder), then
rename `.zip` to `.hrp`.

## Safety switches

`secp 0` / `secp 1` in the console turns the plugin version check and RAM watchdog off / on
([Commands.md](../commands/Commands.md#security-switches)). Leave it on unless you are debugging.

## What can fail

| Symptom | Cause | Fix |
|---|---|---|
| Nothing in `plugins.log` | Plugin logging is off. | Settings > Security > Plugin logging. |
| `script error: ...` | A Lua syntax or runtime error in `main.lua`. Loading is aborted. | Fix the line shown. |
| `on_load() error: ...` | `on_load` raised an error. The plugin stays loaded. | Fix the code. |
| Plugin does not appear | `plugin.json` missing, empty `id`, or the entry script is empty. | Check the three files. |
| `archive extraction failed` | A damaged `.hrp`. | Re-download; check that it opens as a zip. |
| `... access disabled for this plugin` | `permissions` says `false`. | Edit `plugin.json` if you trust the plugin. |
| A hook is never called | Misspelled name, or the function is local. | The function must be a global with the exact name. |
| A console command from the plugin is unknown | The plugin did not load, or registered another name. | Check `plugins.log`. |
| HomRec slows down or uses a lot of RAM | A heavy plugin loop. | Remove it; the RAM watchdog is `secp`. |
