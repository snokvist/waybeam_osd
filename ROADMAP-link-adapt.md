# Adaptive Link Health Agent — Roadmap

## Executive Summary

Add closed-loop link adaptation to the Waybeam vehicle stack: a control agent
that reads WiFi signal quality, video encoder metrics, ground-side packet
loss/jitter (via hub sync), and system health — then adjusts VENC bitrate/GOP
and WiFi TX power/MCS to maintain the best possible video quality for the
current link conditions.

---

## Where Should This Live?

### Candidates Evaluated

| Candidate | Pros | Cons | Verdict |
|---|---|---|---|
| **osd_send** | Has all metric sources wired up; watch loop ready | Read-only by design; OSD display helper, not a controller; adding actuation violates SRP; 99K lines already | **No** — sensor, not actuator |
| **waybeam_hub.c** | Has sync protocol for ground→vehicle feedback; poll-based daemon; action execution infra; already coordinates state | Already 3150 lines of menu/sync/WebUI; mixing link control into menu driver muddies responsibilities | **Partial** — good for carrying ground-side metrics via sync, wrong for the control loop itself |
| **New daemon: `link_adapt`** | Clean SRP; can be started/stopped/tuned independently; small focused codebase; can consume metrics from any source; testable in isolation | New process = new port or IPC; one more daemon to manage on resource-constrained SigmaStar | **Yes** — the control loop deserves its own process |

### Recommended Architecture

```
Vehicle (SigmaStar Infinity6E)
══════════════════════════════

 Metric sources (read-only)              Control targets (write)
 ─────────────────────────               ──────────────────────
 /proc/net/rtl88x2eu/<if>/rssi_a,b       iw dev <if> set txpower
 /proc/net/rtl88x2eu/<if>/rate_info*      iw dev <if> set bitrates
 wpa_cli STATUS / SIGNAL_POLL            /proc/net/rtl88x2eu/<if>/**
 majestic /metrics (venc_bitrate,        majestic /api/v1/set (bitrate)
   isp_fps, soc_temp, mem_used)          majestic /request/idr (keyframe)
 /proc/stat (cpu load)                   MI_VENC SDK (bitrate, GOP, IDR)**
 waybeam_hub sync (ground metrics)

        ┌─────────────────────┐
        │     link_adapt      │ ← new daemon
        │                     │
        │  ┌───────────────┐  │
        │  │ metric_reader  │  │  reads all sources above
        │  └───────┬───────┘  │
        │          ▼          │
        │  ┌───────────────┐  │
        │  │  state_machine │  │  adaptive decisions
        │  └───────┬───────┘  │
        │          ▼          │
        │  ┌───────────────┐  │
        │  │   actuator     │  │  writes to control targets
        │  └───────────────┘  │
        └─────────────────────┘

 ** MI_VENC SDK = direct SigmaStar API (mi_venc.h)
    Available in waybeam_osd/sdk/include/ already.
    Alternative: majestic HTTP API for bitrate/IDR if available.
```

### Why Not osd_send?

osd_send is a **sensor** — it observes and reports. link_adapt is an
**actuator** — it observes and controls. Mixing them creates a monolith where
a misconfigured control loop could break OSD display, and vice versa.

