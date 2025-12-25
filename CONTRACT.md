# OSD Data Contract

## Runtime UDP payload (port 7777)
- Each datagram must be UTF-8 JSON with a top-level `values` array.
- `values` holds up to 8 numeric entries (float/double) for UDP channels `0-7`. Missing entries default to `0` on the device. A second bank of 8 system values is always available on `value_index` slots `8-15` without going over UDP. System slots 8-11 are populated automatically with SoC temperature (read from `/sys/devices/system/cpu/cpufreq/temp_out` when available, otherwise via `ipctool --temp`), CPU load (0–100), encoder FPS, and current encoder bitrate in kilobits per second (parsed from `/proc/mi_modules/mi_venc/mi_venc0` under the `VENC 0 CHN info` table using the `Fps_1s` and adjacent `kbps` columns for channel 0). Slots 12-15 stay reserved for future system metrics. Refresh cadence is configurable via `system_refresh_ms` in the local config (default 1000 ms).
- Extra fields are ignored so senders can add metadata if needed.
- Keep payloads under 1280 bytes (anything larger is dropped).
- Incoming UDP packets are applied in arrival order; the socket is fully drained whenever it becomes readable so every queued packet is processed. The last packet for a given index/property wins, and on-screen pushes are throttled to ~30 fps (about every 32 ms). `idle_ms` only caps the sleep when no data arrives.
- Optional `texts` array (up to 8 strings, max 96 chars each) can be sent alongside `values`. These map to `text_index` on bar assets and override a static `label` if present. `null` entries are ignored (keep existing text); an empty string clears the text and falls back to the asset’s `label`. System text slots `8-15` are reserved for future data and come prefilled with descriptors (`temp`, `cpu`, `enc fps`, `bitrate`, `sys4`, `sys5`, `sys6`, `sys7`).
- Optional `asset_updates` array lets senders retint, reposition, enable/disable, or fully reconfigure assets at runtime. Each object must contain an `id`; if the ID does not exist yet and there is room (max 8 assets), the asset slot is created on the fly. Valid keys include: `enabled` (bool), `type` (`"bar"` or `"text"`), `value_index`, `text_index`, `text_indices` (array), `text_inline`, `label`, `orientation`, `x`, `y`, `width`, `height`, `min`, `max`, `bar_color` (bars only), `text_color`, `background`, `background_opacity`, `segments` (bars only), and `rounded_outline` (bars only). Only valid values that differ from the current config are applied; disabled assets are removed from the screen immediately.

Example:
```json
{
  "values":[0.12,0.5,1.0,0,0,0,0,0],
  "texts":["BAR CH0","BAR CH1","TEXT CH2","CH3","CH4","CH5","CH6","CH7"],
  "asset_updates":[
    {"id":0,"bar_color":65280,"background":5,"background_opacity":80},
    {"id":6,"enabled":true,"type":"bar","value_index":6,"label":"UDP BAR 6","x":10,"y":200,"width":300,"height":24,"bar_color":255},
    {"id":7,"enabled":true,"type":"text","text_indices":[7],"text_inline":true,"label":"UDP TEXT 7","x":360,"y":200,"width":320,"height":60}
  ],
  "timestamp_ms":1712345678
}
```

Each on-screen asset binds to one numeric channel via `value_index`. Indices `0-7` read the UDP `values[i]`; indices `8-15` read the system value bank (temperature, CPU load, encoder FPS, encoder bitrate, and four reserved slots). For bar assets, `text_index` maps the descriptor to the combined text bank: `0-7` pull from UDP `texts[i]`, while `8-15` use the prefilled system descriptors. Otherwise the bar uses the optional static `label`. The stats overlay always lists the system numeric/text banks and, when `udp_stats` is enabled, also lists the UDP numeric/text banks on the same lines to keep the widget compact.

### Partial Update Examples

The `values` and `texts` arrays are positional. `null` entries are ignored (slot keeps its previous value/text), while omitted trailing indices also keep their previous content; system slots 8–15 are populated locally. Use explicit numbers to overwrite value slots, and empty strings to clear either a text slot or a numeric slot (clears to `0`).

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

