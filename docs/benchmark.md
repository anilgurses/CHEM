# ACHEM Benchmark Tool

The ACHEM Benchmark tool (`achem_benchmark`) is a performance testing utility that measures how many nodes your system can support for channel emulation.

## Overview

The benchmark generates synthetic signals and measures:

- End-to-end latency through the channel emulator
- Signal drop rates under various node configurations
- CPU and memory usage
- Processing throughput (signals/second)

## Building the Benchmark

The benchmark is built separately from the main CHEM executable. Enable it with the `ENABLE_BENCHMARK` CMake option:

```bash
cmake -S . -B build -DENABLE_BENCHMARK=ON
cmake --build build
```

The benchmark binary will be located at `build/bin/<build_type>/achem_benchmark`.

## Usage

```bash
achem_benchmark [options]
```

### Options

| Option | Description | Default |
|--------|-------------|---------|
| `-m, --max-connections N` | Maximum TX/RX pairs to test | 8 |
| `-M, --max-mode` | Stop at first failure, double nodes each step | off |
| `-d, --detailed-timing` | Collect detailed per-step channel timing | off |
| `-s, --signals N` | Number of signals per scenario | 10000 |
| `-i, --injection-interval US` | Microseconds between signal injections | 100 |
| `-C, --cir-taps N` | Number of random CIR taps (0 = disabled) | 0 |
| `-f, --frequency F` | Frequency in Hz | 2.68e9 |
| `-r, --sample-rate R` | Sample rate in Hz | 11520000 |
| `-c, --channels N` | Channels per node | 1 |
| `-l, --max-latency MS` | Max acceptable latency in milliseconds | 5 |
| `-t, --threshold P` | Drop threshold percentage (failure criteria) | 10 |
| `-o, --output FILE` | Output CSV file | achem_benchmark_results.csv |
| `-v, --verbose` | Enable verbose output | off |
| `-h, --help` | Show help message | - |

## Benchmark Modes

### Full Mode (default)

Runs all scenarios from 1x1 up to the maximum connections, incrementing by 1:

```bash
achem_benchmark -m 8
```

This tests: 1x1, 2x2, 3x3, 4x4, 5x5, 6x6, 7x7, 8x8

### Max Mode

Doubles the node count each iteration and stops at the first failure. This is useful for quickly determining the maximum supported nodes:

```bash
achem_benchmark -M -m 64
```

This tests: 1x1, 2x2, 4x4, 8x8, 16x16, ... until failure or max-connections is reached.

### Detailed Timing Mode

Use `-d` to collect per-step channel impairment timing:

```bash
achem_benchmark -m 16 -M -d
```

This adds detailed breakdown of channel processing steps:
- **Resample**: Resampling and gain scaling
- **CIR/Multipath**: Channel impulse response application
- **Path Loss**: Path loss calculation and application
- **AWGN Noise**: Noise generation and addition
- **Freq Offset**: Frequency offset and Doppler application

### CIR Benchmarking

Use `-C` to enable CIR/multipath processing with random taps:

```bash
# Benchmark with 16 CIR taps
achem_benchmark -m 20 -M -d -C 16

# Benchmark with 64 CIR taps (heavier multipath)
achem_benchmark -m 20 -M -d -C 64
```

This generates random complex-valued CIR taps with exponentially decaying magnitude for each channel. Useful for measuring the performance impact of multipath processing.

### Injection Rate Control

Use `-i` to control how fast signals are injected:

```bash
# Slower injection (200us between signals)
achem_benchmark -m 20 -M -i 200

# Faster injection (50us between signals)
achem_benchmark -m 20 -M -i 50

# Maximum rate (no delay)
achem_benchmark -m 20 -M -i 0
```

## Example Output

```
========================================
      CHEM Benchmark Tool
========================================

Configuration:
  Mode: Max (stop on first failure)
  Max connections: 64
  Signals per scenario: 10000
  Sample rate: 11.52 MHz
  Frequency: 2.68 GHz
  Channels per node: 1
  Max latency: 5 ms
  Drop threshold: 25%

----------------------------------------
Running scenario: 1TX x 1RX
----------------------------------------

Results:
  Signals generated: 10000
  Expected outputs:  10000
  Signals received:  10000
  Dropped (too old): 0
  Drop rate:         0.00%

Signal Latency (microseconds):
  Recv->ProcStart:   avg=12.34 min=5.21 max=45.67
  Processing:        avg=8.90 min=4.12 max=23.45
  ProcEnd->Send:     avg=3.21 min=1.00 max=12.34
  Total E2E:         avg=24.45 min=10.33 max=81.46

Channel Impairment Step Latency (microseconds):
  Resample:          avg=1.24 min=0.71 max=14.40
  CIR/Multipath:     avg=0.17 min=0.01 max=2.53
  Path Loss:         avg=0.18 min=0.06 max=4.62
  AWGN Noise:        avg=1.81 min=1.07 max=11.85
  Freq Offset:       avg=0.03 min=0.01 max=1.09

Performance:
  Duration:          1234 ms
  Throughput:        8103 signals/sec

Resource Usage:
  CPU User Time:     567.89 ms
  CPU System Time:   123.45 ms
  Peak Memory:       45678 KB

Status: PASSED (threshold: 25% drop rate)

...

========================================
  BENCHMARK STOPPED (first failure)
========================================

  Maximum supported nodes: 8
  Failed at: 16 nodes
  (Based on 25% drop rate threshold)
```

## Output CSV

Results are written to a CSV file (default: `achem_benchmark_results.csv`) containing detailed metrics for each scenario:

- Node counts (TX/RX)
- Signal statistics (generated, received, dropped)
- Drop rate percentage
- Throughput (signals/second)
- Latency breakdown (min/avg/max for each stage)
- Resource usage (CPU time, memory)

## Interpreting Results

### Drop Rate

The drop rate indicates what percentage of signals were not delivered within the acceptable latency window. A scenario **fails** if the drop rate exceeds the threshold (default 25%).

### Maximum Supported Nodes

This is the highest node count that passed the drop rate threshold. Use this to determine how many SDR nodes your system can reliably emulate.

### Latency Breakdown

**Signal Flow Latency:**
- **Recv->ProcStart**: Time from signal reception to processing start (queue wait time)
- **Processing**: Time spent in channel processing (all impairments combined)
- **ProcEnd->Send**: Time from processing completion to transmission
- **Total E2E**: End-to-end latency through the entire pipeline

**Channel Impairment Latency** (with `-d` flag):
- **Resample**: Resampling and gain scaling time
- **CIR/Multipath**: Channel impulse response convolution (requires `-C` to enable)
- **Path Loss**: Path loss model calculation and signal attenuation
- **AWGN Noise**: Noise generation and addition
- **Freq Offset**: Frequency offset and Doppler shift application

## Tips

1. **Start with max mode** (`-M`) to quickly find your system's limit
2. **Use detailed timing** (`-d`) to identify processing bottlenecks
3. **Test with CIR** (`-C 16`) to measure multipath processing overhead
4. **Adjust injection rate** (`-i`) to simulate different traffic patterns
5. **Increase max-latency** (`-l`) if you have a higher latency tolerance
6. **Lower the threshold** (`-t`) for stricter quality requirements
7. **Use more signals** (`-s`) for more statistically significant results
8. **Run on an idle system** for consistent, reproducible results
