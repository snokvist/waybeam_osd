# OSD Data Contract

## Runtime UDP payload (port 7777)
- Each datagram must be UTF-8 JSON with a top-level `values` array.
- `values` holds up to 8 numeric entries (float/double) for UDP channels `0-7`. Missing entries default to `0` on the device.
- Extra fields are ignored so senders can add metadata if needed.
- Keep payloads under 1280 bytes (anything larger is dropped).
- Incoming UDP packets are applied in arrival order; the socket is fully drained whenever it becomes readable so every queued packet is processed. The last packet for a given index/property wins, and on-screen pushes are throttled to ~30 fps (about every 32 ms). `idle_ms` only caps the sleep when no data arrives.
- Optional `texts` array (up to 8 strings, max 96 chars each) can be sent alongside `values`. These map to `text_index` on bar/text assets and override a static `label` if present. `null` entries are ignored (keep existing text); an empty string clears the text and falls back to the asset’s `label`.
- Optional `asset_updates` array lets senders retint, reposition, enable/disable, or fully reconfigure assets at runtime. Each object must contain an `id`; if the ID does not exist yet and there is room (max 8 assets), the asset slot is created on the fly. Valid keys include: `enabled` (bool), `type` (`"bar"`, `"text"`, or `"image"`), `value_index`, `text_index`, `text_indices` (array), `text_inline`, `label`, `orientation`, `x`, `y`, `width`, `height`, `scale_pct` (text/image only), `min`, `max`, `bar_color` (string, bars only), `text_color` (string), `background` (string), `background_opacity`, `segments` (bars only), and `rounded_outline` (bars and text backgrounds). Color fields (`bar_color`, `text_color`, `background`) accept a named color (e.g. `"green"`, `"blue"`) or a hex string (`"#RRGGBB"` or `"RRGGBB"`). Only valid values that differ from the current config are applied; disabled assets are removed from the screen immediately.

Example:
```json
{
  "values":[0.12,0.5,1.0,0,0,0,0,0],
  "texts":["BAR CH0","BAR CH1","TEXT CH2","CH3","CH4","CH5","CH6","CH7"],
  "asset_updates":[
    {"id":0,"bar_color":"green","background":"blue","background_opacity":80},
    {"id":6,"enabled":true,"type":"bar","value_index":6,"label":"UDP BAR 6","x":10,"y":200,"width":300,"height":24,"bar_color":"#0000FF"},
    {"id":7,"enabled":true,"type":"text","text_indices":[7],"text_inline":true,"label":"UDP TEXT 7","x":360,"y":200,"width":320,"height":60}
  ],
  "timestamp_ms":1712345678
}
```

Each on-screen asset binds to one numeric channel via `value_index` (`0-7` mapping to UDP `values[i]`). For bar/text assets, `text_index` maps the descriptor to UDP `texts[i]` (`0-7`), otherwise the optional static `label` is used.

### Partial Update Examples

The `values` and `texts` arrays are positional. `null` entries are ignored (slot keeps its previous value/text), while omitted trailing indices also keep their previous content. Use explicit numbers to overwrite value slots, and empty strings to clear either a text slot or a numeric slot (clears to `0`).

**Update only the first value:**
```json
{"values": [0.75]}
```
*Effect:* `values[0]` is set to 0.75. `values[1..7]` are unchanged.

**Update the first three values:**
```json
{"values": [0.1, 0.2, 0.3]}
```
*Effect:* `values[0..2]` are updated. `values[3..7]` are unchanged.

**Update index 2 (skipping 0 and 1):**
```json
{"values": [null, null, 0.9]}
```
*Effect:* `values[2]` is set to 0.9. `values[0]` and `values[1]` retain their previous values.

### Multi-Sender Interference

The UDP socket is **drained fully** on every poll cycle, meaning every packet in the buffer is processed in order. This mitigates packet loss but introduces specific behavior for multi-sender scenarios:

1.  **Shared Indices:** If Sender A and Sender B both update the *same* UDP index within the same poll cycle, the packet processed last (typically the one arriving last) wins.
2.  **Positional Arrays:** Because arrays are positional, senders must be careful not to overwrite indices owned by others. Using `null` to skip indices (sparse updates) allows independent senders to manage distinct sets of indices without interference, provided they agree on the layout.
3.  **Asset Updates:** `asset_updates` are ID-based and safe to mix, as long as senders target different asset IDs.

**Conclusion:** Multiple independent senders **can** share the display if they use sparse arrays (`null` for unowned indices) or target mutually exclusive asset IDs.

## CLI sender/watch helper
- The `osd_send` helper (`osd_send.c`) is config-driven. It reads `osd_send.json` by default (or `--config <file>`) for both `send` and `watch`.
- Config fields:
  - `network.dest` and `network.port` (`port` can be number/string or `@key`).
  - `runtime.verbose` and `runtime.print_json`.
  - `watch.interval_ms` and `watch.retry_ms`.
  - `sources.ini`, `sources.hostapd`, `sources.wpa_cli`, `sources.rtl8812eu`, `sources.cpu`, `sources.venc` (each with `enabled` plus source-specific fields).
  - `payload.values` and `payload.texts` arrays (0-8 entries).
