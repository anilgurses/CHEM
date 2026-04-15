# Configuration

CHEM is configured using a JSON file. This page intentionally stays minimal and points you to a working example.

## Configuration File

Start from the example configuration in `configs/config.json` and adjust it for your setup.
This page mirrors the shipped sample so the example stays aligned with the binary's current config loader.

### Example Configuration

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

### Extensions

The `extensions` key configures pluggable channel modeling (or RF emulation) backends. Each sub-key names a registered extension. Set `enabled` to `true` to start the extension automatically at CHEM startup, or control it at runtime via PyCHEM. See [Extensions](../extensions.md) for details.

### Optional CPU / NUMA Keys

- `maxCores`: Caps oneTBB worker parallelism.
- `numaEnabled`: Enables NUMA-aware process binding when libnuma is available.
- `cpuAffinity`: CPU affinity mask string such as `"0-7"`.
- `numaNode`: Pins CHEM to a specific NUMA node instead of auto-selecting one.

## Next steps

- Keep `logDirectory` somewhere writable for your environment.
- Increase `maxNode` if you plan to attach more nodes.
- See [Extensions](../extensions.md) to integrate external channel models like Sionna RT (and possibly more).
