# Antenna Radiation Patterns

CHEM models direction-dependent antenna gain so that the link budget reflects realistic radiation characteristics. Every TX and RX node can be assigned an antenna pattern; the gain at the current elevation and azimuth angles is looked up each time `processChannel()` runs and folded into the transmit/receive power equations (see [Channel Models](channel-models.md)).

---

## Coordinate System

CHEM uses a standard spherical coordinate system for antenna patterns:

| Symbol | Description | Range |
|---|---|---|
| $\theta$ | Polar angle measured from the positive z-axis (zenith). $\theta = 0°$ is straight up, $90°$ is the horizon, $180°$ is straight down. | $[0, \pi]$ |
| $\phi$ | Azimuth angle in the x-y plane measured from the positive x-axis. | $[0, 2\pi]$ |

When CHEM receives an **elevation angle** $\alpha$ (positive above horizon, negative below) and an **azimuth** $\phi_\text{az}$ from a node pair's geometry, the DSP layer converts them before querying the antenna object:

$$
\theta = 90° - \alpha, \qquad \phi = \phi_\text{az} + \begin{cases} 0° & \text{(TX)} \\ 180° & \text{(RX)} \end{cases}
$$

The 180° offset on the RX side accounts for the fact that the receiver faces the transmitter.

---

## How Antenna Gain Is Applied

Antenna gain is integrated into the link budget at two points. See `compute_antenna_gain_db()` in `src/chem/dsp/antenna_prop.cpp`.

**Transmit power:**

$$
P_\text{tx} = S_\text{ref} + G_\text{tx} + G_\text{tx,ant}(\theta,\phi) - L_\text{tx,cable}
$$

**Receive power:**

$$
P_\text{rx} = P_\text{tx} - PL + G_\text{rx} + G_\text{rx,ant}(\theta,\phi) - L_\text{rx,cable}
$$

$G_\text{tx,ant}$ and $G_\text{rx,ant}$ are the per-direction antenna gains in dBi returned by the active antenna model's `get_gain(theta, phi)` method.

### Antenna Selection Priority

When determining which antenna pattern to use for a given node, CHEM checks the following in order:

1. **Dynamic antenna pattern**: set at runtime via the coordinator API (`SET_TX_ANTENNA` / `SET_RX_ANTENNA`).
2. **Node characteristics**: from the AERPAW node profile (`tx_antennas` / `rx_antennas` fields).
3. **Channel-level setting**: from the node's channel configuration JSON.
4. **Default**: `isotropic` (0 dBi in all directions).

## Available Antenna Models

### Isotropic

**Name:** `isotropic`

A theoretical reference antenna that radiates equally in all directions.

$$
G(\theta, \phi) = 0 \ \text{dBi}
$$

Useful as a baseline or when antenna effects should be excluded from the emulation.

### Half-Wave Dipole

**Name:** `dipole`

An analytical half-wave dipole aligned with the z-axis (vertical polarization). The dipole length is derived from the operating frequency:

$$
L = \frac{c}{2f}
$$

The far-field gain pattern is:

$$
G(\theta) = 1.64 \left( \frac{\cos\!\bigl(\frac{\pi}{2}\cos\theta\bigr)}{\sin\theta} \right)^{\!2}
$$

expressed in dBi as $10\log_{10}(G)$. Peak gain of **2.15 dBi** occurs at the horizon ($\theta = 90°$) and nulls appear along the dipole axis ($\theta = 0°, 180°$), where the gain is clamped to $-60$ dB for numerical stability.

### Measured Patterns (AERPAW Dataset)

