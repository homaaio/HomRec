# cfg/

Optional startup scripts for the developer console (`ConsoleWindow::RunCfgFile()`
in `src/ui/console_window.cpp`). Both are opt-in - if the file isn't there,
nothing happens, no error.

| File              | Runs when                          |
|-------------------|-------------------------------------|
| `autoexec.cfg`     | HomRec starts                       |
| `startrec.cfg`     | Every time a new recording starts   |

This folder is also auto-created next to `hr.exe` the first time either hook
runs, so it'll exist even if you never add anything to it.

## Format

One console command per line - anything you could type into the console
itself works here. `//` and `#` both start a comment line. Both the `$` and
`!` command prefixes are optional. Blank lines are ignored.

```
// example autoexec.cfg
echo HomRec starting up
alias q quit
```

Copy `autoexec.cfg.example` / `startrec.cfg.example` in this folder to
`autoexec.cfg` / `startrec.cfg` to try it - those two `*.cfg` names are the
only ones ever read automatically, and (per `.gitignore`) are the only files
here git won't track, since they're meant to be a per-machine/per-dev thing.
