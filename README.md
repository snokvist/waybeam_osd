# LVGL OSD (UDP-driven)

- Transparent LVGL OSD that renders up to 8 configurable assets (bars with optional rounded outlines, text blocks sourced from UDP `texts[]`, or image assets) defined in `config.json` and driven by a UDP payload. Optional descriptors live to the right of bars (static `label` or live `texts[]` channel). Assets can start disabled and be enabled on demand over UDP; background stays fully transparent unless an asset-specific background swatch is selected. (`main.c`, `config.json`)
- Assets target 8 UDP-driven value/text channels (`0-7`) sourced from payload `values[]`/`texts[]`. (`main.c`)
- UDP listener on port `7777` consumes JSON payloads documented in `CONTRACT.md` (`values[]` + optional `texts[]`). The socket is fully drained whenever readable; sparse updates use `null` entries to skip untouched slots, empty strings clear a slot (`""` zeroes a value or blanks a text), and coalesced results refresh the screen no faster than every 32 ms (~30 fps). `idle_ms` (default 100 ms, clamped 10–1000) still caps the wait when no new data arrives. (`main.c`, `CONTRACT.md`)
- Single stats widget in the top-left (gated by `show_stats`) shows OSD/display resolution, asset count, FPS, timing, and live UDP value/text banks. `udp_stats` toggles whether the UDP bank lines are shown. (`main.c`, `config.json`)
- MI_RGN canvas info is cached and the driver is only updated once per LVGL frame to avoid per-chunk overhead when LVGL renders in partial buffers. (`main.c`)
- Size-first build: `-Os`, section folding, no unwind tables, linker GC/strip, LVGL demos/examples excluded by default. (`Makefile`, `lvgl/lvgl.mk`, `lv_conf.h`, `build.sh`)
- Clean signal handling: SIGINT/SIGTERM shut down cleanly (timers, UDP socket, LVGL buffers, and RGN), and SIGHUP reloads `config.json` at runtime to rebuild assets, toggle stats, and apply the new idle wait without restarting. (`main.c`)

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

### UDP sender/watch helper
- The lightweight `osd_send` helper (`osd_send.c`) now uses a JSON config file instead of source/payload CLI flags. By default it reads `osd_send.json`; override with `--config <file>`.
- `sources` blocks in config include explicit `enabled` toggles for `ini`, `hostapd`, `wpa_cli`, `rtl8812eu`, `cpu`, and `venc`. CPU keys are `cpu_total` (overall), `cpu0..cpuN` (per-core), and `cpu_cores`. VENC keys (fetched passively from majestic HTTP `/metrics`) are `venc_bitrate` (kbps, computed from byte counter delta), `venc_bytes` (raw counter), `isp_fps`, `isp_exposure`, `isp_again`, `isp_dgain`, `soc_temp` (celsius), `load_1m`, `mem_used_pct`.
- `payload.values` / `payload.texts` are positional arrays (up to 8) with expressions like `"@key"`, numeric literals, `""` (clear), `"null"` (emit JSON null), or `null` (slot disabled in sender config).
- Config parsing is strict: unknown keys in any object fail fast with an error.
- In `watch`, unsent updates are retained and retried every `watch.retry_ms` until a send succeeds, even if no new source changes arrive.
- Build `osd_send` only: `make osd_send`
- Build `osd_send_arm` (cross-compiled for board): `make osd_send_arm`
- Run examples:
  - `./osd_send send --config osd_send.json`
  - `./osd_send watch --config osd_send.json`

#### `osd_send` usage
```bash
./osd_send send  [--config osd_send.json]
./osd_send watch [--config osd_send.json]
```

- `send`: one-shot resolve+send.
- `watch`: continuous polling of enabled sources, sending only changed slots.
- `network.dest` and `network.port` can be literal values or `@key` references.
- If a watched send fails, the unsent delta is retried every `watch.retry_ms`.

