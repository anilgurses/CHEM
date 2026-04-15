#!/usr/bin/env python3
"""
Generate C++ antenna headers from measured pattern files under data/antenna.

Each antenna folder (e.g. data/antenna/SA-1400-5900) holds multiple files named
like "<pattern>-F2100.txt" containing tab-separated columns:
    Phi    Theta   E Total. dB    ...
Phi/Theta are in radians and E Total. dB is used as the gain.

Filename indicates the frequency in MHz.

Produces a .h (struct + declarations + class) and a .cpp (data arrays
+ function definitions) for each antenna pattern.
"""

import argparse
import math
import re
import sys
from pathlib import Path
from typing import List, Optional, Tuple

ROOT_DEFAULT = Path("data/antenna")
HDR_OUT_DEFAULT = Path("include/chem/antennas/generated")
SRC_OUT_DEFAULT = Path("src/chem/antennas/generated")


def parse_pattern_file(path: Path) -> Tuple[float, List[Tuple[float, float, float]]]:
    lines = [ln.strip() for ln in path.read_text().splitlines() if ln.strip()]
    if len(lines) < 3:
        raise ValueError(f"File too short: {path}")

    freq_line = next((ln for ln in lines if ln.lower().startswith("frequency=")), None)
    if not freq_line:
        raise ValueError(f"Frequency line missing in {path}")
    match = re.search(r"frequency\s*=\s*([0-9.]+)", freq_line, re.IGNORECASE)
    if not match:
        raise ValueError(f"Could not parse frequency in {path}")
    freq_mhz = float(match.group(1))

    header_idx = next(i for i, ln in enumerate(lines) if "phi" in ln.lower())
    data_lines = lines[header_idx + 1 :]
    entries: List[Tuple[float, float, float]] = []
    for ln in data_lines:
        parts = re.split(r"[ \t]+", ln)
        if len(parts) < 3:
            continue
        try:
            phi = float(parts[0])
            theta = float(parts[1])
            gain_db = float(parts[2])
        except ValueError:
            continue
        entries.append((phi, theta, gain_db))

    if not entries:
        raise ValueError(f"No gain entries parsed : {path}")
    return freq_mhz, entries


def sanitize_name(name: str) -> str:
    clean = re.sub(r"[^0-9A-Za-z]+", "_", name)
    clean = clean.strip("_")
    if clean and clean[0].isdigit():
        clean = f"A_{clean}"
    return clean or "Antenna"


def unique_sorted(values: List[float], tol: float = 1e-6) -> List[float]:
    values = sorted(values)
    if not values:
        return []
    merged = [values[0]]
    for v in values[1:]:
        if abs(v - merged[-1]) > tol:
            merged.append(v)
    return merged


def uniform_step(values: List[float], tol: float = 1e-6) -> Optional[float]:
    if len(values) < 2:
        return None
    steps = [b - a for a, b in zip(values, values[1:]) if abs(b - a) > tol]
    if not steps:
        return None
    base = steps[0]
    if any(abs(s - base) > tol for s in steps[1:]):
        return None
    return base


def build_grid(entries: List[Tuple[float, float, float]]) -> Optional[Tuple[float, float, int, float, float, int, List[List[float]]]]:
    if not entries:
        return None

    phis = unique_sorted([p for p, _, _ in entries])
    thetas = unique_sorted([t for _, t, _ in entries])
    phi_step = uniform_step(phis)
    theta_step = uniform_step(thetas)
    if phi_step is None or theta_step is None:
        return None

    phi_size = len(phis)
    theta_size = len(thetas)
    if phi_size * theta_size != len(entries):
        return None

    phi_min = phis[0]
    theta_min = thetas[0]

    grid: List[List[float]] = [[0.0 for _ in range(phi_size)] for _ in range(theta_size)]
    for phi, theta, gain in entries:
        phi_idx = round((phi - phi_min) / phi_step)
        theta_idx = round((theta - theta_min) / theta_step)
        if phi_idx < 0 or phi_idx >= phi_size or theta_idx < 0 or theta_idx >= theta_size:
            return None
        grid[theta_idx][phi_idx] = gain

    return phi_min, phi_step, phi_size, theta_min, theta_step, theta_size, grid