osd_send remains the canonical metric collector. link_adapt can:
1. Read the same sources directly (they're just /proc files and sockets), OR
2. Consume osd_send's merged key store via a small shared-memory or file interface

Option 1 is simpler and avoids coupling.

### Why a Separate Daemon Instead of Embedding in waybeam_hub?

waybeam_hub is a **menu driver and state coordinator**. It should carry
ground-side metrics across the sync channel (that's coordination), but the
actual PID-loop / state-machine for link adaptation is a different concern
with different failure modes and tuning cycles.

However: waybeam_hub.c is the **delivery mechanism** for ground-side feedback.
link_adapt receives ground metrics indirectly through waybeam_hub's sync
protocol — either via a small local UDP relay, a shared file, or by
link_adapt subscribing to the same sync socket.

---

## Control Interfaces Available

### Video Encoder (Majestic on SigmaStar)

| Interface | Method | What It Controls |
|---|---|---|
| `GET /metrics` | HTTP (read) | Prometheus counters: venc_bytes, isp_fps, soc_temp, etc. |
| `GET /request/idr` | HTTP (write) | Force IDR/keyframe |
| `GET /api/v1/set?video0.bitrate=N` | HTTP (write) | Runtime bitrate change (if majestic supports it) |
| `MI_VENC_SetRcParam()` | SDK API (write) | Direct rate-control params: CBR/VBR target, min/max QP |
| `MI_VENC_RequestIdr()` | SDK API (write) | Force IDR keyframe via kernel API |

**Preferred approach:** Use majestic HTTP API where available (simpler, no
library linking). Fall back to MI_VENC SDK for fine-grained control (GOP
structure, QP bounds) or if majestic doesn't expose the knob.

**Discovery task:** Enumerate majestic's full HTTP API on the target device.
`/api/v1/set` with `video0.bitrate`, `video0.gopSize`, `video0.gopMode`
are likely candidates based on OpenIPC majestic conventions.

### WiFi (RTL8812EU Softmac Driver)

| Interface | Method | What It Controls |
|---|---|---|
| `/proc/net/rtl88x2eu/<if>/rssi_a,b` | /proc (read) | Per-antenna RSSI |
| `wpa_cli SIGNAL_POLL` | socket (read) | RSSI, LINKSPEED, NOISE, FREQUENCY |
| `wpa_cli STATUS` | socket (read) | BSSID, freq, ssid, mode, state |
| `iw dev <if> set txpower fixed <mBm>` | shell (write) | TX power (100-2000 mBm typical) |
| `iw dev <if> set bitrates ht-mcs-5 <idx>` | shell (write) | Force HT MCS index |
| `/proc/net/rtl88x2eu/<if>/...` | /proc (write?) | Driver-specific knobs (needs enumeration) |

**Discovery task:** On a live device, enumerate all /proc/net/rtl88x2eu/<if>/
entries. Realtek softmac drivers often expose rate_adaptive, tx_power_idx,
and other knobs via /proc writes. This needs to be mapped on actual hardware.

**Important:** `iw` may not work with all softmac drivers. Some Realtek
drivers only respond to `iwpriv` or their /proc interface. This needs
validation on the target.

### Ground-Side Feedback (via waybeam_hub sync)

Currently the sync protocol carries menu/action state. Ground-side link
quality metrics need to be added:

| Metric | Source on Ground | How to Get It to Vehicle |
|---|---|---|
| packet_loss_rate | pixelpilot udp_receiver | Extend sync `state` message |
| jitter_ms | pixelpilot udp_receiver | Extend sync `state` message |
| bitrate_avg_mbps | pixelpilot udp_receiver | Extend sync `state` message |
| fps_avg | pixelpilot udp_receiver | Extend sync `state` message |
| incomplete_frames | pixelpilot udp_receiver | Extend sync `state` message |

These are already computed in pixelpilot's `udp_receiver.c` and published
via SSE:8080. waybeam_hub.py (ground) already consumes SSE. Adding them to
the sync `state` message is a small extension.

---

## State Machine Design

### Link Quality Levels

```
  EXCELLENT ──── GOOD ──── DEGRADED ──── POOR ──── CRITICAL
     │              │           │           │           │
  Max bitrate   High       Medium      Low         Minimum
  High MCS      Mid MCS    Low MCS     MCS 0/1     MCS 0
  Normal TX     Normal TX  +TX boost   Max TX      Max TX
  Long GOP      Long GOP   Short GOP   Short GOP   Min GOP + IDR burst
```

### Input Signals (weighted composite score)

| Signal | Weight | Source | Good | Bad |
|---|---|---|---|---|
| RSSI (local) | 25% | rtl8812eu /proc | > -50 dBm | < -75 dBm |
| Ground loss rate | 30% | hub sync | < 0.1% | > 5% |
| Ground jitter | 15% | hub sync | < 5 ms | > 50 ms |
| Ground FPS match | 10% | hub sync | fps_avg ≈ isp_fps | fps_avg < 0.8×isp_fps |
| SoC temperature | 10% | venc /metrics | < 70°C | > 85°C |
| CPU load | 10% | /proc/stat | < 80% | > 95% |

### Output Actions

| Level | VENC Bitrate | GOP Size | IDR | WiFi MCS | WiFi TX Power |
|---|---|---|---|---|---|
| EXCELLENT | config max | 60-120 frames | normal | auto/high | default |
| GOOD | 80-100% max | 60 frames | normal | auto/mid | default |
| DEGRADED | 50-80% max | 30 frames | on transition | force mid | +3 dB |
| POOR | 25-50% max | 15 frames | burst on entry | force low | +6 dB |
| CRITICAL | config min | 5-10 frames | continuous | MCS 0 | maximum |

### Hysteresis and Damping

**Critical to get right.** Without hysteresis the system oscillates:

1. **Promotion delay:** Must sustain better-level metrics for N seconds
   (configurable, default 5s) before stepping up.
2. **Demotion speed:** Step down immediately on threshold breach (safety).
3. **Rate limiting:** Maximum one level change per 2 seconds.
4. **Coordinated transitions:** When stepping down:
   - First reduce VENC bitrate (takes effect within 1 GOP)
   - Wait 500ms for reduced WiFi load
   - Then adjust WiFi MCS/TX if still needed
   When stepping up: reverse order (WiFi first, then VENC).
5. **Dead zone:** ±5% around thresholds to avoid flickering.

### Transition Coordination (The Hard Part)

Changing bitrate and WiFi settings simultaneously can cause a transient
where the new bitrate exceeds the new WiFi capacity, or the WiFi rate
drops before the encoder has adapted:

```
STEP DOWN sequence:
  1. Request IDR (clean recovery point)
  2. Lower VENC bitrate
  3. Wait for bitrate to take effect (1 GOP period)
  4. Lower WiFi MCS / raise TX power
  5. Confirm ground-side metrics stabilize

STEP UP sequence:
  1. Raise WiFi MCS / lower TX power
  2. Wait for WiFi link to stabilize (2-3 seconds)
  3. Confirm ground-side loss rate is still low
  4. Raise VENC bitrate
  5. Confirm ground-side FPS and loss are stable
```

### Failsafe

- If sync channel is lost (no ground metrics for 10s): assume DEGRADED,
  rely on local RSSI only.
- If majestic HTTP is unreachable: log warning, skip VENC adjustments,
  WiFi-only adaptation.
- If WiFi control fails (iw returns error): log warning, skip WiFi
  adjustments, VENC-only adaptation.
- On daemon crash/restart: start at GOOD level, re-evaluate within 2
  poll cycles.

---

## Implementation Phases

### Phase 0: Discovery and Enumeration (prerequisite)

**On actual hardware**, enumerate:

- [ ] All `/proc/net/rtl88x2eu/<if>/` entries (which are writable?)
- [ ] `iw dev <if> set txpower` — does it work with rtl88x2eu?
- [ ] `iw dev <if> set bitrates` — does MCS forcing work?
- [ ] `iwpriv <if>` — any driver-specific extensions?
- [ ] Majestic HTTP API: `curl http://127.0.0.1/api/v1` — what endpoints exist?
- [ ] Majestic runtime bitrate change: does it apply without restart?
- [ ] MI_VENC_SetRcParam / MI_VENC_RequestIdr — can we call these while
      majestic owns the encoder, or do they conflict?

**This phase determines what's actually possible.** The rest of the roadmap
adapts to what the hardware/drivers actually support.

### Phase 1: Metric Plumbing

**Goal:** All required metrics available to a single process on the vehicle.

1. **Extend waybeam_hub sync protocol** to carry ground-side link quality:
   - Add `link_quality` object to sync `state` messages
   - waybeam_hub.py reads pixelpilot SSE stats, includes in sync pushes
   - waybeam_hub.c receives and caches ground metrics
   - Expose via `/state` endpoint and optionally a local file/socket

2. **Expose ground metrics locally** for link_adapt to consume:
   - Option A: waybeam_hub.c writes a small JSON file (`/tmp/ground_link.json`)
     updated on each sync receive — simple, no IPC complexity
   - Option B: waybeam_hub.c relays to a local UDP port — more real-time
   - Option A recommended for simplicity (link_adapt polls at 1-2 Hz anyway)

3. **Validate osd_send sources work** for local metrics:
   - RTL8812EU RSSI via /proc
   - wpa_cli SIGNAL_POLL for LINKSPEED/NOISE
   - VENC metrics via majestic /metrics
   - CPU/temp via /proc

### Phase 2: link_adapt Daemon (Read-Only Mode)

**Goal:** Daemon runs, reads all metrics, computes link quality level,
logs decisions — but does NOT actuate anything.

- Single C file, poll-based (like osd_send architecture)
- Config file for thresholds, weights, timing parameters
- JSON config for control target addresses/paths
- Reads: /proc, wpa_cli, majestic /metrics, ground_link.json
- Computes: composite score, current level, recommended actions
- Outputs: structured log, optional OSD text update (via UDP:7777)
- **No writes to any control target**

This lets you validate the decision logic by watching logs during real
flights before enabling actuation.

### Phase 3: VENC Control

**Goal:** link_adapt adjusts encoder bitrate and triggers IDR.

- Add HTTP client for majestic API (reuse osd_send's HTTP fetch pattern)
- Implement bitrate stepping with configurable min/max/steps
- Implement IDR request on level transitions
- GOP adjustment via majestic API or MI_VENC SDK
- Coordinated: IDR → bitrate change → wait → verify

### Phase 4: WiFi Control

**Goal:** link_adapt adjusts TX power and MCS rate.

- Implement `iw` / /proc control (based on Phase 0 findings)
- TX power stepping with configurable range
- MCS rate forcing with configurable floor
- Coordinated with VENC changes (sequence matters)

### Phase 5: Closed Loop

**Goal:** Full bidirectional adaptation with ground-side feedback.

- Ground metrics drive vehicle-side decisions
- Verify hysteresis prevents oscillation
- Tune timing parameters based on real-world testing
- Add OSD visualization of current link state/level
- Add WebUI display of adaptation state via waybeam_hub

---

## Configuration (Proposed)

```json
{
  "enabled": true,
  "poll_interval_ms": 500,
  "log_level": "info",

  "sources": {
    "rtl8812eu": { "iface": "wlan0" },
    "wpa_cli": { "iface": "wlan0" },
    "venc": { "url": "http://127.0.0.1/metrics" },
    "cpu": true,
    "ground_link": { "path": "/tmp/ground_link.json" }
  },

  "thresholds": {
    "rssi_good": -55,
    "rssi_degraded": -65,
    "rssi_poor": -75,
    "rssi_critical": -82,
    "loss_good": 0.5,
    "loss_degraded": 2.0,
    "loss_poor": 5.0,
    "loss_critical": 15.0,
    "temp_warning": 75,
    "temp_critical": 85
  },

  "timing": {
    "promote_delay_s": 5,
    "demote_delay_s": 0,
    "min_level_hold_s": 2,
    "venc_settle_ms": 500,
    "wifi_settle_ms": 2000,
    "ground_stale_s": 10
  },

  "venc": {
    "control": "http",
    "url": "http://127.0.0.1",
    "bitrate_max_kbps": 8000,
    "bitrate_min_kbps": 1000,
    "bitrate_steps": [8000, 6000, 4000, 2500, 1000],
    "gop_steps": [120, 60, 30, 15, 5],
    "idr_on_transition": true
  },

  "wifi": {
    "control": "iw",
    "iface": "wlan0",
    "txpower_default_mbm": 1500,
    "txpower_max_mbm": 2000,
    "txpower_steps": [1500, 1700, 1800, 2000, 2000],
    "mcs_steps": ["auto", "auto", "3", "1", "0"]
  },

  "osd": {
    "enabled": true,
    "dest": "127.0.0.1",
    "port": 7777,
    "text_index": 7,
    "value_index": 7
  }
}
```

---

## Traffic Impact

link_adapt adds **zero WiFi packets**. All its reads and writes are local:

| Operation | Transport | Cross-link? |
|---|---|---|
| Read RSSI | /proc file read | No |
| Read wpa_cli | AF_UNIX socket | No |
| Read majestic /metrics | localhost HTTP | No |
| Read ground_link.json | File read | No (hub sync carries it) |
| Set VENC bitrate | localhost HTTP | No |
| Request IDR | localhost HTTP | No |
| Set WiFi TX/MCS | iw / /proc write | No |
| Update OSD | localhost UDP:7777 | No |

The only cross-link addition is the ground metrics in waybeam_hub sync,
which piggybacks on existing sync state messages (~50 extra bytes, no
extra packets).

---

## Risk Assessment

| Risk | Mitigation |
|---|---|
| Oscillation between levels | Hysteresis (promote delay + dead zone) |
| Transient quality drop during transition | Coordinated sequence (IDR first, then changes) |
| Majestic doesn't support runtime bitrate | Phase 0 discovery; fall back to MI_VENC SDK |
| RTL8812EU doesn't support iw txpower | Phase 0 discovery; WiFi-only if possible, VENC-only otherwise |
| CPU overhead on SigmaStar (limited cores) | 1-2 Hz poll, minimal processing; lighter than osd_send |
| link_adapt crash takes down video | Separate process; crash = no adaptation = status quo encoding |
| Ground sync lost | Failsafe to local-RSSI-only mode at DEGRADED level |

---

## Open Questions

1. **Majestic API surface:** What HTTP endpoints does majestic expose for
   runtime encoder control? Need hardware access to enumerate.

2. **RTL8812EU /proc writes:** Which /proc entries accept writes? Need
   `ls -la /proc/net/rtl88x2eu/<if>/` on a live device.

3. **MI_VENC coexistence:** Can link_adapt call MI_VENC SDK functions
   while majestic owns the encoder channel? Or must it go through majestic?

4. **MCS forcing stability:** Does forcing MCS via iw cause the driver's
   rate adaptation to fight back? May need to disable driver-level rate
   adaptation when link_adapt is active.

5. **GOP change mechanism:** Does majestic support runtime GOP changes,
   or only at stream start? If only at start, GOP adaptation may require
   a stream restart (unacceptable latency) — in which case, rely on IDR
   bursts instead of GOP shortening.

6. **Init system integration:** Should link_adapt be a separate init.d
   service, or launched by waybeam_hub.c as a child process? Separate
   service is cleaner for start/stop/restart.