#### `osd_send.json` examples

Example 1: INI + CPU metrics (common default)
```json
{
  "network": { "dest": "127.0.0.1", "port": 7777 },
  "runtime": { "verbose": false, "print_json": false },
  "watch": { "interval_ms": 64, "retry_ms": 5000 },
  "sources": {
    "ini": { "enabled": true, "paths": ["/tmp/aalink_ext.msg"] },
    "hostapd": { "enabled": false, "iface": "wlan0", "sta": "aa:bb:cc:dd:ee:ff" },
    "wpa_cli": { "enabled": false, "iface": "wlan0" },
    "rtl8812eu": { "enabled": false, "iface": "wlan0" },
    "cpu": { "enabled": true }
  },
  "payload": {
    "values": ["@used_rssi", "@mcs", "@cpu_total", "@cpu0", null, null, null, null],
    "texts": ["@used_source", "@gs_string", "@cpu_cores", null, null, null, null, null]
  }
}
```

Example 2: hostapd + wpa_cli + rtl8812eu watch
```json
{
  "network": { "dest": "127.0.0.1", "port": 7777 },
  "runtime": { "verbose": true, "print_json": false },
  "watch": { "interval_ms": 100, "retry_ms": 5000 },
  "sources": {
    "ini": { "enabled": false, "paths": [] },
    "hostapd": { "enabled": true, "iface": "wlan0", "sta": "aa:bb:cc:dd:ee:ff" },
    "wpa_cli": { "enabled": true, "iface": "wlan0" },
    "rtl8812eu": { "enabled": true, "iface": "wlan0" },
    "cpu": { "enabled": false }
  },
  "payload": {
    "values": ["@signal", "@RSSI", "@rssi_a", "@rssi_b", null, null, null, null],
    "texts": ["@tx_packets", "@rx_packets", null, null, null, null, null, null]
  }
}
```

Example 3: static/literal sender packet
```json
{
  "network": { "dest": "127.0.0.1", "port": 7777 },
  "runtime": { "verbose": false, "print_json": true },
  "watch": { "interval_ms": 64, "retry_ms": 5000 },
  "sources": {
    "ini": { "enabled": false, "paths": [] },
    "hostapd": { "enabled": false, "iface": "wlan0", "sta": "aa:bb:cc:dd:ee:ff" },
    "wpa_cli": { "enabled": false, "iface": "wlan0" },
    "rtl8812eu": { "enabled": false, "iface": "wlan0" },
    "cpu": { "enabled": false }
  },
  "payload": {
    "values": [0.25, 0.8, "", "null", null, null, null, null],
    "texts": ["HELLO", "", "null", null, null, null, null, null]
  }
}
```

Example 4: majestic metrics (venc/ISP/system via HTTP)
```json
{
  "network": { "dest": "127.0.0.1", "port": 7777 },
  "runtime": { "verbose": true, "print_json": false },
  "watch": { "interval_ms": 100, "retry_ms": 5000 },
  "sources": {
    "ini": { "enabled": false, "paths": [] },
    "hostapd": { "enabled": false, "iface": "wlan0", "sta": "aa:bb:cc:dd:ee:ff" },
    "wpa_cli": { "enabled": false, "iface": "wlan0" },
    "rtl8812eu": { "enabled": false, "iface": "wlan0" },
    "cpu": { "enabled": false },
    "venc": { "enabled": true, "url": "http://127.0.0.1/metrics" }
  },
  "payload": {
    "values": ["@venc_bitrate", "@isp_fps", "@soc_temp", "@mem_used_pct", null, null, null, null],
    "texts": [null, null, null, null, null, null, null, null]
  }
}
```

Notes:
- Keep only supported top-level keys: `network`, `runtime`, `watch`, `sources`, `payload`.
- Unknown keys are treated as errors (strict parser).
- In `payload` arrays:
  - `null` disables the slot in config.
  - `"null"` emits JSON `null` for that slot.
  - `""` clears the slot (`values` clear to zero on backend; `texts` clear text).

