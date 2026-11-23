| # | Idea | Effect | Diff. | Basis |
|---|---|---|---|---|
| 2 | **WGC path with no CPU readback.** It currently copies to a staging texture and maps it (`wgc_capture.cpp` ~857-880). Feed the GPU texture into the existing GPU NV12 convert instead, as the DXGI path already does. | 8 | 7 | Readback seen; whether WGC can reach the texture path is unverified. |
| 3 | **Replace the ffmpeg process with the in-process encoder** (`mf_encoder.cpp`, already in the tree). No pipe copy and no separate 170+ MB process. | 9 | 9 | The encoder is wired in but experimental and off by default (`mf_encoder=1`, cfg only); muxing, audio, and crash-safe files would all need reworking before it can be the default. |
| 4 | **Headless recorder mode.** The recorder runs as its own process (the homrec CLI is a starting point) and the UI can close during a long recording. | 7 | 7 | Idea only. |
| 6 | **Drop the preview when it can't be seen.** Minimised / hidden / locked / display-off is done (`SetPreviewVisible`). Still missing: the *occluded* case (window fully covered by another), and while recording only the thumbnails stop, the preview's buffers are not freed. | 6 | 2 | Your own numbers: ~50 MB with preview vs ~8-12 MB without. |
| 7 | **Lazy-create heavy dialogs and subsystems** (console window, settings, overlay editor, window picker, Lua) on first use, and destroy them on close. This also helps the "RAM grows with every version" problem. | 5 | 4 | The console window is created at startup (`main_frame.cpp` ~1047); settings is already modal and created on demand. |
| 8 | **Shrink the pool of buffers.** Frame buffers are about 25 MB at 1080p NV12 (queue of 8 plus pool of 4). Allocate them lazily and release them after a burst. | 3 | 3 | Constants seen in `pipeline.cpp`. |
| 9 | **Find where ffmpeg's 170 MB goes.** Test `-rc-lookahead 0`, `-filter_threads 1`, a smaller input queue, and NVENC surface settings explicitly. | 4 | 3 | NVENC args are now `-preset p5 -tune hq -rc vbr -cq N -spatial_aq 1`, which allocates more than the old p1 setup, so re-measure first. |
| 10 | **GPU-side preview downscale.** Today the preview averages NV12 down to RGB on the CPU. | 3 | 5 | The CPU thumbnail code is in `pipeline.cpp` ~340. |
| 11 | **Replace polling with events.** Only two `Sleep(1)` / `Sleep(2)` loops are left (`dxgi_capture.cpp` ~819, `wgc_capture.cpp` ~1560), plus a 20 Hz preview timer and a 500 ms stats timer. This cuts wakeups, which matters on a laptop battery. | 3 | 5 | Some timers already adapt when hidden. |
| 12 | **Build flags and trimming.** LTO and `--gc-sections` are already on. Left: delay-loading rarely used DLLs and dropping unused wx modules. | 3 | 3 | `CMakeLists.txt` checked. |
| 15 | **Crazy: encode on the Intel iGPU (QSV) and leave the RTX free for the game.** This lowers the hit to game FPS rather than HomRec's own cost, and it needs a cross-adapter copy. | 6 | 4 | The QSV args exist; whether it pays off on your laptop is unknown. |

Doing 6 and 9 next gives the most for the least work (both difficulty 3 or less).

Before changing anything, run `perf_test.py` for a baseline, since an effect score means nothing without before/after numbers.

Done and removed: 1 (ddagrab on by default), 5 (static-screen VFR was already on), 13 (NVENC VBR+CQ+AQ), 14 (working-set trim, now also on hide/minimise).