- Parsing is strict: unknown keys at root or in nested config objects are treated as errors.
- Source behavior:
  - `sources.ini.paths` loads one or more key/value files.
  - `sources.hostapd` reads `STA <mac>` from `/run|/var/run/hostapd`.
  - `sources.wpa_cli` queries wpa_supplicant via STATUS + SIGNAL_POLL + STA-FIRST from `/run|/var/run/wpa_supplicant/<iface>`. Works in both AP and station mode. Only whitelisted keys are exposed: STATUS gives `bssid`, `freq`, `ssid`, `mode`, `ip_address`, `wpa_state`; SIGNAL_POLL (station mode) gives `RSSI`, `LINKSPEED`, `NOISE`, `FREQUENCY`; STA-FIRST (AP mode) gives `signal`, `rx_packets`, `tx_packets`, `connected_time`, `inactive_msec`.
  - `sources.rtl8812eu` reads `/proc/net/rtl88x2eu/<iface>/rssi_a` and `rssi_b`.
  - `sources.cpu` reads `/proc/stat` and exposes `cpu_total`, `cpu0..cpuN`, `cpu_cores`.
  - `sources.venc` fetches Prometheus metrics from majestic's HTTP `/metrics` endpoint (passive, never touches the encoder). `url` defaults to `http://127.0.0.1/metrics`. Exposed keys: `venc_bitrate` (kbps, computed from byte counter delta), `venc_bytes` (raw counter), `isp_fps`, `isp_exposure`, `isp_again`, `isp_dgain`, `soc_temp` (celsius), `load_1m`, `mem_used_pct`.
- If `sources.venc` is enabled on a target without majestic `/metrics` (or without the expected counters), the source resolves as missing keys and mapped payload slots emit JSON `null` when requested. Fetch attempts are throttled to about once per second so unsupported targets are not polled at the full watch interval.
- `watch` refreshes enabled sources every interval. Endpoint keys (`network.dest` / `network.port` when `@key`) are re-resolved on each send attempt. On endpoint/serialize/send failure, the unsent delta is retained and retried every `watch.retry_ms` until send succeeds.

### `osd_send` usage
```bash
./osd_send send      [--config osd_send.json]
./osd_send watch     [--config osd_send.json]
./osd_send list-keys [--config osd_send.json]
```

- `send`: one-shot resolve+send.
- `watch`: continuous polling of enabled sources, sending only changed slots.
- `list-keys`: one-shot discovery of all available `@key` names grouped by source with current values. Useful for finding which keys to wire into `payload.values`/`payload.texts`.
- Build helper only: `make osd_send`. Cross-compile for board: `make osd_send_arm`.

### `osd_send.json` examples

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