## Local config file (`config.json`)
- JSON file read at startup; missing keys fall back to defaults. Send `SIGHUP` to the running process to reload the file without restarting (asset layout, stats toggle, and `idle_ms` update in-place; resolution still follows the startup config).
- Top-level fields:
- `width`, `height` (int): OSD canvas resolution. Default 1280x720.
- `osd_x`, `osd_y` (int, optional): On-screen origin for the RGN. Default `0,0`.
  - `show_stats` (bool): show/hide the top-left stats overlay. Default `true`.
  - `udp_stats` (bool): when `true`, the stats overlay also lists the latest UDP and system numeric/text banks on the same lines. Default `true`.
  - `idle_ms` (int): maximum idle wait between UDP polls and screen refreshes in milliseconds (clamped 10–1000); default 100 ms. Legacy configs may still specify `refresh_ms`, which is treated the same way for compatibility.
  - `system_refresh_ms` (int, optional): cadence for polling system metrics (temperature, CPU load, encoder FPS/bitrate). Clamped 100–60000; default 1000.
  - `assets` (array, max 8): list of objects defining what to render and which UDP value to consume.
  - Asset fields:
    - `type`: `"bar"` or `"text"`.
    - `enabled` (bool, optional): when `false`, the asset stays hidden until enabled by config reload or UDP `asset_updates`. Defaults to `true`.
    - `id` (int, optional): unique asset identifier for UDP `asset_updates`. Defaults to the array index when omitted.
    - `value_index` (int): which numeric channel drives this asset (`0–7` for UDP `values[i]`, `8–15` for system values).
    - `text_index` (int, optional, bars/text): which text channel drives the descriptor (`0–7` from UDP `texts[i]`, `8–15` from the system text bank). `-1` or missing skips live text.
    - `text_indices` (array<int>, text only): render multiple UDP text entries; empty strings are skipped.
    - `text_inline` (bool, text only): when `true`, joins `text_indices` on a single line with spaces; otherwise stacks them on new lines. Default `false`.
    - `label` (string, optional, bars/text): static text descriptor. Used when no UDP text is present.
    - `orientation` (string, bars only): `"right"` (default) keeps the bar horizontal with the label to the right; `"left"` mirrors the layout with the label on the left and flips the fill so the bar grows from right-to-left. For `left`, the bar container anchors its right edge at `x` so left- and right-oriented bars can share the same coordinate and grow in opposite directions.
    - `x`, `y` (int): position relative to the OSD top-left. For `orientation: "left"`, `x` represents the right edge of the bar’s rounded container.
    - `width`, `height` (int): size in pixels. For text, enables wrapping.
    - `min`, `max` (float): input range mapped to 0–100% for bars.
    - `bar_color` (int): RGB hex value as a number; used by bar styles.
    - `rounded_outline` (bool, bars only): enables the outlined capsule look. Defaults to `false`.
    - `segments` (int, bars only): when greater than 1, divides the bar fill into that many evenly spaced blocks that extinguish one-by-one as the value drops (useful for battery-style indicators). Defaults to `0`/unset for a continuous fill.
    - `text_color` (int, optional): RGB hex value for labels/text content. Default white.
    - `background` (int, optional): index of a predefined palette of 11 background swatches (including a fully transparent entry and tinted fills). `-1` or omission keeps the default transparent look. For bars, the background is applied to a rounded container that extends across the bar and its label for a unified pill.
    - `background_opacity` (int, optional): percent opacity (0–100) to apply to the chosen background swatch. When omitted, the default palette opacity is used (0%, 50%, 50%, 70%, 90%, 60%, 60%, 60%, 70%, 60%, 70% by index as listed below).
    - Background palette indices:
      - `0`: transparent (0%)
      - `1`: black (defaults to 50%)
      - `2`: white (defaults to 50%)
      - `3`: charcoal (defaults to 70%)
      - `4`: charcoal dark (defaults to 90%)
      - `5`: blue (defaults to 60%)
      - `6`: teal (defaults to 60%)
      - `7`: green (defaults to 60%)
      - `8`: orange (defaults to 70%)
      - `9`: pink (defaults to 60%)
      - `10`: purple (defaults to 70%)

Example:
```json
{
  "width": 1280,
  "height": 720,
    "show_stats": true,
    "udp_stats": true,
  "assets": [
    { "type": "bar", "value_index": 0, "text_index": 0, "label": "BAR CH0", "x": 40, "y": 200, "width": 320, "height": 32, "min": 0.0, "max": 1.0, "orientation": "right", "segments": 8, "bar_color": 2254540, "text_color": 16777215, "background": 4, "background_opacity": 70 },
    { "type": "bar", "value_index": 1, "text_index": 1, "label": "BAR CH1", "x": 420, "y": 140, "width": 220, "height": 24, "min": 0.0, "max": 1.0, "orientation": "left", "bar_color": 2254540, "text_color": 0, "background": 2, "background_opacity": 60, "rounded_outline": true },
    { "type": "text", "text_indices": [2, 3, 4], "text_inline": false, "label": "Status", "x": 40, "y": 260, "width": 320, "height": 80, "background": 1, "background_opacity": 50, "text_color": 16777215 }
  ]
}
```
