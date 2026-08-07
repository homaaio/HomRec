# Hom/

This folder is the "package repo" that `hom` (HomRec's plugin package
manager - see [`tools/hom`](../tools/hom)) reads from. It's not part of
HomRec itself and isn't shipped inside `hr.exe` - it's just files sitting
in this GitHub repo that `hom.exe` fetches over plain HTTPS via
`raw.githubusercontent.com`.

```
Hom/
  version.txt          current hom version, e.g. "1.0.0" - checked by `hom update`
  hom.exe              latest prebuilt hom.exe - downloaded by `hom update`
  plugins/
    index.json         optional listing of available plugins (for humans/tools browsing)
    <name>.hrp          a plugin package - `hom install <name>` downloads this
```

## Publishing a new plugin

1. Build your plugin as a `.hrp` (a `.zip` of the plugin folder - `plugin.json`
   + entry script + assets - renamed to `.hrp`; see the main [README](../README.md#plugins)).
2. Drop it in `Hom/plugins/<name>.hrp`.
3. Commit and push. `hom install <name>` (without the `.hrp`) picks it up
   immediately - there's no build step or index to regenerate for `hom`
   itself to find it (`index.json` is just for human browsing).

## Publishing a hom update

1. Bump the version and rebuild `hom.exe` (`make hom` at the repo root).
2. Update `Hom/version.txt` to the new version number.
3. Replace `Hom/hom.exe` with the new build.
4. Commit and push. Anyone running `hom update` will pick it up.