Example 2: wpa_cli AP mode + venc watch
```json
{
  "network": { "dest": "127.0.0.1", "port": 7777 },
  "runtime": { "verbose": true, "print_json": false },
  "watch": { "interval_ms": 100, "retry_ms": 5000 },
  "sources": {
    "ini": { "enabled": false, "paths": [] },
    "hostapd": { "enabled": false, "iface": "wlan0", "sta": "aa:bb:cc:dd:ee:ff" },
    "wpa_cli": { "enabled": true, "iface": "wlan0" },
    "rtl8812eu": { "enabled": false, "iface": "wlan0" },
    "cpu": { "enabled": true },
    "venc": { "enabled": true, "url": "http://127.0.0.1/metrics" }
  },
  "payload": {
    "values": ["@venc_bitrate", "@cpu_total", "@soc_temp", "@mem_used_pct", "@signal", null, null, null],
    "texts": ["@ssid", "@freq", "@cpu_cores", null, null, null, null, null]
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
- Supported top-level keys: `network`, `runtime`, `watch`, `sources`, `payload`.
- Unknown keys are rejected (strict parser).
- In `payload` arrays:
  - `null` disables the slot in config.
  - `"null"` emits JSON `null` for that slot.
  - `""` clears the slot (`values` clear to zero on backend; `texts` clear text).

## Local config file (`config.json`)
- JSON file read at startup; missing keys fall back to defaults. Send `SIGHUP` to the running process to reload the file without restarting (asset layout, stats toggle, and `idle_ms` update in-place; resolution still follows the startup config).
- Top-level fields:
- `width`, `height` (int): OSD canvas resolution. Default 1280x720.
- `osd_x`, `osd_y` (int, optional): On-screen origin for the RGN. Default `0,0`.
  - `show_stats` (bool): show/hide the top-left stats overlay. Default `true`.
  - `udp_stats` (bool): when `true`, the stats overlay also lists the latest UDP numeric/text banks. Default `true`.
  - `idle_ms` (int): maximum idle wait between UDP polls and screen refreshes in milliseconds (clamped 10–1000); default 100 ms. Legacy configs may still specify `refresh_ms`, which is treated the same way for compatibility.
  - `realtime_flip` (bool, optional): when `true`, startup writes SigmaStar `setRealtimeFlip ... 1` for the active OSD channel binding and prints a confirmation on success. On shutdown the same binding is written back to `... 0` before region teardown. Default `false`; failures are warning-only so OSD init can continue.
  - `assets` (array, max 8): list of objects defining what to render and which UDP value to consume.
  - Asset fields:
    - `type`: `"bar"`, `"text"`, or `"image"`.
    - `enabled` (bool, optional): when `false`, the asset stays hidden until enabled by config reload or UDP `asset_updates`. Defaults to `true`.
    - `id` (int, optional): unique asset identifier for UDP `asset_updates`. Defaults to the array index when omitted.
    - `value_index` (int): which numeric channel drives this asset (`0–7` for UDP `values[i]`).
    - `text_index` (int, optional, bars/text): which text channel drives the descriptor (`0–7` from UDP `texts[i]`). `-1` or missing skips live text.
    - `text_indices` (array<int>, text only): render multiple UDP text entries; empty strings are skipped.
    - `text_inline` (bool, text only): when `true`, joins `text_indices` on a single line; otherwise stacks them on new lines. Default `false`.
    - `label` (string, optional, bars/text): static text descriptor. Used when no UDP text is present.
    - `orientation` (string): `"right"` (default) keeps the bar horizontal with the label to the right; `"left"` mirrors the layout with the label on the left and flips the fill so the bar grows from right-to-left. For `left`, the bar container anchors its right edge at `x` so left- and right-oriented bars can share the same coordinate and grow in opposite directions. Scaled text/image assets reuse this anchor behavior: `"left"` keeps the right edge fixed while zooming, `"right"` keeps the left edge fixed, and `"center"` keeps the midpoint fixed.
    - `x`, `y` (int): position relative to the OSD top-left. For `orientation: "left"`, `x` represents the right edge anchor; for `"center"`, `x` represents the midpoint anchor (text/image assets).
    - `width`, `height` (int): size in pixels. For text, enables wrapping; for image assets, setting either value forces an explicit size override instead of the native image dimensions.
    - `scale_pct` (int, text/image only): object zoom percentage (25–400, default `100`). Text scaling uses LVGL transform zoom so larger text can be rendered without bundling extra font files.
    - `image_path` (string, image only): required path to a local image asset. LVGL filesystem prefixes (for example, `"A:/path/to/icon.png"`) are accepted and stripped before loading.
    - `min`, `max` (float): input range mapped to 0–100% for bars.
    - `bar_color` (string): color name or `#RRGGBB` hex string; used by bar styles.
    - `rounded_outline` (bool, bars/text): enables the outlined capsule look on bars or rounded backgrounds with padded text for text assets. Defaults to `false`.
    - `segments` (int, bars only): when greater than 1, divides the bar fill into that many evenly spaced blocks that extinguish one-by-one as the value drops (useful for battery-style indicators). Defaults to `0`/unset for a continuous fill.
    - `text_color` (string, optional): color name or `#RRGGBB` hex string for labels/text content. Default white.
    - `background` (string, optional): color name or `#RRGGBB` hex string for the background fill. Omission keeps the default transparent look. For bars, the background is applied to a rounded container that extends across the bar and its label for a unified pill.
    - `background_opacity` (int, optional): percent opacity (0–100) to apply to the background color. When omitted and a background color is set, defaults to 50%.
    - Named colors: `black` (#000000), `white` (#FFFFFF), `dark_gray` (#111111), `charcoal` (#222222), `blue` (#2266CC), `teal` (#009688), `green` (#00FF00), `forest` (#4CAF50), `orange` (#FF9800), `pink` (#E91E63), `purple` (#9C27B0), `red` (#FF0000), `yellow` (#FFFF00), `cyan` (#00FFFF), `gray` (#888888), `light_gray` (#CCCCCC). Names are case-insensitive.

Example:
```json
{
  "width": 1280,
  "height": 720,
    "show_stats": true,
    "udp_stats": true,
  "assets": [
    { "type": "bar", "value_index": 0, "text_index": 0, "label": "BAR CH0", "x": 40, "y": 200, "width": 320, "height": 32, "min": 0.0, "max": 1.0, "orientation": "right", "segments": 8, "bar_color": "#2266CC", "text_color": "white", "background": "charcoal", "background_opacity": 70 },
    { "type": "bar", "value_index": 1, "text_index": 1, "label": "BAR CH1", "x": 420, "y": 140, "width": 220, "height": 24, "min": 0.0, "max": 1.0, "orientation": "left", "bar_color": "#2266CC", "text_color": "black", "background": "white", "background_opacity": 60, "rounded_outline": true },
    { "type": "text", "text_indices": [2, 3, 4], "text_inline": true, "label": "Status", "x": 40, "y": 260, "width": 360, "height": 60, "background": "black", "background_opacity": 50, "rounded_outline": true, "text_color": "white" }
  ]
}
```
