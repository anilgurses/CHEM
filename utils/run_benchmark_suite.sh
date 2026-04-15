#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# CHEM Benchmark Suite
# Runs achem_benchmark across sample-rate / signal-duration combinations
# with detailed timing enabled.
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Try common build output locations
BENCHMARK=""
for candidate in \
    "$PROJECT_ROOT/build/bin/RELEASE/achem_benchmark" \
    "$PROJECT_ROOT/build/bin/DEBUG/achem_benchmark" \
    "$PROJECT_ROOT/build/bin/achem_benchmark" \
    "$PROJECT_ROOT/build/achem_benchmark"; do
    if [[ -x "$candidate" ]]; then
        BENCHMARK="$candidate"
        break
    fi
done

if [[ -z "$BENCHMARK" ]]; then
    echo "ERROR: achem_benchmark binary not found."
    echo "Build with: cmake -B build -DENABLE_BENCHMARK=ON && cmake --build build"
    exit 1
fi

get_load_1min() {
    awk '{print $1}' /proc/loadavg
}

wait_for_low_load() {
    if [[ -z "$MAX_LOAD" ]]; then
        return 0
    fi

    local waited=0
    local load
    load=$(get_load_1min)

    while (( $(echo "$load > $MAX_LOAD" | bc -l) )); do
        if [[ $waited -ge $LOAD_WAIT_TIMEOUT ]]; then
            echo "ERROR: System load ($load) exceeded $MAX_LOAD for ${LOAD_WAIT_TIMEOUT}s. Aborting."
            return 1
        fi
        echo "  System load ($load) > $MAX_LOAD, waiting... (${waited}s/${LOAD_WAIT_TIMEOUT}s)"
        sleep 10
        waited=$((waited + 10))
        load=$(get_load_1min)
    done
    return 0
}

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
CONFIGS=(
    # sample_rate   label               duration_us
    "11520000       11.52MHz-1000us     1000"
    "23040000       23.04MHz-1000us     1000"
    "23040000       23.04MHz-500us      500"
    "46080000       46.08MHz-500us      500"
)
CIR_TAPS=(1 10 20)
MAX_NODES=10
DURATION=10
RUNS=3

# CPU affinity: limit which CPUs the benchmark can use
# Examples: "0-3" (CPUs 0-3), "0,2,4,6" (specific CPUs), "" (no limit)
CPU_SET=""

# NUMA node binding: run on a specific NUMA node (CPU + memory)
# Examples: "0" (NUMA node 0), "1" (NUMA node 1), "" (no binding)
NUMA_NODE=""

# Maximum system CPU load (1-min avg) before running a benchmark
MAX_LOAD=""

# Maximum wait time (seconds) for load to drop before aborting
LOAD_WAIT_TIMEOUT=300

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
RESULTS_DIR="$PROJECT_ROOT/benchmark_results_${TIMESTAMP}"
mkdir -p "$RESULTS_DIR"

SUMMARY_FILE="$RESULTS_DIR/summary.txt"

echo "============================================" | tee "$SUMMARY_FILE"
echo "  CHEM Benchmark Suite" | tee -a "$SUMMARY_FILE"
echo "  $(date)" | tee -a "$SUMMARY_FILE"
echo "============================================" | tee -a "$SUMMARY_FILE"
echo "Binary:              $BENCHMARK" | tee -a "$SUMMARY_FILE"
echo "Max nodes:           $MAX_NODES" | tee -a "$SUMMARY_FILE"
echo "Duration:            ${DURATION}s" | tee -a "$SUMMARY_FILE"
echo "Runs per scenario:   $RUNS" | tee -a "$SUMMARY_FILE"
echo "Configurations:      ${#CONFIGS[@]}" | tee -a "$SUMMARY_FILE"
for cfg_line in "${CONFIGS[@]}"; do
    read -r _sr lbl _dur <<< "$cfg_line"
    echo "  $lbl: sr=$_sr, dur=${_dur}us" | tee -a "$SUMMARY_FILE"
done
echo "CIR taps:            ${CIR_TAPS[*]}" | tee -a "$SUMMARY_FILE"
echo "CPU affinity:        ${CPU_SET:-all}" | tee -a "$SUMMARY_FILE"
echo "NUMA node:           ${NUMA_NODE:-any}" | tee -a "$SUMMARY_FILE"
echo "Max system load:     ${MAX_LOAD:-disabled}" | tee -a "$SUMMARY_FILE"
echo "Results directory:   $RESULTS_DIR" | tee -a "$SUMMARY_FILE"
echo "============================================" | tee -a "$SUMMARY_FILE"
echo "" | tee -a "$SUMMARY_FILE"

TOTAL=$(( ${#CONFIGS[@]} * ${#CIR_TAPS[@]} ))
RUN=0
PASSED=0
FAILED=0

for cfg_line in "${CONFIGS[@]}"; do
    read -r SR SR_LABEL SIGNAL_DURATION_US <<< "$cfg_line"

    for CIR in "${CIR_TAPS[@]}"; do
        RUN=$(( RUN + 1 ))
        TAG="sr${SR_LABEL}_cir${CIR}"
        CSV_FILE="$RESULTS_DIR/${TAG}.csv"
        LOG_FILE="$RESULTS_DIR/${TAG}.log"

        echo "--------------------------------------------" | tee -a "$SUMMARY_FILE"
        echo "[$RUN/$TOTAL] ${SR_LABEL}  dur=${SIGNAL_DURATION_US}us  CIR=${CIR}" | tee -a "$SUMMARY_FILE"
        echo "--------------------------------------------" | tee -a "$SUMMARY_FILE"

        # Check system load before running
        if ! wait_for_low_load; then
            echo "  ABORTED due to high system load" | tee -a "$SUMMARY_FILE"
            FAILED=$(( FAILED + 1 ))
            continue
        fi

        NUMA_ARGS=()
        if [[ -n "$NUMA_NODE" ]]; then
            NUMA_ARGS=(-N "$NUMA_NODE")
        fi

        CMD_PREFIX=""
        if [[ -n "$CPU_SET" ]]; then
            CMD_PREFIX="taskset -c $CPU_SET"
        fi

        set +e
        $CMD_PREFIX "$BENCHMARK" \
            -d \
            -m "$MAX_NODES" \
            -s "$DURATION" \
            -R "$RUNS" \
            -r "$SR" \
            -D "$SIGNAL_DURATION_US" \
            -C "$CIR" \
            -o "$CSV_FILE" \
            "${NUMA_ARGS[@]}" \
            -v \
            2>&1 | tee "$LOG_FILE"
        EXIT_CODE=${PIPESTATUS[0]}
        set -e

        if [[ $EXIT_CODE -eq 0 ]]; then
            STATUS="PASSED"
            PASSED=$(( PASSED + 1 ))
        else
            STATUS="FAILED"
            FAILED=$(( FAILED + 1 ))
        fi

        echo "  Result: $STATUS (exit $EXIT_CODE)" | tee -a "$SUMMARY_FILE"
        echo "  CSV:    $CSV_FILE" | tee -a "$SUMMARY_FILE"
        echo "" | tee -a "$SUMMARY_FILE"

        # Brief cooldown between runs
        sleep 2
    done
done

echo "============================================" | tee -a "$SUMMARY_FILE"
echo "  Suite complete: $PASSED passed, $FAILED failed out of $TOTAL" | tee -a "$SUMMARY_FILE"
echo "  Results: $RESULTS_DIR" | tee -a "$SUMMARY_FILE"
echo "============================================" | tee -a "$SUMMARY_FILE"
