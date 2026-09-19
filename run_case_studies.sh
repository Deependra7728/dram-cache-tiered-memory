#!/bin/bash
# Reproduces (as closely as trace-based ChampSim allows) the paper's
# Case Study 1/2/3 configurations, using the DCM experiment knobs added
# to src/main.cc (--dcm_policy, --dcm_bypass, --dcm_link_latency_ns) --
# see docs/case_study_reproduction_plan.md for the full paper-to-config
# mapping and exactly what each configuration does and does not
# reproduce.
#
# No source code is edited between configurations -- every difference
# between the 7 named configurations below is a command-line flag.
#
# Usage:
#   ./run_case_studies.sh [--dry-run] [--configs C1,C2,...] [--traces T1,T2,...] \
#                          [--warmup N] [--sim N] [--binary NAME]
#
# Defaults: all 7 configurations, all available (non-corrupt) traces, and the
# PRODUCTION methodology of 1,000,000,000 warmup / 500,000,000 ROI
# instructions. Lower both with --warmup/--sim for a sanity run.
#
# Warmup methodology (fixed, deterministic):
#   Every configuration -- BASELINE, BEAR_WR_OPT, ORACLE, NO_DRAM_CACHE and
#   the three link-latency variants -- runs exactly 1B warmup instructions
#   followed by exactly 500M ROI instructions. Warmup is a fixed
#   instruction count. It is never extended, never terminated on cache
#   state, and never transferred between configurations. All seven
#   therefore measure the same instruction window by construction.
#
#   The paper does NOT use 1B instructions. It warms for 100 ms of simulated
#   wall-clock time in full-system gem5 and then takes a checkpoint
#   (Section III-C, Fig. 3). ChampSim is trace-based with no full-system
#   clock, so instruction-count warmup is the available adaptation. 1B was
#   chosen because the independent audit measured 100M as insufficient on
#   several traces (cold misses reached 32-90% of ROI misses); 1B is an
#   order of magnitude longer while remaining fixed and reproducible. It is
#   an experimental methodology choice, not a conversion of 100 ms.
#
#   The resulting cold-miss ratio is MEASURED and reported per run, not
#   forced to any target.
#
# ALWAYS run with --dry-run first to see the exact commands before
# actually launching anything.

set -u

TRACE_DIR="$PWD/dpc3_traces"
BINARY="champsim"
# PRODUCTION METHODOLOGY (fixed instruction counts, deterministic).
#   Warmup = 1,000,000,000 instructions
#   ROI    =   500,000,000 instructions
# See the "Warmup methodology" note in the header comment above for why 1B
# was chosen. Every configuration in the matrix uses these same two numbers,
# including NO_DRAM_CACHE.
WARMUP=1000000000
SIM=500000000
DRY_RUN=0
CONFIGS=""
TRACES=""

# ----------------------------------------------------------------------------
# The 7 required configurations. Each is a NAME plus the exact extra
# CLI flags (beyond warmup/sim/trace, which are common to all) that
# select it -- nothing here is hardcoded into any binary or source file.
# ----------------------------------------------------------------------------
ALL_CONFIG_NAMES="NO_DRAM_CACHE BASELINE BEAR_WR_OPT ORACLE BASELINE_100NS BASELINE_500NS BASELINE_1000NS"

config_flags() {
    case "$1" in
        NO_DRAM_CACHE)     echo "--dcm_bypass" ;;
        BASELINE)          echo "" ;;
        BEAR_WR_OPT)       echo "--dcm_policy=bear" ;;
        ORACLE)            echo "--dcm_policy=oracle" ;;
        BASELINE_100NS)    echo "--dcm_link_latency_ns=100" ;;
        BASELINE_500NS)    echo "--dcm_link_latency_ns=500" ;;
        BASELINE_1000NS)   echo "--dcm_link_latency_ns=1000" ;;
        *) echo "[ERROR] Unknown configuration: $1" >&2; exit 1 ;;
    esac
}

# Case Study these each belong to, purely for documentation/output
# labeling -- does not affect what is actually run.
config_case_study() {
    case "$1" in
        NO_DRAM_CACHE|BASELINE)                     echo "1" ;;
        BEAR_WR_OPT|ORACLE)                          echo "2 (BASELINE from Case Study 1 is its 3rd arm)" ;;
        BASELINE_100NS|BASELINE_500NS|BASELINE_1000NS) echo "3" ;;
        *) echo "?" ;;
    esac
}

