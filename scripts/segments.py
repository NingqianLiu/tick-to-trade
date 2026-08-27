#!/usr/bin/env python3
# Split the latency of one order into four segments and give each its own
# distribution.
#
# An order passes five points on its way through:
#   0 the card has the frame              the card's own clock
#   1 the event queue tells us about it   the CPU's clock (an address only so
#                                         far - the body is still in memory)
#   2 the body is in hand                 the CPU's clock
#   3 the order is built, not yet handed over   the CPU's clock
#   4 the card puts the first byte on the wire  the card's own clock
#
# Each segment answers one question, and the names are the ones the README
# uses for them:
#   wait   (1-0)  the frame arrived, how long until we were told
#   fetch  (2-1)  how long to get the body in hand
#   work   (3-2)  the book, the signal and building the order
#                 It splits again into three, from two more points the trader
#                 records:
#                   parse   pick the ITCH messages out of the frames and sort them
#                   book    apply them to the book
#                   signal and order   check the signal, then write the bytes of
#                           the order and its TCP header
#   send   (4-3)  handed to the card, until the bytes are on the wire
#   total  (4-0)  wire-to-wire, which is the number the trader reports
#
# Points 0 and 4 are on the card's clock and 1, 2 and 3 are on the CPU's, and
# the two crystals do not run at the same rate. So wait and send carry the
# difference between two clocks and the two middle segments do not. Below, a
# sliding window minimum fits that difference out, and the residual of the fit
# is printed: wait and send can only be trusted as far as that residual.
#
#   usage: scripts/segments.py <events.csv> [another events.csv ...]

# sys reads the command line. This script takes one or more paths to an
# events.csv from sys.argv and hands a return code back to the shell with
# sys.exit.
import sys
# bisect looks up a time in a sorted list of times, which is how a fitted
# clock offset is read back out below.
import bisect
# deque is a queue that can be pushed and popped at both ends. The sliding
# window minimum keeps its candidates in one.
from collections import deque


# Read one file. Its first two lines are not shaped like an ordinary csv,
# which is what this function is for.
def read(path):
    # Read one events.csv.
    #
    # The csv module will not do here. The first line of this file is a
    # comment carrying how many ticks this machine's clock takes per
    # nanosecond, the second line is the column names, and the orders start on
    # the third. Handed to the csv module, the comment becomes the column names.
    #
    # Returns two things: that ticks per nanosecond number, and a list of
    # dictionaries, one per order. Dictionaries so that the columns can be
    # taken by name rather than by counting them. A few hundred thousand of
    # them is not cheap, but this runs offline and nothing is waiting on it.
    #
    # tpn is ticks per nanosecond. The three CPU points are recorded in ticks
    # and have to be divided by it to become nanoseconds.
    tpn = None
    # Every order ends up here, one dictionary each.
    rows = []
    # with closes the file when the loop ends.
    with open(path) as f:
        # A line at a time. These files reach hundreds of megabytes, so they
        # are not read into memory whole.
        for line in f:
            # A line starting with a hash is a comment.
            if line.startswith('#'):
                # The one that matters looks like "# ticks_per_ns 2.599998".
                # Any other comment is skipped.
                if 'ticks_per_ns' in line:
                    # The number is the last field.
                    tpn = float(line.split()[-1])
                # Comments carry no order, so on to the next line.
                continue
            # The column names are the line starting with window.
            # It has to come before any data line, or cols below is used
            # before it exists. The trader writes it in that order.
            if line.startswith('window,'):
                # Drop the newline and split into names for the zip below.
                cols = line.strip().split(',')
                # No order on this line either.
                continue
            # Anything else is an order.
            p = line.strip().split(',')
            # A half written line is possible: a round stopped in the middle
            # leaves the last line short. Dropping it here beats letting zip
            # quietly leave fields out.
            if len(p) < len(cols):
                # Drop it and read on.
                continue
            # Pair the names with the values and keep it.
            rows.append(dict(zip(cols, p)))
    # Hand both back. tpn is None if the file had no such comment, in which
    # case the division below raises - which is better than dividing by
    # something made up and reporting the answer.
    return tpn, rows