def format_grid_flat(grid: List[List[float]], values_per_line: int = 12) -> str:
    all_vals = []
    for row in grid:
        for val in row:
            all_vals.append(f"{val}f")
    lines = []
    for i in range(0, len(all_vals), values_per_line):
        chunk = ", ".join(all_vals[i : i + values_per_line])
        lines.append(f"    {chunk}")
    return ",\n".join(lines)


def find_pattern_files(pattern_dir: Path) -> List[Path]:
    return sorted([p for p in pattern_dir.glob(f"{pattern_dir.name}-F*.txt") if p.is_file()])


def generate_header(stem: str, freq_tables_sorted: List[Tuple[float, Optional[Tuple]]]) -> str:
    """Generate the lightweight .h content."""
    num_tables = len(freq_tables_sorted)
    return f"""#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include "chem/antennas/dipole.h"

namespace chem::antennas::generated {{

struct PatternTable_{stem} {{
    float freq_mhz;
    float phi_min;
    float phi_step;
    size_t phi_size;
    float theta_min;
    float theta_step;
    size_t theta_size;
    const float* gains;  // row-major [theta][phi]
}};

extern const PatternTable_{stem} kTables_{stem}[{num_tables}];
extern const size_t kTablesCount_{stem};

const PatternTable_{stem}& nearest_table_{stem}(float freq_mhz);
float lookup_gain_{stem}(const PatternTable_{stem}& tbl, float theta, float phi);

class {stem} : public Antenna {{
public:
    explicit {stem}(double freq_hz) : freq_mhz_(static_cast<float>(freq_hz / 1e6)) {{}}
    double get_gain(double theta, double phi) const override {{
        const auto& tbl = nearest_table_{stem}(freq_mhz_);
        return static_cast<double>(lookup_gain_{stem}(tbl, static_cast<float>(theta), static_cast<float>(phi)));
    }}

private:
    float freq_mhz_;
}};

}} // namespace chem::antennas::generated
"""


