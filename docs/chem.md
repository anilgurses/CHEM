# CHEM

CHEM is a wireless channel emulator. It's part of the Digital Twin framework shown in [1], and is responsible for emulating the wireless channel between VUSRP nodes.  


## Node Connection & Controller

CHEM includes a central **Node Controller** that manages node registration, port assignment, and lifecycle tracking. It acts as the gateway for VUSRP nodes to join the emulation environment.

### Connection Process

1. **TCP Handshake:** A node initiates a TCP connection to the CHEM controller's address and port (configured via `controllerIpAddress`/`controllerPort`).
2. **Registration:** The node sends a JSON configuration message containing its operational parameters (see [Node Config](#node-config)).
3. **Port Assignment:** The controller assigns unique UDP ports from its pool for the node's TX and RX streams.
4. **Response:** CHEM responds with a JSON message containing the assigned ports:
   ```json
   {"status": "success", "txPort": 10001, "rxPort": 11001}
   ```
5. **Streaming:** The node then starts streaming IQ data to/from the assigned UDP ports.

### Node Config

When a node registers, it provides a `NodeConfig` object in JSON format. Key fields include:

| Field | Description |
|---|---|
| `id` / `name` | Unique identifiers for the node. |
| `node_type` | Operational mode (Fixed, Vehicle, AERPAW, etc.). |
| `channels` | List of operational channels (TX/RX frequencies). |
| `sample_rate` | Requested TX and RX sample rates. |
| `location` | Initial GPS coordinates (`lat`, `lon`, `alt`). |
| `gains` | Device-specific TX/RX gain settings. |
| `inputFormat` / `outputFormat` | IQ data format (e.g., `fc32`, `sc16`). |
| `vehicle` | (Optional) MAVLink/MAVProxy address for mobile nodes. |

### Node Lifecycle

- **Heartbeats:** Nodes should periodically send a heartbeat message (`{"type": "heartbeat", "id": "node_id"}`) to remain active. CHEM automatically removes nodes that haven't responded for 10 seconds.
- **Updates:** A node can update its configuration (e.g., frequency or location) by sending a new registration message.
- **Disconnect:** Clean disconnection is handled via a disconnect message (`{"type": "disconnect", "id": "node_id"}`).

## Channel Intermediates

An **Intermediate** in CHEM represents a single frequency channel. It acts as the core processing engine for all signals transmitted on a specific center frequency.

### Key Responsibilities

- **Frequency Isolation:** Each frequency assigned in the emulation environment is managed by its own `Intermediate` instance, ensuring signals on different frequencies do not interfere.
- **Source & Destination Management:** It maintains a map of all transmitters (sources) and receivers (destinations) currently tuned to its frequency.
- **Parallel Processing:** Signal processing is highly parallelized. For every incoming signal from a source, the `Intermediate` distributes the task of computing channel effects for all relevant destinations across multiple worker threads.
- **Impairment Pipeline:** For each source-destination link, the intermediate executes a DSP pipeline including:
    - **Resampling:** Adjusting sample rates between different hardware profiles.
    - **CIR Convolution:** Applying Multipath/Channel Impulse Response (taps).
    - **Path Loss & Shadowing:** Calculating received power based on distance and environment.
    - **Noise & Interference:** Adding AWGN or other interference sources.
    - **Doppler & Frequency Offset:** Emulating mobility and oscillator inaccuracies.

![Channel Process Animation](assets/ChannelProcessAnimation.gif)

## Signal & IQ Data Flow

A **Signal** is the unit of data that moves through CHEM. Each Signal wraps a single buffer of IQ samples received from a VUSRP transmitter over UDP, along with metadata such as timestamps, sample rate, center frequency, and the number of RF channels (MIMO ports).

### Lifecycle

1. **Receive:** Incoming UDP packets from a VUSRP node are reassembled into a contiguous IQ buffer. A `Signal` is constructed with this buffer and enqueued to the appropriate receiver's worker thread.
2. **Staleness Check:** The worker checks the signal's timestamp. If it is older than the maximum allowed latency, it is dropped.
3. **Format Conversion:** The raw IQ data (typically `sc16` on the wire) is converted into float-complex (`fc32`) working buffers.
4. **Channel Impairments:** The converted samples are passed through `Channel::processChannel()` for each source-destination link, applying resampling, CIR convolution, path loss, noise, and frequency/Doppler offset.
5. **Output & Transmit:** The impaired samples are converted back to the receiver's wire format and sent over UDP to the destination VUSRP node.

IQ buffers are managed by a pool allocator (`DataArrayPool`) to avoid per-frame heap allocations on the real-time path.

## Node Mobility

One of the important features that ACHEM offers is support for node mobility. Nodes can be attached to a vehicle or placed at a fixed position through [PyCHEM](pychem.md) or the configuration file associated with a V-USRP. A vehicle can be a UAV or a UGV, emulated through a MAVLink source (ArduPilot SITL vehicle software in this work). The ArduPilot SITL emulator initiates the MAVLink connection the same way it does for real vehicles. CHEM uses the SITL's TCP MAVLink connection to receive status messages from the emulated vehicle. Although ArduPilot is used in this work, CHEM can operate with any MAVLink-compatible vehicle emulator or tool that sends MAVLink messages.

The number of vehicles, their node associations, and fixed-node positions are obtained from the CHEM configuration file. In addition to providing updates to CHEM, the vehicle SITL interacts with the application vehicle software for automation and control.

## PyCHEM

PyCHEM provides the management interface and control API for CHEM via a TCP connection. Through PyCHEM, an experimenter can configure and update channel- and node-level parameters, including antenna models, frequency offsets, noise and propagation models, fixed-node locations, and portable-node vehicle types. This interface enables online reconfiguration of active experiments without restarting the emulation framework. See the [PyCHEM documentation](pychem.md) for the full API reference.

## Signal Model

For details on the per-receiver baseband signal model and the overall architecture, see the [Overview](overview.md#signal-model) page.

## Configuration notes (current)

CHEM reads JSON config from:

- default: `~/.config/CHEM/config.json`
- override: pass a `.json` file path as the first CLI argument, e.g. `CHEM /path/to/config.json`
- source of truth: [`configs/config.json`](../configs/config.json)

### Common Keys

- `controllerIpAddress` / `controllerPort`: Node controller bind address and port.
- `coordIpaddress` / `coordPort`: Channel coordinator bind address and port.
- `maxNode`: Maximum number of nodes expected in the scenario.
- `logDirectory` / `logLevel`: Log path and log verbosity.
- `maxCores` (optional): Caps oneTBB parallelism used by channel processing (minimum `1`).
- `cpuAffinity` (optional): CPU affinity mask string (e.g., `"0-7"`).
- `numaEnabled` (optional): Enables NUMA-aware binding when libnuma is available.
- `numaNode` (optional): NUMA node assignment.

### Extensions (Optional)

CHEM supports a pluggable [extension system](extensions.md) for integrating external channel modeling backends. Extensions are configured under the `extensions` key:

- Each sub-key names an extension (e.g., `"sionna"`).
- If `enabled` is `true`, the extension starts automatically.
- Extension-specific parameters are passed directly to the extension's `onStart()` method.

See [Extensions](extensions.md) for the full architecture and [Extension Development Guide](extension-development.md) for creating new extensions.

### Example Config

```json
{
  "controllerIpAddress": "0.0.0.0",
  "controllerPort": 6000,
  "coordIpaddress": "0.0.0.0",
  "coordPort": 5000,
  "maxNode": 64,
  "maxCores": 10,
  "numaEnabled": false,
  "logDirectory": "/tmp/chem.log",
  "logLevel": "info",
  "extensions": {
    "sionna": {
      "enabled": false,
      "serverUrl": "http://localhost:8000",
      "updateRateMs": 500,
      "maxDepth": 3,
      "numSamples": 10000,
      "referenceOrigin": {
        "lat": 35.7272,
        "lon": -78.6960,
        "alt": 0.0
      }
    }
  }
}
```

## Related Pages

- [Overview](overview.md): Overall ACHEM system design and signal model
- [V-USRP](vusrp.md): Virtual radio peripheral
- [PyCHEM](pychem.md): Python management interface
- [Channel Models](channel-models.md): Propagation models and equations
- [Extensions](extensions.md): Pluggable channel modeling backends
- [Benchmark](benchmark.md): Performance testing
- [Getting Started](getting-started/installation.md): Installation and configuration

## Troubleshooting / Known issues

- If `numaEnabled` is `true`, CHEM will attempt to bind the process to a NUMA node during startup.
- On systems or containers without the required CPU-affinity / memory-policy permissions, that NUMA setup can fail. If you hit startup issues in that environment, set `numaEnabled` to `false` or run with the required host permissions and NUMA access.
