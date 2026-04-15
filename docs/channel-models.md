# Channel Models

CHEM supports multiple propagation models for computing path loss between transmitter and receiver nodes. Each model is selectable at runtime via the coordinator API or the PyCHEM TUI.

## VUSRP Gain Conversion

When a VUSRP node sets TX or RX gain through the UHD API (e.g., `set_tx_gain(60)`), the driver converts the AD9361 gain value into a power level in dBm before sending it to CHEM. This conversion happens in the `gain_to_dbm()` function inside the VUSRP driver.

The AD9361 transceiver exposes abstract gain ranges that do not directly correspond to output/input power. VUSRP maps these to realistic power levels:

| Direction | AD9361 Gain Range | Max Power (dBm) | Conversion |
|---|---|---|---|
| TX | 0 - 89.75 dB | 18 dBm | `(gain / 89.75) * 18` |
| RX | 0 - 76 dB | 38 dBm | `(gain / 76) * 38` |

\* These values obtained from various sources. I already have TX power calibration information and they match. However, RX values purely based on specs.

If a `ref_power` property is set on the UHD frontend (via the property tree at `<fe_root>/ref_power/value`), the conversion uses a reference-based formula instead:

$$P_{\text{dBm}} = P_{\text{ref}} + G_{\text{dB}}$$

The result is clamped to $[0,\ P_{\max}]$ in both cases.

CHEM receives the converted gain value (in dBm) through the node's JSON registration message. These values feed into $G_\text{tx}$ and $G_\text{rx}$ in the link budget equations below. If no gain is provided, defaults of 15 dBm (TX) and 10 dBm (RX) are used.

---

## Signal Model

Each call to `processChannel()` transforms a baseband TX signal into a received signal by applying the full RF chain: resampling, multipath CIR convolution, path loss, noise, and frequency offset. The equations below describe how transmit power, receive power, and SNR are derived from node characteristics and the selected propagation model. See `Channel::computeLinkBudget()` and `Channel::processChannel()` in `src/chem/channel/channel.cpp`.

### Reference signal level

All power calculations are anchored to a reference signal level $S_\text{ref}$ (dBFS):

- **Calibrated:** If the node profile sets `source_power_dbfs` (e.g. $-12$ dBFS for AERPAW nodes), that fixed value is used.
- **Measured:** Otherwise, the peak power of the baseband signal is measured (every 4th call, cached between measurements).

### Transmit power

$$
P_\text{tx} = S_\text{ref} + G_\text{tx} + G_\text{tx,ant} - L_\text{tx,cable} \quad \text{(dBm)}
$$

| Term | Description |
|---|---|
| $S_\text{ref}$ | Reference signal level, calibrated or measured (dBFS) |
| $G_\text{tx}$ | Aggregate TX gain: SDR device gain + PA gain (`tx_pa_gain_db`) |
| $G_\text{tx,ant}$ | TX antenna gain from pattern lookup at current elevation/azimuth |
| $L_\text{tx,cable}$ | TX-side cable and interconnect loss |

### Receive power

$$
P_\text{rx} = P_\text{tx} - PL + G_\text{rx} + G_\text{rx,ant} - L_\text{rx,cable} \quad \text{(dBm)}
$$

| Term | Description |
|---|---|
| $PL$ | Path loss from the selected propagation model (see sections below) |
| $G_\text{rx}$ | Aggregate RX gain: SDR device gain + LNA gain |
| $G_\text{rx,ant}$ | RX antenna gain from pattern lookup |
| $L_\text{rx,cable}$ | RX-side cable and interconnect loss |

$P_\text{rx}$ is clamped to $\leq 0$ dBFS to prevent digital saturation.

### SNR

The baseband signal is scaled by $\Delta_\text{rx} = P_\text{rx} - S_\text{ref}$ dB and summed with thermal noise:

$$
\text{SNR} = S_\text{ref} + \Delta_\text{rx} - N_\text{meas} \quad \text{(dB)}
$$

| Term | Description |
|---|---|
| $\Delta_\text{rx}$ | RX scaling factor (`rx_scale_db` in code) |
| $N_\text{meas}$ | Measured AWGN power, determined by bandwidth and composite noise figure |

