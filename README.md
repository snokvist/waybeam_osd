# LVGL OSD (UDP-driven)

- Transparent LVGL OSD that renders up to 8 configurable assets (bars with optional rounded outlines or text blocks sourced from UDP `texts[]`) defined in `config.json` and driven by a UDP payload. Optional descriptors live to the right of bars (static `label` or live `texts[]` channel). Assets can start disabled and be enabled on demand over UDP; background stays fully transparent unless an asset-specific background swatch is selected. (`main.c`, `config.json`)
- Assets can target 16 combined value/text channels: UDP-driven slots `0-7` plus 8 system slots at `8-15`. System values refresh about once per second by default (configurable via `system_refresh_ms`) and expose SoC temperature (read from `/sys/devices/system/cpu/cpufreq/temp_out` when present, otherwise via `ipctool --temp`), CPU load (0-100), encoder FPS, current encoder bitrate in kilobits per second, and four reserved placeholders; system texts are prefilled descriptors for the same slots. (`main.c`)
- Encoder telemetry is read from `/proc/mi_modules/mi_venc/mi_venc0` (channel 0 `Fps_1s` + `kbps`) so no direct module loading is needed. (`main.c`)
- UDP listener on port `7777` consumes JSON payloads documented in `CONTRACT.md` (`values[]` + optional `texts[]`). The socket is fully drained whenever readable; sparse updates use `null` entries to skip untouched slots, empty strings clear a slot (`""` zeroes a value or blanks a text), and coalesced results refresh the screen no faster than every 32 ms (~30 fps). `idle_ms` (default 100 ms, clamped 10–1000) still caps the wait when no new data arrives. (`main.c`, `CONTRACT.md`)
- Single stats widget in the top-left (gated by `show_stats`) shows OSD/display resolution, asset count, FPS, timing, and live system value/text banks. When `udp_stats` is true (default), the UDP numeric/text banks are shown alongside their system counterparts on the same lines to save space. (`main.c`, `config.json`)
- MI_RGN canvas info is cached and the driver is only updated once per LVGL frame to avoid per-chunk overhead when LVGL renders in partial buffers. (`main.c`)
- Size-first build: `-Os`, section folding, no unwind tables, linker GC/strip, LVGL demos/examples excluded by default. (`Makefile`, `lvgl/lvgl.mk`, `lv_conf.h`, `build.sh`)
- Clean signal handling: SIGINT shuts down cleanly (timers, UDP socket, LVGL buffers, and RGN), and SIGHUP reloads `config.json` at runtime to rebuild assets, toggle stats, and apply the new idle wait without restarting. (`main.c`)

## Build
```
./build.sh
```
`lvgltest` is produced in the repo root using the Sigmastar toolchain bundled under `toolchain/`.
## Run
1) Adjust `config.json` (resolution, assets, idle wait, stats). See examples inside the file.
2) Launch the OSD:
```
./lvgltest
```
   - Send `SIGHUP` to the process to reload `config.json` while it is running (assets/stats/idle wait update in-place; screen resolution still follows the startup config).
3) Drive it with the sample generator:
```
./osd_generator [ip] [port] [ms]   # defaults: 127.0.0.1 7777 100
```
The generator emits both `values[]` and sample 96-char-capable `texts[]` for all 8 channels, retints asset 0 live, and enables IDs 6 and 7 with bar/text payloads to demonstrate on-demand assets.

## Config & contract
- `config.json` defines screen size, idle wait, stats toggle, UDP stats toggle, and up to 8 assets with positions, sizes, ranges, background palette slot (11 options including a fully transparent swatch and semi-transparent tints), an optional `background_opacity` percent override, an `enabled` switch, and colors for bar/text. Bars can choose an `orientation` of `right` (default) or `left` to flip label placement; `left` also reverses the bar fill so it grows from right-to-left and anchors the container’s right edge at `x` so left/right bars can share the same coordinate. Each asset can also carry an `id` used by UDP-side `asset_updates` to retint backgrounds, bar colors, text colors, move/resize, or swap types live. See `CONTRACT.md` for the full schema and UDP payload format. Background palette indices: 0 transparent, 1 black, 2 white, 3 charcoal, 4 charcoal dark, 5 blue, 6 teal, 7 green, 8 orange, 9 pink, 10 purple. Default palette opacities mirror the per-index list (0%, 50%, 50%, 70%, 90%, 60%, 60%, 60%, 70%, 60%, 70%), and `background_opacity` lets you pick any 0–100%.
- `width`/`height` define the LVGL/RGN canvas size, and `osd_x`/`osd_y` place that canvas within the video frame.
- To show descriptors on bars, set `label` (static text) and/or `text_index` (binds to a `texts[]` entry from UDP). Bars accept `rounded_outline` to enable the outlined capsule style, `segments` to split the fill into evenly spaced blocks (e.g., for battery-style indicators where blocks extinguish one-by-one as the value falls), plus `text_color`, `bar_color`, `background`, and `background_opacity` to tint the bar and a shared rounded background that wraps its label.
- Text assets (`type: "text"`) render one or more UDP text channels (`text_indices`) stacked on new lines or concatenated inline (`text_inline`), can pair each text with a numeric channel via `value_indices` (aligned by position), and use `inline_separator` to control inline spacing (e.g., `"|"` renders `text: | next: value`). They honor `rounded_outline` for pill-like backgrounds with inner padding, and keep `label`/`text_index` as fallbacks alongside `background`, `background_opacity`, `text_color`, and `orientation` (`left`/`center`/`right` align both the anchor point and text).
- UDP payloads must include a top-level `values` array; missing entries default to 0. Packets up to 1280 bytes are accepted; oversized packets are dropped. Any queued packets are read in order and coalesced before the screen is refreshed, pushes are capped to once every 32 ms to avoid over-updating, and sparse updates are supported via `null` placeholders so multiple senders can avoid clobbering each other. Optional `asset_updates` with matching `id` fields can enable or disable assets, swap types, move/resize them, remap value/text indices, and retint colors/backgrounds on the fly (only valid, changed fields are applied). Unknown IDs are created up to 8 total assets.
- Optional `texts` array (max 8 entries, 96 chars each) can feed asset descriptors when `text_index` is set.
- `udp_stats` controls whether the stats widget also lists the latest 8 numeric values and text channels (on by default).

For schema details and examples, read `CONTRACT.md`.
