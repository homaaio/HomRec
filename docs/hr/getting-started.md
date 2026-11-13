# Getting started

From download to your first finished recording. Each step ends with a **Check** so you know it worked before you
continue.

## 1. Check that your PC can run it

| Requirement | Minimum | How to check |
|---|---|---|
| Windows | 10, 64-bit | Press `Win+R`, type `winver`. |
| CPU / RAM | Intel Core i3-M370 class, 2 GB RAM | Task Manager > Performance. These figures are estimates, not tested limits. |
| Graphics | Any GPU with DirectX 10 | `dxdiag` > Display tab. |
| Disk | ~170 MB for the program, plus space for recordings | Recording refuses to start with less than **200 MB** free on the output drive. |

Some features need a newer Windows 10 build: whole-window capture needs **1903+**, per-application audio needs
**2004+**. Older builds still record the desktop.

## 2. Pick a download

Open the Releases page: <https://github.com/homaaio/HomRec/releases>.

| File | What you get | ffmpeg included? |
|---|---|---|
| `homrec-setup-win64-<version>.exe` (installer) | Installs to `%LOCALAPPDATA%\Programs\HomRec`, no admin rights. In-app updates work. | Depends on how the installer was built. See the check below. |
| `...-portable-full.7z` / `.zip` | Unpack anywhere. | **Yes** |
| `...-portable.7z` / `.zip` | Unpack anywhere. | **No.** You must add `ffmpeg\ffmpeg.exe` yourself. |

> The archive names above come from the packaging script (`tools/homrec_build.py`: "full" bundles ffmpeg,
> "portable" does not). Always read the file list on the release page; the project maintainer chooses the names
> for each release.

HomRec cannot record without ffmpeg. If you picked an archive without it, download a Windows 64-bit ffmpeg
build of **version 6 or newer** (the FFmpeg download page lists Windows builds), and place `ffmpeg.exe` as
`ffmpeg\ffmpeg.exe` next to `hr.exe`.

<!-- LINK: ffmpeg-windows-builds --> HomRec looks for it, in this order: next to `hr.exe`, then `bin\ffmpeg.exe` and
`ffmpeg\ffmpeg.exe` inside the program folder, then `C:\ffmpeg\bin`, `C:\Program Files\ffmpeg\bin`,
`C:\tools\ffmpeg\bin`, and finally anything `where ffmpeg` finds on `PATH`.

## 3. Install

### Installer

1. Run the `.exe`. No admin prompt is expected.
2. Optional tasks (all off by default): desktop shortcut, launch at Windows startup, associate `.hrc` and `.hrp`
   files.
3. Finish, and start HomRec from the Start menu.

### Portable

1. Unpack the archive to a folder you can write to, for example `D:\HomRec`. **Do not** run `hr.exe` from inside
   the archive viewer; unpack it first.
2. A `.7z` archive needs an archiver such as 7-Zip; `.zip` opens with Windows itself.
   <!-- LINK: 7zip -->