usage() {
    echo "Usage: $0 [--dry-run] [--configs C1,C2,...] [--traces T1,T2,...] [--warmup N] [--sim N] [--binary NAME]"
    echo ""
    echo "Available configurations: $ALL_CONFIG_NAMES"
    echo "Available traces (non-corrupt, see docs/case_study_reproduction_plan.md):"
    for f in "$TRACE_DIR"/*.champsimtrace.xz; do
        [ -e "$f" ] && echo "  $(basename "$f")"
    done
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --configs) CONFIGS="$2"; shift 2 ;;
        --traces) TRACES="$2"; shift 2 ;;
        --warmup) WARMUP="$2"; shift 2 ;;
        --sim) SIM="$2"; shift 2 ;;
        --binary) BINARY="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "[ERROR] Unrecognized argument: $1"; usage ;;
    esac
done

if [ -z "$CONFIGS" ]; then
    CONFIGS="$ALL_CONFIG_NAMES"
else
    CONFIGS="$(echo "$CONFIGS" | tr ',' ' ')"
fi

if [ -z "$TRACES" ]; then
    # Default: every .champsimtrace.xz file in TRACE_DIR that is not
    # known-corrupt (docs/case_study_reproduction_plan.md flags
    # 619.lbm_s-3766B.champsimtrace.xz as failing xz integrity check --
    # excluded here by default, still usable via explicit --traces).
    TRACES=""
    for f in "$TRACE_DIR"/*.champsimtrace.xz; do
        [ -e "$f" ] || continue
        base="$(basename "$f")"
        if [ "$base" = "619.lbm_s-3766B.champsimtrace.xz" ]; then
            continue
        fi
        TRACES="$TRACES $base"
    done
else
    TRACES="$(echo "$TRACES" | tr ',' ' ')"
fi

if [ ! -x "bin/$BINARY" ]; then
    echo "[ERROR] Cannot find an executable ChampSim binary: bin/$BINARY (build it first: make)"
    exit 1
fi

if [ ! -d "$TRACE_DIR" ]; then
    echo "[ERROR] Cannot find trace directory: $TRACE_DIR"
    exit 1
fi

RESULTS_DIR="case_study_results"
mkdir -p "$RESULTS_DIR"

echo "=============================================================="
echo "Case Study experiment matrix"
echo "  Binary:        bin/$BINARY"
echo "  Warmup instr:  $WARMUP (fixed)"
echo "  Sim instr:     $SIM"
echo "  Configs:       $CONFIGS"
echo "  Traces:        $TRACES"
echo "  Results dir:   $RESULTS_DIR/"
echo "  Mode:          $([ "$DRY_RUN" -eq 1 ] && echo DRY-RUN -- nothing will be executed || echo LIVE)"
echo "=============================================================="
echo ""

# ----------------------------------------------------------------------------
# Result validation. A run is only accepted if EVERY check below passes, so a
# crashed, killed, truncated or short run can never be silently mistaken for a
# good one (docs/final_independent_audit.md, MEDIUM-2). Echoes one "[FAIL] ..."
# line per problem and returns non-zero if the run is unusable.
# ----------------------------------------------------------------------------
validate_run() {
    local out_file="$1" exit_code="$2" expect_sim="$3" bad=0

    # 1. process exit status
    if [ "$exit_code" -ne 0 ]; then
        echo "    [FAIL] simulator exited non-zero (exit=$exit_code)"
        bad=1
    fi

    # 2. output present and non-empty
    if [ ! -s "$out_file" ]; then
        echo "    [FAIL] output file missing or empty: $out_file"
        return 1   # nothing further can be checked
    fi

    # 3. run reached its natural end (not truncated / killed mid-way)
    if ! grep -q "ChampSim completed all CPUs" "$out_file"; then
        echo "    [FAIL] output truncated: 'ChampSim completed all CPUs' marker absent"
        bad=1
    fi

    # 4. required statistics blocks present
    grep -q "Region of Interest Statistics" "$out_file" || {
        echo "    [FAIL] missing 'Region of Interest Statistics' block"; bad=1; }
    grep -q "DRAM Cache Manager Statistics" "$out_file" || {
        echo "    [FAIL] missing 'DRAM Cache Manager Statistics' block"; bad=1; }

    # The required field set differs by mode: bypass runs deliberately
    # print only the three bypass counters, because the DRAM cache is not
    # in the path at all and the normal-mode fields would all read zero
    # (see print_dcm_stats() in src/main.cc and docs/bypass_mode.md).
    # Checking the wrong set would flag every valid NO_DRAM_CACHE run.
    local required
    if grep -q "DRAM Cache Manager Statistics (.*BYPASSED" "$out_file"; then
        required="DCM BYPASS_READS|BYPASS_WRITES|BYPASS_COMPLETED_READS"
    else
        required="DCM TOTAL_REQUESTS|DCM LOCAL_READS|DCM RESIDUAL|DCM COMPLETED_READS_TO_LLC|BW_UTILIZATION"
    fi
    local old_ifs="$IFS"; IFS='|'
    for field in $required; do
        grep -q "$field" "$out_file" || {
            echo "    [FAIL] required statistic missing: $field"; bad=1; }
    done
    IFS="$old_ifs"

    # 5. the requested instruction count was actually retired
    local finished retired
    finished="$(grep -m1 'Finished CPU 0 instructions:' "$out_file" || true)"
    if [ -z "$finished" ]; then
        echo "    [FAIL] no 'Finished CPU 0' line -- simulation never completed its ROI"
        bad=1
    else
        retired="$(echo "$finished" | sed -n 's/.*instructions: \([0-9]*\).*/\1/p')"
        if [ -z "$retired" ] || [ "$retired" -lt "$expect_sim" ]; then
            echo "    [FAIL] ROI short: retired ${retired:-?} < requested $expect_sim instructions"
            bad=1
        fi
    fi

    # 6. obvious simulation failure signatures
    if grep -qiE "assertion|segmentation fault|std::bad_alloc|terminate called|core dumped" "$out_file"; then
        echo "    [FAIL] simulator reported a fatal error (assertion/segfault/abort)"
        bad=1
    fi

    # 7. DCM-level run-validity: a jammed manager invalidates the run.
    #    (A small residual with the [OK: ...] tag is expected -- ChampSim
    #    stops at the instruction count without draining.)
    if grep -q "WARNING: manager appears jammed" "$out_file"; then
        echo "    [FAIL] DRAM cache manager reported itself jammed -- results unusable"
        bad=1
    fi

    return $bad
}

