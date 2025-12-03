# lvgltest summary

- Added a transparent OSD scene with a lightweight animated box plus two static indicators instead of the heavy LVGL demos; on-screen stats overlay shows resolution, object position, FPS, and frame time. (main.c)
- Throttled rendering/animation to ~10 Hz to reduce load while keeping movement visible. (main.c)
- Size-focused build flags (`-Os`, section folding, no unwind tables, linker GC) and optional demo/example gating to keep the binary small. (Makefile, lvgl/lvgl.mk, lv_conf.h, build.sh)
- Clean shutdown on CTRL+C: timers are deleted and the OSD region is detached/destroyed to avoid hangs. (main.c)
