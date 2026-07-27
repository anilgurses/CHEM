# Sionna RT Extension

CHEM integrates with the [AERPAW Sionna RT Extension](https://github.com/AERPAW/AERPAW-DT-SIONNA-EXTENSION) to replace its built in propagation models with GPU accelerated ray tracing powered by [Sionna RT](https://nvlabs.github.io/sionna/).

Sionna is a built in [CHEM extension](extensions.md). It declares `bypassesPathLoss() = true`, meaning it replaces CHEM's statistical propagation models (FSPL, Two Ray, 3GPP, etc.) with ray traced channel impulse responses. The propagation loss is encoded directly in the CIR taps computed by the ray tracer.

![CHEM Architecture with Sionna RT Extension](assets/ACHEM_ARCH_Detail_w_sionna.png)
*CHEM internal architecture with the Sionna RT extension. The Sionna server performs GPU accelerated ray tracing over 3D terrain and buildings, replacing CHEM's built in statistical propagation models with ray traced channel impulse responses.*

## How It Works

When Sionna is enabled, CHEM runs a background thread that continuously synchronizes node positions and channel impulse responses between CHEM and the Sionna RT server.

### Startup

1. CHEM reads the `extensions.sionna` block from `config.json`.
2. If `enabled` is `true`, the extension is started automatically at startup. It can also be started at runtime via the `EXT` command or PyCHEM.
3. The extension creates a scene on the Sionna server via `POST /scenes`, providing the `referenceOrigin` (GPS lat/lon/alt) that the server uses as the local coordinate system origin.
4. Scene creation is retried every 2 seconds until the server responds.

### Poll Loop

The client runs a poll loop at the configured `updateRateMs` interval (minimum 50ms):

**1. Position Sync** (`syncPositions`):

- Iterates over all CHEM nodes that have reported locations.
- Each node is registered on the Sionna server as both a transmitter (`{name}-tx`) and a receiver (`{name}-rx`). Sionna requires unique names across TX and RX within a scene.
- Position updates are cached; only nodes whose coordinates have changed since the last sync are updated via `PUT`.

**2. CIR Computation** (`computeAndApplyCIR`):

This step is skipped if any of the following are true:

- No scene has been created yet.
- Fewer than 1 TX or 1 RX is registered on the Sionna server.
- No `Intermediate` has active channel links (i.e., no two nodes share a frequency).

When active:

- Requests the server to compute propagation paths via `POST /scenes/{id}/simulation/paths` with the configured `maxDepth` and `numSamples` ray samples.
- Fetches the resulting Channel Impulse Response via `GET /scenes/{id}/simulation/cir`.
- Parses the multi dimensional CIR arrays (delays and complex gains) for each TX, RX pair.
- Converts per path delays and gains into FIR filter taps via `cirToTaps()`, capped at `cir_max_taps`.
- Applies the taps to the corresponding `Channel` in each `Intermediate` via `updateCIR()`.
- Self links (same physical node as both TX and RX) are automatically skipped.

When Sionna CIR is active on a channel, the statistical propagation models are not evaluated. The ray traced CIR taps provide the propagation loss directly.

## Link Budget Interaction

Sionna RT's `paths.cir()` returns complex baseband path coefficients $a_i$ whose squared magnitudes already encode the propagation loss for each ray (FSPL plus reflection / scattering / transmission losses for non LoS paths). The transmitter's `power_dbm` does not scale these coefficients in Sionna RT, so CHEM keeps the Sionna side normalized (`signal_power = 0`) and applies device level gains locally inside `Channel::processChannel()`.

To keep this consistent with CHEM's standard link budget, `processChannel()` derives the loss the convolution will introduce from the tap energy:

$$
L_\text{cir} = 10 \log_{10} \sum_i \lvert a_i \rvert^2 \quad [\text{dB}]
$$

When an extension sets `bypassesPathLoss() = true` and supplies CIR taps, that $L_\text{cir}$ feeds back into the link budget so the receive power is

$$
P_\text{rx} = P_\text{tx} + L_\text{cir} - L_\text{stat} + G_\text{rx} - L_\text{rx,cable}
$$

where $L_\text{stat}$ is the statistical model output (zero in bypass mode) and $L_\text{cir}$ is negative because it is a loss. The post CIR signal is then scaled by $P_\text{rx} - (s_\text{ref} + L_\text{cir})$ in dB so the device gains apply on top of the propagation loss instead of replacing it. Without this adjustment the link budget would compute $P_\text{rx}$ as if no propagation loss happened, hit the 0 dBFS saturation clamp, and silently strip the device gains, producing a low SNR at the receiver.

The same handling applies to any extension in bypass mode: taps with unit total energy ($L_\text{cir} = 0$ dB) are equivalent to no taps for link budget purposes, while taps that carry propagation loss are accounted for automatically.

## CIR Data Format

The Sionna server returns CIR data with the following structure:

| Field | Shape | Description |
|---|---|---|
| `delays` | `[num_rx][num_tx][num_paths]` | Per path propagation delays in seconds |
| `gains.real` | `[num_rx][num_rx_ant][num_tx][num_tx_ant][num_paths][num_time_steps]` | Real part of complex path gains |
| `gains.imag` | same as `gains.real` | Imaginary part of complex path gains |
| `shape` | object | Dimension metadata (`num_rx`, `num_tx`, `num_paths`, etc.) |

### CIR to FIR Tap Conversion

The `cirToTaps()` function converts the raw CIR (per path delays and complex gains) into a discrete FIR tap vector suitable for convolution with IQ samples:

1. Each path delay is quantized to the nearest sample index at the channel's sample rate.
2. Complex gains for paths that map to the same tap index are accumulated (summed).
3. The resulting tap vector length equals `max_tap_index + 1`, capped by `cir_max_taps` (default 64).

## Configuration

Add the `sionna` block under `extensions` in your CHEM `config.json`:

```json
{
  "extensions": {
    "sionna": {
      "enabled": true,
      "serverUrl": "http://192.168.8.173:8000",
      "updateRateMs": 500,
      "maxDepth": 3,
      "numSamples": 100000,
      "sceneConfig": "aerpaw",
      "referenceOrigin": {
        "lat": 35.72750947,
        "lon": -78.69595819,
        "alt": 112.0
      },
      "sceneOffset": {
        "x": 118.1,
        "y": -123.4,
        "z": 0.0
      },
      "scale": 1.0
    }
  }
}
```

| Key | Type | Default | Description |
|---|---|---|---|
| `enabled` | boolean | `false` | Enable automatic Sionna client startup |
| `serverUrl` | string | `"http://localhost:8000"` | URL of the Sionna RT server |
| `updateRateMs` | integer | `500` | Poll interval in milliseconds (minimum 50) |
| `maxDepth` | integer | `3` | Maximum ray tracing depth (reflections/diffractions) |
| `numSamples` | integer | `100000` | Number of ray samples per path computation (minimum 1000) |
| `sceneConfig` | string | `"aerpaw"` | Named server scene preset |
| `referenceOrigin.lat` | float | `35.72750947` | Latitude of the scene coordinate origin |
| `referenceOrigin.lon` | float | `-78.69595819` | Longitude of the scene coordinate origin |
| `referenceOrigin.alt` | float | `112.0` | Ellipsoidal height (HAE) of ground level; scene `z=0` is this ground |
| `sceneOffset.x` / `.y` / `.z` | float | `118.1` / `-123.4` / `0.0` | Meters added after ENU projection: `scene_xyz = ENU_meters(origin) * scale + offset` |
| `scale` | float | `1.0` | Scale factor (1.0 because the aerpaw scene is in meters) |

`scene_origin`, `scene_offset`, and `scale` are sent explicitly on `POST /scenes`; when they differ from the `sceneConfig` preset they take precedence over it. After creation the client calls `GET /scenes/{id}` and surfaces the echoed `scene_config`, `scene_path`, `offset`, `scale`, and `units` in the extension status (`sceneAlignment`) to confirm alignment.

> **Altitude datum.** Transmitter/receiver positions in add/update calls stay `{lat, lon, alt}`, but `alt` must be in the same HAE datum as `scene_origin.alt` (ground = `112.0 m`). CHEM node altitudes come from the vehicle's `relative_altitude_m` (AGL, height above the takeoff/home point), so the client adds `scene_origin.alt` before sending: a device on the ground (AGL 0) is sent as `alt ≈ 112`, a drone 35 m up as `alt ≈ 147`.

## Sionna Server Setup

The Sionna RT Extension runs as a Docker container with GPU access:

```bash
git clone https://github.com/AERPAW/AERPAW-DT-SIONNA-EXTENSION.git
cd AERPAW-DT-SIONNA-EXTENSION
docker compose up --build
```

**Requirements:**

- NVIDIA GPU with CUDA support
- Docker with `nvidia-container-toolkit` installed
- The server listens on port 8000 by default

### Server API Endpoints

| Method | Endpoint | Description |
|---|---|---|
| `POST` | `/scenes` | Create a new scene with `scene_config`, `scene_origin`, `scene_offset`, `scale` |
| `GET` | `/scenes/{id}` | Get scene info (echoes `scene_config`, `scene_path`, `offset`, `scale`, `units`) |
| `POST` | `/scenes/{id}/transmitters` | Register a transmitter (name, position, power) |
| `POST` | `/scenes/{id}/receivers` | Register a receiver (name, position) |
| `PUT` | `/scenes/{id}/transmitters/{name}` | Update transmitter position |
| `PUT` | `/scenes/{id}/receivers/{name}` | Update receiver position |
| `POST` | `/scenes/{id}/simulation/paths` | Compute propagation paths (max_depth, num_samples) |
| `GET` | `/scenes/{id}/simulation/cir` | Retrieve the computed CIR |

## Runtime control

Sionna supports the following actions via the `EXT` command:

| Action | Description |
|---|---|
| `start` | Start the extension with the provided config params |
| `stop` | Stop the extension and clear CIR state |
| `status` | Return current running state, server URL, scene ID |
| `config` | Update `updateRateMs`, `maxDepth`, `numSamples`, `sceneConfig`, `referenceOrigin`, `sceneOffset`, and/or `scale` at runtime (offset/scale/config apply to the next scene creation; origin changes are pushed via `update_origin`) |

Example:

```json
{"CMD": "EXT", "extension": "sionna", "action": "start", "params": {
    "serverUrl": "http://192.168.8.173:8000",
    "sceneConfig": "aerpaw",
    "referenceOrigin": {"lat": 35.72750947, "lon": -78.69595819, "alt": 112.0},
    "sceneOffset": {"x": 118.1, "y": -123.4, "z": 0.0},
    "scale": 1.0,
    "updateRateMs": 200, "maxDepth": 3, "numSamples": 100000
}}
```

## Source files

| File | Description |
|---|---|
| `extensions/sionna/include/sionna_client.h` | `SionnaExtension` class declaration |
| `extensions/sionna/src/sionna_client.cpp` | Extension implementation (HTTP, sync, CIR parsing) |
| `include/chem/extensions/extension.h` | `ChannelExtension` base class |
| `src/chem/channel/coordinator.cpp` | Extension registration and `EXT`/`EXT_LIST` command routing |
