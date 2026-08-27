#!/bin/bash
# Two builds, alternately, N rounds each, then their percentiles side by side.
#
#   ./scripts/ab.sh ./build/old ./build/new        # three rounds each
#   ./scripts/ab.sh ./build/old ./build/new 2      # two rounds each
#
# Everything scripts/run.sh reads from the environment is passed through, so a
# round of this is configured the same way a single round is:
#
#   DATA=data/itch.txt ITCH_WINDOW_FROM=9:30 ITCH_WINDOW_TO=10:10 \
#       ./scripts/ab.sh ./build/old ./build/new
#
# Why the two builds have to run in turn, rather than one now and the other
# tomorrow: what this machine does between rounds is not fixed. Temperatures
# move, the page cache fills, something wakes up. A latency.csv saved last week
# is a record of that day, not a control for this one. Alternating puts the
# day's state on both builds equally, so what is left between them is the code.
#
# And why the spread at the bottom of the table matters more than the table:
# the same binary on the same input does not repeat exactly. On this machine
# two rounds of one build differ by 0.6% at p50, 2.4% at p90, 6.5% at p99 and
# 14.4% at p99.9. A build that is 3% better at p99 is inside that, and is not
# yet a result.
#
# Three rounds each rather than two: two rounds can both come out clean and
# hide the third kind of round, the one that is slow the whole way through.

set -u

# The build to treat as the baseline. Its rounds are printed first.
OLD=${1:?usage: ab.sh <old build> <new build> [rounds]}
# The build being judged.
NEW=${2:?usage: ab.sh <old build> <new build> [rounds]}
# How many rounds each build gets.
N=${3:-3}

# Both have to exist before anything starts, because finding out an hour in
# wastes every round already run.
for b in "$OLD" "$NEW"; do
    [[ -x $b ]] || { echo "$b is not there; build it first" >&2; exit 1; }
done
# The same file twice is a legitimate thing to do - it measures the spread of
# one build - but it is more often a mistake, so say so and carry on.
if cmp -s "$OLD" "$NEW"; then
    echo "note: both paths are the same binary, so this measures the spread"
fi

# Where this comparison's samples go. Every round gets its own directory
# underneath, and an old comparison is not overwritten.
DIR=${DIR:-results/ab}
mkdir -p "$DIR"

# The arguments handed to percentiles.py at the end, as "name=path" pairs. The
# baseline rounds are appended first so that they are printed first.
old_args=()
new_args=()
# Rounds that failed acceptance. They are left out of the table.
failed=()

# One pass of this loop is one round of each build.
for ((i = 1; i <= N; i++)); do
    # Both builds in the same iteration, baseline first.
    for side in old new; do
        # Pick the binary for this side.
        [[ $side == old ]] && bin=$OLD || bin=$NEW
        # This round's own output directory and log.
        out=$DIR/${side}_$i
        log=$DIR/${side}_$i.log
        # Say which round is starting, with the time, because a comparison
        # runs for hours and this is how far along it is.
        echo "== $(date +%H:%M:%S)  $side round $i of $N  ($bin)"
        # run.sh does the round. Its exit code is the verdict: zero means
        # every acceptance check passed and these numbers can be compared.
        if BIN=$bin OUT=$out LOG=$log ./scripts/run.sh > /dev/null; then
            # Keep it for the table.
            if [[ $side == old ]]; then
                old_args+=("old $i=$out/latency.csv")
            else
                new_args+=("new $i=$out/latency.csv")
            fi
        else
            # A round that failed a check cannot be compared with one that
            # passed, so it is named here and left out rather than quietly
            # averaged in.
            echo "   round failed acceptance; see $log"
            failed+=("$side $i")
        fi
    done
done

# Nothing survived, so there is no table to print.
if [[ ${#old_args[@]} -eq 0 && ${#new_args[@]} -eq 0 ]]; then
    echo "every round failed; nothing to compare" >&2
    exit 1
fi

# The table. Baseline rounds first, then the new ones.
echo
./scripts/percentiles.py "${old_args[@]}" "${new_args[@]}"

# And what was thrown away, so that a table built from fewer rounds than asked
# for does not look like a full one.
if [[ ${#failed[@]} -gt 0 ]]; then
    echo
    echo "left out of the table: ${failed[*]}"
fi
