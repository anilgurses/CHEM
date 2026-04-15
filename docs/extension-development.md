# Extension Development Guide

This guide walks through creating a new CHEM extension from scratch. By the end you will have a working extension that integrates with the channel pipeline, responds to runtime commands, and is auto-discovered by the build system.

## Overview

A CHEM extension is a C++ class that inherits from `chem::ChannelExtension` and is registered in the `ExtensionRegistry`. Extensions can:

- Apply custom CIR (channel impulse response) taps to any channel link
- Replace or augment the built-in statistical propagation models
- Run background threads (e.g. polling an external server)
- Respond to runtime commands from PyCHEM or the coordinator protocol
- Access the full node map and intermediate (frequency) map

## File layout

Create a directory under `extensions/` with `include/` and `src/` subdirectories:

```
extensions/
  myext/
    include/
      myext.h
    src/
      myext.cpp
```

The CMake build system auto-discovers all directories under `extensions/*/` and compiles their sources. No changes to `CMakeLists.txt` or `SourcesAndHeaders.cmake` are needed.

## Step 1: Define the extension class

Create `extensions/myext/include/myext.h`:

```cpp
#pragma once

#include <atomic>
#include <string>

#include "chem/extensions/extension.h"

namespace chem {

class MyExtension : public ChannelExtension {
   public:
    MyExtension() = default;
    ~MyExtension() override = default;

    // -- Required interface --

    std::string name() const override { return "myext"; }

    bool onStart(const nlohmann::json& config) override;
    void onStop() override;

    bool isRunning() const override { return m_running.load(); }

    nlohmann::json handleCommand(const std::string& action,
                                 const nlohmann::json& params) override;

    nlohmann::json getStatus() const override;

    // -- Optional overrides --

    nlohmann::json getConfigSchema() const override;

    // Return true if this extension replaces CHEM's statistical propagation
    // models with its own loss computation. Return false (default) to layer
    // impairments on top of CHEM's built-in propagation models.
    // Check for more details in the documentation.
    bool bypassesPathLoss() const override { return false; }

   private:
    std::atomic<bool> m_running{false};
};

}  // namespace chem
```

**`name()`** : must return a unique string. This is used as the key in `config.json`, the `EXT` command's `extension` field, and the `EXT_LIST` response.

**`bypassesPathLoss()`** : controls how the extension interacts with CHEM's statistical propagation models (FSPL, Two-Ray, 3GPP 38.901, Okumura-Hata, Longley-Rice):

| Return value | Behavior | Use case |
|---|---|---|
| `false` (default) | CHEM still computes path loss using the configured statistical model. The extension adds impairments on top (e.g. additional multipath, fading). | Fading models, interference injection, custom multipath |
| `true` | CHEM does **not** run its statistical propagation model. The extension provides its own propagation loss. This can be either encoded in CIR taps, or set explicitly per-channel via `setExtensionPathLossDb()`, or both. | Ray tracing, measurement-based models, deterministic propagation |

When `bypassesPathLoss()` returns `true`, the extension can provide path loss through two mechanisms:

1. **CIR taps** : propagation loss encoded in the tap coefficients (applied via convolution before the path loss stage).
2. **Explicit path loss** : call `channel.setExtensionPathLossDb(pl_db)` to set a dB path loss value that replaces the statistical model output. Defaults to 0 dB if not set.

## Step 2: Implement the extension

Create `extensions/myext/src/myext.cpp`:

```cpp
#include "myext.h"

#include "spdlog/fmt/fmt.h"
#include "chem/common.h"
#include "chem/channel/intermediate.h"

namespace chem {

bool MyExtension::onStart(const nlohmann::json& config) {
    // Parse extension-specific configuration
    // 'config' is the JSON object from config.json under extensions.myext
    //
    // Example:
    //   int rate = config.value("updateRateMs", 1000);

    m_running.store(true);
    LOG_INFO("MYEXT", "MyExtension started");
    return true;
}

void MyExtension::onStop() {
    if (!m_running.load()) return;
    m_running.store(false);
    LOG_INFO("MYEXT", "MyExtension stopped");
}

nlohmann::json MyExtension::handleCommand(const std::string& action,
                                          const nlohmann::json& params) {
    nlohmann::json resp;

    if (action == "start") {
        onStart(params);
        resp["status"] = "success";
    } else if (action == "stop") {
        onStop();
        resp["status"] = "success";
    } else if (action == "status") {
        resp = getStatus();
    } else {
        resp["status"] = "fail";
        resp["message"] = "Unknown action: " + action;
    }

    return resp;
}

nlohmann::json MyExtension::getStatus() const {
    return {
        {"running", m_running.load()},
    };
}

nlohmann::json MyExtension::getConfigSchema() const {
    return {
        {"updateRateMs", {{"type", "integer"}, {"default", 1000}}},
    };
}

}  // namespace chem
```

