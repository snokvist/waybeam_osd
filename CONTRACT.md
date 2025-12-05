# OSD Data Contract

## Runtime UDP payload (port 7777)
- Each datagram must be UTF-8 JSON with a top-level `values` array.
- `values` holds up to 8 numeric entries (float/double). Missing entries default to `0` on the device.
- Extra fields are ignored so senders can add metadata if needed.
- Keep payloads under 512 bytes (anything larger is dropped).
- The UDP socket is drained whenever it becomes readable so only the latest datagram drives the screen; older queued packets are discarded. Incoming packets trigger an immediate refresh when received, while `idle_ms` only caps the sleep when no data arrives.
- Optional `texts` array (up to 8 strings, max 16 chars each) can be sent alongside `values`. These map to `text_index` on bar assets and override a static `label` if present. Missing or empty entries fall back to the asset’s `label`.
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

Each on-screen asset binds to one `values[i]` entry via `value_index`. For bar assets, `text_index` maps the descriptor to `texts[i]`; otherwise the bar uses the optional static `label`. When `udp_stats` is enabled, the stats overlay also lists all 8 numeric values and text entries vertically.

## Local config file (`config.json`)
- JSON file read at startup; missing keys fall back to defaults. Send `SIGHUP` to the running process to reload the file without restarting (asset layout, stats toggle, and `idle_ms` update in-place; resolution still follows the startup config).
- Top-level fields:
  - `width`, `height` (int): OSD canvas resolution. Default 1280x720.
  - `show_stats` (bool): show/hide the top-left stats overlay. Default `true`.
  - `udp_stats` (bool): when `true`, the stats overlay also lists the latest 8 numeric values and text channels. Default `false`.
  - `idle_ms` (int): maximum idle wait between UDP polls and screen refreshes in milliseconds (clamped 10–1000); default 100 ms. Legacy configs may still specify `refresh_ms`, which is treated the same way for compatibility.
  - `assets` (array, max 8): list of objects defining what to render and which UDP value to consume.
  - Asset fields:
    - `type`: `"bar"` or `"text"`.
    - `enabled` (bool, optional): when `false`, the asset stays hidden until enabled by config reload or UDP `asset_updates`. Defaults to `true`.
    - `id` (int, optional): unique asset identifier for UDP `asset_updates`. Defaults to the array index when omitted.
    - `value_index` (int): which UDP `values[i]` drives this asset (0–7).
    - `text_index` (int, optional, bars/text): which UDP `texts[i]` drives the descriptor (0–7). `-1` or missing skips UDP text.
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
  "udp_stats": false,
  "assets": [
    { "type": "bar", "value_index": 0, "text_index": 0, "label": "BAR CH0", "x": 40, "y": 200, "width": 320, "height": 32, "min": 0.0, "max": 1.0, "orientation": "right", "segments": 8, "bar_color": 2254540, "text_color": 16777215, "background": 4, "background_opacity": 70 },
    { "type": "bar", "value_index": 1, "text_index": 1, "label": "BAR CH1", "x": 420, "y": 140, "width": 220, "height": 24, "min": 0.0, "max": 1.0, "orientation": "left", "bar_color": 2254540, "text_color": 0, "background": 2, "background_opacity": 60, "rounded_outline": true },
    { "type": "text", "text_indices": [2, 3, 4], "text_inline": false, "label": "Status", "x": 40, "y": 260, "width": 320, "height": 80, "background": 1, "background_opacity": 50, "text_color": 16777215 }
  ]
}
```
