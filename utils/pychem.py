#!/usr/bin/env python3

# Copyright 2022 Anil Gurses
#
# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

import curses
import curses.textpad
import yaml

MAX_JSON_LENGTH = 4096
DEFAULT_CONFIG_PATH = Path("~/.config/uhd/conf.yaml").expanduser()
TUI_POLL_TIMEOUT_MS = 200
TX_ACTIVE_WINDOW_S = 5

_ANSI_ESCAPE_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


def _strip_ansi(s: str) -> str:
    return _ANSI_ESCAPE_RE.sub("", s)

CIR_SCENARIOS: dict[str, list[list[float]]] = {
    "None (clear)": [],
    "Single-path (unit)": [[1.0, 0.0]],
    "Two-path echo (3 samples, -6 dB)": [
        [1.0, 0.0],
        [0.0, 0.0],
        [0.0, 0.0],
        [0.501187, 0.0],  # 10^(-6/20)
    ],
    "Three-path (0,2,5 samples)": [
        [1.0, 0.0],
        [0.0, 0.0],
        [0.4, 0.0],
        [0.0, 0.0],
        [0.0, 0.0],
        [0.0, 0.2],
    ],
    "Dense 8-tap (fixed)": [
        [0.70, 0.00],
        [0.25, -0.10],
        [0.10, 0.18],
        [0.05, -0.07],
        [0.03, 0.02],
        [0.02, -0.01],
        [0.01, 0.00],
        [0.005, 0.002],
    ],
}

BUILTIN_PROFILES: dict[str, dict] = {
    # Ref. include/chem/aerpaw/portable_node.h | include/chem/aerpaw/fixed_node.h
    "AERPAW Outdoor": {
        "name": "AERPAW Outdoor",
        "description": "Default AERPAW outdoor UAV experiment",
        "note": "Two-ray path loss (groundCoeff=-0.3) for AERPAW node types. "
                "Suitable for open-air UAV flights with moderate shadowing.",
        "propagation": {"model": "2_RAY", "groundCoeff": -0.3},
        "shadowingStd": 0.0,
        "sourcePowerDbfs": {
            "AERPAW Portable": -12.0,
            "AERPAW Fixed": -12.0,
        },
    },
    "Lab / Loopback": {
        "name": "Lab / Loopback",
        "description": "Cabled bench test, no impairments",
        "note": "Disables all channel impairments (propagation, shadowing). "
                "Use for cabled RF loopback or baseband verification.",
        "propagation": {"model": "NONE"},
        "shadowingStd": 0.0,
    },
    "Urban Dense (3GPP UMi)": {
        "name": "Urban Dense (3GPP UMi)",
        "description": "Dense urban scenario using 3GPP UMi-StreetCanyon",
        "note": "3GPP 38.901 UMi-StreetCanyon model with higher shadowing. "
                "Represents short-range links in dense urban canyons.",
        "propagation": {"model": "3GPP_38_901", "scenario": "UMi-StreetCanyon"},
        "shadowingStd": 3.0,
        "sourcePowerDbfs": {
            "AERPAW Portable": -12.0,
            "AERPAW Fixed": -12.0,
        },
    },
    "Rural Macro (3GPP RMa)": {
        "name": "Rural Macro (3GPP RMa)",
        "description": "Rural scenario using 3GPP RMa",
        "note": "3GPP 38.901 RMa model with low shadowing. "
                "Represents long-range macro cells in rural areas.",
        "propagation": {"model": "3GPP_38_901", "scenario": "RMa"},
        "shadowingStd": 1.5,
        "sourcePowerDbfs": {
            "AERPAW Portable": -12.0,
            "AERPAW Fixed": -12.0,
        },
    },
}


def _profiles_dir() -> Path:
    return Path("~/.config/chem/profiles").expanduser()


def load_custom_profiles() -> dict[str, dict]:
    d = _profiles_dir()
    if not d.is_dir():
        return {}
    profiles: dict[str, dict] = {}
    for p in sorted(d.glob("*.json")):
        try:
            with open(p, "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, dict) and "name" in data:
                profiles[data["name"]] = data
        except Exception:
            pass
    return profiles


def save_profile(profile: dict) -> Path:
    d = _profiles_dir()
    d.mkdir(parents=True, exist_ok=True)
    name = str(profile.get("name", "untitled"))
    safe = re.sub(r"[^\w\s\-]", "", name).strip().replace(" ", "_") or "profile"
    path = d / f"{safe}.json"
    with open(path, "w", encoding="utf-8") as f:
        json.dump(profile, f, indent=2)
    return path


def all_profiles() -> dict[str, dict]:
    return {**BUILTIN_PROFILES, **load_custom_profiles()}


class ChemConnectionError(RuntimeError):
    pass


def _ellipsis(text: str, width: int) -> str:
    if width <= 0:
        return ""
    if len(text) <= width:
        return text
    if width <= 3:
        return text[:width]
    return text[: width - 3] + "..."


def _format_mhz(value: Any) -> str:
    try:
        return f"{float(value):.3f}".rstrip("0").rstrip(".") + " MHz"
    except Exception:
        return str(value)


def _format_msa(value: Any) -> str:
    try:
        return f"{float(value) / 1_000_000:.3f}".rstrip("0").rstrip(".") + " MSa/s"
    except Exception:
        return str(value)


def _format_distance(value: Any) -> str:
    try:
        return f"{float(value):.2f} m"
    except Exception:
        return str(value)


def _is_tx_active(info: dict[str, Any]) -> bool:
    if isinstance(info.get("txActive"), bool):
        return bool(info.get("txActive"))
    try:
        return float(info.get("msSinceLastTx")) <= TX_ACTIVE_WINDOW_S * 1000
    except Exception:
        return False


def _format_tx_marker(info: dict[str, Any]) -> str:
    return "●" if _is_tx_active(info) else "○"


def _safe_json(obj: Any) -> str:
    try:
        return json.dumps(obj, indent=2, sort_keys=True)
    except Exception:
        return str(obj)

def _value_type(v: Any) -> str:
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, int):
        return "int"
    if isinstance(v, float):
        return "float"
    if isinstance(v, str):
        return "string"
    if isinstance(v, list):
        return "list"
    if isinstance(v, dict):
        return "object"
    return "null"


def _flatten_scalars(data: Any, prefix: str = "") -> list[tuple[str, Any]]:
    out: list[tuple[str, Any]] = []
    if not isinstance(data, dict):
        return out
    for k, v in data.items():
        key = f"{prefix}{k}"
        if isinstance(v, dict):
            out.extend(_flatten_scalars(v, prefix=f"{key}."))
        elif isinstance(v, (bool, int, float, str)) or v is None:
            out.append((key, v))
    return out


def _fmt_config_value(v: Any) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if v is None:
        return "null"
    return str(v)


def _scale_complex_taps(taps: list[list[float]], scale: float) -> list[list[float]]:
    try:
        s = float(scale)
    except Exception:
        s = 1.0
    if not (s == s):  # NaN guard
        s = 1.0
    return [[float(re) * s, float(im) * s] for re, im in taps]


def _format_node_row(name: str, info: dict[str, Any], widths: tuple[int, ...]) -> str:
    tx_state = _format_tx_marker(info)
    tx = _format_mhz(info.get("txFreq", "-"))
    rx = _format_mhz(info.get("rxFreq", "-"))
    txr = _format_msa(info.get("txsampleRate", "-"))
    rxr = _format_msa(info.get("rxsampleRate", "-"))
    ch = str(info.get("numChannels", "-"))
    ident = str(info.get("id", ""))
    ntype = str(info.get("nodeType", "-"))
    # Show TX/RX antenna patterns if available, otherwise fall back to unified pattern
    tx_ant = info.get("txAntennaPattern", info.get("antennaPattern", "-"))
    rx_ant = info.get("rxAntennaPattern", info.get("antennaPattern", "-"))
    if tx_ant == rx_ant:
        antenna = str(tx_ant) if tx_ant else "-"
    else:
        antenna = f"T:{tx_ant}/R:{rx_ant}"
    def _fmt_gain(val: Any) -> str:
        try:
            return f"{float(val):.1f}".rstrip("0").rstrip(".")
        except Exception:
            return str(val)

    tx_gain = _fmt_gain(info.get("txGainDb", "-"))
    rx_gain = _fmt_gain(info.get("rxGainDb", "-"))
    vehicle_addr = info.get("vehicleAddress")
    if not vehicle_addr and isinstance(info.get("vehicle"), dict):
        vehicle_addr = info["vehicle"].get("address")
    veh = "Yes" if vehicle_addr else "No"
    fmt = (
        f"{{:<{widths[0]}}}  {{:<{widths[1]}}}  {{:<{widths[2]}}}  "
        f"{{:<{widths[3]}}}  {{:<{widths[4]}}}  {{:<{widths[5]}}}  "
        f"{{:<{widths[6]}}}  {{:<{widths[7]}}}  {{:>{widths[8]}}}  "
        f"{{:>{widths[9]}}}  {{:<{widths[10]}}}  {{:<{widths[11]}}}"
    )
    rate = txr if txr != "0 MSa/s" else rxr
    return fmt.format(
        _ellipsis(tx_state, widths[0]).ljust(widths[0]),
        _ellipsis(name, widths[1]).ljust(widths[1]),
        _ellipsis(ident, widths[2]).ljust(widths[2]),
        _ellipsis(tx, widths[3]).ljust(widths[3]),
        _ellipsis(rx, widths[4]).ljust(widths[4]),
        _ellipsis(ch, widths[5]).ljust(widths[5]),
        _ellipsis(rate, widths[6]).ljust(widths[6]),
        _ellipsis(antenna, widths[7]).ljust(widths[7]),
        _ellipsis(tx_gain, widths[8]).rjust(widths[8]),
        _ellipsis(rx_gain, widths[9]).rjust(widths[9]),
        _ellipsis(veh, widths[10]).ljust(widths[10]),
        _ellipsis(ntype, widths[11]).ljust(widths[11]),
    )