## Config & contract
- `config.json` defines screen size, idle wait, stats toggle, UDP stats toggle, and up to 8 assets with positions, sizes, ranges, background palette slot (11 options including a fully transparent swatch and semi-transparent tints), an optional `background_opacity` percent override, an `enabled` switch, and colors for bar/text/image assets. Bars can choose an `orientation` of `right` (default) or `left` to flip label placement; `left` also reverses the bar fill so it grows from right-to-left and anchors the container’s right edge at `x` so left/right bars can share the same coordinate. Each asset can also carry an `id` used by UDP-side `asset_updates` to retint backgrounds, bar colors, text colors, move/resize, or swap types live. See `CONTRACT.md` for the full schema and UDP payload format. Background palette indices: 0 transparent, 1 black, 2 white, 3 charcoal, 4 charcoal dark, 5 blue, 6 teal, 7 green, 8 orange, 9 pink, 10 purple. Default palette opacities mirror the per-index list (0%, 50%, 50%, 70%, 90%, 60%, 60%, 60%, 70%, 60%, 70%), and `background_opacity` lets you pick any 0–100%.
- `width`/`height` define the LVGL/RGN canvas size, and `osd_x`/`osd_y` place that canvas within the video frame.
- `realtime_flip` optionally enables SigmaStar `setRealtimeFlip` for the attached OSD VPE channel at startup (default `false`), prints the bound path on success, and attempts to disable it again during shutdown for safer teardown.
- To show descriptors on bars, set `label` (static text) and/or `text_index` (binds to a `texts[]` entry from UDP). Bars accept `rounded_outline` to enable the outlined capsule style, `segments` to split the fill into evenly spaced blocks (e.g., for battery-style indicators where blocks extinguish one-by-one as the value falls), plus `text_color`, `bar_color`, `background`, and `background_opacity` to tint the bar and a shared rounded background that wraps its label.
- Text assets (`type: "text"`) render one or more UDP text channels (`text_indices`) stacked on new lines or concatenated inline (`text_inline`). They honor `rounded_outline` for pill-like backgrounds with inner padding, and keep `label`/`text_index` as fallbacks alongside `background`, `background_opacity`, `text_color`, and `orientation` (`left`/`right`/`center`).
- Text assets also support `scale_pct` (25–400, default 100) via LVGL transform zoom, which lets you enlarge text without bundling additional larger font files. For scaled text/image assets, `orientation: "left"` anchors the right edge (pivot + placement), `"right"` anchors the left edge, and `"center"` anchors the midpoint.
- Image assets (`type: "image"`) render a local image file from `image_path` (LVGL fs driver prefixes like `"A:/path.png"` are accepted) at `x`/`y`; optional `width`/`height` set a size override, otherwise the native image size is used. `scale_pct` (25–400, default 100) can up/down-scale the rendered image at runtime. Image assets still honor `background` and `background_opacity` for tinting the image object, but ignore bar/text-only fields like `label` or `text_index`.
- UDP payloads must include a top-level `values` array; missing entries default to 0. Packets up to 1280 bytes are accepted; oversized packets are dropped. Any queued packets are read in order and coalesced before the screen is refreshed, pushes are capped to once every 32 ms to avoid over-updating, and sparse updates are supported via `null` placeholders so multiple senders can avoid clobbering each other. Optional `asset_updates` with matching `id` fields can enable or disable assets, swap types, move/resize them, remap value/text indices, and retint colors/backgrounds on the fly (only valid, changed fields are applied). Unknown IDs are created up to 8 total assets.
- Optional `texts` array (max 8 entries, 96 chars each) can feed asset descriptors when `text_index` is set.
- `udp_stats` controls whether the stats widget also lists the latest 8 numeric values and text channels (on by default).

For schema details and examples, read `CONTRACT.md`.
