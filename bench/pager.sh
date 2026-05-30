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

Output is key=value rows. `less` rows are emitted only when less is installed.
Fixtures are deterministic, non-sparse, non-zero text; /dev/zero is never used.
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

gen_text_fixture() {
    out=$1
    mb=$2
    bytes=$((mb * 1024 * 1024))
    awk -v target="$bytes" '
        BEGIN {
            srand(1);
            chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789     {}[],:.-_";
            n = length(chars);
            written = 0;
            line = 1;
            while (written < target) {
                s = sprintf("%08d ", line);
                for (i = 0; i < 112; i++)
                    s = s substr(chars, int(rand() * n) + 1, 1);
                print s;
                written += length(s) + 1;
                line++;
            }
            print "PAIGE_NEEDLE_LATE";
        }' > "$out"
}

gen_huge_fixture() {
    out=$1
    mb=$2
    bytes=$((mb * 1024 * 1024))
    awk -v target="$bytes" '
        BEGIN {
            srand(2);
            chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
            n = length(chars);
            for (i = 0; i < target; i++)
                printf "%s", substr(chars, int(rand() * n) + 1, 1);
            printf "PAIGE_NEEDLE_LATE\n";
        }' > "$out"
}

if [ ! -s "$FIXTURE" ]; then
    gen_text_fixture "$FIXTURE" "$SIZE_MB"
fi
if [ ! -s "$HUGE_FIXTURE" ]; then
    gen_huge_fixture "$HUGE_FIXTURE" "$SIZE_MB"
fi

printf 'context tool=paige-bench smoke=%s runs=%s size_mb=%s fixture=%s huge_fixture=%s\n' \
    "$SMOKE" "$RUNS" "$SIZE_MB" "$FIXTURE" "$HUGE_FIXTURE"
printf 'context uname=%s\n' "$(uname -a)"
printf 'context rss_note=%s\n' "rss_max is getrusage(2) ru_maxrss; units vary by OS"

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

if [ "$KEEP" -eq 0 ] && [ "$SMOKE" -eq 1 ]; then
    rm -f "$FIXTURE" "$HUGE_FIXTURE"
fi
