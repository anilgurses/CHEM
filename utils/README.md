# Utils

Utility scripts for running benchmarks, generating support files, and controlling CHEM during development.

- `pychem.py`: CLI/TUI client for controlling the CHEM coordinator and applying channel settings/profiles.
- `run_benchmark_suite.sh`: Runs a predefined `achem_benchmark` sweep and writes timestamped CSV/log/summary outputs.
- `postprocess_benchmark.py`: Converts benchmark CSV data (single file or suite directory) into plots.
- `generate_antenna_headers.py`: Generates C++ antenna `.h`/`.cpp` pairs from measured pattern files in `data/antenna/`.

Quick start:

```bash
python3 utils/pychem.py --help
bash utils/run_benchmark_suite.sh
python3 utils/postprocess_benchmark.py benchmark_results_YYYYMMDD_HHMMSS/
python3 utils/generate_antenna_headers.py
```
