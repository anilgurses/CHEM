# Installation

This page covers the minimal steps to build CHEM from source.

## Prerequisites

- C++ Compiler (supporting C++17)
- CMake (version 3.10 or higher)
- Doxygen (optional, for API docs)

## Building the Project

From the repository root:

```bash
cmake -S . -B build
cmake --build build
```

### Build Options

| Option | Description | Default |
|--------|-------------|---------|
| `ENABLE_BENCHMARK` | Build the benchmark tool | OFF |
| `ENABLE_DEBUG_LOG` | Enable debug logging | OFF |
| `ENABLE_PROFILING` | Enable profiling | OFF |

Example with benchmark enabled:

```bash
cmake -S . -B build -DENABLE_BENCHMARK=ON
cmake --build build
```

## Running the Application

After a successful build, look for the binaries under `build/` (often `build/bin/` depending on the generator).

```bash
./build/bin/CHEM
```

### Running the Benchmark

If built with `ENABLE_BENCHMARK=ON`:

```bash
./build/bin/achem_benchmark --help
./build/bin/achem_benchmark -M -m 16  # Quick max-mode test
```

See [Benchmark](../benchmark.md) for detailed usage.
