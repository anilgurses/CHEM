# PyCHEM

PyCHEM is the Python-facing layer around CHEM, intended for scripting, experiments, and driving CHEM from higher-level workflows. It comes pre-installed with CHEM and VUSRP.

## What it's for

- Interactively inspecting and tuning the running emulator (TUI)
- Programmatic control and orchestration (Python client)
- Rapid experiment iteration (configs, scenarios, automation)

![PyCHEM](img/pychem_demo.gif)

## Quick start

PyCHEM connects to the CHEM coordinator over TCP (default `localhost:5000`, see "Address resolution").

```bash
# Launch the TUI (default)
pychem

# Or run it from a source checkout
python3 utils/pychem.py
```

If your CHEM coordinator is on another host/port:

```bash
pychem --addr 192.0.2.10 --port 5000
```

## CLI options

PyCHEM is a single executable (`pychem`) with a small number of CLI flags:

| Option | Default | Meaning |
|---|---:|---|
| `--addr` / `--chem_addr` | auto | CHEM server address (see "Address resolution") |
| `--port` | `5000` | CHEM coordinator port |
| `--tui` | `false` | Launch TUI (currently the default either way) |
| `--version` | `false` | Print CHEM version and exit |

### Address resolution

If you don't pass `--addr`, PyCHEM resolves the CHEM address in this order:

1. `~/.config/uhd/conf.yaml` (key: `chem_address`)
2. Environment variable `AP_EXPENV_CHEMVM_XE`
3. Fallback: `localhost`

Example `~/.config/uhd/conf.yaml`:

```yaml
chem_address: 192.0.2.10
```

If you want PyCHEM to always use `AP_EXPENV_CHEMVM_XE`, set:

```yaml
chem_address: ENV
```

## Using the TUI

The TUI is intended for "real-time" inspection and tuning while CHEM is running.

### Global keys

- `↑/↓` or `k/j`: move selection
- `Enter`: open the selected menu / run the selected action
- `q` or `Esc`: go back / quit (from main menu)
- `r`: refresh (in views that support it)

### Main menu

Pick one of:

- `Nodes`: inspect nodes and change antenna patterns
- `Channels`: inspect per-frequency channel state, tune links, adjust channel impairments
- `Profiles`: apply or save experiment profiles
- `Extensions`: manage extensions (Sionna RT, etc.)
- `Status`: CHEM's runtime status (CPU/memory, uptime)
- `Config`: adjust poll/update rates and CIR settings
- `Set Default Propagation`: set the default path loss model for all frequencies

### Nodes

Shows (per node): frequency settings, sample rates, number of channels, antenna pattern, and whether the node is linked to a "vehicle".

Actions (press `Enter` on a node):

- **Change antenna**: select a pattern from the coordinator-provided list (`GET_ANTENNA_PATTERNS`)
- **Refresh**: re-fetch node data

### Channels

The Channels menu is split into:

- a frequency list (left)
- details for the selected frequency (top-right)
- a link list for the selected frequency (bottom-right)

Keys:

- `Tab`: toggle focus between frequency list and link list
- `Enter` on a frequency: open channel-level actions
- `Enter` on a link: open link-level actions

Channel-level actions (for a selected frequency):

- **Change pathloss**
  - Models: `FREE_SPACE`, `2_RAY`, `3GPP_38_901`, `OKUMURA_HATA` (experimental), `LONGLEY_RICE` (experimental), `NONE`
  - `2_RAY` asks for a ground reflection coefficient
  - `3GPP_38_901` asks for a scenario: `UMa`, `UMi-StreetCanyon`, `RMa`
  - `OKUMURA_HATA` asks for an environment: `URBAN`, `SUBURBAN`, `OPEN`
  - `LONGLEY_RICE` asks for surface refractivity and climate zone
  - See [Channel Models](channel-models.md) for equations and parameter details
- **Set shadowing**: set shadowing standard deviation (optionally per-frequency)
- **Refresh**

Link-level actions (for a selected `src -> dest` link):