class ChemClient:
    def __init__(self, addr: str = "", port: int = 5000):
        self.addr = self._resolve_addr(addr)
        self.port = port
        self.socket: Optional[socket.socket] = None
        self.last_error: Optional[str] = None

    def _resolve_addr(self, cli_addr: str) -> str:
        if cli_addr:
            return cli_addr

        config_addr = self._read_config_addr()
        if config_addr:
            return config_addr

        env_addr = os.getenv("AP_EXPENV_CHEMVM_XE")
        if env_addr:
            return env_addr

        return "localhost"

    def _read_config_addr(self) -> Optional[str]:
        if not DEFAULT_CONFIG_PATH.exists():
            return None
        try:
            with open(DEFAULT_CONFIG_PATH, "r", encoding="utf-8") as config_file:
                config = yaml.safe_load(config_file) or {}
        except Exception:
            return None

        chem_addr = config.get("chem_address")
        if chem_addr == "ENV":
            return os.getenv("AP_EXPENV_CHEMVM_XE") or None
        return chem_addr

    @property
    def connected(self) -> bool:
        return self.socket is not None

    def connect(self) -> None:
        if self.socket is not None:
            return
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(2)
        try:
            sock.connect((self.addr, self.port))
        except Exception as e:
            try:
                sock.close()
            finally:
                self.socket = None
                self.last_error = str(e)
            raise ChemConnectionError(str(e)) from e
        self.last_error = None
        self.socket = sock

    def close(self) -> None:
        if self.socket is None:
            return
        try:
            self.socket.close()
        finally:
            self.socket = None

    def _send(self, body: dict[str, Any]) -> Any:
        try:
            if self.socket is None:
                self.connect()

            payload = json.dumps(body).encode("utf-8") + b"\n"
            assert self.socket is not None
            self.socket.sendall(payload)

            # Server terminates each response with '\n' (tcp_server.cpp:72).
            # Read until the delimiter so large payloads (e.g. logs) aren't
            # truncated mid-JSON.
            chunks: list[bytes] = []
            while True:
                chunk = self.socket.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
                if b"\n" in chunk:
                    break
            data = b"".join(chunks).decode("utf-8", errors="replace").strip()
            if not data:
                return None
            return json.loads(data)
        except ChemConnectionError:
            raise
        except Exception as e:
            self.last_error = str(e)
            self.close()
            raise ChemConnectionError(str(e)) from e

    def get_version(self) -> Optional[str]:
        resp = self._send({"CMD": "GET_VERSION"})
        if not isinstance(resp, dict):
            return None
        return resp.get("version")

    def get_nodes(self) -> dict[str, Any]:
        resp = self._send({"CMD": "GET_NODES"})
        return resp if isinstance(resp, dict) else {}

    def get_antenna_patterns(self) -> list[str]:
        resp = self._send({"CMD": "GET_ANTENNA_PATTERNS"})
        if isinstance(resp, list):
            return [str(v) for v in resp]
        return []

    def get_antenna_types(self) -> list[str]:
        resp = self._send({"CMD": "GET_ANTENNA_TYPES"})
        if isinstance(resp, list):
            return [str(v) for v in resp]
        return []

    def set_node_antenna(
        self,
        node: str,
        pattern: Optional[str] = None,
        tx_pattern: Optional[str] = None,
        rx_pattern: Optional[str] = None,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"CMD": "CHG_ANTENNA", "node": str(node)}
        if pattern is not None:
            body["pattern"] = str(pattern)
        if tx_pattern is not None:
            body["tx_pattern"] = str(tx_pattern)
        if rx_pattern is not None:
            body["rx_pattern"] = str(rx_pattern)
        return self._send(body) or {}

    def get_status(self) -> dict[str, Any]:
        resp = self._send({"CMD": "GET_STATUS"})
        return resp if isinstance(resp, dict) else {}

    def get_logs(self, lines: int = 500) -> list[str]:
        resp = self._send({"CMD": "GET_LOGS", "lines": int(lines)})
        if isinstance(resp, dict):
            out = resp.get("lines", [])
            return [str(x) for x in out] if isinstance(out, list) else []
        return []

    def get_config(self) -> dict[str, Any]:
        resp = self._send({"CMD": "GET_CONFIG"})
        return resp if isinstance(resp, dict) else {}

    def set_config(
        self,
        *,
        vehicle_poll_rate_ms: Optional[int] = None,
        channel_update_rate_ms: Optional[int] = None,
        cir_max_taps: Optional[int] = None,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"CMD": "SET_CONFIG"}
        if vehicle_poll_rate_ms is not None:
            body["vehiclePollRateMs"] = int(vehicle_poll_rate_ms)
        if channel_update_rate_ms is not None:
            body["channelUpdateRateMs"] = int(channel_update_rate_ms)
        if cir_max_taps is not None:
            body["cirMaxTaps"] = int(cir_max_taps)
        resp = self._send(body)
        return resp if isinstance(resp, dict) else {}

    def get_config_file(self) -> dict[str, Any]:
        resp = self._send({"CMD": "GET_CONFIG_FILE"})
        return resp if isinstance(resp, dict) else {}

    def save_config_file(
        self,
        config: Optional[dict[str, Any]] = None,
        path: Optional[str] = None,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"CMD": "SAVE_CONFIG_FILE"}
        if config is not None:
            body["config"] = config
        if path:
            body["path"] = str(path)
        return self._send(body) or {}

    def get_channels(self) -> dict[str, Any]:
        resp = self._send({"CMD": "GET_CHANNELS"})
        return resp if isinstance(resp, dict) else {}

    def get_individual_channels(self) -> dict[str, Any]:
        resp = self._send({"CMD": "GET_IND_CHANNELS"})
        return resp if isinstance(resp, dict) else {}

    def set_pathloss(
        self,
        freq_mhz: float,
        model: str,
        ground_coeff: float = -1,
        *,
        scenario: Optional[str] = None,
        environment: Optional[str] = None,
        refractivity: Optional[float] = None,
        ground_conductivity: Optional[float] = None,
        ground_permittivity: Optional[float] = None,
        climate_zone: Optional[int] = None,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"CMD": "CHG_PL", "freq": float(freq_mhz), "plMode": model}
        if model == "2_RAY":
            body["groundCoeff"] = float(ground_coeff)
        if model == "3GPP_38_901":
            if scenario is not None:
                body["scenario"] = str(scenario)
        if model == "OKUMURA_HATA":
            if environment is not None:
                body["environment"] = str(environment)
        if model == "LONGLEY_RICE":
            if refractivity is not None:
                body["refractivity"] = float(refractivity)
            if ground_conductivity is not None:
                body["groundConductivity"] = float(ground_conductivity)
            if ground_permittivity is not None:
                body["groundPermittivity"] = float(ground_permittivity)
            if climate_zone is not None:
                body["climateZone"] = int(climate_zone)
        return self._send(body) or {}

    def set_coeff(
        self,
        freq_mhz: float,
        src: str,
        dest: str,
        p_src: int,
        p_dest: int,
        coeff: float,
    ) -> dict[str, Any]:
        body = {
            "CMD": "CHG_COEFF",
            "freq": float(freq_mhz),
            "src": src,
            "dest": dest,
            "p_src": int(p_src),
            "p_dest": int(p_dest),
            "coeff": float(coeff),
        }
        return self._send(body) or {}

    def set_noise_model(
        self, freq_mhz: float, src: str, dest: str, noise_model: str, snr: float
    ) -> dict[str, Any]:
        body = {
            "CMD": "CHG_NOISE_MDL",
            "freq": float(freq_mhz),
            "src": src,
            "dest": dest,
            "noiseModel": noise_model,
            "pwr": float(snr),
        }
        return self._send(body) or {}

    def set_frequency_offset(
        self, freq_mhz: float, src: str, dest: str, offset_hz: float
    ) -> dict[str, Any]:
        body = {
            "CMD": "CHG_FREQ_OFFSET",
            "freq": float(freq_mhz),
            "src": str(src),
            "dest": str(dest),
            "offsetHz": float(offset_hz),
        }
        return self._send(body) or {}

    def set_doppler(self, freq_mhz: float, src: str, dest: str, enabled: bool) -> dict[str, Any]:
        body = {
            "CMD": "CHG_DOPPLER",
            "freq": float(freq_mhz),
            "src": str(src),
            "dest": str(dest),
            "enabled": bool(enabled),
        }
        return self._send(body) or {}

    def set_cir(
        self,
        freq_mhz: float,
        src: str,
        dest: str,
        p_src: int,
        p_dest: int,
        taps: list[list[float]],
    ) -> dict[str, Any]:
        body = {
            "CMD": "CHG_CIR",
            "freq": float(freq_mhz),
            "src": str(src),
            "dest": str(dest),
            "p_src": int(p_src),
            "p_dest": int(p_dest),
            "taps": taps,
        }
        return self._send(body) or {}

    def get_shadow_std(self) -> Optional[float]:
        resp = self._send({"CMD": "GET_SHADOW_STD"})
        if not isinstance(resp, dict):
            return None
        return resp.get("std")

    def get_shadow_std_for_freq(self, freq_mhz: float) -> Optional[float]:
        resp = self._send({"CMD": "GET_SHADOW_STD", "freq": float(freq_mhz)})
        if not isinstance(resp, dict):
            return None
        return resp.get("std")

    def set_shadow_std(self, std: float, freq_mhz: Optional[float] = None) -> dict[str, Any]:
        body: dict[str, Any] = {"CMD": "UPD_SHADOW_STD", "std": float(std)}
        if freq_mhz is not None:
            body["freq"] = float(freq_mhz)
        return self._send(body) or {}

    def set_source_power(
        self, node: str, source_power_dbfs: Optional[float] = None
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"CMD": "CHG_SOURCE_POWER", "node": str(node)}
        if source_power_dbfs is not None:
            body["sourcePowerDbfs"] = float(source_power_dbfs)
        return self._send(body) or {}

    def set_default_pathloss(
        self,
        model: str,
        ground_coeff: float = -1.0,
        scenario: Optional[str] = None,
        *,
        environment: Optional[str] = None,
        refractivity: Optional[float] = None,
        ground_conductivity: Optional[float] = None,
        ground_permittivity: Optional[float] = None,
        climate_zone: Optional[int] = None,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"CMD": "SET_DEFAULT_PL", "plMode": model}
        if model == "2_RAY":
            body["groundCoeff"] = float(ground_coeff)
        if model == "3GPP_38_901" and scenario is not None:
            body["scenario"] = str(scenario)
        if model == "OKUMURA_HATA" and environment is not None:
            body["environment"] = str(environment)
        if model == "LONGLEY_RICE":
            if refractivity is not None:
                body["refractivity"] = float(refractivity)
            if ground_conductivity is not None:
                body["groundConductivity"] = float(ground_conductivity)
            if ground_permittivity is not None:
                body["groundPermittivity"] = float(ground_permittivity)
            if climate_zone is not None:
                body["climateZone"] = int(climate_zone)
        return self._send(body) or {}

    def apply_profile(self, profile: dict) -> list[str]:
        actions: list[str] = []

        prop = profile.get("propagation")
        if isinstance(prop, dict) and "model" in prop:
            self.set_default_pathloss(
                prop["model"],
                prop.get("groundCoeff", -1.0),
                scenario=prop.get("scenario"),
                environment=prop.get("environment"),
                refractivity=prop.get("refractivity"),
                ground_conductivity=prop.get("groundConductivity"),
                ground_permittivity=prop.get("groundPermittivity"),
                climate_zone=prop.get("climateZone"),
            )
            actions.append(f"Propagation: {prop['model']}")

        shadow = profile.get("shadowingStd")
        if shadow is not None:
            self.set_shadow_std(float(shadow))
            actions.append(f"Shadowing std: {shadow}")

        ref_map = profile.get("sourcePowerDbfs")
        if isinstance(ref_map, dict):
            nodes = self.get_nodes()
            for node_name, node_info in nodes.items():
                ntype = str(node_info.get("nodeType", "Unknown"))
                val = ref_map.get(ntype, ref_map.get("*"))
                if val is not None:
                    self.set_source_power(node_name, float(val))
                    actions.append(f"Source power {node_name} ({ntype}): {val} dBFS")

        cfg = profile.get("config")
        if isinstance(cfg, dict):
            self.set_config(
                vehicle_poll_rate_ms=cfg.get("vehiclePollRateMs"),
                channel_update_rate_ms=cfg.get("channelUpdateRateMs"),
                cir_max_taps=cfg.get("cirMaxTaps"),
            )
            actions.append(f"Config: {cfg}")

        return actions

    def extension(
        self, name: str, action: str, params: Optional[dict[str, Any]] = None
    ) -> dict[str, Any]:
        body: dict[str, Any] = {
            "CMD": "EXT",
            "extension": name,
            "action": action,
        }
        if params:
            body["params"] = params
        return self._send(body) or {}

    def extension_list(self) -> dict[str, Any]:
        resp = self._send({"CMD": "EXT_LIST"})
        return resp if isinstance(resp, dict) else {}

    def sandbox_status(self) -> dict[str, Any]:
        return self.extension("sandbox", "status")

    # ---- Sionna RT  ----

    def sionna_start(
        self,
        server_url: str = "http://localhost:8000",
        scene_config: str = "aerpaw",
        ref_lat: float = 35.72750947,
        ref_lon: float = -78.69595819,
        ref_alt: float = 112.0,
        offset_x: float = 118.1,
        offset_y: float = -123.4,
        offset_z: float = 0.0,
        scale: float = 1.0,
        update_rate_ms: Optional[int] = None,
        max_depth: Optional[int] = None,
    ) -> dict[str, Any]:
        params: dict[str, Any] = {
            "serverUrl": server_url,
            "sceneConfig": scene_config,
            "referenceOrigin": {
                "lat": ref_lat,
                "lon": ref_lon,
                "alt": ref_alt,
            },
            "sceneOffset": {
                "x": offset_x,
                "y": offset_y,
                "z": offset_z,
            },
            "scale": scale,
        }
        if update_rate_ms is not None:
            params["updateRateMs"] = int(update_rate_ms)
        if max_depth is not None:
            params["maxDepth"] = int(max_depth)
        return self.extension("sionna", "start", params)

    def sionna_stop(self) -> dict[str, Any]:
        return self.extension("sionna", "stop")

    def sionna_status(self) -> dict[str, Any]:
        return self.extension("sionna", "status")


@dataclass
class ListState:
    items: list[str]
    selected: int = 0
    scroll: int = 0

    def clamp(self) -> None:
        if not self.items:
            self.selected = 0
            self.scroll = 0
            return
        self.selected %= len(self.items)
        self.scroll = max(0, min(self.scroll, max(0, len(self.items) - 1)))

    def move(self, delta: int) -> None:
        if not self.items:
            return
        self.selected = (self.selected + delta) % len(self.items)


class Theme:
    def __init__(self) -> None:
        curses.start_color()
        self.has_colors = curses.has_colors()
        if self.has_colors:
            try:
                curses.use_default_colors()
            except curses.error:
                pass
            curses.init_pair(1, curses.COLOR_CYAN, -1)  # title
            curses.init_pair(2, curses.COLOR_YELLOW, -1)  # header / selected
            curses.init_pair(3, curses.COLOR_GREEN, -1)  # normal
            curses.init_pair(4, curses.COLOR_RED, -1)  # error
            curses.init_pair(5, curses.COLOR_WHITE, -1)  # help

    def title(self) -> int:
        return curses.color_pair(1) | curses.A_BOLD if self.has_colors else curses.A_BOLD

    def header(self) -> int:
        return curses.color_pair(2) | curses.A_BOLD if self.has_colors else curses.A_BOLD

    def normal(self) -> int:
        return curses.color_pair(3) if self.has_colors else curses.A_NORMAL

    def selected(self) -> int:
        base = curses.color_pair(2) | curses.A_BOLD if self.has_colors else curses.A_BOLD
        return base | curses.A_REVERSE

    def help(self) -> int:
        return curses.color_pair(5) if self.has_colors else curses.A_DIM

    def error(self) -> int:
        return curses.color_pair(4) | curses.A_BOLD if self.has_colors else curses.A_BOLD

    def tx_active(self) -> int:
        return curses.color_pair(3) | curses.A_BOLD if self.has_colors else curses.A_BOLD

    def tx_inactive(self) -> int:
        return curses.color_pair(5) | curses.A_DIM if self.has_colors else curses.A_DIM


def _draw_box(stdscr: curses.window, y: int, x: int, h: int, w: int, title: str, theme: Theme) -> None:
    if h <= 2 or w <= 2:
        return
    try:
        win = stdscr.derwin(h, w, y, x)
        win.erase()
        win.box()
        if title:
            label = f" {title} "
            win.addnstr(0, 2, label, w - 4, theme.header())
    except curses.error:
        return