## Step 3: Register the extension

In `src/chem/channel/coordinator.cpp`, add your extension's header and register it in the constructor:

```cpp
#include "myext.h"

// In the Coordinator constructor, after the existing registrations:
m_extensionRegistry.registerExtension(
    std::make_unique<chem::MyExtension>());
```

This is the only change needed in core CHEM code.

## Step 4: Add configuration

Add your extension's config block to `config.json`:

```json
{
  "extensions": {
    "myext": {
      "enabled": true,
      "updateRateMs": 1000
    }
  }
}
```

If `enabled` is `true`, `onStart()` is called automatically at CHEM startup with this JSON object. If `enabled` is `false` or omitted, the extension is registered but not started. It can be started later via the `EXT` command.

## Working with channels

Extensions access CHEM's shared state through two base class pointers set automatically by the registry:

- **`m_nodeMap`** (`std::map<std::string, chem::Node>*`) : all registered nodes, keyed by node ID.
- **`m_intermediateMap`** (`std::map<double, std::shared_ptr<chem::Intermediate>>*`) : all frequency intermediates, keyed by center frequency in Hz.

### Applying CIR taps to a channel

```cpp
#include "chem/channel/intermediate.h"
#include "chem/dsp/channel.h"

void MyExtension::applyTaps() {
    if (!m_intermediateMap) return;

    for (auto& [freq, intermediate] : *m_intermediateMap) {
        auto& channelList = intermediate->getChannelList();

        for (auto& [key, channel] : channelList) {
            // key is a pair of (srcId, destId)
            const auto& [srcId, destId] = key;

            // Build FIR taps (complex coefficients at sample-spaced delays)
            chem::signal_v taps = {
                {1.0f, 0.0f},     // direct path
                {0.0f, 0.0f},     // 1-sample delay (no energy)
                {0.3f, 0.1f},     // 2-sample delay reflection
            };

            // Apply taps via the Intermediate's updateCIR method
            chId ch{0, 0};  // port 0 -> port 0
            intermediate->updateCIR(srcId, destId, ch, taps);

            // Mark the extension as active on this channel
            // Second argument: whether this extension bypasses statistical models
            channel.setActiveExtension("myext", bypassesPathLoss());
        }
    }
}
```

### Setting extension-provided path loss

If your extension computes its own path loss (separate from CIR taps), set it per-channel. This value replaces the statistical model output when `bypassesPathLoss()` returns `true`:

```cpp
// Extension computes path loss from its own model (e.g. measurement data)
float computed_pl_db = myModel.computePathLoss(srcPos, destPos, freq);
channel.setExtensionPathLossDb(computed_pl_db);
```

If you encode all propagation in CIR taps (like Sionna RT), you don't need to call `setExtensionPathLossDb()`. It defaults to 0 dB.

### Clearing extension state on stop

When your extension stops, clear the active extension flag on all channels it touched:

```cpp
void MyExtension::onStop() {
    if (!m_running.load()) return;
    m_running.store(false);

    // Clear extension flags from all channels
    if (m_intermediateMap) {
        for (auto& [freq, intermediate] : *m_intermediateMap) {
            for (auto& [key, channel] : intermediate->getChannelList()) {
                if (channel.getActiveExtension() == name()) {
                    channel.clearActiveExtension();
                }
            }
        }
    }
}
```

## Running a background thread

Many extensions need a periodic update loop (e.g. polling a server, recomputing CIR). Use a `std::thread` started in `onStart()`:

