# hom
`hom` is a small separate program that downloads and installs HomRec plugins (and, on its own, other files, archives
and programs). It works like `apt` or `pacman`, but for HomRec.

> **Where this information comes from.** `hom` lives in its own repository, <https://github.com/homaaio/hom>
> (currently version 0.6.2 according to the HomRec changelog). Its source and README were **not available** while
> these pages were written. What is documented here is what the HomRec 3.1.0 source code proves (marked **checked**)
> plus the older hom 0.4 documentation from the previous HomRec website (marked **older docs**). Always confirm with
> `hom --help` and the hom README for your version.

| Page | Read it for |
|---|---|
| [install.md](install.md) | Getting `hom.exe`, checking it works, updating, removing |
| [usage.md](usage.md) | Every command, with examples |
| [troubleshooting.md](troubleshooting.md) | Error messages and fixes |

## What hom is, and is not

- It is a **separate program** (`hom.exe`). HomRec works without it; you only need it to install plugins from a package
  list instead of importing `.hrp` files by hand.
- It does **not** run inside HomRec. HomRec's console command `hom` starts `hom.exe` as a child process in the HomRec
  folder, so the effect is the same as running it in a terminal there (**checked**).
- Plugins it installs end up in `plugins\` next to `hr.exe`, where HomRec loads them at the next start
  (**checked**: hom runs with the HomRec folder as its working directory).
- The plugins themselves are Lua code with full file and network access. Only install ones you trust
  ([../hr/plugins.md](../hr/plugins.md)).

## How HomRec uses it

1. At startup, if `hom.exe` is next to `hr.exe`, HomRec runs `hom list --upgradable` in the background (15-second
   limit) and shows a badge when plugin updates exist (**checked**).
2. In the console, `hom ...` forwards to `hom.exe` and shows progress as a single live bar (**checked**).
3. If `hom.exe` is missing, HomRec downloads it again the first time you type a `hom` command, or on `hom restore`
   (**checked**).

<!-- ![bagde](../images/hom/badge-upgradable.png) -->
