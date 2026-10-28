# homrec

`homrec` is the HomRec command-line interface. Like `hom.exe` it is a separate, standalone
executable (no wxWidgets, no Lua) that you keep next to `hr.exe`.

## Build

```bash
make homrec        # from the repo root -> ./homrec.exe
```

(or the `homrec` target of the CMake build). `-municode` is needed because `homrec.cpp` uses a
wide `wmain()`, so non-ASCII characters in paths survive.

## Usage

```
homrec --version                 Show the HomRec version
homrec config path               Show the .hrc config homrec is bound to
homrec config change <path>      Bind homrec to another .hrc config
homrec help | --help | -h        Usage (also works after any command, e.g. `homrec config -h`)
```

Run it from anywhere once the folder containing `homrec.exe` is in `PATH`. Nothing is looked up
relative to the current directory: the default config and the saved settings are found through
the location of `homrec.exe` itself.

### Which config is "bound"

- Default: `homrec.hrc` in the folder of `homrec.exe`.
- `homrec config change <path>` stores the **absolute** path (a relative argument is resolved
  against where you ran the command) in `cfg/homrec_cli.cfg` next to `homrec.exe`:

  ```
  config=D:\HomRec\configs\main.hrc
  ```
- The file must exist and have the `.hrc` extension; its contents are not validated.
- `homrec config path` prints just the path on stdout (a note on stderr if the file doesn't exist
  yet), so `$(homrec config path)` works in scripts.

## Adding a command

1. Write `int CmdXxx(const Args &args)` in `homrec.cpp` (return 0 on success, `Fail("...")` on error).
2. Add one `Leaf(...)` (or a `Group(...)` of leaves) line to `BuildCommands()`.

Help text, "unknown command" errors and `-h/--help` come from that table - see `homrec_cli.h`.
