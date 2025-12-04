# LVGL OSD (UDP-driven)

- Transparent LVGL OSD that renders up to 8 configurable assets (basic bar, `lv_example_bar_2`-styled bar, `lv_example_scale_10`-styled gauge, or file-based Lottie animation) defined in `config.json` and driven by a UDP payload. Optional descriptors live to the right of bars/Lottie assets (static `label` or live `texts[]` channel). Background stays fully transparent; assets refresh at the configured cadence. (`main.c`, `config.json`)
- UDP listener on port `7777` consumes JSON payloads documented in `CONTRACT.md` (`values[]` + optional `texts[]`). Incoming packets are drained whenever the socket is readable so only the latest datagram drives the screen, and `refresh_ms` (default 100 ms, clamped 10–1000) sets the maximum idle wait between UDP polls and screen refreshes. (`main.c`, `CONTRACT.md`)
- Single stats widget in the top-left (gated by `show_stats`) shows OSD/display resolution, asset count, FPS, and timing. When `udp_stats` is true, it also lists all 8 numeric values and text channels vertically to avoid width overflow. (`main.c`, `config.json`)
- Size-first build: `-Os`, section folding, no unwind tables, linker GC/strip, LVGL demos/examples excluded by default. (`Makefile`, `lvgl/lvgl.mk`, `lv_conf.h`, `build.sh`)
- Clean SIGINT handling: timers, UDP socket, LVGL buffers, and RGN are torn down to avoid hangs. (`main.c`)

## Build
```
./build.sh
```
`lvgltest` is produced in the repo root using the Sigmastar toolchain bundled under `toolchain/`.

## Run
1) Adjust `config.json` (resolution, assets, refresh, stats). See examples inside the file.  
2) Launch the OSD:
```
./lvgltest
```
3) Drive it with the sample generator:
```
./bar_generator [ip] [port] [ms]   # defaults: 127.0.0.1 7777 100
```
The generator emits both `values[]` and sample 16-char `texts[]` for all 8 channels.

## Config & contract
- `config.json` defines screen size, refresh rate, stats toggle, UDP stats toggle, and up to 8 assets with positions, sizes, ranges, and color. See `CONTRACT.md` for the full schema and UDP payload format.
- To show descriptors on bars, set `label` (static text) and/or `text_index` (binds to a `texts[]` entry from UDP). Scale assets ignore descriptors.
- Lottie assets (`type: "lottie"`) load animations from a local JSON file path via `file` and accept `label`/`text_index` descriptors like bars. If no readable file is supplied, an embedded spinner-style animation bundled in `main.c` is used instead so no external LVGL Lottie component is required.
- UDP payloads must include a top-level `values` array; missing entries default to 0. Oversized packets are dropped. The loop keeps only the newest packet per frame.
- Optional `texts` array (max 8 entries, 16 chars each) can feed asset descriptors when `text_index` is set.
- `udp_stats` controls whether the stats widget also lists the latest 8 numeric values and text channels (on by default).

For schema details and examples, read `CONTRACT.md`.
