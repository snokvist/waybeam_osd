# Waybeam OSD

A lightweight, UDP-driven On-Screen Display (OSD) for Sigmastar Infinity6e devices (e.g., OpenIPC cameras).

## Features

- **High Performance:** Uses specialized hardware regions (RGN) and VPE for zero-copy overlay.
- **Low Memory Footprint:** Custom optimized PNG decoder (`lodepng_opt.c`) reduces peak memory usage by decoding directly into the display buffer, saving ~50% RAM compared to standard decoding.
- **Dynamic Updates:** Assets can be updated in real-time via UDP JSON packets.
- **Asset Types:**
    - **Bar:** Horizontal progress bars with rounded corners, segments, and dynamic coloring.
    - **Text:** Text labels with background styles and dynamic updates.
    - **Image:** PNG images overlay.

## Configuration

Configuration is loaded from `/etc/waybeam_osd.json`.

```json
{
  "width": 1280,
  "height": 720,
  "osd_x": 0,
  "osd_y": 0,
  "idle_ms": 100,
  "assets": [
    {
      "type": "bar",
      "id": 0,
      "enabled": true,
      "x": 50,
      "y": 50,
      "width": 300,
      "height": 30,
      "color": 0xFF0000,
      "min": 0,
      "max": 100
    },
    {
      "type": "image",
      "image_path": "/usr/share/logo.png",
      "x": 500,
      "y": 50
    }
  ]
}
```

## UDP Protocol

Send JSON packets to port 7777:

```json
{
  "values": [50, 75, 100, ...],  // Updates value_index 0, 1, 2...
  "texts": ["Hello", "World"],   // Updates text_index 0, 1...
  "asset_updates": [             // Reconfigure specific assets
    { "id": 0, "color": 0x00FF00 }
  ]
}
```

## Building

Requires the Sigmastar SDK toolchain.

```bash
make
```
