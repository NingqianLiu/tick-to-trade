#!/bin/bash
# One round of the benchmark: start the exchange side, start the trader, replay a day of market
# data at it, and write the latency samples out.
#
# What it starts, in this order and for these reasons:
#   1 ds_check   the exchange side. It listens on TCP and reads the OUCH orders the trader sends.
#                It has to be up first, or the trader's handshake finds nobody listening.
#   2 trader     the thing being measured. It needs about half a minute before the first packet
#                arrives: it reserves several gigabytes of huge pages, touches every page of the
#                book, and registers the receive buffers with the card.
#   3 sender     the replay. It reads the ITCH file and puts it on the wire, keeping the real
#                spacing of the day inside the measurement windows and running flat out between
#                them.
#
# What this box needs before any of it works:
#   a Solarflare card with two ports, one for market data and one for orders
#   a network namespace called trader (scripts/netns_setup.sh makes it)
#   2 MB huge pages available, and none of them already reserved
#   the host settings scripts/preflight.sh checks - none of them survive a reboot
#
# Every setting below is an environment variable with a default, so a round is one line:
#   DATA=... FROM=9:30 TO=10:10 ./scripts/run.sh

set -u

# Where the ITCH file is. It is not in this repository - scripts/download_nasdaq_itch.sh fetches
# one.
DATA=${DATA:-data/itch.txt}
# The previous close of every security, which is what the price space of each is centred on.
# Without it not one security gets a price space and not one order can go out.
REFERENCE=${REFERENCE:-results/prev_close.csv}
# Which securities to subscribe to. Leaving it out subscribes to the whole market, which is six
# times the orders and a p50 in milliseconds rather than nanoseconds.
SYMBOLS=${SYMBOLS:-results/symbols.csv}
# Where the samples go. latency.csv, events.csv and windows.csv all land in this directory.
OUT=${OUT:-results/run}
# Which core the trader starts pinning at. The acknowledgement thread takes the one after it, and
# the huge pages come from whichever half of the memory that core is on.
BASE=${BASE:-52}
# The imbalance threshold as a percentage. Comparing the latency of two rounds means keeping this
# the same, because it decides directly how many orders go out.
THRESHOLD=${THRESHOLD:-75}
# Which build to run. Pointing it at another build is how two versions are compared.
BIN=${BIN:-./build/trader}
# Where the trader's own output goes. The numbers that have to add up are all in here.
LOG=${LOG:-/tmp/run.log}
# How long a warm up runs before each measurement window, in milliseconds. Both sides read the
# same value, or they would disagree about where a window starts.
SETTLE=${SETTLE:-200}
# How long between packets outside the windows, in nanoseconds. Inside a window the real spacing
# of the day is kept and this does nothing.
GAP=${GAP:-50000}
# Extra arguments for the trader, such as --segments to time the parts of the path. Deliberately
# unquoted below so that several arguments split into several.
EXTRA=${EXTRA:-}
# Extra arguments for the replay, such as --speed 50x to run a stretch faster than the day did.
SEND_EXTRA=${SEND_EXTRA:-}

# Which of the two Solarflare ports is which. ports.sh finds them by their driver rather than by
# name: the name changes with which slot the card is in, and an address changes with DHCP, while
# the driver does not.
# shellcheck source=scripts/ports.sh
. "$(dirname "$0")/ports.sh" || exit 1

# The input files have to be there. Reporting it here saves a round that starts, runs for minutes
# and produces nothing.
for f in "$DATA" "$REFERENCE" "$SYMBOLS"; do
    [[ -r $f ]] || { echo "cannot read $f" >&2; exit 1; }
done
# And the build has to be there.
[[ -x $BIN ]] || { echo "$BIN is not there; build it first" >&2; exit 1; }

# Huge pages already reserved mean something did not clean up after itself, and the trader would
# fail to get its own. They are not cleared automatically here, because clearing them would hide
# a process that is still using them.
reserved=$(cat /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null |
           paste -sd+ | bc)
if [[ ${reserved:-0} -ne 0 ]]; then
    echo "$reserved huge pages are already reserved; something did not clean up" >&2
    exit 1
fi

# An exchange side left over from a previous round would still hold the TCP port.
pkill -x ds_check 2>/dev/null
sleep 1

# 1 - the exchange side. It listens for the trader's orders and counts them, and the count is
# compared with the trader's own at the end: one frame may carry several orders, and only the
# receiving side can prove that none was lost or run together.
./build/ds_check --listen 10.9.9.1 46200 > /tmp/ds_check.log 2>&1 &
# A moment for it to bind and listen, so the trader's first connect does not arrive too early.
sleep 2

# 2 - the trader.
# It runs inside the network namespace so that the card's ports are its own, and under onload so
# that the parts of the path that are not ef_vi bypass the kernel too.
# --idle-ms says how long without a packet counts as the replay having finished, which is what
# ends a round by itself.
ITCH_WINDOW_SETTLE_MS="$SETTLE" \
    ip netns exec trader onload "$BIN" \
        --intf "$TRADER_IF" \
        --cpu-base "$BASE" --threshold "$THRESHOLD" \
        --reference "$REFERENCE" --symbols "$SYMBOLS" \
        --order-ip 10.9.9.1 --order-port 46200 \
        --idle-ms 20000 ${EXTRA} \
        --out "$OUT" > "$LOG" 2>&1 &
trader=$!

# The trader needs this long to reserve its huge pages, touch the whole book and register the
# receive buffers. Starting the replay earlier would count that setting up as a gap between
# packets.
sleep 35

# 3 - the replay.
# The same warm up value as the trader, or the two would disagree about where a window starts.
# ITCH_GAP_NS only sets the spacing outside the windows; inside them the real spacing of the day
# is kept.
ITCH_WINDOW_SETTLE_MS="$SETTLE" ITCH_GAP_NS="$GAP" \
    ./build/sender "$DATA" --intf "$PRODUCER_IF" \
        --drift-out "/tmp/$(basename "$OUT").drift" ${SEND_EXTRA} 2>&1 | tail -16

# The replay has finished sending; the trader still has to notice the silence, write its samples
# out and release its huge pages.
wait $trader
rc=$?

# The exchange side is no longer needed.
pkill -x ds_check 2>/dev/null

# The trader's exit code is this round's verdict: a 0 means every acceptance check passed - no
# gaps in the sequence numbers, nothing lapped, no unknown order ids, nothing the card dropped,
# the order table never full, and no measurement window thrown away.
# Anything else means the numbers of this round cannot be compared with another's.
echo
echo "trader exited with $rc; the numbers are in $LOG and the samples in $OUT"
exit $rc
