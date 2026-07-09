# Performance Measurement

The plugin should be measured as extra work on top of OBS, not as a replacement for the built-in OBS preview.

OBS already renders the Program output. The built-in preview mostly displays that rendered output locally. This plugin adds more work:

- OBS raw video callback and scaling/conversion into BGR.
- A CPU-side frame copy into plugin-owned memory.
- JPEG encoding through Windows Imaging Component.
- HTTP/MJPEG socket work for every connected client.

This work is demand-driven: with no MJPEG stream clients and no snapshot request waiting for a fresh frame, the OBS raw video callback is not registered. While clients are connected, the plugin is expected to use more CPU than just leaving the built-in preview visible, especially at higher resolution, higher FPS, higher JPEG quality, or with multiple clients.

## What to Compare

Use the same scene collection, OBS canvas/output settings, visible windows, and idle time for every run. Do not stream or record unless that is part of the scenario being measured.

Recommended phases:

1. `baseline-preview-on`: OBS open, built-in preview enabled, LAN Preview disabled.
2. `baseline-preview-off`: OBS open, built-in preview disabled, LAN Preview disabled.
3. `plugin-no-client`: LAN Preview enabled, no browser connected.
4. `plugin-1-client`: LAN Preview enabled, one MJPEG client connected.
5. `plugin-4-clients`: LAN Preview enabled, four MJPEG clients connected.

The two baseline runs estimate the cost of OBS itself and the local preview. The plugin runs show the incremental cost of capture, encoding, and serving clients.

## Run the Script

Start OBS first, then run PowerShell from the repository root.

```powershell
.\scripts\measure-obs-performance.ps1 -Label baseline-preview-on -DurationSeconds 120
```

For LAN Preview enabled but no clients:

```powershell
.\scripts\measure-obs-performance.ps1 -Label plugin-no-client -DurationSeconds 120
```

For one local MJPEG client:

```powershell
.\scripts\measure-obs-performance.ps1 -Label plugin-1-client -DurationSeconds 120 -Clients 1
```

For four local MJPEG clients:

```powershell
.\scripts\measure-obs-performance.ps1 -Label plugin-4-clients -DurationSeconds 120 -Clients 4
```

If OBS is running under a different process name, pass `-ProcessName`.

```powershell
.\scripts\measure-obs-performance.ps1 -ProcessName obs64 -Label plugin-1-client -Clients 1
```

The script writes:

- `measurements/*-samples.csv` with per-interval process samples.
- `measurements/*-summary.json` with averages, p95, max, MJPEG client receive rate, and `/health` snapshots when available.

CPU is reported in two forms:

- `cpu_machine_pct`: percentage of the whole machine CPU capacity.
- `cpu_one_core_pct`: percentage of one logical CPU core.

For example, `100%` one-core CPU on an 8-logical-processor machine is `12.5%` machine CPU.

The `/health` endpoint also exposes plugin-side counters that help explain process-level deltas:

- `streamClients` and `snapshotWaiters`
- `frameDemand`
- `submittedFrames`, `encodedFrames`, and `droppedFrames`
- `rawBuffersAllocated` and `rawBuffersReused`
- `latestJpegBytes`
- `avgEncodeMs` and `maxEncodeMs`

The measurement script stores `health_before`, `health_after`, and `health_delta` in the summary JSON. On a no-client run, `health_delta.encodedFrames` should stay at `0`.

## Reading the Results

Use deltas, not isolated numbers.

```text
Built-in preview cost ~= baseline-preview-on - baseline-preview-off
Plugin idle cost      ~= plugin-no-client - baseline-preview-on
Plugin client cost    ~= plugin-1-client - baseline-preview-on
Extra client cost     ~= plugin-4-clients - plugin-1-client
```

For default settings (`640x360`, `5 FPS`, JPEG quality `70`), the expected release target is low single-digit average CPU on a typical desktop with one client. If the average delta is much higher, inspect JPEG encode time, capture resolution, FPS, and client count first.

## Settings That Matter Most

- FPS usually scales CPU almost linearly because every frame is encoded.
- Resolution scales by pixel count: `1280x720` is 4x the pixels of `640x360`.
- JPEG quality increases encode work and network bandwidth.
- More clients mostly increase socket send work and bandwidth; encoding is shared because the server keeps one latest JPEG frame.
- With zero clients, the current implementation keeps only the HTTP listener active and does not register the OBS raw video callback.