def generate_source(stem: str, freq_tables_sorted: List[Tuple[float, Optional[Tuple]]], hdr_include: str) -> str:
    """Generate the .cpp content with data arrays and function definitions."""
    parts: List[str] = []
    parts.append(f'#include "{hdr_include}"')
    parts.append("")
    parts.append(f"namespace chem::antennas::generated {{")
    parts.append("")

    table_inits: List[str] = []
    for freq_mhz, grid_info in freq_tables_sorted:
        freq_tag = int(round(freq_mhz))
        arr_name = f"kGrid_F{freq_tag}_{stem}"
        phi_min, phi_step, phi_size, theta_min, theta_step, theta_size, grid_data = grid_info

        grid_text = format_grid_flat(grid_data)
        total = theta_size * phi_size
        parts.append(f"static const float {arr_name}[{total}] = {{")
        parts.append(grid_text)
        parts.append("};")
        parts.append("")

        table_inits.append(
            f"    {{{freq_mhz}f, {phi_min}f, {phi_step}f, {phi_size}, "
            f"{theta_min}f, {theta_step}f, {theta_size}, {arr_name}}},"
        )

    num_tables = len(table_inits)
    table_list = "\n".join(table_inits)
    parts.append(f"const PatternTable_{stem} kTables_{stem}[{num_tables}] = {{")
    parts.append(table_list)
    parts.append("};")
    parts.append("")
    parts.append(f"const size_t kTablesCount_{stem} = {num_tables};")
    parts.append("")

    parts.append(f"""const PatternTable_{stem}& nearest_table_{stem}(float freq_mhz) {{
    const PatternTable_{stem}* best = &kTables_{stem}[0];
    float best_diff = std::abs(freq_mhz - best->freq_mhz);
    for (size_t i = 1; i < kTablesCount_{stem}; ++i) {{
        const float diff = std::abs(freq_mhz - kTables_{stem}[i].freq_mhz);
        if (diff < best_diff) {{
            best = &kTables_{stem}[i];
            best_diff = diff;
        }}
    }}
    return *best;
}}""")
    parts.append("")

    parts.append(f"""float lookup_gain_{stem}(const PatternTable_{stem}& tbl, float theta, float phi) {{
    constexpr float two_pi = 2.0f * {math.pi:.9f}f;
    float rel_phi = std::fmod(phi - tbl.phi_min, two_pi);
    if (rel_phi < 0.0f)
        rel_phi += two_pi;
    const size_t phi_idx = static_cast<size_t>(std::llround(rel_phi / tbl.phi_step)) % tbl.phi_size;

    const float theta_max = tbl.theta_min + tbl.theta_step * static_cast<float>(tbl.theta_size - 1);
    const float clamped_theta = std::clamp(theta, tbl.theta_min, theta_max);
    const size_t theta_idx = static_cast<size_t>(std::llround((clamped_theta - tbl.theta_min) / tbl.theta_step));

    return tbl.gains[theta_idx * tbl.phi_size + phi_idx];
}}""")
    parts.append("")
    parts.append(f"}} // namespace chem::antennas::generated")
    parts.append("")
    return "\n".join(parts)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate C++ antenna headers from measured data.")
    parser.add_argument("--root", type=Path, default=ROOT_DEFAULT, help="Root directory containing antenna pattern folders.")
    parser.add_argument("--out", type=Path, default=HDR_OUT_DEFAULT, help="Output directory for generated headers.")
    parser.add_argument("--src-out", type=Path, default=SRC_OUT_DEFAULT, help="Output directory for generated source files.")
    parser.add_argument("--pattern", action="append", help="Only process specific pattern directory names (can be repeated).")
    args = parser.parse_args()

    root: Path = args.root
    hdr_out: Path = args.out
    src_out: Path = args.src_out
    wanted = set(args.pattern) if args.pattern else None

    pattern_dirs = [p for p in root.iterdir() if p.is_dir() and (not wanted or p.name in wanted)]
    if not pattern_dirs:
        raise SystemExit("No antenna pattern directories found.")

    generated: List[Path] = []
    for pattern_dir in pattern_dirs:
        freq_tables: List[Tuple[float, List[Tuple[float, float, float]]]] = []
        for file_path in find_pattern_files(pattern_dir):
            try:
                freq_mhz, entries = parse_pattern_file(file_path)
            except ValueError as exc:
                print(f"[skip] {file_path}: {exc}")
                continue
            freq_tables.append((freq_mhz, entries))

        if not freq_tables:
            print(f"[skip] {pattern_dir}: no valid frequency tables")
            continue

        stem = sanitize_name(pattern_dir.name)
        freq_tables_sorted = sorted(freq_tables, key=lambda x: x[0])

        grid_tables: List[Tuple[float, Tuple]] = []
        for freq_mhz, entries in freq_tables_sorted:
            grid = build_grid(entries)
            if grid is None:
                print(f"[warn] {pattern_dir.name} F{int(round(freq_mhz))}: no uniform grid, skipping frequency", file=sys.stderr)
                continue
            grid_tables.append((freq_mhz, grid))

        if not grid_tables:
            print(f"[skip] {pattern_dir}: no frequencies with valid grids")
            continue

        hdr_out.mkdir(parents=True, exist_ok=True)
        src_out.mkdir(parents=True, exist_ok=True)

        hdr_path = hdr_out / f"{stem}.h"
        src_path = src_out / f"{stem}.cpp"

        hdr_include = f"chem/antennas/generated/{stem}.h"
        hdr_content = generate_header(stem, grid_tables)
        src_content = generate_source(stem, grid_tables, hdr_include)

        hdr_path.write_text(hdr_content)
        src_path.write_text(src_content)

        generated.append(hdr_path)
        generated.append(src_path)
        print(f"[ok] {pattern_dir.name} -> {hdr_path}, {src_path} ({len(grid_tables)} freqs)")

    if not generated:
        raise SystemExit("No files were generated.")


if __name__ == "__main__":
    main()
