# hom troubleshooting

Messages in the **first column** appear in the HomRec console (**checked** in the 3.1.0 source) unless marked otherwise.

| Message / symptom | Cause | Fix |
|---|---|---|
| `hom: hom.exe not found in <folder> - downloading it again.` | `hom.exe` is not next to `hr.exe`. | Wait for the download, or [install by hand](install.md#3-download-by-hand). |
| `hom: couldn't download <url>` + `check your connection, or copy hom.exe next to hr.exe by hand` | No network, proxy, firewall or antivirus blocking. | Download `hom.exe` from <https://github.com/homaaio/hom/releases> and copy it. |
| `hom: couldn't start <path>` | `hom.exe` is damaged, blocked or not a Windows executable. | `hom restore`; check antivirus quarantine. |
| `hom: no output for 30s - killed (network hang?)` | The network stalled. | Check the connection, try again. |
| `hom: exited with code N` | hom reported an error. Read the lines printed just above. | Fix what they say; run `hom --help`. |
| `hom: the previous hom command is still running - wait for it to finish.` | Two commands at once. | Wait. |
| `blocked - needs the "inwid" confirmation prefix` | A risky hom command. | `inwid hom ...` |
| Install worked but the plugin is not active | HomRec loads plugins at start. | Restart HomRec. |
| `plugins.log` shows nothing | Plugin logging is off. | Settings > Security > Plugin logging. |
| `hom list --installed` says "version unknown" (older docs) | A fresh install; the version is read after HomRec has run once. | Start HomRec, or `hom upgrade`. |
| No update badge | `hom.exe` is missing, offline, or the check took longer than 15 seconds. | Run `hom list --upgradable` in the console. |
| A plugin is installed in the wrong folder | hom was run from another folder. | Run it from the HomRec folder (the console does this). |
| Plugin misbehaves | Plugin bug. | `inwid hom remove <name> -r`, see [../hr/plugins.md](../hr/plugins.md). |

For anything not in this table: run `hom --help` for your version, and open an issue in the hom repository
<https://github.com/homaaio/hom/issues> (the issue tracker address is assumed from the repository address; check it).