The composite noise figure sums the TX noise figure, RX noise figure, and RX gain (the receiver's gain amplifies its own noise floor).

---

## Free Space Path Loss (FSPL)

**Model key:** `FREE_SPACE`

The simplest propagation model, derived from the Friis transmission equation [1]. Assumes an unobstructed line-of-sight path with no reflections, diffraction, or scattering.

$$
PL_{\text{FSPL}}(d, f) = 20\log_{10}(d) + 20\log_{10}(f) + 20\log_{10}\!\left(\frac{4\pi}{c}\right)
$$

where:

- $d$ is the distance in meters
- $f$ is the carrier frequency in Hz
- $c = 299\,792\,458$ m/s (speed of light)

**Parameters:** none (beyond distance and frequency).

---

## Two-Ray Ground Reflection

**Model key:** `2_RAY`

Models a direct ray and a single ground-reflected ray. Captures the constructive/destructive interference pattern that creates distance-dependent fading nulls.

The received power ratio is:

$$
\frac{P_r}{P_t} = \left(\frac{\lambda}{4\pi}\right)^{\!2} \left| \frac{e^{-jkd_1}}{d_1} + \Gamma\,\frac{e^{-jkd_2}}{d_2} \right|^2
$$

where:

- $d_1 = \sqrt{d^2 + (h_t - h_r)^2}$ is the direct path length
- $d_2 = \sqrt{d^2 + (h_t + h_r)^2}$ is the reflected path length
- $k = 2\pi / \lambda$ is the wave number
- $\Gamma$ is the ground reflection coefficient (typically $-1$ for perfect reflection)
- $h_t$, $h_r$ are the transmitter and receiver heights

**Parameters:**

| Parameter | JSON key | Default | Description |
|---|---|---|---|
| Ground coefficient | `groundCoeff` | $-1.0$ | Reflection coefficient $\Gamma \in [-1, 1]$ |

---

## 3GPP TR 38.901

**Model key:** `3GPP_38_901`

Implements the path loss models from 3GPP TR 38.901 [2]. Three deployment scenarios are supported, each with distinct LOS and NLOS formulas.

### LOS Probability

LOS/NLOS condition is determined probabilistically per [2, Table 7.4.2-1]:

- **RMa:** $P_{\text{LOS}} = \exp\!\bigl(-(d_{2D} - 10)/1000\bigr)$ for $d_{2D} > 10$ m, else $1$
- **UMa:** $P_{\text{LOS}} = \bigl(\min(18/d_{2D},\,1)\cdot(1 - e^{-d_{2D}/63}) + e^{-d_{2D}/63}\bigr)$ for $d_{2D} > 18$ m
- **UMi:** $P_{\text{LOS}} = \bigl(\min(18/d_{2D},\,1)\cdot(1 - e^{-d_{2D}/36}) + e^{-d_{2D}/36}\bigr)$ for $d_{2D} > 18$ m

### RMa (Rural Macro)

**LOS** uses a two-segment formula with breakpoint distance:

$$
d_{BP} = \frac{2\pi \cdot h_{BS} \cdot h_{UT} \cdot f_c}{c}
$$

For $d_{2D} \leq d_{BP}$:

$$
PL_1 = 20\log_{10}\!\left(\frac{40\pi \cdot d_{3D} \cdot f_c}{3}\right)
+ \min(0.03 h^{1.72},\,10)\,\log_{10}(d_{3D})
- \min(0.044 h^{1.72},\,14.77)
+ 0.002\,\log_{10}(h)\,d_{3D}
$$

For $d_{2D} > d_{BP}$:

$$
PL_2 = PL_1(d_{BP}) + 40\,\log_{10}\!\left(\frac{d_{3D}}{d_{3D,BP}}\right)
$$

where $h = 5$ m (average building height), $W = 20$ m (street width), $f_c$ is in GHz.

**NLOS:**

$$
PL_{\text{NLOS}} = 161.04 - 7.1\log_{10}(W) + 7.5\log_{10}(h)
- \bigl(24.37 - 3.7(h/h_{BS})^2\bigr)\log_{10}(h_{BS})
+ \bigl(43.42 - 3.1\log_{10}(h_{BS})\bigr)(\log_{10}(d_{3D}) - 3)
+ 20\log_{10}(f_c)
- \bigl(3.2(\log_{10}(11.75\,h_{UT}))^2 - 4.97\bigr)
$$

Final NLOS path loss is $\max(PL_{\text{NLOS}},\, PL_{\text{LOS}})$.

### UMa (Urban Macro)

**LOS:**

$$
PL = 28.0 + 22\log_{10}(d_{3D}) + 20\log_{10}(f_c)
$$

**NLOS:**

$$
PL = 13.54 + 39.08\log_{10}(d_{3D}) + 20\log_{10}(f_c) - 0.6(h_{UT} - 1.5)
$$

### UMi-StreetCanyon (Urban Micro)

**LOS:**

$$
PL = 32.4 + 21\log_{10}(d_{3D}) + 20\log_{10}(f_c)
$$

**NLOS:**

$$
PL = 35.3\log_{10}(d_{3D}) + 22.4 + 21.3\log_{10}(f_c) - 0.3(h_{UT} - 1.5)
$$

### Variable definitions

| Symbol | Description | Typical range |
|---|---|---|
| $d_{2D}$ | 2D horizontal distance (m) | |
| $d_{3D}$ | 3D distance including height difference (m) | |
| $f_c$ | Carrier frequency (GHz) | 0.5 to 100 |
| $h_{BS}$ | Base station height (m) | 10 to 150 |
| $h_{UT}$ | User terminal height (m) | 1.5 to 22.5 |
| $h$ | Average building height (m) | 5 (default) |
| $W$ | Street width (m) | 20 (default) |

**Parameters:**

| Parameter | JSON key | Default | Description |
|---|---|---|---|
| Scenario | `scenario` | `UMa` | `UMa`, `UMi-StreetCanyon`, or `RMa` |

---

## Okumura-Hata (experimental)

**Model key:** `OKUMURA_HATA`

An empirical model for macro-cell urban, suburban, and open-area propagation in the 150 to 1500 MHz range. Based on Okumura's measurements [3] with Hata's empirical formulation [4].

### Urban formula

$$
L_{\text{urban}} = 69.55 + 26.16\log_{10}(f_c) - 13.82\log_{10}(h_{BS}) - a(h_{MS})
+ (44.9 - 6.55\log_{10}(h_{BS}))\log_{10}(d)
$$

where the mobile station antenna height correction (small/medium city) is:

$$
a(h_{MS}) = (1.1\log_{10}(f_c) - 0.7)\,h_{MS} - (1.56\log_{10}(f_c) - 0.8)
$$

### Suburban correction

$$
L_{\text{suburban}} = L_{\text{urban}} - 2\left(\log_{10}\!\frac{f_c}{28}\right)^{\!2} - 5.4
$$

### Open-area correction

$$
L_{\text{open}} = L_{\text{urban}} - 4.78(\log_{10} f_c)^2 + 18.33\log_{10}(f_c) - 40.94
$$

### Variable definitions

| Symbol | Description | Valid range |
|---|---|---|
| $f_c$ | Carrier frequency (MHz) | 150 to 1500 |
| $d$ | Distance (km) | 1 to 20 |
| $h_{BS}$ | Base station height (m) | 30 to 200 |
| $h_{MS}$ | Mobile station height (m) | 1 to 10 |

Note: CHEM accepts frequency in Hz and distance in meters; conversion is handled internally.

**Parameters:**

| Parameter | JSON key | Default | Description |
|---|---|---|---|
| Environment | `environment` | `URBAN` | `URBAN`, `SUBURBAN`, or `OPEN` |

**Example (coordinator JSON):**

```json
{"CMD": "CHG_PL", "freq": 915, "plMode": "OKUMURA_HATA", "environment": "SUBURBAN"}
```

---

## Longley-Rice / ITM (experimental)

**Model key:** `LONGLEY_RICE`

A simplified flat-terrain implementation of the Irregular Terrain Model (ITM), also known as the Longley-Rice model [5]. Suitable for predicting median transmission loss over irregular terrain at frequencies from 20 MHz to 20 GHz and distances from 1 km to 2000 km.

This implementation uses a smooth-earth approximation (no terrain profile input).

### Effective earth radius

The effective earth radius factor accounts for atmospheric refraction:

$$
k = \frac{1}{1 - 0.04665\,\exp(0.005577\,N_s)}
$$

where $N_s$ is the surface refractivity in N-units. The effective earth radius is $a_e = k \cdot R_E$ with $R_E = 6\,371\,000$ m.

### Radio horizon distance

For smooth earth, the radio horizon distance for an antenna at height $h$ is:

$$
d_h = \sqrt{2\,k\,R_E\,h}
$$

The total LOS horizon distance is $d_{\text{LOS}} = d_{h,\text{tx}} + d_{h,\text{rx}}$.

### Path loss regions

**Line-of-sight** ($d \leq d_{\text{LOS}}$): Free-space path loss is used.

**Beyond horizon** ($d > d_{\text{LOS}}$): The model blends two loss mechanisms and takes the lesser (dominant propagation mode):

1. **Diffraction loss** : Fresnel-Kirchhoff single knife-edge approximation:

$$
L_{\text{diff}}(v) = \begin{cases}
0 & v \leq -0.78 \\
6.02 + 9.11v - 1.27v^2 & -0.78 < v < 0 \\
6.02 + 9.0v + 1.65v^2 & v \geq 0
\end{cases}
$$

   where $v$ is the Fresnel parameter computed from the smooth-earth geometry.

2. **Troposcatter loss** : Simplified Yeh model:

$$
L_s = 190.1 + 20\log_{10}(f_{\text{GHz}}) + 20\log_{10}(d_{\text{km}}) + 0.573\,\theta_s^\circ - 0.15\,N_s
$$

   where $\theta_s$ is the scattering angle in degrees.

Final beyond-horizon path loss: $PL = PL_{\text{FSPL}} + \min(L_{\text{diff}},\, L_s)$

### Variable definitions

| Symbol | Description | Typical value |
|---|---|---|
| $N_s$ | Surface refractivity (N-units) | 250 to 400 (301 typical mid-latitude) |
| $\sigma_g$ | Ground conductivity (S/m) | 0.001 to 0.03 |
| $\epsilon_r$ | Relative ground permittivity | 4 to 80 |
| Climate zone | ITM climate classification | 1 to 7 |

### Climate zones

| Zone | Description |
|---|---|
| 1 | Equatorial |
| 2 | Continental subtropical |
| 3 | Maritime subtropical |
| 4 | Desert |
| 5 | Continental temperate |
| 6 | Maritime temperate over land |
| 7 | Maritime temperate over sea |

**Parameters:**

| Parameter | JSON key | Default | Description |
|---|---|---|---|
| Refractivity | `refractivity` | `301.0` | Surface refractivity ($N_s$) |
| Ground conductivity | `groundConductivity` | `0.005` | Ground conductivity (S/m) |
| Ground permittivity | `groundPermittivity` | `15.0` | Relative ground permittivity |
| Climate zone | `climateZone` | `5` | ITM climate zone (1 to 7) |

**Example (coordinator JSON):**

```json
{"CMD": "CHG_PL", "freq": 915, "plMode": "LONGLEY_RICE", "refractivity": 301, "climateZone": 5}
```

---

## Log-Normal Shadowing

All models optionally apply log-normal shadowing on top of the deterministic path loss:

$$
PL_{\text{total}} = PL_{\text{model}} + X_\sigma
$$

where $X_\sigma \sim \mathcal{N}(0, \sigma^2)$ is a zero-mean Gaussian random variable with standard deviation $\sigma$ (in dB). The shadowing standard deviation is configurable per frequency via the `UPD_SHADOW_STD` coordinator command.

---

## Model Selection via API

### Per-frequency (CHG_PL)

```json
{"CMD": "CHG_PL", "freq": 915, "plMode": "FREE_SPACE"}
{"CMD": "CHG_PL", "freq": 915, "plMode": "2_RAY", "groundCoeff": -0.8}
{"CMD": "CHG_PL", "freq": 3500, "plMode": "3GPP_38_901", "scenario": "RMa"}
{"CMD": "CHG_PL", "freq": 915, "plMode": "OKUMURA_HATA", "environment": "SUBURBAN"}
{"CMD": "CHG_PL", "freq": 915, "plMode": "LONGLEY_RICE", "refractivity": 301, "climateZone": 5}
```

### Global default (SET_DEFAULT_PL)

```json
{"CMD": "SET_DEFAULT_PL", "plMode": "3GPP_38_901", "scenario": "UMa"}
```

The default model is applied to all existing and future intermediates (frequency channels).

## Source Files

| File | Description |
|---|---|
| `include/chem/channel_models/TR_38_901_3GPP.hpp` | 3GPP TR 38.901 path loss (RMa, UMa, UMi) |
| `include/chem/channel_models/okumura_hata.hpp` | Okumura-Hata model |
| `include/chem/channel_models/longley_rice.hpp` | Simplified Longley-Rice (ITM) model |
| `src/chem/dsp/channel.cpp` | DSP wrappers (`calc_fspl`, `calc_2ray`, `calc_3gpp_38_901`, `calc_okumura_hata`, `calc_longley_rice`) |
| `src/chem/channel/channel.cpp` | Per-link `processChannel()` switch dispatch |

## References

[1] International Telecommunication Union, "Calculation of free-space attenuation," Recommendation ITU-R P.525-4, Sep. 2019.

[2] 3GPP, "Study on channel model for frequencies from 0.5 to 100 GHz," 3GPP TR 38.901, V16.1.0, Jan. 2020.

[3] Y. Okumura, E. Ohmori, T. Kawano, and K. Fukuda, "Field strength and its variability in VHF and UHF land-mobile radio service," *Rev. Elect. Commun. Lab.*, vol. 16, no. 9--10, pp. 825--873, Sep.--Oct. 1968.

[4] M. Hata, "Empirical formula for propagation loss in land mobile radio services," *IEEE Trans. Veh. Technol.*, vol. VT-29, no. 3, pp. 317--325, Aug. 1980, doi: 10.1109/T-VT.1980.23859.

[5] G. A. Hufford, A. G. Longley, and W. A. Kissick, "A guide to the use of the ITS Irregular Terrain Model in the area prediction mode," NTIA Report 82-100, U.S. Dept. of Commerce, Apr. 1982.