# Percentiles. Nothing is interpolated: every number reported is a sample that
# really happened.
def quant(v):
    # Sort a list of nanosecond values and pick a few points out of it. The
    # count comes back too, because the tail percentiles jump around when
    # there are few samples.
    #
    # Returns a dictionary of the count, p50, p90, p99, p99.9 and the largest,
    # or None for an empty list, which the printing decides what to do with.
    if not v:
        # No samples is not an error. A segment can be empty for a whole round
        # - with no order ever sent, point 3 stays at zero throughout.
        return None
    # Sorting is the whole of it; a percentile is then a position.
    v = sorted(v)
    # The count is needed for the positions and for the printing.
    n = len(v)
    # Given a fraction, return the value at that position. The min keeps the
    # index inside the list: n * 1.0 is exactly n, which is one past the end.
    pick = lambda q: v[min(n - 1, int(n * q))]
    # The largest is the last one, no position needed.
    return {'n': n, 'p50': pick(.5), 'p90': pick(.9), 'p99': pick(.99),
            'p99.9': pick(.999), 'max': v[-1]}


# Print one segment as one line. Formatting only, nothing is worked out here.
def line(name, d):
    # name is the segment, d is what quant returned. Nanoseconds throughout,
    # and smaller is better.
    if d is None:
        # Say there were none rather than printing a row of zeroes, which
        # reads like a measurement.
        print(f"    {name:<18} no samples")
        # Nothing else to print for this segment.
        return
    # Five numbers on one line, fixed widths so the segments line up under
    # each other. In the format specifiers >9 is nine columns right aligned,
    # the comma is a thousands separator and .0f means no decimals.
    print(f"    {name:<18} p50 {d['p50']:>8,.0f}  p90 {d['p90']:>8,.0f}  "
          f"p99 {d['p99']:>8,.0f}  p99.9 {d['p99.9']:>8,.0f}  "
          f"max {d['max']:>9,.0f}   ({d['n']:,} samples)")


# Line up the two clocks. This is the hardest part of the file, and why it can
# only be done this way is below.
def base_line(rx_ns, cpu_ns, win_ns=50_000_000):
    # The card and the CPU each have their own crystal. On this machine they
    # differ by several thousand parts per million, which is a millisecond and
    # a half of drift over four hundred seconds - three orders of magnitude
    # more than the microseconds being measured. So "CPU time minus card time"
    # is not a constant and subtracting one from the other means nothing on
    # its own.
    #
    # What works is a sliding window minimum. A real delivery time is never
    # negative, so over a short stretch of time the smallest difference seen
    # is the offset between the two clocks at that moment.
    #
    # The window cannot be opened up. At plus or minus five hundred
    # milliseconds the drift alone is six thousand nanoseconds and it drowns
    # what is being measured. This uses plus or minus fifty.
    # The median of the whole round does not work either - the offset it
    # produces swings by milliseconds.
    # Nor does fitting one straight line to the round: the points sit inside
    # separate measurement windows with full speed replay in between, and the
    # gaps pull the line off.
    #
    # Takes two lists of the same length: the card's times, and the CPU's
    # times already converted to nanoseconds. Returns a function that, given a
    # card time, says how far apart the two clocks were at that moment.
    #
    # Pair them up and sort by the card's time, which the sliding window needs.
    pts = sorted(zip(rx_ns, cpu_ns))
    # The card's times on their own are the time axis.
    xs = [p[0] for p in pts]
    # The difference at each point. This is what the minimum is taken over.
    ds = [p[1] - p[0] for p in pts]
    # A few hundred thousand points in a round.
    n = len(xs)
    # Room for the baseline of every point.
    out = [0.0] * n
    # Two indexes walk the array once, marking off the points within win_ns of
    # the current one, and the smallest difference in that stretch is kept in a
    # queue. Giving every point its own scan would be quadratic, which is tens
    # of minutes at this many points; this way is linear.
    #
    # The queue holds indexes, and the ds values at those indexes increase from
    # front to back, so the front is always the smallest in the window.
    dq = deque()
    # These two only ever move forward, never back, which is what makes the
    # whole thing one pass.
    #
    # The left edge of the window.
    lo = 0
    # The right edge, meaning the first point not yet in the window.
    hi = 0
    # Every point gets its own baseline.
    for i in range(n):
        # Push the right edge out to the last point still inside the window.
        while hi < n and xs[hi] <= xs[i] + win_ns:
            # A new point smaller than the back of the queue makes those
            # entries useless: they are larger and they leave the window
            # sooner, so they can never be the minimum again.
            while dq and ds[dq[-1]] >= ds[hi]:
                # Drop the back. Several may go, hence while and not if.
                dq.pop()
            # The new point goes on the back.
            dq.append(hi)
            # And the right edge moves on.
            hi += 1
        # Pull the left edge up, dropping points that have fallen out.
        while xs[lo] < xs[i] - win_ns:
            # If the one falling out is the front of the queue it leaves too.
            # If it is not, it was already dropped by the pop above.
            if dq and dq[0] == lo:
                # At most one goes per step, because the edge moves one step.
                dq.popleft()
            # And the left edge moves on.
            lo += 1
        # The front of the queue is the smallest difference in this window,
        # which is the offset between the clocks at this moment. The fallback
        # is all but unreachable: the window always holds point i itself.
        out[i] = ds[dq[0]] if dq else ds[i]
    # Called twice for every order, so it has to be a binary search.
    def offset(x):
        # Where x belongs on the time axis.
        j = bisect.bisect_left(xs, x)
        # Use the baseline there. Both ends are clamped: an x past every point
        # gives j == n, which would run off the end.
        return out[min(n - 1, max(0, j))]
    # The returned function carries xs and out with it, so the caller gets a
    # table that has already been worked out rather than one that recomputes.
    return offset


