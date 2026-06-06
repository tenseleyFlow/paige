#!/bin/sh
# Enforcing performance gate.
#
# paige's thesis is laziness: it renders a screenful regardless of file size.
# render= and segments= are the engine's own counters and, at a fixed 24x80 tty,
# they are EXACT and constant across file size -- so a blow-up is an algorithmic
# regression (e.g. wrapped-line rendering going O(line) again), not measurement
# noise. That makes them a far more robust gate than wall-clock ever could be.
#
# We assert only the deterministic, size-independent cases as upper bounds.
# search_* counters scale with corpus/codebase and are deliberately not gated;
# every paige row is still checked for status=ok as a basic liveness guard.
#
# Usage:
#   bench/check.sh          run the gate (exit 1 on any breach)
#   bench/check.sh --show   print the measured counters and exit 0 (re-record aid)
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASELINE="$ROOT/bench/baseline.json"
SHOW=0
[ "${1:-}" = "--show" ] && SHOW=1

# Gates, newline-separated: "<row selector>|<baseline key>". The selector is a
# fixed substring of a bench row; its trailing space stops name=paige from also
# matching name=paige-huge-*.
GATES="mode=first name=paige |first.paige
mode=jump name=paige |jump.paige
mode=first name=paige-huge-wrap |first.paige-huge-wrap
mode=first name=paige-huge-chop |first.paige-huge-chop"

# field <row> <key>   integer value of ' key=NNN' in a bench row
field()   { printf '%s\n' "$1" | sed -n "s/.*[ ]$2=\\([0-9][0-9]*\\).*/\\1/p"; }
# jmax <baseline key> <field>   the *_max bound from baseline.json
jmax()    { grep "\"$1\"" "$BASELINE" | sed -n "s/.*\"$2\": *\\([0-9][0-9]*\\).*/\\1/p"; }

echo "perf-gate: running smoke bench (1 MiB corpus, fixed 24x80 tty)"
OUT=$(sh "$ROOT/bench/pager.sh" --smoke)

breaches=0

# Liveness: every paige row must report status=ok.
for st in $(printf '%s\n' "$OUT" | grep ' name=paige' | sed -n 's/.* status=\([a-z]*\).*/\1/p'); do
    [ "$st" = "ok" ] || { echo "  FAIL: a paige bench row reported status=$st"; breaches=$((breaches + 1)); }
done

# Counter bounds on the deterministic cases. Split GATES on newlines only so the
# space-bearing selectors survive; restore IFS inside the loop body.
OIFS=$IFS
IFS='
'
for line in $GATES; do
    IFS=$OIFS
    sel=${line%|*}
    key=${line##*|}
    row=$(printf '%s\n' "$OUT" | grep -F "$sel" | head -n1 || true)
    if [ -z "$row" ]; then
        echo "  FAIL: $key -- no matching bench row (selector: '$sel')"
        breaches=$((breaches + 1))
        IFS='
'
        continue
    fi
    r=$(field "$row" render)
    s=$(field "$row" segments)
    if [ "$SHOW" -eq 1 ]; then
        printf '  %-24s render=%s segments=%s\n' "$key" "$r" "$s"
        IFS='
'
        continue
    fi
    rmax=$(jmax "$key" render_max)
    smax=$(jmax "$key" segments_max)
    ok=1
    [ "$r" -le "$rmax" ] || { ok=0; echo "  FAIL: $key render=$r > max $rmax (O(screen) regression?)"; }
    [ "$s" -le "$smax" ] || { ok=0; echo "  FAIL: $key segments=$s > max $smax"; }
    if [ "$ok" -eq 1 ]; then
        printf '  PASS: %-24s render=%s/%s segments=%s/%s\n' "$key" "$r" "$rmax" "$s" "$smax"
    else
        breaches=$((breaches + 1))
    fi
    IFS='
'
done
IFS=$OIFS

[ "$SHOW" -eq 1 ] && exit 0

if [ "$breaches" -ne 0 ]; then
    echo "perf-gate: FAILED ($breaches breach(es))"
    exit 1
fi
echo "perf-gate: all counters within bounds"
exit 0
