# OSD Data Contract

## Runtime UDP payload (port 7777)
- Each datagram must be UTF-8 JSON with a top-level `values` array.
- `values` holds up to 8 numeric entries (float/double). Missing entries default to `0` on the device.
- Extra fields are ignored so senders can add metadata if needed.
- Keep payloads under 512 bytes (anything larger is dropped).
- The UDP socket is drained whenever it becomes readable so only the latest datagram drives the screen; older queued packets are discarded. Sending faster than the configured `refresh_ms` cap will still work, but packets may be coalesced when idle.
- Optional `texts` array (up to 8 strings, max 16 chars each) can be sent alongside `values`. These map to `text_index` on bar assets and override a static `label` if present. Missing or empty entries fall back to the asset’s `label`.

Example:
```json
{
  "values":[0.12,0.5,1.0,0,0,0,0,0],
  "texts":["BAR CH0","BAR CH1","SCALE CH2","CH3","CH4","CH5","CH6","CH7"],
  "timestamp_ms":1712345678
}
```

Each on-screen asset binds to one `values[i]` entry via `value_index`. For bar assets, `text_index` maps the descriptor to `texts[i]`; otherwise the bar uses the optional static `label`. Scale assets ignore `text_index`/`label`. When `udp_stats` is enabled, the stats overlay also lists all 8 numeric values and text entries vertically.

## Local config file (`config.json`)
- JSON file read at startup; missing keys fall back to defaults.
- Top-level fields:
  - `width`, `height` (int): OSD canvas resolution. Default 1280x720.
  - `show_stats` (bool): show/hide the top-left stats overlay. Default `true`.
  - `udp_stats` (bool): when `true`, the stats overlay also lists the latest 8 numeric values and text channels. Default `true`.
  - `refresh_ms` (int): maximum idle wait between UDP polls and screen refreshes in milliseconds (clamped 10–1000); default 100 ms.
  - `assets` (array, max 8): list of objects defining what to render and which UDP value to consume.
- Asset fields:
  - `type`: `"bar"`, `"example_bar_2"`, `"example_scale_10"`, or `"lottie"`.
  - `value_index` (int): which UDP `values[i]` drives this asset (0–7).
  - `text_index` (int, optional, bars and lottie): which UDP `texts[i]` drives the descriptor (0–7). `-1` or missing skips UDP text.
  - `label` (string, optional, bars and lottie): static text descriptor. Used when no `text_index` or when the mapped UDP text is empty.
  - `file` (string, lottie only): path to a readable `.json` Lottie animation on disk. When missing or unreadable, the device falls back to a locally rendered embedded sample animation baked into `main.c`.
  - `x`, `y` (int): position relative to the OSD top-left.
  - `width`, `height` (int): size in pixels (optional for the scale; defaults to 200x200). For lottie, controls the animation canvas.
  - `min`, `max` (float): input range mapped to 0–100% for bars or to the scale range.
  - `color` (int): RGB hex value as a number; used by bar styles.

Example:
```json
{
  "width": 1280,
  "height": 720,
  "show_stats": true,
  "udp_stats": true,
  "assets": [
    { "type": "bar", "value_index": 0, "text_index": 0, "label": "BAR CH0", "x": 40, "y": 200, "width": 320, "height": 32, "min": 0.0, "max": 1.0, "color": 2254540 },
    { "type": "example_bar_2", "value_index": 1, "text_index": 1, "label": "BAR CH1", "x": 420, "y": 140, "width": 220, "height": 24, "min": 0.0, "max": 1.0, "color": 2254540 },
    { "type": "example_scale_10", "value_index": 2, "x": 760, "y": 260, "width": 200, "height": 200, "min": 98.0, "max": 195.0 },
    { "type": "lottie", "file": "/customer/animation.json", "label": "Lottie", "text_index": 2, "x": 1040, "y": 80, "width": 160, "height": 160 }
  ]
}
```
