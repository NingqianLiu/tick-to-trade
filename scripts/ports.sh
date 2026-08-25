sf_ports() {
    local found=()
    local dev drv
    for dev in /sys/class/net/*; do
        [[ -e $dev/device/driver ]] || continue
        drv=$(basename "$(readlink -f "$dev/device/driver")")
        [[ $drv == sfc ]] && found+=("$(basename "$dev")")
    done
    if ip netns list 2>/dev/null | grep -qw trader; then
        for dev in $(ip netns exec trader ls /sys/class/net 2>/dev/null); do
            drv=$(ip netns exec trader readlink -f "/sys/class/net/$dev/device/driver" 2>/dev/null || true)
            [[ $(basename "${drv:-none}") == sfc ]] && found+=("$dev")
        done
    fi
    [[ ${#found[@]} -gt 0 ]] || return 0
    printf '%s\n' "${found[@]}" | sort -u
}

mapfile -t ITCH_SF_PORTS < <(sf_ports)
if [[ ${#ITCH_SF_PORTS[@]} -ne 2 ]]; then
    echo "expected two ports on the sfc driver, found ${#ITCH_SF_PORTS[@]}: ${ITCH_SF_PORTS[*]}" >&2
    return 1 2>/dev/null || exit 1
fi
PRODUCER_IF=${PRODUCER_IF:-${ITCH_SF_PORTS[0]}}
TRADER_IF=${TRADER_IF:-${ITCH_SF_PORTS[1]}}