- **Set coeff**: set the channel coefficient for the selected endpoints/ports (linear scale)
- **Apply CIR scenario**: choose from predefined multipath scenarios (see [CIR scenarios](#cir-scenarios)), then set a tap scale multiplier
- **Set CIR taps**: manually enter CIR taps as JSON (see [CIR tap formats](#cir-tap-formats))
- **Clear CIR taps**: remove all CIR taps from the link
- **Set frequency offset**: set per-link frequency offset (Hz)
- **Toggle Doppler**: enable or disable velocity-based Doppler shift
- **Set channel noise model**
  - Models: `AWGN`, `NONE`
  - `AWGN` asks for an SNR value (dB)
- **Refresh**

### Profiles

The Profiles menu shows all available profiles (built-in and custom) with a details panel describing the selected profile's settings.

Keys:

- `Enter`: apply the selected profile to all frequencies/links
- `s`: save the current channel configuration as a new custom profile
- `p`: set default propagation model (shortcut)

See [Profiles](#profiles) for details on built-in profiles and the profile format.

### Extensions

The Extensions menu is built dynamically after connecting to CHEM. It queries `EXT_LIST` and renders all registered extensions with their current status fields (fetched from each extension's `getStatus()` response).

Actions (per extension):

- **Start**: prompts for each field defined in the extension's `configSchema` (type, default value), then sends a `start` command with the collected parameters.
- **Stop**: stop the selected extension.
- **Configure `<field>`**: for each non-object field in the config schema, a configure option is shown. The current value is pre-filled from the extension's status.

The menu adapts to whatever extensions are registered. No hardcoded extension knowledge is needed. When an extension is active on a channel, it either replaces or augments CHEM's statistical propagation models (see [Extensions](extensions.md)).

### Config

Press `Enter` to edit:

- **Vehicle poll rate (ms)**: how often the CHEM polls vehicle state (20 to 60000)
- **Channel update rate (ms)**: channel update/coherency time (1 to 60000)
- **CIR max taps**: maximum number of CIR taps per link (0 to 8192)

## Key concepts

Several parameters appear throughout PyCHEM's menus, profiles, and client API. This section explains what they mean and how they interact. For the full signal model equations, see the [Signal Model](channel-models.md#signal-model) section in Channel Models.

### Source power level (`sourcePowerDbfs`)

CHEM needs a known baseline power level to convert between the digital baseband domain (dBFS) and the RF domain (dBm). This baseline is the **reference signal level**.

- **Calibrated (fixed):** A node profile can declare a fixed reference level. For example, AERPAW nodes use $-12$ dBFS, meaning the SDR's DAC output is calibrated so that a full-scale digital signal corresponds to a known RF power at the antenna port. This is the recommended mode because it produces stable, repeatable link budgets regardless of the waveform's instantaneous amplitude.
- **Measured (dynamic):** If no fixed level is set (or it is cleared via `set_source_power(node, None)`), CHEM measures the average power of the baseband signal every few processing cycles and uses that as the reference. This is useful for uncalibrated setups but can cause SNR fluctuations when the waveform's peak-to-average ratio changes (e.g. between data bursts and silence).

The reference level feeds directly into the transmit power calculation: $P_\text{tx} = S_\text{ref} + G_\text{tx} + G_\text{tx,ant} - L_\text{tx,cable}$. A higher reference level means CHEM models a stronger transmit signal, which in turn increases the received power and SNR.

### Propagation model

Determines how path loss ($PL$) is computed as a function of distance, frequency, and environment. See [Channel Models](channel-models.md) for the full list of supported models and their parameters. The model can be set per-frequency or as a global default.

### Shadowing

Log-normal shadowing adds a random component on top of the deterministic path loss: $PL_\text{total} = PL_\text{model} + X_\sigma$, where $X_\sigma \sim \mathcal{N}(0, \sigma^2)$. The standard deviation $\sigma$ (in dB) is configurable. Set to 0 to disable.

### CIR (Channel Impulse Response)

Multipath taps applied via convolution before path loss and noise. Each tap is a complex coefficient at a sample-spaced delay. When an extension like Sionna RT is active, the extension provides CIR taps automatically; otherwise they can be set manually per link.

## Profiles

Profiles bundle propagation model, shadowing, reference signal, and runtime config into a single preset that can be applied in one step.

### Built-in profiles

| Profile | Propagation | Shadowing | Ref signal | Description |
|---|---|---:|---|---|
| **AERPAW Outdoor** | `2_RAY` (ground coeff $-0.3$) | 0 dB | $-12$ dBFS | Default for AERPAW UAV outdoor experiments |
| **Lab / Loopback** | `NONE` | 0 dB | -- | Cabled bench test, disables all impairments |
| **Urban Dense (3GPP UMi)** | `3GPP_38_901` (UMi-StreetCanyon) | 3 dB | $-12$ dBFS | Dense urban scenario |
| **Rural Macro (3GPP RMa)** | `3GPP_38_901` (RMa) | 1.5 dB | $-12$ dBFS | Rural macro coverage |

### Custom profiles

Custom profiles are stored as JSON in `~/.config/chem/profiles/` and loaded alongside built-ins. Save a custom profile from the TUI with `s` in the Profiles menu, or create one manually:

```json
{
  "name": "My Scenario",
  "description": "Indoor office with multipath",
  "propagation": {
    "plMode": "3GPP_38_901",
    "scenario": "UMi-StreetCanyon"
  },
  "shadowingStd": 4.0,
  "sourcePowerDbfs": {
    "AERPAW Portable": -12.0,
    "AERPAW Fixed": -12.0
  },
  "config": {
    "channelUpdateRateMs": 10,
    "cirMaxTaps": 128
  }
}
```

### Profile fields

| Field | Type | Description |
|---|---|---|
| `name` | string | Display name |
| `description` | string | Short description shown in the TUI details panel |
| `note` | string | Optional additional notes |
| `propagation` | object | `plMode` plus model-specific keys (`groundCoeff`, `scenario`, `environment`, etc.) |
| `shadowingStd` | float | Log-normal shadowing standard deviation (dB), 0 to disable |
| `sourcePowerDbfs` | object | Per-node-type (`"portable"`, `"fixed"`) or per-node-name reference signal levels (dBFS) |
| `config` | object | Runtime settings: `vehiclePollRateMs`, `channelUpdateRateMs`, `cirMaxTaps` |

## CIR (Channel Impulse Response)

PyCHEM supports setting multipath CIR taps per link. Taps are applied via FIR convolution (or direct convolution) before path loss and noise.

### CIR scenarios

The TUI offers predefined scenarios via **Apply CIR scenario**:

| Scenario | Taps | Description |
|---|---:|---|
| None (clear) | 0 | Remove all taps |
| Single-path (unit) | 1 | `[1+0j]`, unity pass-through |
| Two-path echo (3 samples, $-6$ dB) | 4 | Direct path + delayed echo at $-6$ dB |
| Three-path (0,2,5 samples) | 6 | Three arrivals at different delays and phases |
| Dense 8-tap (fixed) | 8 | Realistic dense multipath profile |

After selecting a scenario, the TUI prompts for a **tap scale** (linear multiplier applied to all taps).

### CIR tap formats

When entering taps manually (TUI or Python client), the following JSON formats are accepted:

- **Scalar list**: `[1, 0.2]`, interpreted as real-only taps (`[[1,0], [0.2,0]]`)
- **Complex pairs**: `[[1,0], [0.2,0.1]]`, each element is `[real, imag]`
- **Object form**: `[{"re":1,"im":0}, {"re":0.2,"im":0.1}]`

## Extensions

CHEM supports a pluggable extension system. Extensions can be controlled generically or through convenience wrappers. See [Extensions](extensions.md) for the full architecture and [Sionna RT](sionna.md) for the ray-tracing extension.

### Generic extension control

```python
# Start an extension
client.extension("sionna", "start", {
    "serverUrl": "http://localhost:8000",
    "referenceOrigin": {"lat": 35.7272, "lon": -78.6960, "alt": 0.0},
})

# Query status
status = client.extension("sionna", "status")

# Stop
client.extension("sionna", "stop")

# List all registered extensions
all_status = client.extension_list()
```

### Sionna convenience methods

```python
client.sionna_start(
    server_url="http://localhost:8000",
    ref_lat=35.7272,
    ref_lon=-78.6960,
    ref_alt=0.0,
    update_rate_ms=200,
    max_depth=3,
)

status = client.sionna_status()
# {'running': True, 'serverUrl': '...', 'updateRateMs': 200, 'maxDepth': 3}

client.sionna_stop()
```

## Python client (scripting)

PyCHEM includes a small Python client (`ChemClient`) that speaks CHEM's JSON-over-TCP control protocol.
In a source checkout, you can import it as follows:

```python
from pychem import ChemClient

client = ChemClient(addr="localhost", port=5000)
print(client.get_version())
print(client.get_nodes())
print(client.get_channels())
client.close()
```

### Available client methods

Connection:

- `ChemClient(addr: str = "", port: int = 5000)`: creates a client (address auto-resolves if `addr` is empty)
- `connect()`, `close()`, `connected`

Inspection:

- `get_version() -> str | None`
- `get_status() -> dict`
- `get_config() -> dict`
- `get_nodes() -> dict`
- `get_channels() -> dict`
- `get_individual_channels() -> dict`
- `get_antenna_patterns() -> list[str]`
- `get_antenna_types() -> list[str]`
- `get_shadow_std() -> float | None`
- `get_shadow_std_for_freq(freq_mhz: float) -> float | None`

Tuning:

- `set_config(vehicle_poll_rate_ms: int | None = None, channel_update_rate_ms: int | None = None, cir_max_taps: int | None = None) -> dict`
- `set_node_antenna(node: str, pattern: str | None = None, tx_pattern: str | None = None, rx_pattern: str | None = None) -> dict`
- `set_source_power(node: str, source_power_dbfs: float | None = None) -> dict` : set fixed source power level, or `None` to revert to measured power
- `set_pathloss(freq_mhz: float, model: str, ground_coeff: float = -1, *, scenario: str | None = None, environment: str | None = None, refractivity: float | None = None, ground_conductivity: float | None = None, ground_permittivity: float | None = None, climate_zone: int | None = None) -> dict`
- `set_default_pathloss(model: str, ground_coeff: float = -1, *, scenario: str | None = None, environment: str | None = None, refractivity: float | None = None, ground_conductivity: float | None = None, ground_permittivity: float | None = None, climate_zone: int | None = None) -> dict`
- `set_coeff(freq_mhz: float, src: str, dest: str, p_src: int, p_dest: int, coeff: float) -> dict`
- `set_noise_model(freq_mhz: float, src: str, dest: str, noise_model: str, snr: float) -> dict`
- `set_frequency_offset(freq_mhz: float, src: str, dest: str, offset_hz: float) -> dict`
- `set_doppler(freq_mhz: float, src: str, dest: str, enabled: bool) -> dict`
- `set_shadow_std(std: float, freq_mhz: float | None = None) -> dict`

CIR:

- `set_cir(freq_mhz: float, src: str, dest: str, p_src: int, p_dest: int, taps: list) -> dict` : see [CIR tap formats](#cir-tap-formats)

Profiles:

- `apply_profile(profile: dict) -> None` : apply a profile dict (propagation, shadowing, ref signal, config)

Extensions:

- `extension(name: str, action: str, params: dict | None = None) -> dict` : send a command to a named extension
- `extension_list() -> dict` : list all registered extensions and their status
- `sionna_start(server_url: str = "http://localhost:8000", ref_lat: float = 35.7272, ref_lon: float = -78.6960, ref_alt: float = 0.0, update_rate_ms: int | None = None, max_depth: int | None = None) -> dict`
- `sionna_stop() -> dict`
- `sionna_status() -> dict`

### Profile helpers (module-level)

These functions manage profile storage outside of a client connection:

- `pychem.all_profiles() -> dict` : returns all profiles (built-in + custom)
- `pychem.load_custom_profiles() -> dict` : loads custom profiles from `~/.config/chem/profiles/`
- `pychem.save_profile(profile: dict) -> None` : saves a profile to `~/.config/chem/profiles/`

## Examples

### Inspect nodes and channels

```python
from pychem import ChemClient

client = ChemClient(addr="localhost", port=5000)

# List connected nodes
nodes = client.get_nodes()
for name, info in nodes.items():
    print(f"{name}: {info.get('nodeType')} @ {info.get('freq')} MHz")

# List channels per frequency
channels = client.get_channels()
for freq, ch in channels.items():
    print(f"{freq} MHz: {len(ch.get('links', []))} links")

client.close()
```

### Change propagation model

```python
# Set free-space path loss for 915 MHz
client.set_pathloss(915, "FREE_SPACE")

# Set two-ray with custom ground coefficient for 3500 MHz
client.set_pathloss(3500, "2_RAY", ground_coeff=-0.3)

# Set 3GPP model for a specific frequency
client.set_pathloss(3500, "3GPP_38_901", scenario="UMi-StreetCanyon")

# Set a default model for all current and future frequencies
client.set_default_pathloss("3GPP_38_901", scenario="RMa")
```

### Adjust link impairments

```python
# Set channel coefficient (linear scale) between two nodes on port 0->0
client.set_coeff(915, "node1", "node2", 0, 0, 0.5)

# Add a 200 Hz frequency offset
client.set_frequency_offset(915, "node1", "node2", 200.0)

# Enable Doppler shift (velocity-based, computed from vehicle movement)
client.set_doppler(915, "node1", "node2", True)

# Set AWGN noise with 20 dB SNR
client.set_noise_model(915, "node1", "node2", "AWGN", 20.0)
```

### Set CIR taps

```python
# Two-path channel: direct path + echo at -6 dB after 3 samples
taps = [[1.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.501187, 0.0]]
client.set_cir(915, "node1", "node2", 0, 0, taps)

# Clear CIR taps (empty list)
client.set_cir(915, "node1", "node2", 0, 0, [])
```

### Apply a profile

```python
from pychem import ChemClient, all_profiles

client = ChemClient(addr="localhost", port=5000)

# List available profiles
for name, profile in all_profiles().items():
    print(f"{name}: {profile.get('description')}")

# Apply a built-in profile
profiles = all_profiles()
client.apply_profile(profiles["AERPAW Outdoor"])
```

### Set reference signal level

```python
# Fix reference level to -12 dBFS (calibrated mode)
client.set_source_power("node1", -12.0)

# Revert to measured peak power (dynamic mode)
client.set_source_power("node1", None)
```

## Troubleshooting

- Connection errors: confirm CHEM is running and listening on `coordPort` (see `configs/config.json` and `docs/getting-started/configuration.md`).
- Address surprises: check `~/.config/uhd/conf.yaml` and `AP_EXPENV_CHEMVM_XE` if you didn't pass `--addr`.
- Stale data in the TUI: use `r` to refresh, or enable auto-refresh by staying on a menu (nodes/channels/status/config auto-refresh periodically).
