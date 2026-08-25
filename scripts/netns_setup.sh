#!/bin/bash

set -euo pipefail

NS=trader
. "$(dirname "$0")/ports.sh"
PRODUCER_IP=10.9.9.1/24
TRADER_IP=10.9.9.2/24

require_solarflare() {
    local dev=$1 drv
    if [[ ! -e /sys/class/net/$dev ]]; then
        drv=$(ip netns exec "$NS" readlink -f "/sys/class/net/$dev/device/driver" 2>/dev/null)
        [[ $(basename "${drv:-none}") == sfc ]] && return 0
        echo "refusing: $dev is neither here nor in namespace $NS" >&2
        exit 1
    fi
    drv=$(basename "$(readlink -f "/sys/class/net/$dev/device/driver")")
    if [[ $drv != sfc ]]; then
        echo "refusing: $dev is driven by $drv, not sfc" >&2
        exit 1
    fi
    if ip route show default | grep -qw "$dev"; then
        echo "refusing: $dev carries the default route" >&2
        exit 1
    fi
}

require_solarflare "$PRODUCER_IF"
require_solarflare "$TRADER_IF"

nmcli device set "$PRODUCER_IF" managed no 2>/dev/null || true
nmcli device set "$TRADER_IF" managed no 2>/dev/null || true

ip netns list | grep -qw "$NS" || ip netns add "$NS"

if [[ ! -e /sys/class/net/$TRADER_IF ]]; then
    echo "$TRADER_IF is already in a namespace"
else
    ip link set "$TRADER_IF" netns "$NS"
fi

ip addr flush dev "$PRODUCER_IF"
ip addr add "$PRODUCER_IP" dev "$PRODUCER_IF"
ip link set "$PRODUCER_IF" up

ip netns exec "$NS" ip addr flush dev "$TRADER_IF"
ip netns exec "$NS" ip addr add "$TRADER_IP" dev "$TRADER_IF"
ip netns exec "$NS" ip link set "$TRADER_IF" up
ip netns exec "$NS" ip link set lo up

echo "default namespace:"
ip -br addr show "$PRODUCER_IF"
echo "namespace $NS:"
ip netns exec "$NS" ip -br addr show "$TRADER_IF"
echo
echo "the trader runs as:  ip netns exec $NS <command>"
