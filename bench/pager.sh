#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FIXDIR="$ROOT/bench/.fixtures"
BUILDDIR="$ROOT/bench/build"
SIZE_MB=16
RUNS=3
SMOKE=0
KEEP=0
FIXTURE=""
HUGE_FIXTURE=""

usage() {
    cat <<'USAGE'
usage: bench/pager.sh [options]

Options:
  --smoke              run a tiny one-iteration benchmark
  --size-mb N          generated normal fixture size (default: 16)
  --runs N             repetitions per benchmark (default: 3)
  --fixture PATH       use an existing normal fixture
  --huge-fixture PATH  use an existing huge-line fixture
  --keep-fixtures      keep generated fixtures under bench/.fixtures
  -h, --help           show this help

Output is key=value rows. render=/segments=/search_lines= are paige's own stat
counters (-1 for less/ov/moar) and isolate engine work from first_ms, which is
end-to-end and includes the demo host's file slurp+index. Competitor rows
(less/ov/moar) appear only when those tools are installed. Fixtures are this
repo's real sources tiled to size (realistic line lengths and structure), never
random or sparse; /dev/zero is never used.
USAGE
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --smoke)
        SMOKE=1
        SIZE_MB=1
        RUNS=1
        ;;
    --size-mb)
        SIZE_MB=$2
        shift
        ;;
    --runs)
        RUNS=$2
        shift
        ;;
    --fixture)
        FIXTURE=$2
        shift
        ;;
    --huge-fixture)
        HUGE_FIXTURE=$2
        shift
        ;;
    --keep-fixtures)
        KEEP=1
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
    esac
    shift
done

mkdir -p "$FIXDIR" "$BUILDDIR"

if [ -z "$FIXTURE" ]; then
    FIXTURE="$FIXDIR/pager-${SIZE_MB}m.txt"
fi
if [ -z "$HUGE_FIXTURE" ]; then
    HUGE_FIXTURE="$FIXDIR/huge-line-${SIZE_MB}m.txt"
fi

make -C "$ROOT" paige-demo >/dev/null
if ! ${CC:-cc} ${CFLAGS:- -std=c11 -O2} -D_DEFAULT_SOURCE \
    -D_DARWIN_C_SOURCE "$ROOT/bench/pty_driver.c" \
    -o "$BUILDDIR/pty_driver" ${LIBS:-} >/dev/null 2>&1; then
    ${CC:-cc} ${CFLAGS:- -std=c11 -O2} -D_DEFAULT_SOURCE \
        -D_DARWIN_C_SOURCE "$ROOT/bench/pty_driver.c" \
        -o "$BUILDDIR/pty_driver" ${LIBS:-} -lutil
fi