def _draw_list(
    stdscr: curses.window,
    y: int,
    x: int,
    h: int,
    w: int,
    state: ListState,
    theme: Theme,
) -> None:
    if h <= 0 or w <= 0:
        return
    state.clamp()

    visible = max(1, h)
    blank = " " * max(0, w)
    for row in range(visible):
        try:
            stdscr.addnstr(y + row, x, blank, w, theme.normal())
        except curses.error:
            pass
    # keep selected in view
    if state.selected < state.scroll:
        state.scroll = state.selected
    if state.selected >= state.scroll + visible:
        state.scroll = state.selected - visible + 1

    for row in range(visible):
        idx = state.scroll + row
        if idx >= len(state.items):
            break
        text = state.items[idx]
        style = theme.selected() if idx == state.selected else theme.normal()
        try:
            stdscr.addnstr(y + row, x, text.ljust(w), w, style)
        except curses.error:
            pass


def _wrap_lines(text: str, width: int) -> list[str]:
    if width <= 0:
        return [""]
    words = text.split()
    if not words:
        return [""]
    lines: list[str] = []
    current = words[0]
    for word in words[1:]:
        if len(current) + 1 + len(word) <= width:
            current += " " + word
        else:
            lines.append(current)
            current = word
    lines.append(current)
    return lines


def _modal_begin(stdscr: curses.window) -> None:
    stdscr.nodelay(False)
    stdscr.timeout(-1)


def _modal_end(stdscr: curses.window) -> None:
    stdscr.nodelay(True)
    stdscr.timeout(TUI_POLL_TIMEOUT_MS)


def _modal_select(
    stdscr: curses.window,
    theme: Theme,
    title: str,
    options: list[str],
    help_text: str = "↑/↓ select, Enter accept, Esc cancel",
) -> Optional[str]:
    if not options:
        return None

    height, width = stdscr.getmaxyx()
    w = min(width - 4, max(30, max(len(o) for o in options) + 6))
    h = min(height - 4, max(8, min(len(options) + 4, height - 4)))
    y = (height - h) // 2
    x = (width - w) // 2
    state = ListState(items=options, selected=0, scroll=0)

    _modal_begin(stdscr)
    try:
        while True:
            _draw_box(stdscr, y, x, h, w, title, theme)
            inner_y = y + 1
            inner_x = x + 1
            inner_h = h - 3
            inner_w = w - 2
            _draw_list(stdscr, inner_y, inner_x, inner_h, inner_w, state, theme)
            try:
                stdscr.addnstr(y + h - 2, x + 2, help_text.ljust(w - 4), w - 4, theme.help())
            except curses.error:
                pass
            stdscr.refresh()

            key = stdscr.getch()
            if key in (curses.KEY_UP, ord("k")):
                state.move(-1)
            elif key in (curses.KEY_DOWN, ord("j")):
                state.move(1)
            elif key in (curses.KEY_ENTER, 10, 13):
                return options[state.selected]
            elif key in (27, ord("q")):
                return None
    finally:
        _modal_end(stdscr)


def _modal_input(
    stdscr: curses.window,
    theme: Theme,
    title: str,
    prompt: str,
    initial: str = "",
    help_text: str = "Type value, Enter accept, Esc cancel",
) -> Optional[str]:
    height, width = stdscr.getmaxyx()
    w = min(width - 4, max(50, len(prompt) + 10))
    prompt_lines = _wrap_lines(prompt, max(1, w - 4))
    h = 8 if len(prompt_lines) > 1 else 7
    y = (height - h) // 2
    x = (width - w) // 2

    _modal_begin(stdscr)
    try:
        try:
            curses.curs_set(1)
        except curses.error:
            pass

        _draw_box(stdscr, y, x, h, w, title, theme)
        prompt_y = y + 2
        for i, line in enumerate(prompt_lines[:2]):
            try:
                stdscr.addnstr(prompt_y + i, x + 2, line.ljust(w - 4), w - 4, theme.normal())
            except curses.error:
                pass

        edit_w = max(10, w - 4)
        edit_row = y + 3 if len(prompt_lines) <= 1 else y + 4
        edit_win = stdscr.derwin(1, edit_w - 2, edit_row, x + 2)
        edit_win.erase()
        edit_win.addnstr(0, 0, initial, edit_w - 3)
        edit_win.refresh()
        tb = curses.textpad.Textbox(edit_win)

        while True:
            try:
                stdscr.addnstr(y + h - 2, x + 2, help_text.ljust(w - 4), w - 4, theme.help())
            except curses.error:
                pass
            stdscr.refresh()

            key = stdscr.getch()
            if key == 27:  # Esc
                return None
            if key in (curses.KEY_ENTER, 10, 13):
                break
            tb.do_command(key)
            edit_win.refresh()

        value = tb.gather().strip()
        return value
    finally:
        try:
            curses.curs_set(0)
        except curses.error:
            pass
        _modal_end(stdscr)


