# Overlays, layers and scenes

Put text, images, GIFs, a webcam or a key-press display on top of your recording, and save different set-ups as scenes.

## Concepts

| Term | Meaning |
|---|---|
| **Layers panel** (called *Overlays* in the window) | The list of everything that is drawn in the recording. The **first row is the capture source**. |
| **Overlay** | One extra item on top: text, image, GIF, webcam or input overlay. |
| **Scene** | A saved set of layers and overlays. Stored as `scenes\scene_*.hrc` next to `hr.exe`. |

<!-- IMG: hr-overlays-layers-panel -->

## Add a layer or overlay, step by step

1. In the Layers panel click **+**. The menu offers: **Window capture...**, **Region capture...**, **Text**, **Image**,
   **GIF**, **Webcam**, **External Overlay (.json + .png)...** and, when built-in input-overlay presets exist,
   **Select Input-Overlay...**. An empty list says "No overlays yet. Click + to add one."
2. Answer the question that appears:

| Type | You provide | Failure message |
|---|---|---|
| **Text** | The text to display ("New Text" by default) | n/a |
| **Image** | A picture file | n/a |
| **GIF** | A `.gif` file | n/a |
| **Webcam** | Chosen from the cameras found | "No webcam was found. Make sure a camera is connected ..." |
| **External Overlay / Input-Overlay** | A `.json` layout and a `.png` spritesheet, or a built-in preset | "That .json file couldn't be read as an input-overlay layout. Make sure it's the plain layout file, not something else." / "That preset's layout file couldn't be read." |

3. Right-click a layer for: **Hide / Show**, **Rename...**, **Settings...**, **Duplicate**, **Move up**, **Move down**,
   **Center**, **Lock position and size**, **Keep proportions when resizing**, **Delete**.
4. **Settings...** opens one editor window with position, size, parameters and a live preview. This replaces the older
   separate "Edit Parameters" and "Position Overlays" entries.

<!-- IMG: hr-overlays-add-menu -->
<!-- IMG: hr-overlays-edit-dialog -->

**Check:** the overlay is visible on the preview. It is drawn into the file the same way.

### Overlays and the recording path

Overlays are drawn on the GPU when a hardware encoder is used, and on the CPU otherwise. They cannot be used in the
experimental full-GPU ddagrab mode; in that case HomRec quietly uses the normal pipeline ([codecs.md](codecs.md)).

## Window and region layers

Window capture and Region capture are layers like any other: add several to build one picture from many parts.
Choose the window or region the same way as in [capture-sources.md](capture-sources.md).

## Scenes

The **Scenes** panel lists your scenes.

1. The **New scene** button adds a scene (up to 64).
2. Select a scene in the list. Buttons move it **up** / **down**; **Delete scene** removes it.
3. Scene files are plain text; back up `scenes\` to keep them.

<!-- IMG: hr-scenes-panel -->

## What can fail

| Symptom | Cause | Fix |
|---|---|---|
| The overlay shows on the preview but not in the file | A path or mode that skips overlays (ddagrab). | Untick the full-GPU option ([codecs.md](codecs.md)). |
| Input overlay shows garbage or nothing | Wrong layout/spritesheet pair. | Use a matching `.json` + `.png`. |
| Webcam overlay empty | Another app holds the camera, or Windows blocks camera access. | Close the other app; Windows Settings > Privacy & security > Camera. |
| Browse... shows unreadable file names | A path with unusual characters. | Move the file to a plain path such as `C:\\overlays\\`. |
| A scene does not switch | The scene file is damaged. | Rename `scenes\scene_*.hrc`, restart, and re-create it. |
| Overlay missing after an update | `.hrc` written by another version. | Re-add the overlay; report it with the log ([logs-and-diagnostics.md](logs-and-diagnostics.md)). |