CHEM ships with three measured antenna patterns from the [AERPAW 3D Antenna Radiation Pattern Dataset](https://aerpaw.org/dataset/3d-antenna-radiation-pattern-measurement/). Each pattern is split into a small generated header (struct definitions, declarations, and the antenna class) and a corresponding `.cpp` source file (data arrays and function definitions) so the large gain tables are compiled once rather than reparsed in every translation unit.

These are conditionally compiled. If the generated headers are present in `include/chem/antennas/generated/`, they are automatically available.

#### SA-1400-5900 (Omnidirectional, 1.4 to 5.9 GHz)

**Name:** `sa-1400-5900`

A wideband omnidirectional antenna covering 1.4 to 5.9 GHz. Used as the **receiver (portable node)** antenna in AERPAW field experiments. Pattern data is available at the following frequencies:

| Frequency | Description |
|---|---|
| 902 MHz | Lower band edge |
| 1700 MHz | L-band |
| 2100 MHz | S-band |
| 3300 MHz | C-band (lower) |
| 3400 MHz | C-band |
| 3500 MHz | C-band (CBRS) |
| 3800 MHz | C-band (upper) |
| 5040 MHz | U-NII |
| 5060 MHz | U-NII |

The antenna object selects the **nearest frequency table**. Each frequency table stores a regular grid enabling fast $O(1)$ gain lookup by snapping the requested $(\theta, \phi)$ to the nearest grid cell.

#### RM-WB1-DN-BLK Right Side Up

**Name:** `rm-wb1-dn-blk right side up`

A broadband directional antenna measured in its standard (right-side-up) mounting orientation. Same frequency coverage as the SA-1400-5900. Used as the **transmitter (fixed node)** antenna in AERPAW field experiments.

#### RM-WB1-DN-BLK Upside Down

**Name:** `rm-wb1-dn-blk upside down`

The same broadband antenna measured in an inverted mounting orientation. This captures the significantly different radiation characteristics when the antenna is mounted upside down (e.g., on the underside of a UAV).

---

## 3D Radiation Pattern Visualization

Interactive 3D visualizations of the measured antenna patterns are available. The plots show the radiation pattern as a 3D surface where the radius from the origin represents the gain magnitude at each $(\theta, \phi)$ direction, color-mapped to gain in dB.

<iframe src="../assets/AntennaRadiationPatterns.html" width="100%" height="900" frameborder="0"></iframe>

The visualization shows both the **Fixed Node** (RM-WB1-DN-BLK) and **Portable Node** (SA-1400-5900) antennas at 3.3, 3.4, and 3.5 GHz. Red lines indicate the elevation and azimuth reference axes.

??? note "Generating visualizations from measurement data"

    The 3D plots are generated from raw antenna measurement files using a Python utility class from the [USRP-Channel-Sounder](https://github.com/anilgurses/USRP-Channel-Sounder) repository. Each measurement file contains columns for $\phi$ (rad), $\theta$ (rad), total gain (dBi), $\phi$-polarized gain, and $\theta$-polarized gain.

    ```python
    from utils.antenna import Antenna

    ant = Antenna()
    ant.read("SA-1400-5900/SA-1400-5900-F3500.txt")
    ant.plot3d()          # Interactive Plotly 3D surface
    ant.plotPolar()       # 2D polar slice
    ant.generateStl("output")  # Export STL for 3D printing
    ```

    The `plot3d()` method converts spherical gain data to Cartesian coordinates:

    $$
    x = (G + G_\text{offset}) \sin\theta\cos\phi, \quad
    y = (G + G_\text{offset}) \sin\theta\sin\phi, \quad
    z = (G + G_\text{offset}) \cos\theta
    $$

    where $G_\text{offset} = \max(|G_\text{min}|, |G_\text{max}|)$ shifts all values positive so the surface does not collapse through the origin.

---

## Lookup Data Structure

Each measured antenna pattern is split into a header (`.h`) and a source file (`.cpp`). The header declares the struct and the antenna class; the source file contains the gain data arrays and lookup functions.

```cpp
struct PatternTable {
    float freq_mhz;
    float phi_min;
    float phi_step;
    size_t phi_size;
    float theta_min;
    float theta_step;
    size_t theta_size;
    const float* gains;  // row major [theta][phi]
};
```

Grid parameters are embedded directly in `PatternTable`, and all gain values use `float` precision (sufficient for dBi data, also helps with memory usage). The lookup function snaps the requested $(\theta, \phi)$ to the nearest grid cell for $O(1)$ retrieval. When the operating frequency does not exactly match a measured frequency, the nearest `PatternTable` entry is selected.

---

## Configuration

### JSON configuration

Antenna patterns can be set per-channel in the node configuration:

```json
{
  "channels": [
    {
      "freq": 3500,
      "txAntenna": "rm-wb1-dn-blk upside down",
      "rxAntenna": "sa-1400-5900"
    }
  ]
}
```

### Runtime API

Antenna patterns can be changed at runtime via the coordinator:

```json
{"CMD": "SET_TX_ANTENNA", "nodeId": "node1", "antenna": "dipole"}
{"CMD": "SET_RX_ANTENNA", "nodeId": "node2", "antenna": "sa-1400-5900"}
```

---

## Source Files

| File | Description |
|---|---|
| `include/chem/antennas/dipole.h` | Base `Antenna` class, `IsotropicAntenna`, `DipoleAntenna` |
| `include/chem/antennas/antennas.h` | Factory (`MakeAntenna`), name normalization, available-antenna list |
| `include/chem/antennas/generated/` | Generated headers (structs, declarations, antenna classes) |
| `src/chem/antennas/generated/` | Generated sources (gain data arrays, lookup functions) |
| `src/chem/dsp/antenna_prop.cpp` | DSP integration: coordinate conversion, caching, gain computation |
| `include/chem/dsp/antenna_prop.h` | Public API: `compute_antenna_gain_db()` |
| `data/antenna/` | Raw measurement data from AERPAW |
