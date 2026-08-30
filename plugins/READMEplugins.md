# Installing plugins

HomRec loads everything it finds directly inside this `plugins/` folder
the next time it starts. There are two ways to get a plugin in here -
pick whichever is easier for you.

## Option A - download it and drop it in yourself (no tools needed)

1. Get the plugin file. It'll either be:
   - a **`.hrp` file** (e.g. `bter.hrp`) - a packaged plugin, just a
     renamed `.zip`. Nothing to extract yourself; HomRec unpacks it
     internally the next time it starts.
   - a **plain folder** containing a `plugin.json` and an entry script
     (e.g. `main.lua`) - some plugins are distributed this way instead.
2. Move it straight into this `plugins/` folder:
   ```
   plugins/
     bter.hrp                 <- a .hrp goes here as-is, don't unzip it
   ```
   or, for a folder-style plugin:
   ```
   plugins/
     my_plugin/
       plugin.json
       main.lua
   ```
3. Restart HomRec. That's it - no install step, no config to edit.

## Option B - use `hom`, the plugin package manager

`hom.exe` (in `tools/hom/`, ships next to `hr.exe`) automates exactly
the step above: it downloads the `.hrp` from the official repo and
places it in `plugins/` for you.

```
hom install <plugin-name>    # downloads plugins/<name>.hrp for you
hom install update-hrp       # updates every .hrp plugin already installed
hom remove <plugin-name> -r  # deletes it again (-r confirms the delete)
hom --version                # print hom's own version
```

Useful if you're grabbing an official plugin (see
[`Hom/plugins/index.json`](../Hom/plugins/index.json) for what's
currently published) and don't want to find/download the `.hrp` by
hand - but it's not required. Dragging a `.hrp` (or a plugin folder)
into `plugins/` yourself works exactly the same way `hom` does it
internally.

See the main [README](../README.md#plugins) for how to *write* a
plugin, and [`Hom/README.md`](../Hom/README.md) for how plugins get
published in the first place.
