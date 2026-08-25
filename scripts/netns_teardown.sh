#!/bin/bash

set -euo pipefail

NS=trader
. "$(dirname "$0")/ports.sh"

if ip netns list | grep -qw "$NS"; then
    if ip netns exec "$NS" test -e "/sys/class/net/$TRADER_IF"; then
        drv=$(ip netns exec "$NS" basename \
              "$(ip netns exec "$NS" readlink -f "/sys/class/net/$TRADER_IF/device/driver")")
        if [[ $drv != sfc ]]; then
            echo "refusing: $TRADER_IF is driven by $drv, not sfc" >&2
            exit 1
        fi
        ip netns exec "$NS" ip link set "$TRADER_IF" netns 1
    fi
    ip netns delete "$NS"
fi

ip addr flush dev "$PRODUCER_IF" || true
ip link set "$TRADER_IF" up || true
ip -br addr show "$PRODUCER_IF"
ip -br addr show "$TRADER_IF"