# A realistic corpus: this repo's own C sources and docs, tiled to size. Real
# code has a varied line-length distribution, comments, indentation, and
# identifiers — unlike the fixed-width uniform-random text that flatters a
# width-wrapping pager. Nothing here is sparse or trivially compressible, and
# /dev/zero is never used.
corpus_seed() {
    cat "$ROOT"/src/*.c "$ROOT"/src/*.h "$ROOT"/include/*.h "$ROOT"/README.md \
        "$ROOT"/tests/pty_test.c 2>/dev/null
}

gen_text_fixture() {
    out=$1
    mb=$2
    bytes=$((mb * 1024 * 1024))
    seedbytes=$(corpus_seed | wc -c)
    [ "$seedbytes" -gt 0 ] || seedbytes=1
    reps=$((bytes / seedbytes + 1))
    : >"$out"
    i=0
    while [ "$i" -lt "$reps" ]; do
        corpus_seed >>"$out"
        i=$((i + 1))
    done
    printf 'PAIGE_NEEDLE_LATE\n' >>"$out"
}

# The huge-line stress: the same real bytes with newlines stripped, so it is one
# enormous logical line (think minified JSON or a long log record), not random.
gen_huge_fixture() {
    out=$1
    mb=$2
    bytes=$((mb * 1024 * 1024))
    seedbytes=$(corpus_seed | tr -d '\n' | wc -c)
    [ "$seedbytes" -gt 0 ] || seedbytes=1
    reps=$((bytes / seedbytes + 1))
    : >"$out"
    i=0
    while [ "$i" -lt "$reps" ]; do
        corpus_seed | tr -d '\n' >>"$out"
        i=$((i + 1))
    done
    printf '\n' >>"$out"
}

if [ ! -s "$FIXTURE" ]; then
    gen_text_fixture "$FIXTURE" "$SIZE_MB"
fi
if [ ! -s "$HUGE_FIXTURE" ]; then
    gen_huge_fixture "$HUGE_FIXTURE" "$SIZE_MB"
fi

# Make the demo print its stat counters on quit; the driver parses them. less,
# ov, and moar ignore this env var (they report render/search/segments = -1).
export PAIGE_STATS=1

printf 'context tool=paige-bench smoke=%s runs=%s size_mb=%s fixture=%s huge_fixture=%s\n' \
    "$SMOKE" "$RUNS" "$SIZE_MB" "$FIXTURE" "$HUGE_FIXTURE"
printf 'context uname=%s\n' "$(uname -a)"
printf 'context rss_note=%s\n' "rss_max is getrusage(2) ru_maxrss; units vary by OS"
printf 'context first_ms_note=%s\n' \
    "first_ms is end-to-end TTFB incl. host slurp+index; render=/segments=/search_lines= isolate engine work"

run_driver() {
    mode=$1
    name=$2
    shift 2
    i=1
    while [ "$i" -le "$RUNS" ]; do
        printf 'run=%s ' "$i"
        "$BUILDDIR/pty_driver" "$mode" "$name" "$@"
        i=$((i + 1))
    done
}

run_driver first paige -- "$ROOT/paige-demo" "$FIXTURE"
run_driver jump paige -- "$ROOT/paige-demo" "$FIXTURE"
run_driver search paige-late PAIGE_NEEDLE_LATE -- "$ROOT/paige-demo" "$FIXTURE"
run_driver search paige-nohit PAIGE_NEEDLE_MISSING -- "$ROOT/paige-demo" "$FIXTURE"
run_driver first paige-huge-wrap -- "$ROOT/paige-demo" "$HUGE_FIXTURE"
run_driver first paige-huge-chop -- env PAIGE_CHOP=1 "$ROOT/paige-demo" "$HUGE_FIXTURE"

if command -v less >/dev/null 2>&1; then
    run_driver first less -- less "$FIXTURE"
    run_driver jump less -- less "$FIXTURE"
    run_driver search less-late PAIGE_NEEDLE_LATE -- less "$FIXTURE"
    run_driver first less-huge -- less "$HUGE_FIXTURE"
else
    printf 'bench mode=skip name=less status=missing\n'
fi

if command -v ov >/dev/null 2>&1; then
    run_driver first ov -- ov "$FIXTURE"
    run_driver jump ov -- ov "$FIXTURE"
    run_driver search ov-late PAIGE_NEEDLE_LATE -- ov "$FIXTURE"
    run_driver first ov-huge -- ov "$HUGE_FIXTURE"
else
    printf 'bench mode=skip name=ov status=missing\n'
fi

if command -v moar >/dev/null 2>&1; then
    run_driver first moar -- moar "$FIXTURE"
    run_driver jump moar -- moar "$FIXTURE"
    run_driver first moar-huge -- moar "$HUGE_FIXTURE"
else
    printf 'bench mode=skip name=moar status=missing\n'
fi

# mat is the production host that embeds the paige engine. This is the honest
# "is the engine already at parity with less?" comparison: mat indexes lazily
# (mmap + on-demand newline index) where the paige-demo above slurps eagerly.
# Decorations/highlighting are off so we measure mat's paging+indexing path, not
# its optional render cost -- the same plain workload less does. Point $MAT at a
# build, or have `mat` on PATH.
#
# Only in full runs, never under --smoke: mat probes the terminal on startup and
# can stall briefly under this minimal bench pty (the driver's 15s timeout caps
# it). The perf gate runs --smoke and only reads the demo's own counters, so it
# must never depend on driving a full external app like mat.
MAT=${MAT:-mat}
MAT_PLAIN="--paging=always --decorations=never --color=never"
if [ "$SMOKE" -eq 0 ] && command -v "$MAT" >/dev/null 2>&1; then
    run_driver first mat -- "$MAT" $MAT_PLAIN "$FIXTURE"
    run_driver jump mat -- "$MAT" $MAT_PLAIN "$FIXTURE"
    run_driver search mat-late PAIGE_NEEDLE_LATE -- "$MAT" $MAT_PLAIN "$FIXTURE"
    run_driver first mat-huge -- "$MAT" $MAT_PLAIN "$HUGE_FIXTURE"
else
    printf 'bench mode=skip name=mat status=%s\n' \
        "$([ "$SMOKE" -eq 1 ] && echo smoke-excluded || echo missing)"
fi

if [ "$KEEP" -eq 0 ] && [ "$SMOKE" -eq 1 ]; then
    rm -f "$FIXTURE" "$HUGE_FIXTURE"
fi