class App:
    def __init__(self, client: ChemClient):
        self.client = client
        self.version: str = "Unknown"
        self.status_line: str = ""
        self.auto_refresh_interval = 1.0
        self.banner = [
            "      _____ __ __ ____ __  ___",
            "     / ___// // // __//  |/  /",
            "    / /__ / _  // _/ / /|_/ / ",
            "    \\___//_//_//___//_/  /_/   @ 2022",
        ]
        self.footnote_line = "docs.digitaltwin.sh"

        self.screen: str = "main"  # main | nodes | channels | status | config | profiles | extensions | logs
        self.main_state = ListState(
            [
                "Nodes",
                "Channels",
                "Profiles",
                "Status",
                "Logs",
                "Config",
                "Extensions",
                "Set Default Propagation",
                "Quit",
            ]
        )

        self.nodes_state = ListState([])
        self.nodes_data: dict[str, Any] = {}
        self.nodes_last_fetch = 0.0

        self.channels_state = ListState([])
        self.channels_data: dict[str, Any] = {}
        self.ind_channels_data: dict[str, Any] = {}
        self.links_state = ListState([])
        self.links_header: str = ""
        self.channels_focus: str = "freq"  # freq | link
        self.channels_last_fetch = 0.0

        self.shadow_last_value: Optional[float] = None
        self.status_data: dict[str, Any] = {}
        self.status_last_fetch = 0.0
        self.sandbox_enabled = False
        self._status_prev_cpu_ticks: Optional[int] = None
        self._status_prev_uptime: Optional[float] = None
        self.config_data: dict[str, Any] = {}
        self.config_last_fetch = 0.0
        self.configfile_data: dict[str, Any] = {}
        self.configfile_path: str = ""
        self.config_rows: list[dict[str, Any]] = []
        self.config_state = ListState([])

        self.profiles_list: list[str] = []
        self.profiles_map: dict[str, dict] = {}
        self.profiles_state = ListState([])
        self.profiles_detail_lines: list[str] = []
        self.extensions_data: dict[str, Any] = {}
        self.extensions_last_fetch = 0.0

        self.logs_lines: list[str] = []
        self.logs_scroll: int = 0
        self.logs_follow: bool = True
        self.logs_fetch_count: int = 500
        self.logs_last_fetch: float = 0.0

    def _set_status(self, msg: str) -> None:
        self.status_line = msg

    def _fetch_version(self) -> None:
        try:
            self.version = self.client.get_version() or "Unknown"
        except Exception as e:
            self.version = "Unknown"
            self._set_status(f"Version error: {e}")

    def _fetch_nodes(self) -> None:
        self.nodes_data = self.client.get_nodes()
        self.nodes_state.items = sorted(self.nodes_data.keys())
        self.nodes_state.clamp()
        self.nodes_last_fetch = time.time()

    def _selected_node_name(self) -> Optional[str]:
        if not self.nodes_state.items:
            return None
        return self.nodes_state.items[self.nodes_state.selected]

    def _fetch_channels(self) -> None:
        self.channels_data = self.client.get_channels()
        freq_keys = sorted(self.channels_data.keys(), key=lambda v: float(v) if str(v).replace(".", "", 1).isdigit() else str(v))
        self.channels_state.items = [str(k) for k in freq_keys]
        self.channels_state.clamp()
        self.channels_last_fetch = time.time()

    def _fetch_ind_channels(self) -> None:
        self.ind_channels_data = self.client.get_individual_channels()

    def _fetch_shadow_std(self) -> None:
        freq_key = self._selected_freq_key()
        if freq_key:
            try:
                self.shadow_last_value = self.client.get_shadow_std_for_freq(float(freq_key))
                return
            except Exception:
                pass
        self.shadow_last_value = self.client.get_shadow_std()

    def _selected_freq_key(self) -> Optional[str]:
        if not self.channels_state.items:
            return None
        return self.channels_state.items[self.channels_state.selected]

    def _links_for_selected_freq(self) -> list[dict[str, Any]]:
        key = self._selected_freq_key()
        if not key:
            return []
        # Normalize freq key: try string, numeric, and approximate match
        def _normalize(val: Any) -> Optional[list[dict[str, Any]]]:
            if isinstance(val, list):
                return [v for v in val if isinstance(v, dict)]
            return None

        # Exact string match
        if (val := self.ind_channels_data.get(str(key))) is not None:
            if (found := _normalize(val)) is not None:
                return found

        # Numeric match
        try:
            fkey = float(key)
        except Exception:
            fkey = None

        if fkey is not None:
            if (val := self.ind_channels_data.get(fkey)) is not None:
                if (found := _normalize(val)) is not None:
                    return found

            for k, v in self.ind_channels_data.items():
                try:
                    if abs(float(k) - fkey) < 1e-6:
                        if (found := _normalize(v)) is not None:
                            return found
                except Exception:
                    continue
        return []

    def _refresh_links_list(self, max_width: int = 120) -> None:
        links = self._links_for_selected_freq()

        headers = ("TX", "Channel", "Coeff", "SNR", "PL", "FO", "Dop", "CIR", "S", "Dist", "Elev", "Az")
        rows: list[tuple[str, ...]] = []
        for entry in links:
            tx_state = _format_tx_marker(entry)
            src = entry.get("src", "?")
            dest = entry.get("dest", "?")
            coeff = entry.get("coeff", "?")
            snr = entry.get("snrMeasuredDb", entry.get("snr", "?"))
            pl_db = entry.get("plDb", "?")
            freq_offset = entry.get("freqOffsetHz", 0.0)
            doppler_enabled = bool(entry.get("dopplerEnabled", False))
            doppler_hz = entry.get("dopplerHz", 0.0)
            cir_taps = entry.get("cirTapCount", 0)
            sionna_active = bool(entry.get("extensionActive", False))
            distance = entry.get("distance", "?")
            elevation = entry.get("elevation", "?")
            azimuth = entry.get("azimuth", "?")
            channel = f"{src}->{dest}"
            def _fmt_num(val: Any, places: int = 2) -> str:
                try:
                    return f"{float(val):.{places}f}"
                except Exception:
                    return str(val)

            rows.append((
                tx_state,
                channel,
                _fmt_num(coeff, 3),
                _fmt_num(snr, 2),
                _fmt_num(pl_db, 1),
                _fmt_num(freq_offset, 1),
                (_fmt_num(doppler_hz, 1) if doppler_enabled else "off"),
                str(int(cir_taps) if isinstance(cir_taps, (int, float)) else cir_taps),
                "*" if sionna_active else "",
                _format_distance(distance),
                _fmt_num(elevation, 1),
                _fmt_num(azimuth, 1),
            ))

        w_total = max(20, int(max_width))
        w_tx = 2
        w_coeff = 7
        w_snr = 6
        w_pl = 6
        w_fo = 7
        w_dop = 7
        w_cir = 4
        w_s = 2
        w_dist = 10
        w_elev = 6
        w_az = 6
        sep = "  "
        fixed = w_tx + w_coeff + w_snr + w_pl + w_fo + w_dop + w_cir + w_s + w_dist + w_elev + w_az
        sep_total = len(sep) * (len(headers) - 1)
        w_channel = max(8, w_total - fixed - sep_total)
        w_channel = min(w_channel, 24)

        def _cell(val: str, w: int, align: str = "<") -> str:
            s = str(val)
            if len(s) > w:
                s = _ellipsis(s, w)
            return f"{s:{align}{w}}"

        def _row_to_str(row: tuple[str, ...]) -> str:
            tx_s, ch, coeff_s, snr_s, pl_s, fo_s, dop_s, cir_s, s_s, dist_s, elev_s, az_s = row
            parts = [
                _cell(tx_s, w_tx, "^"),
                _cell(ch, w_channel, "<"),
                _cell(coeff_s, w_coeff, ">"),
                _cell(snr_s, w_snr, ">"),
                _cell(pl_s, w_pl, ">"),
                _cell(fo_s, w_fo, ">"),
                _cell(dop_s, w_dop, ">"),
                _cell(cir_s, w_cir, ">"),
                _cell(s_s, w_s, "^"),
                _cell(dist_s, w_dist, ">"),
                _cell(elev_s, w_elev, ">"),
                _cell(az_s, w_az, ">"),
            ]
            return sep.join(parts)

        if rows:
            header_row = (
                headers[0],
                headers[1],
                headers[2],
                headers[3],
                headers[4],
                headers[5],
                headers[6],
                headers[7],
                headers[8],
                headers[9],
                headers[10],
                headers[11],
            )
            self.links_header = _row_to_str(header_row)
            self.links_state.items = [_row_to_str(r) for r in rows]
        else:
            self.links_header = ""
            self.links_state.items = ["(no channel details)"]
        self.links_state.clamp()

    def _selected_link(self) -> Optional[dict[str, Any]]:
        links = self._links_for_selected_freq()
        if not links:
            return None
        idx = min(self.links_state.selected, len(links) - 1)
        return links[idx]

    def _draw_header(self, stdscr: curses.window, theme: Theme) -> None:
        height, width = stdscr.getmaxyx()
        prefix = f"pychem  |  CHEM {self.client.addr}:{self.client.port}  |  "
        conn = "CONNECTED" if self.client.connected else "NO CONNECTION"
        suffix = f"  |  Version {self.version}"
        conn_style = theme.header() if self.client.connected else theme.error()
        help_line = {
            "main": "↑/↓ select, Enter open, q quit",
            "nodes": "↑/↓ select node, auto-refresh (r to force), q back",
            "channels": "↑/↓ select, Tab switch, Enter actions, auto-refresh (r to force), q back",
            "status": "r refresh, q back",
            "config": "↑/↓ select, Enter edit, r refresh, q back",
            "profiles": "↑/↓ select, Enter apply, s save new, p set propagation, q back",
            "extensions": "Enter actions, r refresh, q back",
            "logs": "↑/↓ scroll, PgUp/PgDn page, g top, G bottom, f follow, r refresh, c clear, q back",
        }.get(self.screen, "")

        try:
            col = 0
            stdscr.addnstr(0, col, prefix[:width], width - col, theme.title())
            col = min(width, col + len(prefix))
            if col < width:
                stdscr.addnstr(0, col, conn[: width - col], width - col, conn_style)
                col = min(width, col + len(conn))
            if col < width:
                stdscr.addnstr(0, col, suffix.ljust(width - col)[: width - col], width - col, theme.title())
            stdscr.addnstr(1, 0, help_line.ljust(width), width, theme.help())
            if self.sandbox_enabled and width > 0:
                note = "[ SANDBOX MODE ]"
                if len(note) > width:
                    note = _ellipsis("SANDBOX MODE", width)
                x = max(0, width - len(note))
                stdscr.addnstr(1, x, note, width - x, theme.error())
            stdscr.hline(2, 0, curses.ACS_HLINE, width)
        except curses.error:
            pass

    def _draw_footer(self, stdscr: curses.window, theme: Theme) -> None:
        height, width = stdscr.getmaxyx()
        line = _ellipsis(self.status_line, max(0, width))
        try:
            stdscr.hline(height - 2, 0, curses.ACS_HLINE, width)
            stdscr.addnstr(height - 1, 0, line.ljust(width), width, theme.help())
        except curses.error:
            pass

    def _render_main(self, stdscr: curses.window, theme: Theme) -> None:
        height, width = stdscr.getmaxyx()
        box_y, box_x = 3, 2
        box_h, box_w = height - 6, width - 4
        _draw_box(stdscr, box_y, box_x, box_h, box_w, "Main Menu", theme)

        inner_y, inner_x = box_y + 1, box_x + 1
        inner_h, inner_w = box_h - 2, box_w - 2
        if inner_h <= 0 or inner_w <= 0:
            return

        def _draw_credit(area_x: int, area_w: int) -> None:
            if inner_h < 2 or inner_w <= 0:
                return
            line = _ellipsis(self.footnote_line, max(0, area_w))
            x = area_x + max(0, (area_w - len(line)) // 2)
            y = inner_y + inner_h - 1
            try:
                stdscr.addnstr(y, x, line, max(0, area_w - (x - area_x)), theme.help())
            except curses.error:
                pass

        # Prefer a 2-column layout (menu left, banner right) when there's room.
        use_columns = inner_w >= 70 and inner_h >= 12
        if use_columns:
            left_w = max(18, inner_w // 3)
            gap = 2
            right_w = max(0, inner_w - left_w - gap)
            menu_x = inner_x
            banner_x0 = inner_x + left_w + gap

            try:
                stdscr.addnstr(inner_y, menu_x, "Menu".ljust(left_w)[:left_w], left_w, theme.help())
            except curses.error:
                pass
            _draw_list(
                stdscr,
                inner_y + 1,
                menu_x,
                inner_h - 1,
                left_w,
                self.main_state,
                theme,
            )

            if right_w > 0:
                banner_w = max(len(line) for line in self.banner) if self.banner else 0
                banner_w = min(banner_w, right_w)
                banner_x = banner_x0 + max(0, (right_w - banner_w) // 2)
                banner_y = inner_y + max(0, (inner_h - len(self.banner)) // 2)
                for i, line in enumerate(self.banner):
                    try:
                        stdscr.addnstr(banner_y + i, banner_x, line[:banner_w], banner_w, theme.header())
                    except curses.error:
                        pass
                _draw_credit(banner_x, banner_w)
            return

        # Fallback: banner at top, menu below (small terminals).
        banner_y = inner_y + 1
        banner_x = inner_x + 1
        banner_w = max(0, inner_w - 2)
        for i, line in enumerate(self.banner):
            try:
                stdscr.addnstr(banner_y + i, banner_x, line[:banner_w], banner_w, theme.header())
            except curses.error:
                pass

        _draw_list(
            stdscr,
            banner_y + len(self.banner) + 1,
            inner_x + 1,
            inner_h - (len(self.banner) + 2),
            inner_w - 2,
            self.main_state,
            theme,
        )
        _draw_credit(banner_x, banner_w)

    def _render_nodes(self, stdscr: curses.window, theme: Theme) -> None:
        height, width = stdscr.getmaxyx()
        content_h = height - 6
        content_w = width - 4
        y0, x0 = 3, 2

        _draw_box(stdscr, y0, x0, content_h, content_w, "Nodes", theme)

        # Table header
        headers = ("TX", "Name", "ID", "TxFreq", "RxFreq", "Ch", "Rate", "Antenna", "TxG(dBm)", "RxG(dBm)", "Vehicle", "Type")
        col_widths = (
            2,
            max(10, content_w // 8),
            max(6, content_w // 18),
            max(9, content_w // 13),
            max(9, content_w // 13),
            4,
            max(9, content_w // 13),
            max(8, content_w // 13),
            7,
            7,
            max(7, content_w // 14),
            max(8, content_w // 10),
        )
        header_fmt = (
            f"{{:<{col_widths[0]}}}  {{:<{col_widths[1]}}}  {{:<{col_widths[2]}}}  "
            f"{{:<{col_widths[3]}}}  {{:<{col_widths[4]}}}  {{:<{col_widths[5]}}}  "
            f"{{:<{col_widths[6]}}}  {{:<{col_widths[7]}}}  {{:>{col_widths[8]}}}  "
            f"{{:>{col_widths[9]}}}  {{:<{col_widths[10]}}}  {{:<{col_widths[11]}}}"
        )
        header_line = header_fmt.format(*headers)
        try:
            stdscr.addnstr(y0 + 2, x0 + 1, header_line[: content_w - 2], content_w - 2, theme.help())
        except curses.error:
            pass

        table_y = y0 + 3
        table_h = content_h - 4
        table_w = content_w - 2

        self.nodes_state.clamp()
        visible = max(1, table_h)
        if self.nodes_state.selected < self.nodes_state.scroll:
            self.nodes_state.scroll = self.nodes_state.selected
        if self.nodes_state.selected >= self.nodes_state.scroll + visible:
            self.nodes_state.scroll = self.nodes_state.selected - visible + 1

        names = self.nodes_state.items
        for row in range(visible):
            idx = self.nodes_state.scroll + row
            if idx >= len(names):
                break
            name = names[idx]
            info = self.nodes_data.get(name, {})
            line = _format_node_row(name, info, col_widths)
            attr = theme.selected() if idx == self.nodes_state.selected else theme.normal()
            try:
                stdscr.addnstr(table_y + row, x0 + 1, line[:table_w], table_w, attr)
                dot_attr = theme.tx_active() if _is_tx_active(info) else theme.tx_inactive()
                stdscr.addstr(table_y + row, x0 + 1, line[0], dot_attr)
            except curses.error:
                pass

    def _action_nodes(self, stdscr: curses.window, theme: Theme) -> None:
        node_name = self._selected_node_name()
        if not node_name:
            self._set_status("No node selected")
            return
        action = _modal_select(stdscr, theme, "Node Actions", [
            "Change antenna (both TX/RX)",
            "Change TX antenna",
            "Change RX antenna",
            "Refresh",
            "Back"
        ])
        if not action or action == "Back":
            return
        if action == "Refresh":
            self._enter_nodes()
            return

        if action in ("Change antenna (both TX/RX)", "Change TX antenna", "Change RX antenna"):
            patterns = self.client.get_antenna_patterns()
            types = self.client.get_antenna_types()
            # Merge coordinator patterns and compiled antenna types
            opts = [str(v) for v in (patterns or [])]
            for t in types or []:
                if t not in opts:
                    opts.append(t)
            # Always include isotropic as an option
            if "isotropic" not in opts:
                opts.append("isotropic")
            if not opts:
                self._set_status("No antenna patterns available")
                return

            node_info = self.nodes_data.get(node_name, {})
            if action == "Change TX antenna":
                current = str(node_info.get("txAntennaPattern", node_info.get("antennaPattern", "isotropic")))
                title = "TX Antenna Pattern"
            elif action == "Change RX antenna":
                current = str(node_info.get("rxAntennaPattern", node_info.get("antennaPattern", "isotropic")))
                title = "RX Antenna Pattern"
            else:
                current = str(node_info.get("antennaPattern", "isotropic"))
                title = "Antenna Pattern (TX & RX)"

            options = opts
            if current in options:
                options = [current] + [p for p in options if p != current]
            chosen = _modal_select(stdscr, theme, title, options)
            if not chosen:
                return
            try:
                if action == "Change TX antenna":
                    resp = self.client.set_node_antenna(node_name, tx_pattern=chosen)
                elif action == "Change RX antenna":
                    resp = self.client.set_node_antenna(node_name, rx_pattern=chosen)
                else:
                    resp = self.client.set_node_antenna(node_name, pattern=chosen)
                self._fetch_nodes()
                self._set_status(f"Antenna updated: {resp}")
            except Exception as e:
                self._set_status(f"Antenna update failed: {e}")

    def _enter_status(self) -> None:
        self._set_status("Fetching status…")
        try:
            self._fetch_status()
            self._set_status("Status (r to refresh)")
        except ChemConnectionError as e:
            self._set_status(f"Warning: no connection to CHEM: {e}")
        except Exception as e:
            self._set_status(f"Status error: {e}")

    def _render_status(self, stdscr: curses.window, theme: Theme) -> None:
        height, width = stdscr.getmaxyx()
        _draw_box(stdscr, 3, 2, height - 6, width - 4, "CHEM Status", theme)

        data = self.status_data or {}
        nodes = data.get("nodesConnected", "-")
        freqs = data.get("freqChannels", "-")
        uptime = data.get("uptimeSec", None)
        cpu_now = data.get("cpuPercentNow", None)
        cpu_pct = data.get("cpuPercentAvg", None)
        cpu_time = data.get("cpuTimeSec", None)
        cores = data.get("cpuCores", None)
        rss_bytes = data.get("rssBytes", None)
        vmsize_bytes = data.get("vmSizeBytes", None)

        def _fmt_float(v: Any, places: int = 2) -> str:
            try:
                return f"{float(v):.{places}f}"
            except Exception:
                return str(v)

        def _fmt_bytes(v: Any) -> str:
            try:
                mib = float(v) / (1024.0 * 1024.0)
                return f"{mib:.2f} MiB"
            except Exception:
                return str(v)

        lines = [
            f"Nodes connected: {nodes}",
            f"Frequencies:     {freqs}",
            "",
            f"Uptime:          {_fmt_float(uptime, 1)} s" if uptime is not None else "Uptime:          -",
            f"CPU time:        {_fmt_float(cpu_time, 2)} s" if cpu_time is not None else "CPU time:        -",
            f"CPU now:         {_fmt_float(cpu_now, 2)} % of {cores} core(s)"
            if cpu_now is not None and cores is not None
            else "CPU now:         -",
            f"CPU avg:         {_fmt_float(cpu_pct, 2)} % of {cores} core(s)"
            if cpu_pct is not None and cores is not None
            else "CPU avg:         -",
            "",
            f"Memory (RSS):    {_fmt_bytes(rss_bytes)}" if rss_bytes is not None else "Memory (RSS):    -",
            f"Memory (VmSize): {_fmt_bytes(vmsize_bytes)}" if vmsize_bytes is not None else "Memory (VmSize): -",
        ]

        y = 5
        if self.sandbox_enabled:
            note = "SANDBOX MODE ACTIVE"
            try:
                stdscr.addnstr(y, 4, _ellipsis(note, width - 8), width - 8, theme.error())
            except curses.error:
                pass
            y += 2

        for line in lines:
            try:
                stdscr.addnstr(y, 4, _ellipsis(line, width - 8), width - 8, theme.normal())
            except curses.error:
                pass
            y += 1

    def _render_logs(self, stdscr: curses.window, theme: Theme) -> None:
        height, width = stdscr.getmaxyx()
        box_y, box_x = 3, 2
        box_h, box_w = height - 6, width - 4
        follow_tag = "FOLLOW" if self.logs_follow else "PAUSED"
        title = f"CHEM Logs ({len(self.logs_lines)} lines, {follow_tag})"
        _draw_box(stdscr, box_y, box_x, box_h, box_w, title, theme)

        inner_y, inner_x = box_y + 1, box_x + 1
        inner_h, inner_w = box_h - 2, box_w - 2
        if inner_h <= 0 or inner_w <= 0:
            return

        total = len(self.logs_lines)
        if total == 0:
            try:
                stdscr.addnstr(inner_y, inner_x, "(no logs yet)", inner_w, theme.help())
            except curses.error:
                pass
            return

        if self.logs_follow:
            self.logs_scroll = max(0, total - inner_h)
        else:
            self.logs_scroll = max(0, min(self.logs_scroll, total - 1))

        start = self.logs_scroll
        end = min(total, start + inner_h)
        for row, idx in enumerate(range(start, end)):
            line = self.logs_lines[idx]
            attr = theme.normal()
            low = line.lower()
            if "[error]" in low or "[critical]" in low:
                attr = theme.error()
            elif "[warn" in low:
                attr = theme.help()
            elif "[info]" in low:
                attr = theme.header()
            try:
                stdscr.addnstr(
                    inner_y + row, inner_x, _ellipsis(line, inner_w), inner_w, attr
                )
            except curses.error:
                pass

    def _build_config_rows(self) -> None:
        rows: list[dict[str, Any]] = []
        rt = self.config_data or {}
        for key in ("vehiclePollRateMs", "channelUpdateRateMs", "cirMaxTaps"):
            if key in rt:
                rows.append({"key": key, "value": rt.get(key),
                             "type": "int", "source": "runtime"})
        for dotted, value in _flatten_scalars(self.configfile_data):
            rows.append({"key": dotted, "value": value,
                         "type": _value_type(value), "source": "file"})
        self.config_rows = rows
        self.config_state.items = [r["key"] for r in rows]
        self.config_state.clamp()

    def _enter_config(self) -> None:
        self._set_status("Fetching config…")
        try:
            self._fetch_config()
            self._set_status("Config (Enter edit, r refresh)")
        except ChemConnectionError as e:
            self._set_status(f"Warning: no connection to CHEM: {e}")
        except Exception as e:
            self._set_status(f"Config error: {e}")

    def _render_config(self, stdscr: curses.window, theme: Theme) -> None:
        height, width = stdscr.getmaxyx()
        y0, x0 = 3, 2
        content_h, content_w = height - 6, width - 4
        path = self.configfile_path or "config.json"
        _draw_box(stdscr, y0, x0, content_h, content_w, f"CHEM Config — {path}", theme)

        inner_x = x0 + 1
        table_w = content_w - 2

        # Columns: Setting | Value | Type | Source
        w_type = 7
        w_source = 8
        w_value = max(10, min(40, table_w // 3))
        sep = "  "
        w_key = max(10, table_w - w_value - w_type - w_source - len(sep) * 3)

        def _row(key: str, value: str, typ: str, source: str) -> str:
            return sep.join([
                f"{_ellipsis(key, w_key):<{w_key}}",
                f"{_ellipsis(value, w_value):<{w_value}}",
                f"{_ellipsis(typ, w_type):<{w_type}}",
                f"{_ellipsis(source, w_source):<{w_source}}",
            ])

        try:
            stdscr.addnstr(y0 + 2, inner_x, _row("Setting", "Value", "Type", "Source"),
                           table_w, theme.help())
        except curses.error:
            pass

        if not self.config_rows:
            try:
                stdscr.addnstr(y0 + 4, inner_x, "(no config available)", table_w, theme.normal())
            except curses.error:
                pass
            return

        table_y = y0 + 3
        visible = max(1, content_h - 4)
        self.config_state.clamp()
        if self.config_state.selected < self.config_state.scroll:
            self.config_state.scroll = self.config_state.selected
        if self.config_state.selected >= self.config_state.scroll + visible:
            self.config_state.scroll = self.config_state.selected - visible + 1

        for row in range(visible):
            idx = self.config_state.scroll + row
            if idx >= len(self.config_rows):
                break
            r = self.config_rows[idx]
            line = _row(str(r["key"]), _fmt_config_value(r["value"]), r["type"], r["source"])
            attr = theme.selected() if idx == self.config_state.selected else theme.normal()
            try:
                stdscr.addnstr(table_y + row, inner_x, line, table_w, attr)
            except curses.error:
                pass

    def _action_config(self, stdscr: curses.window, theme: Theme) -> None:
        if not self.config_rows:
            return
        row = self.config_rows[self.config_state.selected]
        key = row["key"]
        typ = row["type"]
        cur = row["value"]
        source = row["source"]

        if typ == "bool":
            choice = _modal_select(stdscr, theme, f"{key} (bool)", ["true", "false"])
            if choice is None:
                return
            new_value: Any = (choice == "true")
        elif typ in ("int", "float"):
            raw = _modal_input(
                stdscr, theme, f"Edit {key}",
                f"New {typ} value:", initial=_fmt_config_value(cur),
            )
            if raw is None or raw == "":
                return
            try:
                new_value = int(float(raw)) if typ == "int" else float(raw)
            except ValueError:
                self._set_status(f"Invalid {typ}: {raw!r}")
                return
        elif typ == "string":
            raw = _modal_input(
                stdscr, theme, f"Edit {key}", "New value:",
                initial=_fmt_config_value(cur),
            )
            if raw is None:
                return
            new_value = raw
        else:
            self._set_status(f"{key}: '{typ}' is not editable here")
            return

        try:
            if source == "runtime":
                kwarg = {
                    "vehiclePollRateMs": "vehicle_poll_rate_ms",
                    "channelUpdateRateMs": "channel_update_rate_ms",
                    "cirMaxTaps": "cir_max_taps",
                }[key]
                self.client.set_config(**{kwarg: int(new_value)})
                note = "applied"
            else:
                patch: dict[str, Any] = {}
                node = patch
                parts = key.split(".")
                for part in parts[:-1]:
                    node = node.setdefault(part, {})
                node[parts[-1]] = new_value
                resp = self.client.save_config_file(patch)
                if resp.get("status") != "success":
                    self._set_status(f"Save failed: {resp.get('message', 'error')}")
                    return
                note = "saved to config.json"
            self._fetch_config()
            self._set_status(f"{key} = {_fmt_config_value(new_value)} ({note})")
        except Exception as e:
            self._set_status(f"Update failed: {e}")

    # ---- Extensions screen ----

    def _enter_extensions(self) -> None:
        self._set_status("Fetching extensions status...")
        try:
            self._fetch_extensions()
            self._set_status("Extensions (Enter actions, r refresh)")
        except ChemConnectionError as e:
            self._set_status(f"Warning: no connection to CHEM: {e}")
        except Exception as e:
            self._set_status(f"Extensions error: {e}")

    def _fetch_extensions(self) -> None:
        resp = self.client.extension_list()
        self.extensions_data = resp.get("extensions", {}) if isinstance(resp, dict) else {}
        sandbox_status = self.extensions_data.get("sandbox")
        if isinstance(sandbox_status, dict):
            self.sandbox_enabled = bool(
                sandbox_status.get("enabled", sandbox_status.get("running", False))
            )
        self.extensions_last_fetch = time.time()

    def _render_extensions(self, stdscr: curses.window, theme: Theme) -> None:
        height, width = stdscr.getmaxyx()
        _draw_box(stdscr, 3, 2, height - 6, width - 4, "Extensions", theme)

        ext_data = self.extensions_data or {}
        y = 5

        if not ext_data:
            try:
                stdscr.addnstr(y, 4, "No extensions registered", width - 8, theme.normal())
            except curses.error:
                pass
            return

        skip_keys = {"status", "name", "running", "configSchema"}
        for ext_name, ext_status in ext_data.items():
            if not isinstance(ext_status, dict):
                continue
            running = ext_status.get("running", False)
            status_str = "RUNNING" if running else "STOPPED"

            lines = [f"[{ext_name}]  {status_str}"]
            for k, v in ext_status.items():
                if k in skip_keys:
                    continue
                if isinstance(v, dict):
                    lines.append(f"  {k}:")
                    for sk, sv in v.items():
                        lines.append(f"    {sk}: {sv}")
                else:
                    lines.append(f"  {k}: {v}")
            lines.append("")

            for line in lines:
                try:
                    stdscr.addnstr(y, 4, _ellipsis(line, width - 8), width - 8, theme.normal())
                except curses.error:
                    pass
                y += 1
                if y >= height - 4:
                    break
            if y >= height - 4:
                break

        try:
            stdscr.addnstr(y, 4, "Press Enter for actions", width - 8, theme.normal())
        except curses.error:
            pass

    @staticmethod
    def _schema_prompt_for_field(
        stdscr: curses.window,
        theme: Theme,
        field_name: str,
        field_schema: dict,
        current_value: Any = None,
    ) -> Optional[str]:
        ftype = field_schema.get("type", "string")
        default = field_schema.get("default", "")
        initial = str(current_value) if current_value is not None else str(default)
        if ftype == "object":
            # For object fields, prompt for each sub-property
            props = field_schema.get("properties", {})
            if not props:
                return _modal_input(stdscr, theme, field_name, f"{field_name} (JSON):", initial=initial)
            parts: dict[str, Any] = {}
            cur_obj = current_value if isinstance(current_value, dict) else {}
            for pname, pschema in props.items():
                sub_default = pschema.get("default", cur_obj.get(pname, ""))
                raw = _modal_input(
                    stdscr, theme, field_name, f"{pname}:", initial=str(sub_default)
                )
                if raw is None:
                    return None
                stripped = raw.strip()
                ptype = pschema.get("type", "string") if isinstance(pschema, dict) else "string"
                if ptype in ("number", "integer"):
                    # Empty input falls back to the schema default rather than
                    # being sent as "" (which the server rejects as non-numeric).
                    candidate = stripped if stripped else str(sub_default).strip()
                    if not candidate:
                        continue
                    try:
                        parts[pname] = int(float(candidate)) if ptype == "integer" else float(candidate)
                    except ValueError:
                        continue
                else:
                    parts[pname] = stripped if stripped else str(sub_default)
            return json.dumps(parts)
        return _modal_input(stdscr, theme, field_name, f"{field_name}:", initial=initial)

    @staticmethod
    def _coerce_schema_value(raw: str, field_schema: dict) -> Any:
        ftype = field_schema.get("type", "string")
        if ftype == "integer":
            return int(float(raw))
        if ftype == "number":
            return float(raw)
        if ftype == "object":
            return json.loads(raw)
        return raw.strip()

    def _action_extensions(self, stdscr: curses.window, theme: Theme) -> None:
        ext_data = self.extensions_data or {}
        ext_names = list(ext_data.keys())

        if not ext_names:
            self._set_status("No extensions registered")
            return

        # If only one extension, act on it directly; otherwise pick
        if len(ext_names) == 1:
            ext_name = ext_names[0]
        else:
            choice = _modal_select(stdscr, theme, "Select Extension", ext_names + ["Back"])
            if not choice or choice == "Back":
                return
            ext_name = choice

        data = ext_data.get(ext_name, {})
        running = data.get("running", False)
        schema = data.get("configSchema", {})

        options = []
        if running:
            options.append("Stop")
        else:
            options.append("Start")

        config_fields: list[str] = []
        for field_name, field_schema in schema.items():
            if not isinstance(field_schema, dict):
                continue
            ftype = field_schema.get("type", "string")
            if ftype == "object":
                continue
            config_fields.append(field_name)
            options.append(f"Configure {field_name}")

        options.extend(["Refresh", "Back"])

        action = _modal_select(stdscr, theme, f"{ext_name} Actions", options)
        if not action or action == "Back":
            return

        if action == "Refresh":
            self._enter_extensions()
            return

        if action == "Start":
            params: dict[str, Any] = {}
            # Prompt for each schema field to build start params
            for field_name, field_schema in schema.items():
                if not isinstance(field_schema, dict):
                    continue
                current = data.get(field_name)
                raw = self._schema_prompt_for_field(
                    stdscr, theme, field_name, field_schema, current
                )
                if raw is None:
                    return  # user cancelled
                try:
                    params[field_name] = self._coerce_schema_value(raw, field_schema)
                except Exception:
                    self._set_status(f"Invalid value for {field_name}")
                    return

            try:
                resp = self.client.extension(ext_name, "start", params if params else None)
                self._set_status(
                    f"{ext_name} started: {resp.get('message', resp.get('status', ''))}"
                )
                self._fetch_extensions()
            except Exception as e:
                self._set_status(f"Start failed: {e}")
            return

        if action == "Stop":
            try:
                resp = self.client.extension(ext_name, "stop")
                self._set_status(
                    f"{ext_name} stopped: {resp.get('message', resp.get('status', ''))}"
                )
                self._fetch_extensions()
            except Exception as e:
                self._set_status(f"Stop failed: {e}")
            return

        # Handle dynamic "Configure <field>" actions
        if action.startswith("Configure "):
            field_name = action[len("Configure "):]
            field_schema = schema.get(field_name, {})
            current = data.get(field_name)
            default = field_schema.get("default", "")
            initial = str(current) if current is not None else str(default)
            raw = _modal_input(
                stdscr, theme, field_name, f"{field_name}:", initial=initial
            )
            if raw is None:
                return
            try:
                val = self._coerce_schema_value(raw, field_schema)
            except Exception:
                self._set_status(f"Invalid {field_name}")
                return
            try:
                resp = self.client.extension(ext_name, "config", {field_name: val})
                self._set_status(f"{field_name} updated")
                self._fetch_extensions()
            except Exception as e:
                self._set_status(f"Update failed: {e}")
            return

    def _render_channels(self, stdscr: curses.window, theme: Theme) -> None:
        height, width = stdscr.getmaxyx()
        content_h = height - 6
        content_w = width - 4
        y0, x0 = 3, 2

        left_w = max(22, content_w // 4)
        right_w = content_w - left_w - 1
        top_h = max(8, content_h // 3)
        bottom_h = content_h - top_h - 1

        _draw_box(stdscr, y0, x0, content_h, left_w, "Frequencies", theme)
        _draw_box(stdscr, y0, x0 + left_w + 1, top_h, right_w, "Channel", theme)
        _draw_box(stdscr, y0 + top_h + 1, x0 + left_w + 1, bottom_h, right_w, "Links", theme)

        _draw_list(
            stdscr,
            y0 + 2,
            x0 + 1,
            content_h - 3,
            left_w - 2,
            self.channels_state,
            theme,
        )

        freq_key = self._selected_freq_key()
        summary = self.channels_data.get(freq_key, {}) if freq_key else {}
        links = self._links_for_selected_freq() if freq_key else []
        propagation = summary.get("pathLoss", "-")
        if links:
            propagation = links[0].get("pathLoss", propagation)
        shadow = (
            f"{self.shadow_last_value:.2f}" if isinstance(self.shadow_last_value, (int, float)) else "-"
        )
        any_ext = any(bool(e.get("extensionActive", False)) for e in links)
        active_ext_name = ""
        if any_ext:
            for e in links:
                name = e.get("activeExtension", "")
                if name:
                    active_ext_name = name
                    break
        lines = [
            f"Frequency: {_format_mhz(freq_key)}",
            f"Propagation: {propagation}",
            f"SampleRate:{_format_msa(summary.get('sRate', '-'))}",
            f"Shadow std: {shadow}",
            f"Links: {len(links)}",
        ]
        if any_ext:
            label = active_ext_name or "extension"
            lines.append(f"! {label} active on channel(s)")
        lines.append("")
        lines.append(f"Focus: {self.channels_focus} (Tab to switch)")
        for i, line in enumerate(lines[: top_h - 3]):
            try:
                stdscr.addnstr(y0 + 2 + i, x0 + left_w + 2, line, right_w - 2, theme.normal())
            except curses.error:
                pass

        link_y = y0 + top_h + 2
        link_x = x0 + left_w + 2
        link_w = right_w - 2
        # Rebuild link rows for current terminal width.
        self._refresh_links_list(link_w)
        if self.links_header:
            try:
                stdscr.addnstr(link_y, link_x, self.links_header[:link_w].ljust(link_w), link_w, theme.help())
            except curses.error:
                pass
            link_y += 1
        link_h = bottom_h - (4 if self.links_header else 3)
        link_theme = theme
        if self.channels_focus == "link":
            link_theme = theme
        _draw_list(stdscr, link_y, link_x, link_h, link_w, self.links_state, link_theme)
        for row in range(max(0, link_h)):
            idx = self.links_state.scroll + row
            if idx >= len(links):
                break
            entry = links[idx]
            dot_attr = theme.tx_active() if _is_tx_active(entry) else theme.tx_inactive()
            try:
                stdscr.addstr(link_y + row, link_x, _format_tx_marker(entry), dot_attr)
            except curses.error:
                pass

    def render(self, stdscr: curses.window, theme: Theme) -> None:
        stdscr.erase()
        self._draw_header(stdscr, theme)

        if self.screen == "main":
            self._render_main(stdscr, theme)
        elif self.screen == "nodes":
            self._render_nodes(stdscr, theme)
        elif self.screen == "channels":
            self._render_channels(stdscr, theme)
        elif self.screen == "status":
            self._render_status(stdscr, theme)
        elif self.screen == "config":
            self._render_config(stdscr, theme)
        elif self.screen == "profiles":
            self._render_profiles(stdscr, theme)
        elif self.screen == "extensions":
            self._render_extensions(stdscr, theme)
        elif self.screen == "logs":
            self._render_logs(stdscr, theme)

        self._draw_footer(stdscr, theme)
        stdscr.refresh()

    def _enter_nodes(self) -> None:
        self._set_status("Fetching nodes…")
        try:
            self._fetch_nodes()
            self._set_status(f"Nodes: {len(self.nodes_state.items)} (r to refresh)")
        except ChemConnectionError as e:
            self._set_status(f"Warning: no connection to CHEM: {e}")
        except Exception as e:
            self._set_status(f"Nodes error: {e}")

    def _enter_channels(self) -> None:
        self._set_status("Fetching channels…")
        try:
            self._fetch_channels()
            self._fetch_ind_channels()
            self._refresh_links_list()
            try:
                self._fetch_shadow_std()
            except Exception:
                self.shadow_last_value = None
            self._set_status(f"Channels: {len(self.channels_state.items)} (r to refresh)")
        except ChemConnectionError as e:
            self._set_status(f"Warning: no connection to CHEM: {e}")
        except Exception as e:
            self._set_status(f"Channels error: {e}")

    def _enter_logs(self) -> None:
        self.logs_follow = True
        self.logs_scroll = 0
        self._set_status("Fetching logs…")
        try:
            self._fetch_logs()
            self._set_status(
                f"Logs: {len(self.logs_lines)} lines (f follow, g/G top/bottom, q back)"
            )
        except ChemConnectionError as e:
            self._set_status(f"Warning: no connection to CHEM: {e}")
        except Exception as e:
            self._set_status(f"Logs error: {e}")

    def _fetch_logs(self) -> None:
        lines = self.client.get_logs(self.logs_fetch_count)
        self.logs_lines = [_strip_ansi(ln).rstrip("\r\n") for ln in lines]
        self.logs_last_fetch = time.time()

    def _channels_refresh_all(self) -> None:
        self._fetch_channels()
        self._fetch_ind_channels()
        self._refresh_links_list()
        try:
            self._fetch_shadow_std()
        except Exception:
            pass

    def _fetch_status(self) -> None:
        data = self.client.get_status()
        cpu_now: Optional[float] = None

        try:
            ticks_raw = data.get("cpuTicks", None)
            clk_tck_raw = data.get("clkTck", None)
            uptime_raw = data.get("uptimeSec", None)
            cores_raw = data.get("cpuCores", None)

            ticks = int(ticks_raw) if ticks_raw is not None else None
            clk_tck = float(clk_tck_raw) if clk_tck_raw is not None else None
            uptime = float(uptime_raw) if uptime_raw is not None else None
            cores = int(cores_raw) if cores_raw is not None else None

            if (
                ticks is not None
                and clk_tck is not None
                and clk_tck > 0
                and uptime is not None
                and cores is not None
                and cores > 0
                and self._status_prev_cpu_ticks is not None
                and self._status_prev_uptime is not None
            ):
                dticks = ticks - self._status_prev_cpu_ticks
                duptime = uptime - self._status_prev_uptime
                if dticks >= 0 and duptime > 0:
                    cpu_sec = float(dticks) / float(clk_tck)
                    cpu_now = (cpu_sec / (duptime * float(cores))) * 100.0
                    if cpu_now < 0:
                        cpu_now = 0.0

            if ticks is not None:
                self._status_prev_cpu_ticks = ticks
            if uptime is not None:
                self._status_prev_uptime = uptime
        except Exception:
            cpu_now = None

        if cpu_now is not None:
            data["cpuPercentNow"] = cpu_now

        self.status_data = data
        if "sandboxEnabled" in data:
            self.sandbox_enabled = bool(data.get("sandboxEnabled", False))
        else:
            try:
                self._fetch_sandbox_status()
            except Exception:
                pass
        self.status_last_fetch = time.time()

    def _fetch_sandbox_status(self) -> None:
        data = self.client.sandbox_status()
        if isinstance(data, dict):
            self.sandbox_enabled = bool(
                data.get("enabled", data.get("running", False))
            )

    def _fetch_config(self) -> None:
        self.config_data = self.client.get_config()
        try:
            resp = self.client.get_config_file()
        except Exception:
            resp = {}
        if isinstance(resp, dict) and resp.get("status") == "success":
            self.configfile_data = resp.get("config", {}) or {}
            self.configfile_path = str(resp.get("path", ""))
        else:
            self.configfile_data = {}
            self.configfile_path = ""
        self._build_config_rows()
        self.config_last_fetch = time.time()

    def _auto_refresh(self) -> None:
        now = time.time()
        if self.screen == "nodes" and (now - self.nodes_last_fetch) >= self.auto_refresh_interval:
            try:
                self._fetch_nodes()
            except Exception as e:
                self._set_status(f"Nodes refresh error: {e}")
        if self.screen == "channels" and (
            now - self.channels_last_fetch) >= self.auto_refresh_interval:
            try:
                self._channels_refresh_all()
            except Exception as e:
                self._set_status(f"Channels refresh error: {e}")
        if self.screen == "status" and (now - self.status_last_fetch) >= self.auto_refresh_interval:
            try:
                self._fetch_status()
            except Exception as e:
                self._set_status(f"Status refresh error: {e}")
        if self.screen == "config" and (now - self.config_last_fetch) >= self.auto_refresh_interval:
            try:
                self._fetch_config()
            except Exception as e:
                self._set_status(f"Config refresh error: {e}")
        if self.screen == "extensions" and (now - self.extensions_last_fetch) >= self.auto_refresh_interval:
            try:
                self._fetch_extensions()
            except Exception as e:
                self._set_status(f"Extensions refresh error: {e}")
        if self.screen == "logs" and (now - self.logs_last_fetch) >= self.auto_refresh_interval:
            try:
                self._fetch_logs()
            except Exception as e:
                self._set_status(f"Logs refresh error: {e}")

    def _action_channels_freq(self, stdscr: curses.window, theme: Theme) -> None:
        freq_key = self._selected_freq_key()
        if not freq_key:
            self._set_status("No channel selected")
            return
        action = _modal_select(
            stdscr,
            theme,
            "Channel Actions",
            ["Change pathloss", "Set shadowing", "Refresh", "Back"],
        )
        if not action or action == "Back":
            return
        if action == "Refresh":
            self._set_status("Refreshing…")
            self._channels_refresh_all()
            self._set_status("Refreshed")
            return
        if action == "Change pathloss":
            model = _modal_select(
                stdscr, theme, "Propagation Model",
                ["FREE_SPACE", "2_RAY", "3GPP_38_901", "OKUMURA_HATA (experimental)", "LONGLEY_RICE (experimental)", "NONE"],
            )
            if not model:
                return
            model = model.split(" (")[0]  # strip label suffix
            ground = -1.0
            scenario: Optional[str] = None
            environment: Optional[str] = None
            refractivity: Optional[float] = None
            ground_conductivity: Optional[float] = None
            ground_permittivity: Optional[float] = None
            climate_zone: Optional[int] = None
            if model == "2_RAY":
                raw = _modal_input(stdscr, theme, "2_RAY", "Ground reflection coefficient:", initial="-1")
                if raw is None:
                    return
                try:
                    ground = float(raw)
                except Exception:
                    self._set_status("Invalid ground coefficient")
                    return
            if model == "3GPP_38_901":
                scenario = _modal_select(
                    stdscr,
                    theme,
                    "3GPP Scenario",
                    ["UMa", "UMi-StreetCanyon", "RMa"],
                )
                if not scenario:
                    return
            if model == "OKUMURA_HATA":
                environment = _modal_select(
                    stdscr, theme, "Hata Environment", ["URBAN", "SUBURBAN", "OPEN"],
                )
                if not environment:
                    return
            if model == "LONGLEY_RICE":
                raw = _modal_input(stdscr, theme, "Longley-Rice", "Surface refractivity (N-units):", initial="301")
                if raw is None:
                    return
                try:
                    refractivity = float(raw)
                except Exception:
                    self._set_status("Invalid refractivity")
                    return
                raw = _modal_input(stdscr, theme, "Longley-Rice", "Climate zone (1-7, 5=Continental Temperate):", initial="5")
                if raw is None:
                    return
                try:
                    climate_zone = int(float(raw))
                except Exception:
                    self._set_status("Invalid climate zone")
                    return
            try:
                resp = self.client.set_pathloss(
                    float(freq_key),
                    model,
                    ground,
                    scenario=scenario,
                    environment=environment,
                    refractivity=refractivity,
                    ground_conductivity=ground_conductivity,
                    ground_permittivity=ground_permittivity,
                    climate_zone=climate_zone,
                )
                self._set_status(f"Pathloss updated: {resp}")
                self._channels_refresh_all()
            except Exception as e:
                self._set_status(f"Pathloss update failed: {e}")
            return

        if action == "Set shadowing":
            initial = str(self.shadow_last_value if self.shadow_last_value is not None else "2.0")
            raw = _modal_input(stdscr, theme, "Shadowing", "Shadowing std:", initial=initial)
            if raw is None:
                return
            try:
                val = float(raw)
            except Exception:
                self._set_status("Invalid std")
                return
            try:
                resp = self.client.set_shadow_std(val, freq_mhz=float(freq_key))
                self._fetch_shadow_std()
                self._set_status(f"Shadowing updated: {resp}")
            except Exception as e:
                self._set_status(f"Shadowing update failed: {e}")

    def _action_channels_link(self, stdscr: curses.window, theme: Theme) -> None:
        freq_key = self._selected_freq_key()
        link = self._selected_link()
        if not freq_key or not link:
            self._set_status("No link selected")
            return
        action = _modal_select(
            stdscr,
            theme,
            "Link Actions",
            [
                "Set coeff",
                "Apply CIR scenario",
                "Set CIR taps",
                "Clear CIR taps",
                "Set frequency offset",
                "Toggle doppler",
                "Set channel noise model",
                "Refresh",
                "Back",
            ],
        )
        if not action or action == "Back":
            return
        if action == "Refresh":
            self._set_status("Refreshing…")
            self._channels_refresh_all()
            self._set_status("Refreshed")
            return

        src = str(link.get("src", ""))
        dest = str(link.get("dest", ""))
        src_id = str(link.get("srcId", src))
        dest_id = str(link.get("destId", dest))
        p_src = int(link.get("p_src", 0))
        p_dest = int(link.get("p_dest", 0))

        if action == "Apply CIR scenario":
            if not CIR_SCENARIOS:
                self._set_status("No CIR scenarios available")
                return
            scenario = _modal_select(stdscr, theme, "CIR Scenario", list(CIR_SCENARIOS.keys()))
            if not scenario:
                return

            max_taps = 64
            try:
                cfg = self.client.get_config()
                if isinstance(cfg, dict) and isinstance(cfg.get("cirMaxTaps"), (int, float)):
                    max_taps = int(cfg["cirMaxTaps"])
            except Exception:
                pass

            raw_scale = _modal_input(
                stdscr,
                theme,
                "CIR Scale",
                "Tap scale (linear):",
                initial="1.0",
            )
            if raw_scale is None:
                return
            try:
                scale = float(raw_scale)
            except Exception:
                self._set_status("Invalid scale")
                return

            taps_payload = _scale_complex_taps(CIR_SCENARIOS.get(scenario, []), scale)
            if max_taps > 0:
                taps_payload = taps_payload[:max_taps]

            try:
                resp = self.client.set_cir(float(freq_key), src_id, dest_id, p_src, p_dest, taps_payload)
                self._set_status(f"CIR scenario applied: {scenario}")
                self._channels_refresh_all()
            except Exception as e:
                self._set_status(f"CIR update failed: {e}")
            return

        if action in ("Set CIR taps", "Clear CIR taps"):
            max_taps = 64
            try:
                cfg = self.client.get_config()
                if isinstance(cfg, dict) and isinstance(cfg.get("cirMaxTaps"), (int, float)):
                    max_taps = int(cfg["cirMaxTaps"])
            except Exception:
                pass

            taps_payload: list[list[float]] = []
            if action == "Set CIR taps":
                raw = _modal_input(
                    stdscr,
                    theme,
                    "Set CIR Taps",
                    f"CIR taps JSON (max {max_taps}). Examples: [1, 0.2] or [[1,0],[0.2,0.1]]",
                    initial="[]",
                )
                if raw is None:
                    return
                try:
                    parsed = json.loads(raw.strip())
                    if not isinstance(parsed, list):
                        raise ValueError("CIR taps must be a JSON array")
                    for t in parsed:
                        if max_taps > 0 and len(taps_payload) >= max_taps:
                            break
                        if isinstance(t, (int, float)):
                            taps_payload.append([float(t), 0.0])
                        elif isinstance(t, (list, tuple)):
                            re = float(t[0]) if len(t) >= 1 else 0.0
                            im = float(t[1]) if len(t) >= 2 else 0.0
                            taps_payload.append([re, im])
                        elif isinstance(t, dict):
                            re = float(t.get("re", 0.0))
                            im = float(t.get("im", 0.0))
                            taps_payload.append([re, im])
                        else:
                            raise ValueError("Unsupported tap format")
                except Exception as e:
                    self._set_status(f"Invalid CIR taps: {e}")
                    return

            try:
                resp = self.client.set_cir(float(freq_key), src_id, dest_id, p_src, p_dest, taps_payload)
                self._set_status(f"CIR updated: {resp}")
                self._channels_refresh_all()
            except Exception as e:
                self._set_status(f"CIR update failed: {e}")
            return

        if action == "Set coeff":
            raw = _modal_input(
                stdscr,
                theme,
                "Set Coefficient",
                f"Coeff for {src}:{p_src} -> {dest}:{p_dest}:",
                initial=str(link.get("coeff", "1.0")),
            )
            if raw is None:
                return
            try:
                coeff = float(raw)
            except Exception:
                self._set_status("Invalid coefficient")
                return
            try:
                resp = self.client.set_coeff(float(freq_key), src_id, dest_id, p_src, p_dest, coeff)
                self._set_status(f"Coeff updated: {resp}")
                self._channels_refresh_all()
            except Exception as e:
                self._set_status(f"Coeff update failed: {e}")
            return

        if action == "Set channel noise model":
            model = _modal_select(stdscr, theme, "Noise Model", ["AWGN", "NONE"])
            if not model:
                return
            raw = _modal_input(
                stdscr,
                theme,
                "Set SNR",
                f"SNR for channel {src} -> {dest}:",
                initial=str(link.get("snrOverrideDb", link.get("snr", "0"))),
            )
            if raw is None:
                return
            try:
                snr = float(raw)
            except Exception:
                self._set_status("Invalid SNR")
                return
            try:
                resp = self.client.set_noise_model(float(freq_key), src_id, dest_id, model, snr)
                self._set_status(f"Noise updated: {resp}")
                self._channels_refresh_all()
            except Exception as e:
                self._set_status(f"Noise update failed: {e}")
            return

        if action == "Toggle doppler":
            enabled = not bool(link.get("dopplerEnabled", False))
            try:
                resp = self.client.set_doppler(float(freq_key), src_id, dest_id, enabled)
                self._set_status(f"Doppler {'enabled' if enabled else 'disabled'}: {resp}")
                self._channels_refresh_all()
            except Exception as e:
                self._set_status(f"Doppler update failed: {e}")
            return

        if action == "Set frequency offset":
            raw = _modal_input(
                stdscr,
                theme,
                "Frequency Offset",
                f"Offset (Hz) for channel {src} -> {dest}:",
                initial=str(link.get("freqOffsetHz", "0.0")),
            )
            if raw is None:
                return
            try:
                offset_hz = float(raw)
            except Exception:
                self._set_status("Invalid offset")
                return
            try:
                resp = self.client.set_frequency_offset(float(freq_key), src_id, dest_id, offset_hz)
                self._set_status(f"Freq offset updated: {resp}")
                self._channels_refresh_all()
            except Exception as e:
                self._set_status(f"Freq offset update failed: {e}")

    def _action_set_default_propagation(self, stdscr: curses.window, theme: Theme) -> None:
        model = _modal_select(
            stdscr, theme, "Default Propagation Model",
            ["FREE_SPACE", "2_RAY", "3GPP_38_901", "OKUMURA_HATA (experimental)", "LONGLEY_RICE (experimental)", "NONE"],
        )
        if not model:
            return
        model = model.split(" (")[0]  
        ground = -1.0
        scenario: Optional[str] = None
        environment: Optional[str] = None
        refractivity: Optional[float] = None
        climate_zone: Optional[int] = None
        if model == "2_RAY":
            raw = _modal_input(stdscr, theme, "2_RAY", "Ground reflection coefficient:", initial="-1")
            if raw is None:
                return
            try:
                ground = float(raw)
            except Exception:
                self._set_status("Invalid ground coefficient")
                return
        if model == "3GPP_38_901":
            scenario = _modal_select(
                stdscr, theme, "3GPP Scenario", ["UMa", "UMi-StreetCanyon", "RMa"]
            )
            if not scenario:
                return
        if model == "OKUMURA_HATA":
            environment = _modal_select(
                stdscr, theme, "Hata Environment", ["URBAN", "SUBURBAN", "OPEN"],
            )
            if not environment:
                return
        if model == "LONGLEY_RICE":
            raw = _modal_input(stdscr, theme, "Longley-Rice", "Surface refractivity (N-units):", initial="301")
            if raw is None:
                return
            try:
                refractivity = float(raw)
            except Exception:
                self._set_status("Invalid refractivity")
                return
            raw = _modal_input(stdscr, theme, "Longley-Rice", "Climate zone (1-7, 5=Continental Temperate):", initial="5")
            if raw is None:
                return
            try:
                climate_zone = int(float(raw))
            except Exception:
                self._set_status("Invalid climate zone")
                return
        try:
            resp = self.client.set_default_pathloss(
                model, ground, scenario=scenario,
                environment=environment,
                refractivity=refractivity,
                climate_zone=climate_zone,
            )
            status = resp.get("status", "unknown")
            self._set_status(f"Default propagation set to {model}: {status}")
        except Exception as e:
            self._set_status(f"Failed to set default propagation: {e}")

    def _enter_profiles(self) -> None:
        self.profiles_map = all_profiles()
        builtin_names = set(BUILTIN_PROFILES.keys())
        self.profiles_list = list(self.profiles_map.keys())
        self.profiles_state.items = [
            f"{name}  {'[built-in]' if name in builtin_names else '[custom]'}"
            for name in self.profiles_list
        ]
        self.profiles_state.clamp()
        self._refresh_profile_detail()
        self._set_status("Profiles (Enter to apply, p to set propagation)")

    def _selected_profile(self) -> Optional[dict]:
        if not self.profiles_list:
            return None
        idx = min(self.profiles_state.selected, len(self.profiles_list) - 1)
        name = self.profiles_list[idx]
        return self.profiles_map.get(name)

    def _refresh_profile_detail(self) -> None:
        profile = self._selected_profile()
        lines: list[str] = []
        if not profile:
            lines.append("(no profiles)")
            self.profiles_detail_lines = lines
            return

        name = profile.get("name", "Unnamed")
        desc = profile.get("description", "")
        lines.append(f"Name:  {name}")
        if desc:
            lines.append(f"       {desc}")
        lines.append("")

        note = profile.get("note", "")
        if note:
            lines.append("Note:")
            for wline in _wrap_lines(note, 50):
                lines.append(f"  {wline}")
            lines.append("")

        prop = profile.get("propagation")
        if isinstance(prop, dict) and "model" in prop:
            lines.append(f"Propagation")
            lines.append(f"  Model:       {prop['model']}")
            for key, label in (
                ("scenario", "Scenario"),
                ("environment", "Environment"),
                ("groundCoeff", "Ground coeff"),
                ("refractivity", "Refractivity"),
                ("groundConductivity", "Ground cond."),
                ("groundPermittivity", "Ground perm."),
                ("climateZone", "Climate zone"),
            ):
                if key in prop:
                    lines.append(f"  {label + ':':<15}{prop[key]}")
            lines.append("")

        shadow = profile.get("shadowingStd")
        if shadow is not None:
            lines.append(f"Shadowing std: {shadow}")
            lines.append("")

        ref_map = profile.get("sourcePowerDbfs")
        if isinstance(ref_map, dict):
            lines.append("Ref signal (dBFS)")
            for ntype, val in ref_map.items():
                label = "Any type" if ntype == "*" else ntype
                lines.append(f"  {label + ':':<20}{val}")
            lines.append("")

        cfg = profile.get("config")
        if isinstance(cfg, dict):
            lines.append("Config")
            for key, val in cfg.items():
                lines.append(f"  {key + ':':<24}{val}")
            lines.append("")

        if not any(k in profile for k in ("propagation", "shadowingStd", "sourcePowerDbfs", "config")):
            lines.append("(empty profile — nothing to apply)")

        self.profiles_detail_lines = lines

    def _render_profiles(self, stdscr: curses.window, theme: Theme) -> None:
        height, width = stdscr.getmaxyx()
        content_h = height - 6
        content_w = width - 4
        y0, x0 = 3, 2

        left_w = max(28, content_w // 3)
        right_w = content_w - left_w - 1

        _draw_box(stdscr, y0, x0, content_h, left_w, "Profiles", theme)
        _draw_box(stdscr, y0, x0 + left_w + 1, content_h, right_w, "Details", theme)

        # Profile list (left panel)
        _draw_list(
            stdscr,
            y0 + 2,
            x0 + 1,
            content_h - 3,
            left_w - 2,
            self.profiles_state,
            theme,
        )

        # Detail table (right panel)
        detail_y = y0 + 2
        detail_x = x0 + left_w + 2
        detail_w = right_w - 2
        detail_h = content_h - 3
        for i, line in enumerate(self.profiles_detail_lines[:detail_h]):
            try:
                stdscr.addnstr(detail_y + i, detail_x, _ellipsis(line, detail_w).ljust(detail_w), detail_w, theme.normal())
            except curses.error:
                pass

    def _action_apply_selected_profile(self, stdscr: curses.window, theme: Theme) -> None:
        profile = self._selected_profile()
        if not profile:
            self._set_status("No profile selected")
            return
        name = profile.get("name", "Unnamed")
        confirm = _modal_select(
            stdscr, theme, f"Apply '{name}'?",
            ["Apply", "Cancel"],
            help_text="Enter to confirm, Esc to cancel",
        )
        if not confirm or confirm == "Cancel":
            return
        try:
            actions = self.client.apply_profile(profile)
            msg = f"Profile '{name}' applied: " + "; ".join(actions) if actions else f"Profile '{name}' applied (no changes)"
            self._set_status(_ellipsis(msg, 120))
        except Exception as e:
            self._set_status(f"Profile apply failed: {e}")

    def _action_save_profile(self, stdscr: curses.window, theme: Theme) -> None:
        name = _modal_input(stdscr, theme, "New Profile", "Profile name:")
        if not name:
            return
        desc = _modal_input(stdscr, theme, "New Profile", "Description (optional):", initial="")
        note = _modal_input(stdscr, theme, "New Profile", "Note (optional):", initial="")

        profile: dict[str, Any] = {"name": name}
        if desc:
            profile["description"] = desc
        if note:
            profile["note"] = note

        # Snapshot propagation from individual channels
        try:
            ind = self.client.get_individual_channels()
            if ind:
                first_freq = next(iter(ind.values()), [])
                if isinstance(first_freq, list) and first_freq:
                    ch = first_freq[0] if isinstance(first_freq[0], dict) else {}
                    pl = ch.get("pathLoss")
                    if pl:
                        prop: dict[str, Any] = {"model": str(pl)}
                        for key in ("scenario", "environment", "groundCoeff",
                                    "refractivity", "groundConductivity",
                                    "groundPermittivity", "climateZone"):
                            if key in ch:
                                prop[key] = ch[key]
                        profile["propagation"] = prop
        except Exception:
            pass

        # Snapshot shadowing
        try:
            shadow = self.client.get_shadow_std()
            if shadow is not None:
                profile["shadowingStd"] = shadow
        except Exception:
            pass

        # Snapshot ref signal per node
        try:
            nodes = self.client.get_nodes()
            if nodes:
                ref_map: dict[str, float] = {}
                for node_name, node_info in nodes.items():
                    ref = node_info.get("sourcePowerDbfs")
                    if ref is not None:
                        ntype = str(node_info.get("nodeType", "Unknown"))
                        ref_map[ntype] = float(ref)
                if ref_map:
                    profile["sourcePowerDbfs"] = ref_map
        except Exception:
            pass

        # Snapshot config
        try:
            cfg = self.client.get_config()
            if cfg:
                config_save: dict[str, Any] = {}
                for key in ("vehiclePollRateMs", "channelUpdateRateMs", "cirMaxTaps"):
                    if key in cfg:
                        config_save[key] = cfg[key]
                if config_save:
                    profile["config"] = config_save
        except Exception:
            pass

        try:
            path = save_profile(profile)
            self._set_status(f"Profile saved: {path}")
            # Reload list so the new profile appears
            self._enter_profiles()
        except Exception as e:
            self._set_status(f"Save failed: {e}")

    def handle_key(self, stdscr: curses.window, theme: Theme, key: int) -> bool:
        # return False to exit
        if self.screen == "main":
            if key in (curses.KEY_UP, ord("k")):
                self.main_state.move(-1)
            elif key in (curses.KEY_DOWN, ord("j")):
                self.main_state.move(1)
            elif key in (curses.KEY_ENTER, 10, 13):
                choice = self.main_state.items[self.main_state.selected] if self.main_state.items else ""
                if choice == "Quit":
                    return False
                if choice == "Nodes":
                    self.screen = "nodes"
                    self._enter_nodes()
                elif choice == "Channels":
                    self.screen = "channels"
                    self._enter_channels()
                elif choice == "Status":
                    self.screen = "status"
                    self._enter_status()
                elif choice == "Config":
                    self.screen = "config"
                    self._enter_config()
                elif choice == "Profiles":
                    self.screen = "profiles"
                    self._enter_profiles()
                elif choice == "Extensions":
                    self.screen = "extensions"
                    self._enter_extensions()
                elif choice == "Logs":
                    self.screen = "logs"
                    self._enter_logs()
                elif choice == "Set Default Propagation":
                    self._action_set_default_propagation(stdscr, theme)
            elif key in (ord("q"), 27):
                return False
            return True

        if key in (ord("q"), 27):  # back
            self.screen = "main"
            self._set_status("")
            return True

        if self.screen == "nodes":
            if key in (curses.KEY_UP, ord("k")):
                self.nodes_state.move(-1)
                if self.nodes_state.items:
                    self._set_status(f"Selected: {self.nodes_state.items[self.nodes_state.selected]}")
            elif key in (curses.KEY_DOWN, ord("j")):
                self.nodes_state.move(1)
                if self.nodes_state.items:
                    self._set_status(f"Selected: {self.nodes_state.items[self.nodes_state.selected]}")
            elif key in (curses.KEY_ENTER, 10, 13):
                self._action_nodes(stdscr, theme)
            elif key in (ord("r"),):
                try:
                    self._enter_nodes()
                except Exception as e:
                    self._set_status(f"Refresh failed: {e}")
            return True

        if self.screen == "channels":
            if key == 9:  # Tab
                self.channels_focus = "link" if self.channels_focus == "freq" else "freq"
                self._set_status(f"Focus: {self.channels_focus}")
                return True

            if key in (ord("r"),):
                try:
                    self._set_status("Refreshing…")
                    self._channels_refresh_all()
                    self._set_status("Refreshed")
                except Exception as e:
                    self._set_status(f"Refresh failed: {e}")
                return True

            if self.channels_focus == "freq":
                if key in (curses.KEY_UP, ord("k")):
                    self.channels_state.move(-1)
                    self._refresh_links_list()
                    try:
                        self._fetch_shadow_std()
                    except Exception:
                        pass
                elif key in (curses.KEY_DOWN, ord("j")):
                    self.channels_state.move(1)
                    self._refresh_links_list()
                    try:
                        self._fetch_shadow_std()
                    except Exception:
                        pass
                elif key in (curses.KEY_ENTER, 10, 13):
                    self._action_channels_freq(stdscr, theme)
                return True

            # link focus
            if key in (curses.KEY_UP, ord("k")):
                self.links_state.move(-1)
            elif key in (curses.KEY_DOWN, ord("j")):
                self.links_state.move(1)
            elif key in (curses.KEY_ENTER, 10, 13):
                self._action_channels_link(stdscr, theme)
            return True

        if self.screen == "status":
            if key in (ord("r"),):
                try:
                    self._enter_status()
                except Exception as e:
                    self._set_status(f"Refresh failed: {e}")
            return True

        if self.screen == "config":
            if key in (curses.KEY_UP, ord("k")):
                self.config_state.move(-1)
            elif key in (curses.KEY_DOWN, ord("j")):
                self.config_state.move(1)
            elif key in (ord("r"),):
                try:
                    self._enter_config()
                except Exception as e:
                    self._set_status(f"Refresh failed: {e}")
            elif key in (curses.KEY_ENTER, 10, 13):
                self._action_config(stdscr, theme)
            return True

        if self.screen == "profiles":
            if key in (curses.KEY_UP, ord("k")):
                self.profiles_state.move(-1)
                self._refresh_profile_detail()
            elif key in (curses.KEY_DOWN, ord("j")):
                self.profiles_state.move(1)
                self._refresh_profile_detail()
            elif key in (curses.KEY_ENTER, 10, 13):
                self._action_apply_selected_profile(stdscr, theme)
            elif key == ord("s"):
                self._action_save_profile(stdscr, theme)
            elif key == ord("p"):
                self._action_set_default_propagation(stdscr, theme)
            return True

        if self.screen == "extensions":
            if key in (ord("r"),):
                try:
                    self._enter_extensions()
                except Exception as e:
                    self._set_status(f"Refresh failed: {e}")
                return True
            if key in (curses.KEY_ENTER, 10, 13):
                self._action_extensions(stdscr, theme)
                return True
            return True

        if self.screen == "logs":
            height, _ = stdscr.getmaxyx()
            page = max(1, height - 8)
            total = len(self.logs_lines)
            if key in (curses.KEY_UP, ord("k")):
                self.logs_follow = False
                self.logs_scroll = max(0, self.logs_scroll - 1)
            elif key in (curses.KEY_DOWN, ord("j")):
                self.logs_scroll = min(max(0, total - 1), self.logs_scroll + 1)
                if self.logs_scroll >= max(0, total - page):
                    self.logs_follow = True
            elif key in (curses.KEY_PPAGE,):
                self.logs_follow = False
                self.logs_scroll = max(0, self.logs_scroll - page)
            elif key in (curses.KEY_NPAGE,):
                self.logs_scroll = min(max(0, total - 1), self.logs_scroll + page)
                if self.logs_scroll >= max(0, total - page):
                    self.logs_follow = True
            elif key == ord("g"):
                self.logs_follow = False
                self.logs_scroll = 0
            elif key == ord("G"):
                self.logs_follow = True
                self.logs_scroll = max(0, total - page)
            elif key == ord("f"):
                self.logs_follow = not self.logs_follow
                self._set_status(f"Follow: {'on' if self.logs_follow else 'off'}")
            elif key == ord("c"):
                self.logs_lines = []
                self.logs_scroll = 0
                self._set_status("Log view cleared")
            elif key == ord("r"):
                try:
                    self._fetch_logs()
                    self._set_status(f"Logs: {len(self.logs_lines)} lines")
                except Exception as e:
                    self._set_status(f"Refresh failed: {e}")
            return True

        return True


def run_tui(client: ChemClient) -> None:
    def _main(stdscr: curses.window) -> None:
        curses.curs_set(0)
        stdscr.nodelay(True)
        stdscr.timeout(TUI_POLL_TIMEOUT_MS)
        stdscr.keypad(True)
        theme = Theme()

        app = App(client)
        app._set_status(f"Connecting to CHEM {client.addr}:{client.port}…")
        try:
            app._fetch_version()
            app._fetch_sandbox_status()
            app._set_status("Connected")
        except ChemConnectionError as e:
            app.version = "Unknown"
            app._set_status(f"Warning: no connection to CHEM ({client.addr}:{client.port}): {e}")
        except Exception as e:
            app.version = "Unknown"
            app._set_status(f"Startup error: {e}")

        while True:
            app._auto_refresh()
            app.render(stdscr, theme)
            key = stdscr.getch()
            if key == -1:
                continue
            try:
                if not app.handle_key(stdscr, theme, key):
                    break
            except Exception as e:
                app._set_status(f"Error: {e}")

    curses.wrapper(_main)


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="pychem: CHEM channel emulator TUI")
    parser.add_argument("--addr", "--chem_addr", dest="addr", type=str, default="", help="CHEM server address")
    parser.add_argument("--port", dest="port", type=int, default=5000, help="CHEM coordinator port")
    parser.add_argument("--tui", action="store_true", help="Launch TUI (default)")
    parser.add_argument("--version", action="store_true", help="Print CHEM version and exit")
    parser.add_argument(
        "--propagation",
        choices=["FREE_SPACE", "2_RAY", "3GPP_38_901", "OKUMURA_HATA", "LONGLEY_RICE", "NONE"],
        default=None,
        help="Set default propagation model before launching TUI (OKUMURA_HATA and LONGLEY_RICE are experimental)",
    )
    parser.add_argument("--ground-coeff", type=float, default=-1.0, help="Ground reflection coefficient (for 2_RAY)")
    parser.add_argument("--scenario", type=str, default=None, help="3GPP scenario (for 3GPP_38_901, e.g. UMa, UMi-StreetCanyon, RMa)")
    parser.add_argument("--environment", type=str, default=None, help="Hata environment (for OKUMURA_HATA: URBAN, SUBURBAN, OPEN)")
    parser.add_argument("--refractivity", type=float, default=None, help="Surface refractivity in N-units (for LONGLEY_RICE)")
    parser.add_argument("--climate-zone", type=int, default=None, help="ITM climate zone 1-7 (for LONGLEY_RICE, 5=Continental Temperate)")
    parser.add_argument("--profile", type=str, default=None, help="Apply a named profile on startup (built-in or custom)")
    parser.add_argument("--no-tui", action="store_true", help="Apply CLI options and exit without launching TUI")
    parser.add_argument("--ext-start", type=str, nargs=2, default=None, metavar=("NAME", "JSON_PARAMS"),
                        help="Start an extension with JSON params (e.g. --ext-start sionna '{\"serverUrl\":\"http://localhost:8000\"}')")
    parser.add_argument("--ext-stop", type=str, default=None, metavar="NAME",
                        help="Stop an extension by name")
    args = parser.parse_args(argv)

    client = ChemClient(addr=args.addr, port=args.port)

    if args.version:
        try:
            print(client.get_version() or "Unknown")
            return 0
        except Exception as e:
            print(f"Error: {e}")
            return 1

    # Send default propagation model if specified via CLI
    if args.propagation:
        try:
            resp = client.set_default_pathloss(
                args.propagation,
                args.ground_coeff,
                scenario=args.scenario,
                environment=args.environment,
                refractivity=args.refractivity,
                climate_zone=args.climate_zone,
            )
            status = resp.get("status", "unknown")
            print(f"Default propagation set to {args.propagation}: {status}")
        except Exception as e:
            print(f"Warning: failed to set default propagation: {e}")

    if args.profile:
        profiles = all_profiles()
        profile = profiles.get(args.profile)
        if not profile:
            print(f"Error: profile '{args.profile}' not found. Available: {', '.join(profiles.keys())}")
            return 1
        try:
            actions = client.apply_profile(profile)
            for a in actions:
                print(f"  {a}")
            print(f"Profile '{args.profile}' applied")
        except Exception as e:
            print(f"Error applying profile: {e}")
            return 1

    # Extensions
    if args.ext_stop:
        try:
            resp = client.extension(args.ext_stop, "stop")
            print(f"{args.ext_stop} stopped: {resp.get('message', resp.get('status', ''))}")
        except Exception as e:
            print(f"Warning: failed to stop {args.ext_stop}: {e}")

    if args.ext_start:
        ext_name, params_json = args.ext_start
        try:
            params = json.loads(params_json) if params_json else {}
        except json.JSONDecodeError:
            print(f"Error: invalid JSON params: {params_json}")
            return 1
        try:
            resp = client.extension(ext_name, "start", params)
            print(f"{ext_name} started: {resp.get('message', resp.get('status', ''))}")
        except Exception as e:
            print(f"Warning: failed to start {ext_name}: {e}")

    if args.no_tui:
        return 0

    # default mode is TUI
    try:
        run_tui(client)
        return 0
    except KeyboardInterrupt:
        return 130
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
