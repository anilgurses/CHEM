<img src="CHEM_Logo.png" style="width:400px; height:auto;">

# Channel Emulator (CHEM)

CHEM is a real-time wireless channel emulator for digital twin deployments. It provides software-based channel emulation between SDR nodes, supporting various propagation models, MIMO configurations, and dynamic channel conditions.

## Dependencies

### System Packages (Ubuntu/Debian)

```bash
sudo apt update && sudo apt install -y \
    build-essential cmake git \
    libboost-system-dev libboost-thread-dev libboost-regex-dev \
    libuhd-dev uhd-host \
    libsqlite3-dev \
    libtbb-dev \
    libvolk2-dev \
    libfmt-dev \
    python3-dev python3-pip python3-numpy python3-scipy
```

### System Packages (Fedora/RHEL)

```bash
sudo dnf install -y \
    gcc-c++ cmake git \
    boost-devel \
    uhd-devel \
    sqlite-devel \
    tbb-devel \
    volk-devel \
    fmt-devel \
    python3-devel python3-pip python3-numpy python3-scipy
```

### Additional Dependencies

**MAVSDK** (for vehicle tracking):
```bash
# See https://mavsdk.mavlink.io/main/en/cpp/guide/installation.html
# Or use system package if available
sudo apt install libmavsdk-dev  # If available
```

## Build & Install

### Basic Build

```bash
git clone https://github.com/anilgurses/CHEM.git
cd CHEM
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
sudo make install
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `CMAKE_BUILD_TYPE` | `DEBUG` | Build type: `DEBUG`, `Release`, `RelWithDebInfo` |
| `ENABLE_DEBUG_LOG` | `OFF` | Enable verbose debug logging |
| `ENABLE_PROFILING` | `OFF` | Enable performance profiling |
| `ENABLE_BENCHMARK` | `OFF` | Build the benchmark tool |
| `INSTALL_PYCHEM` | `ON` | Install pychem Python module |

Example with options:
```bash
cmake -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_DEBUG_LOG=ON \
      -DENABLE_PROFILING=ON \
      ..
```

### Installed Components

After `make install`:
- `CHEM` - Main channel emulator executable (`/usr/local/bin/CHEM`)
- `achem_benchmark` - Performance benchmark tool (`/usr/local/bin/achem_benchmark`)
- `pychem` - Python control interface (`/usr/local/bin/pychem`)
- Configuration files copied to `~/.config/CHEM/`

## Usage

### Running CHEM

```bash
# Run with default configuration
CHEM

# Run with custom config file
CHEM --config /path/to/config.json
```


### PyCHEM Python Interface

```python
from pychem import PyCHEM

# Connect to running CHEM instance
chem = PyCHEM(host="localhost", port=5555)

# Update channel parameters
chem.update_distance("node1", "node2", 100.0)  # 100 meters
chem.update_pathloss("free_space")
chem.enable_awgn("node1", "node2", snr=20.0)
```

## Architecture

### Overview

The Controller receives configuration messages from nodes and creates the interfaces (Channels, Transmitters, and Receivers). Each frequency band has its own Intermediate that handles signal processing.

### Signal Flow

```
                                   FR1-CHANNEL
                                 ______________
 UHD-TX-1 --\                   |              |
             CHEM-Rx-fr1 -----> | Intermediate | ---> CHEM-Tx-fr1-id1 -> UHD-RX-1
 UHD-TX-2 --/                   |______________|

                                   FR2-CHANNEL
                                 ______________
                                |              | ---> CHEM-Tx-fr2-id2 -> UHD-RX-2
 UHD-TX-3 -> CHEM-Rx-fr2 -----> | Intermediate |
                                |______________| ---> CHEM-Tx-fr2-id3 -> UHD-RX-3
```

### Components

**Intermediate**
- Core signal processing unit for each frequency band
- Per-receiver worker threads for parallel processing
- Applies channel effects: path loss, noise, Doppler, multipath

**Transmitter (CHEM-TX)**
- UDP socket-based output to UHD receivers
- One transmitter per destination node
- Can be dynamically attached/detached from Intermediate

**Receiver (CHEM-RX)**
- UDP socket-based input from UHD transmitters
- Multiple transmitters can feed into one receiver
- Signals routed to Intermediate worker queues

### Block Diagram

```
    Channel Info--\ /--- PyCHEM
                   |
          _________|_____________
         |         |             |
         |         |          /--|--0-->CHEM-TX
         |     ____|____     /   |
    RX-->|----|=CHANNEL=|---|    |
         |         |         \   |
         |         |          \--|--0-->CHEM-TX
         |_________|_____________|
                   |
    Monitoring ---/-\--- Prometheus
```

* Prometheus is still in progress. I haven't finished that feature yet. 

## Documentation

See the [`docs/`](docs/) directory for detailed documentation:

- [Overview](docs/overview.md) : system design and component overview
- [Channel Models](docs/channel-models.md) : propagation model details
- [PyCHEM](docs/pychem.md) : Python API reference
- [Antennas](docs/antennas.md) : antenna pattern configuration
- [Extensions](docs/extensions.md) : extension system overview
- [Extension Development](docs/extension-development.md) : building custom channel model extensions
- [Under the Hood](docs/under-the-hood.md) : UDP streaming, fragmentation, SIMD optimization
- [Benchmark](docs/benchmark.md) : performance benchmarking guide

## Configuration

Configuration is stored in JSON format. Default location: `~/.config/CHEM/config.json`

See `configs/config.json` for example configuration.

## License

This project is licensed under the GNU Affero General Public License v3.0 - see the LICENSE file for details.

## Contributing

Contributions are welcome! Please submit pull requests to the main repository.

### Benchmark Tool

The `achem_benchmark` tool measures CHEM performance across different TX/RX configurations:

```bash
achem_benchmark [options]

Options:
  -m, --max-connections N   Maximum TX/RX pairs (default: 8)
  -s, --signals N           Signals per scenario (default: 10000)
  -f, --frequency F         Frequency in Hz (default: 2.68e9)
  -r, --sample-rate R       Sample rate in Hz (default: 11520000)
  -c, --channels N          Channels per node (default: 2)
  -t, --threshold P         Drop threshold percentage (default: 25)
  -o, --output FILE         Output CSV file (default: achem_benchmark_results.csv)
  -v, --verbose             Enable verbose output
  -h, --help                Show this help message
```

Example:
```bash
# Run benchmark with 8x8 configuration at 30.72 MHz sample rate
achem_benchmark -m 8 -s 10000 -r 30720000 -v -o results.csv
```

## Authors

- Anil Gurses <agurses@ncsu.edu>
