#!/usr/bin/env python3
"""One line per round. Rounds of the same build are never averaged together.

Averaging them hides the one thing the reader needs most: how far apart two
rounds of the same code can be. That distance is the floor on how big a
difference between two different builds has to be before it means anything,
and out in the tail it is not small.

  ./scripts/percentiles.py "old 1=results/ab_old_1/latency.csv" \\
                           "new 1=results/ab_new_1/latency.csv"
"""

# sys gives the command line arguments and the exit code. Nothing else is needed.
import sys

# Which percentiles to report. The name and the fraction are written separately
# because the name is also the column heading.
# Stopping at p99.9 is deliberate: the next one up needs more samples than a
# single round produces. p99.99 puts one sample in ten thousand behind each
# point, so reporting it from one round would be reporting noise.
QUANTILES = [("p50", .5), ("p90", .9), ("p99", .99), ("p99.9", .999)]


# This is the main logic of the script.
#
# The order is: read every "name=path" argument into a row, print the table,
# then group the rows by name and work out how far apart the rounds of one
# build are. Returns 0 when it worked and 2 when there were no arguments.
def main(argv):
    # Nothing given, so print the usage text at the top of this file.
    if len(argv) < 2:
        # __doc__ is that triple quoted block.
        print(__doc__)
        # 2 means the command line was wrong, as opposed to the work failing.
        return 2
    # One row per round. The order is the order given on the command line and
    # is not sorted: the caller puts the baseline rounds first on purpose.
    rows = []
    # Argument 0 is the name of this script, so the data starts at 1.
    for arg in argv[1:]:
        # Each argument is "name=path". The name is for the reader, the path is
        # a latency.csv. partition splits on the first equals sign only, which
        # split would not do: a path with an equals sign in it would be cut in
        # the wrong place.
        name, _, path = arg.partition("=")
        # Open this round's samples. A bad path raises here rather than being
        # caught, because a round that quietly goes missing is far harder to
        # notice than one that stops the script.
        with open(path) as f:
            # Skip the latency_ns heading.
            next(f)
            # This one line is the whole reason the script exists: the
            # percentiles come from sorting the raw samples. The "tick to
            # trade" line the trader prints at the end comes from a histogram
            # whose buckets are 2,000 ns wide above 20 us, so what it prints up
            # there is a bucket edge and not a measurement.
            #
            # It matters more than it sounds. A printed 26,000 means the real
            # value is somewhere in (24,000, 26,000] - it was 24,579. Two
            # rounds whose p99.9 lands in the same bucket print the same number
            # and look identical when they are 1,900 ns apart.
            #
            # The if skips blank lines, of which there is usually one at the end.
            v = sorted(int(line) for line in f if line.strip())
        # How many samples this round produced.
        n = len(v)
        # Pick the percentiles. min keeps p99.9 inside the list when a round is
        # short of samples.
        q = [v[min(int(p * n), n - 1)] for _, p in QUANTILES]
        # Five things per row: the name, the sample count, the percentiles, how
        # many samples went past a millisecond, and the largest one.
        # Past a millisecond gets a column of its own because a sample that
        # large is no longer latency. A normal order takes a few microseconds,
        # so a millisecond is several hundred times that and means something
        # else happened on the machine - the core was taken away, or a UEFI
        # variable was read. Left inside the percentiles it only muddies them;
        # it needs chasing on its own.
        rows.append((name, n, q, sum(1 for x in v if x > 1_000_000), v[-1]))

    # The heading. The widths are fixed so that the numbers of several rounds
    # line up under each other and can be compared by eye. The first two
    # columns are fixed and the rest are built from the names in QUANTILES.
    head = f"{'run':<22} {'samples':>10}" + "".join(f"{k:>11}" for k, _ in QUANTILES)
    # Print it, with the last two columns added on the end.
    print(head + f"{'>1ms':>8}{'max':>12}")
    # One line per round. Thousands separators, because past six digits the
    # size of a number is hard to see at a glance without them.
    for name, n, q, over, mx in rows:
        # The name goes left, every number goes right, so each column lines up.
        print(f"{name:<22} {n:>10,}" + "".join(f"{x:>11,}" for x in q) +
              f"{over:>8,}{mx:>12,}")

    # What follows is the spread of one build against itself.
    #
    # The table above is not enough to decide anything on its own. "The new
    # code is 8% better at p99" is not a result until 8% is bigger than the
    # distance between two rounds of the same binary on the same input. On this
    # machine that distance is 0.6% at p50, 2.4% at p90, 6.5% at p99 and 14.4%
    # at p99.9, measured over three rounds of one build.
    #
    # The key is the name of the build, the value is the percentiles of each of
    # its rounds.
    groups = {}
    # Walk the table again. Only the name and the percentiles are needed.
    for name, _, q, _, _ in rows:
        # Group by everything before the last space, so that "old 1" and
        # "old 2" land in the same group, "old". rsplit with a count of 1
        # splits from the right once, so a name with a space in it survives.
        groups.setdefault(name.rsplit(" ", 1)[0], []).append(q)
    # A blank line between the table and the spread.
    print()
    # One line per build.
    for build, qs in groups.items():
        # One round gives nothing to compare against. Everything above the
        # median should be left alone in that case - the tail moves by
        # multiples from round to round.
        if len(qs) < 2:
            # Say so rather than printing a number that looks like a result.
            print(f"{build:<22} one run only, nothing above the median is worth reading")
            # On to the next build.
            continue
        # zip(*qs) turns "rounds by percentile" into "percentile by round", so
        # each percentile can be measured on its own. The spread is
        # (largest - smallest) / smallest, as a percentage.
        spread = ["%.1f%%" % (100 * (max(c) - min(c)) / min(c)) for c in zip(*qs)]
        # One line: how many rounds this build ran, and its spread at each
        # percentile.
        print(f"{build:<22} {len(qs)} runs, spread " +
              "  ".join(f"{k} {s}" for (k, _), s in zip(QUANTILES, spread)))
    # Everything printed.
    return 0


# Only run when this file is the program, not when something imports it.
if __name__ == "__main__":
    # main indexes from argv[1], so it wants the whole thing.
    sys.exit(main(sys.argv))