```cpp
bool MyExtension::onStart(const nlohmann::json& config) {
    m_updateRateMs = config.value("updateRateMs", 1000);
    m_running.store(true);
    m_thread = std::thread(&MyExtension::pollLoop, this);
    return true;
}

void MyExtension::onStop() {
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
    // ... clear extension flags ...
}

void MyExtension::pollLoop() {
    while (m_running.load()) {
        // Do work: recompute CIR, sync positions, etc.
        applyTaps();

        std::this_thread::sleep_for(
            std::chrono::milliseconds(m_updateRateMs));
    }
}
```

## Controlling from PyCHEM

Once registered, your extension is available through PyCHEM's generic extension API:

```python
from pychem import ChemClient

client = ChemClient(addr="localhost", port=5000)

# Start the extension at runtime
client.extension("myext", "start", {"updateRateMs": 500})

# Query status
print(client.extension("myext", "status"))

# Stop
client.extension("myext", "stop")

# List all extensions
print(client.extension_list())
```

You can also add convenience wrapper methods in `pychem.py` following the pattern used by Sionna:

```python
def myext_start(self, **params) -> dict:
    return self.extension("myext", "start", params)

def myext_stop(self) -> dict:
    return self.extension("myext", "stop")
```

### TUI integration via `getConfigSchema()`

The PyCHEM TUI builds its Extensions menu dynamically from the `EXT_LIST` response. When `getConfigSchema()` returns a non-empty schema, the TUI uses it to:

- **Start prompts**: each field in the schema becomes an input prompt (with type-appropriate parsing and the default value pre-filled).
- **Configure actions**: each non-object field appears as a "Configure `<field>`" menu option, with the current value from `getStatus()` pre-filled.

This means a new extension gets a fully functional TUI without any Python code changes. Just implement `getConfigSchema()`:

```cpp
nlohmann::json MyExtension::getConfigSchema() const {
    return {
        {"serverUrl", {{"type", "string"}, {"default", "http://localhost:5000"}}},
        {"updateRateMs", {{"type", "integer"}, {"default", 1000}, {"min", 50}}},
        {"threshold", {{"type", "number"}, {"default", 0.5}}},
    };
}
```

Supported schema types: `string`, `integer`, `number`, `object` (with nested `properties`).

## ChannelExtension interface reference

| Method | Required | Description |
|---|---|---|
| `name()` | Yes | Unique extension identifier string |
| `onStart(config)` | Yes | Initialize and start the extension. `config` is the JSON object from `config.json`. Return `true` on success. |
| `onStop()` | Yes | Stop the extension, clean up resources, clear channel flags. |
| `isRunning()` | Yes | Return whether the extension is currently active. |
| `handleCommand(action, params)` | Yes | Handle a runtime command. Return a JSON response with at least a `status` field. |
| `getStatus()` | Yes | Return a JSON object with current state for `EXT_LIST` responses. Include fields matching `configSchema` keys so the TUI can show current values. |
| `getConfigSchema()` | No | Return a JSON schema describing accepted configuration keys. Used by the TUI to build start prompts and configure actions dynamically. Default: empty object. |
| `bypassesPathLoss()` | No | Return `true` if the extension replaces CHEM's statistical propagation models. The extension then provides loss via CIR taps and/or `setExtensionPathLossDb()`. Default: `false`. |

## Channel API reference (for extensions)

| Method | Description |
|---|---|
| `setActiveExtension(name, bypassPathLoss)` | Mark this channel as managed by the named extension. `bypassPathLoss` controls whether CHEM's statistical models are skipped. |
| `clearActiveExtension()` | Remove extension ownership. Resets bypass flag and extension path loss to 0. |
| `setExtensionPathLossDb(pl_db)` | Set a path loss value (dB) that replaces the statistical model output when `bypassesPathLoss` is true. Defaults to 0 dB. |
| `getExtensionPathLossDb()` | Read the current extension-provided path loss. |

## Checklist

- [ ] Extension class inherits `chem::ChannelExtension`
- [ ] `name()` returns a unique identifier
- [ ] Files placed under `extensions/<name>/include/` and `extensions/<name>/src/`
- [ ] Extension registered in `coordinator.cpp` constructor
- [ ] `onStop()` clears `activeExtension` on all channels it touched
- [ ] `bypassesPathLoss()` returns the correct value for your use case
- [ ] If bypassing: extension provides loss via CIR taps and/or `setExtensionPathLossDb()`
- [ ] Config block added to `config.json` under `extensions.<name>`