total=0
ok_runs=0
failed_runs=0
skipped_runs=0
FAILED_LIST=""
for cfg in $CONFIGS; do
    flags="$(config_flags "$cfg")"
    cs="$(config_case_study "$cfg")"
    for trace in $TRACES; do
        trace_path="$TRACE_DIR/$trace"
        out_file="$RESULTS_DIR/${cfg}__${trace}.txt"
        cmd="./bin/$BINARY -warmup_instructions $WARMUP -simulation_instructions $SIM $flags -traces $trace_path"
        total=$((total + 1))
        echo "[$total] Case Study $cs | $cfg | $trace"
        echo "    $cmd"
        echo "    -> $out_file"
        if [ "$DRY_RUN" -eq 0 ]; then
            if [ ! -f "$trace_path" ]; then
                echo "    [SKIP] trace file not found: $trace_path"
                skipped_runs=$((skipped_runs + 1))
                FAILED_LIST="$FAILED_LIST\n    SKIPPED (no trace): $cfg / $trace"
                continue
            fi
            # Capture the exit status of the simulator itself, before any
            # other command can overwrite $?.
            set +e
            eval "$cmd" > "$out_file" 2>&1
            run_exit=$?
            set -e 2>/dev/null || true
            if validate_run "$out_file" "$run_exit" "$SIM"; then
                echo "    [OK] exit=$run_exit, $(grep -c . "$out_file") lines, all validity checks passed"
                ok_runs=$((ok_runs + 1))
            else
                echo "    [INVALID] this run FAILED validation and must not be used"
                failed_runs=$((failed_runs + 1))
                FAILED_LIST="$FAILED_LIST\n    INVALID: $cfg / $trace  ->  $out_file"
            fi
        fi
    done
done

echo ""
echo "=============================================================="
echo "$total run(s) $([ "$DRY_RUN" -eq 1 ] && echo "would be" || echo "were") executed."
if [ "$DRY_RUN" -eq 1 ]; then
    echo "This was a DRY RUN -- nothing was actually executed. Re-run without"
    echo "--dry-run to launch for real. The defaults shown above are the"
    echo "PRODUCTION methodology (1,000,000,000 warmup / 500,000,000 ROI"
    echo "instructions, fixed, identical for all 7 configurations). Lower"
    echo "both with --warmup/--sim for a quick sanity run."
    echo "=============================================================="
    exit 0
fi

echo "  valid:   $ok_runs"
echo "  INVALID: $failed_runs"
echo "  skipped: $skipped_runs"
if [ "$failed_runs" -ne 0 ] || [ "$skipped_runs" -ne 0 ]; then
    # shellcheck disable=SC2059
    printf "$FAILED_LIST\n"
    echo ""
    echo "*** DO NOT USE THIS RESULT SET: $((failed_runs + skipped_runs)) of $total run(s)"
    echo "*** did not produce a valid, complete result file. Re-run the listed"
    echo "*** configurations before drawing any conclusion from the matrix."
    echo "=============================================================="
    exit 1
fi
echo ""
echo "All $ok_runs run(s) completed and passed every validity check."
echo "=============================================================="
exit 0