3. Do not put it in `C:\Program Files`: HomRec writes `homrec.hrc` and `logs\` next to `hr.exe`, which needs
   write access.
4. Double-click `hr.exe`.

**Check:** the program folder contains `hr.exe`, several `.dll` files, and (for "full" downloads)
`ffmpeg\ffmpeg.exe`.

<!-- IMG: hr-gs-program-folder -->

### If it fails

| Symptom | Cause | Fix |
|---|---|---|
| Windows SmartScreen: "Windows protected your PC" | New, unsigned program. | **More info > Run anyway**. |
| Antivirus quarantines `hr.exe` | Heuristic false positive reported with some products. The source is public. | Restore the file and add the HomRec folder to the exclusions. |
| "The code execution cannot proceed because ... .dll was not found" | A runtime `.dll` next to `hr.exe` is missing (a partial copy, or the archive was not fully extracted). | Extract the whole archive again; do not copy `hr.exe` alone. |
| Nothing happens, or it closes at once | A crash during startup. | See [troubleshooting.md > HomRec will not start](troubleshooting.md#homrec-does-not-start-or-closes-immediately). |
| A second launch does nothing | Only one copy runs at a time. | Look in the system tray; HomRec may already be running. |

## 4. First run: the welcome wizard

On first launch a short wizard appears.

1. **Welcome.** Click **Next**, or tick *I understand* to skip the questions and keep the defaults.
2. **A couple of basic settings.** Choose *Save recordings to* (default: a `recordings` folder), a *Resolution*
   (100% is native) and a frame rate.
3. **You're all set!** Buttons for the changelog and the project page. Re-open the wizard any time from
   **Help > Show Welcome Screen**.

<!-- IMG: hr-gs-welcome-wizard -->

Desktop shortcut, launch at startup and tray options are in **Settings > System**.

## 5. Your first recording

1. **Pick what to record.** The first row of the **Layers** panel is the capture source (Desktop, a Monitor, a
   Window or a Region). Double-click it to change it. The default is the full desktop. Details:
   [capture-sources.md](capture-sources.md).
2. **Check the sound.** In the **Audio Mixer** panel, speak into the microphone: the Microphone bar should move.
   Play any sound: the Desktop Audio bar should move. See [audio.md](audio.md).
3. **Press `F9`** (or the Start button in the Controls panel). If *Countdown (3s)* is on, wait for it. The status
   switches to *Recording* and the time counts up.
4. **Press `F10`** to pause and resume if you need to. **Press `F9`** again to stop.
5. Wait until saving finishes. Long recordings take a few seconds to finalize.
6. **File > Open Recordings Folder** shows the result.

<!-- IMG: hr-gs-main-window -->

**Check:** open the file in a player. It should have picture and sound. Open `logs\homrec.log` and look for a
line like `Recording: encoding with libx264 (software)` or `... with h264_nvenc (hardware)`, which tells you
which encoder was used. The file name follows the template in Settings > Advanced (`{date}`, `{time}`, `{app}`).

### If it fails

| What you see | Meaning | Fix |
|---|---|---|
| "FFmpeg not found." | No `ffmpeg.exe` was found. | Step 2 above. |
| "Output folder doesn't exist and couldn't be created." | The folder path is wrong or not writable. | Settings > General > Output folder: pick a folder in your own profile. |
| "Not enough free disk space on the output drive (... MB free, need at least 200 MB)." | Disk almost full. | Free space or choose another drive. |
| "Failed to start the ffmpeg process." | ffmpeg exists but cannot run. | [recording-troubleshooting.md](recording-troubleshooting.md). |
| "Failed to start the capture pipeline." | Screen capture could not start. | [troubleshooting.md > Black or frozen picture](troubleshooting.md#the-recording-is-black-frozen-or-shows-the-wrong-thing). |
| The file plays without sound | Audio source off or muted. | [audio.md](audio.md). |

## 6. Keep going

| Hotkey (default) | Action |
|---|---|
| `F9` | Start / stop recording |
| `F10` | Pause / resume |
| `F11` | Fullscreen for HomRec's own window |
| `F8` | Save Instant Replay |
| `Ctrl+Shift+T` | Open the developer console |

All four function keys can be changed in **Settings > Hotkeys**.

## 7. Updating

- **Installed copy:** **Help > Check for Updates**. HomRec asks GitHub for the latest release, downloads the Setup
  `.exe` attached to it and upgrades in place.
- **Portable copy:** it cannot update itself. HomRec tells you so and offers to open the release page. Download the
  new archive and unpack it over the old folder. Keep `homrec.hrc`, `presets\`, `scenes\`, `plugins\` and `cfg\`.
- If the update check says "No installer was found in that release", the release has no `.exe` attached. Download
  manually from the release page.

## 8. Uninstalling

- **Installed:** Windows Settings > Apps. The uninstaller asks once whether to also delete `homrec.hrc` and
  `logs\`. Recordings are never touched.
- **Portable:** delete the folder. Your recordings stay wherever the output folder pointed.
