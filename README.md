# LVGL OSD (UDP-driven)

- Transparent LVGL OSD that renders up to 8 configurable assets (basic bar, `lv_example_bar_2`-styled bar, or text blocks sourced from UDP `texts[]`) defined in `config.json` and driven by a UDP payload. Optional descriptors live to the right of bars (static `label` or live `texts[]` channel). Assets can start disabled and be enabled on demand over UDP; background stays fully transparent unless an asset-specific background swatch is selected. (`main.c`, `config.json`)
- UDP listener on port `7777` consumes JSON payloads documented in `CONTRACT.md` (`values[]` + optional `texts[]`). Incoming packets are drained whenever the socket is readable so only the latest datagram drives the screen and trigger an immediate refresh; `idle_ms` (default 100 ms, clamped 10–1000) only sets the maximum idle wait between UDP polls when no new data arrives. (`main.c`, `CONTRACT.md`)
- Single stats widget in the top-left (gated by `show_stats`) shows OSD/display resolution, asset count, FPS, and timing. When `udp_stats` is true, it also lists all 8 numeric values and text channels vertically to avoid width overflow. (`main.c`, `config.json`)
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
The generator emits both `values[]` and sample 16-char `texts[]` for all 8 channels, retints asset 0 live, and enables IDs 6 and 7 with bar/text payloads to demonstrate on-demand assets.

## Config & contract
- `config.json` defines screen size, idle wait, stats toggle, UDP stats toggle, and up to 8 assets with positions, sizes, ranges, background palette slot (11 options including a fully transparent swatch and semi-transparent tints), an optional `background_opacity` percent override, an `enabled` switch, and colors for bar/text. Each asset can also carry an `id` used by UDP-side `asset_updates` to retint backgrounds, bar colors, text colors, move/resize, or swap types live. See `CONTRACT.md` for the full schema and UDP payload format. Background palette indices: 0 transparent, 1 black, 2 white, 3 charcoal, 4 charcoal dark, 5 blue, 6 teal, 7 green, 8 orange, 9 pink, 10 purple. Default palette opacities follow the names (transparent/50%/70%/90%/60%/70%), and `background_opacity` lets you pick any 0–100%.
- To show descriptors on bars, set `label` (static text) and/or `text_index` (binds to a `texts[]` entry from UDP). Both bar styles accept `text_color`, `bar_color`, `background`, and `background_opacity` to tint the bar and a shared rounded background that wraps its label.
- Text assets (`type: "text"`) render one or more UDP text channels (`text_indices`) stacked on new lines or concatenated inline (`text_inline`), with optional `label` fallback plus `background`, `background_opacity`, and `text_color` styling.
- UDP payloads must include a top-level `values` array; missing entries default to 0. Oversized packets are dropped. The loop keeps only the newest packet per frame. Optional `asset_updates` with matching `id` fields can enable or disable assets, swap types, move/resize them, remap value/text indices, and retint colors/backgrounds on the fly (only valid, changed fields are applied). Unknown IDs are created up to 8 total assets.
- Optional `texts` array (max 8 entries, 16 chars each) can feed asset descriptors when `text_index` is set.
- `udp_stats` controls whether the stats widget also lists the latest 8 numeric values and text channels (off by default).

For schema details and examples, read `CONTRACT.md`.
