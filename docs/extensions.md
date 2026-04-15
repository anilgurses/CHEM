# Extensions

CHEM supports a pluggable extension system that allows external channel modeling backends to integrate with the emulator. Extensions can replace or augment CHEM's built-in propagation models with custom channel impulse responses, ray-traced propagation, or any other channel modeling technique that you might think of (because I can't think of :)).

## Architecture

The extension system consists of three parts:

1. **`ChannelExtension`** : an abstract base class that every extension implements.
2. **`ExtensionRegistry`** : a central registry that manages extension lifecycles and routes commands.
3. **`CMake auto-discovery`** : extensions live under `extensions/<name>/` and are compiled automatically.

### How extensions interact with the channel pipeline

Each `Channel` object in CHEM can have an **active extension**. When an extension activates itself on a channel (via `setActiveExtension()`), it declares whether it **replaces** CHEM's built-in statistical propagation models or augments them:

- **Bypass mode** (`bypassesPathLoss() = true`): the extension replaces CHEM's statistical propagation models (FSPL, Two-Ray, 3GPP, Okumura-Hata (experimental), Longley-Rice (experimental)). The extension is responsible for providing its own propagation loss. It can be either encoded in CIR taps, or set explicitly via `setExtensionPathLossDb()` on each channel, or both. CHEM's statistical model is not executed, basically bypassed.
- **Overlay mode** (`bypassesPathLoss() = false`): CHEM's statistical propagation model still runs. The extension adds impairments on top of it (e.g. additional multipath, fading, interference). More testing might be needed for this as you might end up having duplicate impairments.

This behavior is declared by the extension class itself, not configured externally.

## Configuration

Extensions are configured under the `extensions` key in `config.json`:

```json
{
  "extensions": {
    "sionna": {
      "enabled": true,
      "serverUrl": "http://localhost:8000",
      "updateRateMs": 500,
      "maxDepth": 3,
      "numSamples": 100000,
      "referenceOrigin": {
        "lat": 35.7272,
        "lon": -78.6960,
        "alt": 0.0
      }
    }
  }
}
```

Each key under `extensions` matches the extension's `name()`. If `enabled` is `true` (or absent), CHEM calls `onStart()` with the extension's config block at startup.

## Runtime control

Extensions are controlled at runtime through two coordinator commands:

### `EXT`: route a command to a specific extension

```json
{
  "CMD": "EXT",
  "extension": "sionna",
  "action": "start",
  "params": {
    "serverUrl": "http://192.168.8.173:8000",
    "referenceOrigin": {"lat": 35.7272, "lon": -78.6960, "alt": 0.0}
  }
}
```

The `action` and `params` fields are passed directly to the extension's `handleCommand()` method. Supported actions depend on the extension.

### `EXT_LIST`: list all registered extensions and their status

```json
{"CMD": "EXT_LIST"}
```

Returns:

```json
{
  "status": "success",
  "extensions": {
    "sionna": {
      "running": true,
      "serverUrl": "http://localhost:8000",
      "sceneId": "abc123",
      "updateRateMs": 500,
      "maxDepth": 3
    }
  }
}
```

## PyCHEM API

PyCHEM provides generic extension methods plus convenience wrappers:

```python
from pychem import ChemClient

client = ChemClient(addr="localhost", port=5000)

# Generic extension control
result = client.extension("sionna", "start", {
    "serverUrl": "http://localhost:8000",
    "referenceOrigin": {"lat": 35.7272, "lon": -78.6960, "alt": 0.0},
})

# List all extensions and their status
status = client.extension_list()

# Sionna convenience methods
client.sionna_start(server_url="http://localhost:8000")
client.sionna_stop()
status = client.sionna_status()
```

See [PyCHEM](pychem.md) for the full client API reference.

### TUI

The PyCHEM TUI's Extensions screen is fully dynamic. It discovers registered extensions via `EXT_LIST` at runtime and builds menus from each extension's `configSchema`. No hardcoded extension knowledge is needed in the TUI. See the [Extension Development Guide](extension-development.md#tui-integration-via-getconfigschema) for details on how `getConfigSchema()` drives the UI.

## Available extensions

| Extension | Description | Path loss behavior |
|---|---|---|
| [Sionna RT](sionna.md) | GPU-accelerated ray tracing via Sionna RT server | Bypasses statistical models |

## Developing a new extension

See the [Extension Development Guide](extension-development.md) for a step-by-step walkthrough of creating a new extension.

## Source files

| File | Description |
|---|---|
| `include/chem/extensions/extension.h` | `ChannelExtension` abstract base class |
| `include/chem/extensions/extension_registry.h` | `ExtensionRegistry` class declaration |
| `src/chem/extensions/extension_registry.cpp` | Registry implementation |
| `extensions/sionna/` | Sionna RT extension (reference implementation) |