# Handle one file. Everything above is strung together here.
def one(path):
    # This is the main logic. The order is: read the file, take the five
    # points out of it, subtract the two middle segments directly, line up the
    # clocks before subtracting the outer two, print the four distributions,
    # print how good the alignment was, check the four against the total, and
    # finally split the orders by how many frames their poll picked up.
    #
    # Nothing is returned; it all goes to the screen.
    tpn, rows = read(path)
    # No orders at all usually means the wrong path, or a round that produced
    # no samples.
    if not rows:
        # Say so rather than skipping quietly, which would read as success.
        print(f"  {path}: empty")
        # Nothing to work out here, back to main for the next file.
        return
    # The file name as a heading, so several files can be told apart.
    print(f"\n  {path}")
    # The count and the clock rate. A wrong rate makes every nanosecond below
    # wrong, so it is worth seeing.
    print(f"    {len(rows):,} orders, this machine's clock runs "
          f"{tpn:.4f} ticks per nanosecond")

    # Each of these pulls one column out into a list, in the same order as
    # rows, so the same index i reaches the same order in all of them.
    #
    # The moment the card had the frame, in nanoseconds. Point 0.
    rx   = [int(r['rx_ns']) for r in rows]
    # The total the trader reports, in nanoseconds. Point 4 minus point 0.
    lat  = [int(r['latency_ns']) for r in rows]
    # The moment the event queue told us, in ticks. Point 1.
    poll = [int(r['poll_tsc']) for r in rows]
    # The moment the body was in hand, in ticks. Point 2.
    body = [int(r['body_tsc']) for r in rows]
    # The moment the order was built, in ticks. Point 3.
    tcp  = [int(r['tcp_tsc']) for r in rows]
    # The moment parsing finished, in ticks. An events.csv written before
    # these two columns existed has neither, and they come back as zero: the
    # three sub-segments then find no samples and are not printed, which is
    # not an error.
    par  = [int(r.get('parse_tsc', 0)) for r in rows]
    # The moment the book was up to date, in ticks.
    bok  = [int(r.get('book_tsc', 0)) for r in rows]
    # How many frames the poll that carried this order picked up. The last
    # table groups on it.
    pn   = [int(r['poll_n']) for r in rows]

    # The two middle segments are CPU clock minus CPU clock, so they only need
    # converting to nanoseconds. No alignment problem here.
    #
    # fetch: how long to get the body in hand.
    # The condition picks the orders where both points were recorded and came
    # in the right order. A zero means that step was never reached, as when no
    # order went out.
    s21 = [(body[i] - poll[i]) / tpn for i in range(len(rows))
           if body[i] > poll[i] > 0]
    # work: the book, the signal and building the order. This is the part the
    # code can still change; measured at 620 ns at the median and 1,360 at p99.
    s32 = [(tcp[i] - body[i]) / tpn for i in range(len(rows))
           if tcp[i] > body[i] > 0]
    # The three pieces of work. They add up to it.
    # parse: pick the ITCH messages out of this poll's frames and sort them.
    sPar = [(par[i] - body[i]) / tpn for i in range(len(rows))
            if par[i] > body[i] > 0]
    # book: apply them to the book.
    sBok = [(bok[i] - par[i]) / tpn for i in range(len(rows))
            if bok[i] > par[i] > 0]
    # signal and order: check the signal, then write the bytes of the order
    # and its TCP header.
    sSig = [(tcp[i] - bok[i]) / tpn for i in range(len(rows))
            if tcp[i] > bok[i] > 0]

    # The outer two segments cross the two clocks, so the offset and the drift
    # have to be fitted out first.
    #
    # ok holds indexes, not values, and every list below is indexed with it,
    # so they all have to line up.
    ok = [i for i in range(len(rows)) if poll[i] > 0 and rx[i] > 0]
    # Build the offset table from those points. The CPU times are divided by
    # tpn first: both sides have to be in the same unit to be subtracted.
    off = base_line([rx[i] for i in ok], [poll[i] / tpn for i in ok])
    # wait: the frame reached the card, and this is how long until we were
    # told. Taking the offset off leaves the real waiting.
    s10 = [poll[i] / tpn - rx[i] - off(rx[i]) for i in ok]
    # Point 4 is when the card sent, which is the moment it received plus the
    # total it reported. Move that onto the CPU's ruler and take point 3 off.
    #
    # The offset is added here and subtracted on the line above, which is easy
    # to get backwards. The definition settles it: the offset is CPU time
    # minus card time, so a card time being moved onto the CPU's ruler has it
    # added, and a CPU time being moved onto the card's has it subtracted.
    s43 = [(rx[i] + lat[i]) + off(rx[i] + lat[i]) - tcp[i] / tpn
           for i in ok if tcp[i] > 0]

    # Now the results. The four segments, with the total for reference.
    print("\n    the four segments, the third split in three "
          "(ns, smaller is better):")
    # How long until we were told. It is not a problem of its own: the reason
    # a frame sits on the card is that we are still inside the last poll's
    # work and are not asking. Whatever comes off work comes off this too.
    line('wait  (1-0)', quant(s10))
    # Picking the body up. 50 ns at p99, small enough to leave alone.
    line('fetch (2-1)', quant(s21))
    # The book, the signal and the order.
    line('work  (3-2)', quant(s32))
    # The three pieces of it. An older file without those two columns prints
    # no samples here.
    if sPar or sBok or sSig:
        line('  parse', quant(sPar))
        line('  book', quant(sBok))
        line('  signal and order', quant(sSig))
    # Handed to the card until the bytes are out. This is the floor: 2,453 ns
    # at the median, most of it the card reading back across PCIe into host
    # memory, and it barely moves whatever the code does.
    line('send  (4-3)', quant(s43))
    # Wire-to-wire. Converted to float because the four segments above are,
    # and the printing has to match to be comparable.
    line('total (4-0)', quant([float(x) for x in lat]))

    # These next two numbers mark the table above.
    # A real elapsed time cannot be negative, so if the alignment is any good
    # there should be almost no negative values. Many of them means the
    # baseline was taken too high, and wait and send should be discounted.
    #
    # How many clearly negative values are in wait. The 50 ns of slack is
    # there because the floating point division leaves a remainder.
    n10 = sum(1 for x in s10 if x < -50)
    # And in send.
    n43 = sum(1 for x in s43 if x < -50)
    # Print them, with what counts as good, so the numbers mean something
    # without going and looking it up.
    print(f"\n    negative values left after the alignment (past 50 ns): "
          f"{n10} in wait, {n43} in send")
    print("       a negative means the baseline was taken too high; "
          "these should be near zero")

    # The check: the four segments should add up to the total. When they do
    # not, one of the points is being recorded in the wrong place.
    #
    # This is worth doing because that kind of mistake does not make any one
    # segment look wrong. It only stops them adding up.
    #
    # How many orders are off by more than a nanosecond.
    bad = 0
    # And by how much, at worst.
    worst = 0.0
    # Only the orders with both ends recorded.
    for i in ok:
        # No point 3, or point 2 before point 1, and this order cannot make
        # four segments.
        if tcp[i] <= 0 or body[i] <= poll[i]:
            # On to the next order.
            continue
        # Work the four out again from scratch rather than taking them from
        # the lists above. Taking them from the lists would mean a mistake in
        # the conditions above got repeated here, and the check would agree
        # with itself instead of checking anything.
        tot = (poll[i] / tpn - rx[i] - off(rx[i])) + (body[i] - poll[i]) / tpn + \
              (tcp[i] - body[i]) / tpn + \
              ((rx[i] + lat[i]) + off(rx[i] + lat[i]) - tcp[i] / tpn)
        # Against the total the trader itself reported.
        d = abs(tot - lat[i])
        # Keep the worst one.
        if d > worst:
            # This one is now the worst.
            worst = d
        # Past a nanosecond counts. The threshold cannot be zero: there are
        # several floating point divisions in the way and they leave a
        # remainder every time.
        if d > 1:
            # This order does not add up.
            bad += 1
    # A good round is a fraction of a nanosecond at worst and none past one.
    # A large worst here is the residual of the clock fit, and it is the limit
    # on how far wait and send can be trusted in this round.
    print(f"    check: the four segments and the total differ by at most "
          f"{worst:.1f} ns, and {bad} orders differ by more than 1 ns")

    # Split by how many frames the poll picked up, because whatever batching
    # buys should grow with the depth.
    #
    # The question is whether a poll that picks up more frames costs less per
    # order. If it does, batching is worth doing; if every depth looks the
    # same, there is nothing there to win.
    print("\n    split by frames in the poll (only the deeper ones):")
    # The heading, written to the same widths as the row below it.
    print("      frames   orders     fetch p50     work p50    total p50")
    # Keyed by how many frames were in the poll.
    by = {}
    # Walk every order and drop it in its bucket.
    for i in range(len(rows)):
        # An order missing one of the three points cannot give two segments.
        if body[i] <= poll[i] or tcp[i] <= body[i]:
            # It cannot go in the table, so on to the next.
            continue
        # setdefault starts the list the first time a depth turns up. Three
        # numbers per order: fetch, work and the total.
        by.setdefault(pn[i], []).append(
            ((body[i] - poll[i]) / tpn, (tcp[i] - body[i]) / tpn, float(lat[i])))
    # Shallowest first.
    for d in sorted(by):
        # Every order at this depth.
        v = by[d]
        # The deep buckets are rare by nature, and the median of a dozen
        # samples is noise. Printing it would only mislead.
        if len(v) < 20:
            # Skip this depth.
            continue
        # The median of the kth number in this bucket.
        m = lambda k: sorted(x[k] for x in v)[len(v) // 2]
        # One line per depth: the frames, the orders, and three medians.
        print(f"      {d:>6}   {len(v):>6,}   {m(0):>11,.0f}   "
              f"{m(1):>10,.0f}   {m(2):>10,.0f}")


# The command line. Several files can be given and are printed one after another.
def main():
    # Nothing given, so print the usage and stop.
    if len(sys.argv) < 2:
        # Better than an index error nobody can read.
        print("usage: segments.py <events.csv> [events.csv ...]")
        # 1 means it did not run.
        return 1
    # Argument 0 is this script, so the files start at 1.
    for p in sys.argv[1:]:
        # One at a time. A file that cannot be read raises and stops the rest,
        # which for offline analysis is what should happen: a failure here has
        # to be seen.
        one(p)
    # All of them done.
    return 0


# Only run when this file is the program, not when something imports it.
if __name__ == '__main__':
    # The return value becomes the exit code of the process.
    sys.exit(main())
