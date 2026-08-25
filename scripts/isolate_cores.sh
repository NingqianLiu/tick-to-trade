#!/bin/bash

set -uo pipefail

CORES=${CORES:-4-7}
REST=${REST:-0-3,8-95}
GROUP=/sys/fs/cgroup/hft

expand() {
    local out=() part
    for part in ${1//,/ }; do
        if [[ $part == *-* ]]; then out+=($(seq "${part%-*}" "${part#*-}"));
        else out+=("$part"); fi
    done
    echo "${out[@]}"
}

nic_irqs() {
    local f
    for f in /sys/class/net/*/device/msi_irqs/*; do
        [[ -e $f ]] && echo "${f##*/}"
    done
}

steer_interrupts() {
    local away=$1 n irq skip
    declare -A skip=()
    for irq in $(nic_irqs); do skip[$irq]=1; done
    for n in /proc/irq/[0-9]*; do
        irq=${n##*/}
        [[ -n ${skip[$irq]:-} ]] && continue
        [[ -w $n/smp_affinity_list ]] || continue
        echo "$away" > "$n/smp_affinity_list" 2>/dev/null || true
    done
}

case ${1:-on} in
run)
    shift
    [[ ${1:-} == -- ]] && shift
    if [[ ! -d $GROUP ]]; then
        echo "no reserved cores; run isolate_cores.sh on first" >&2
        exit 1
    fi
    echo $$ > "$GROUP/cgroup.procs" || exit 1
    exec "$@"
    ;;
off)
    for slice in system.slice user.slice init.scope; do
        [[ -e /sys/fs/cgroup/$slice/cpuset.cpus ]] && echo "" > "/sys/fs/cgroup/$slice/cpuset.cpus"
    done
    [[ -d $GROUP ]] && rmdir "$GROUP" 2>/dev/null
    steer_interrupts "0-$(($(nproc) - 1))"
    echo "cores released"
    exit 0
    ;;
esac

mkdir -p "$GROUP"
echo "$CORES" > "$GROUP/cpuset.cpus"
echo root > "$GROUP/cpuset.cpus.partition" 2>/dev/null || true
printf 'cpuset partition: %s\n' "$(cat "$GROUP/cpuset.cpus.partition" 2>/dev/null)"

for slice in system.slice user.slice init.scope; do
    [[ -e /sys/fs/cgroup/$slice/cpuset.cpus ]] && echo "$REST" > "/sys/fs/cgroup/$slice/cpuset.cpus"
done

steer_interrupts "$REST"

echo "reserved $CORES; everything else is on $REST"
echo "left $(nic_irqs | wc -l) network card interrupts where they were"
echo "launch with:  $0 run -- <command>"
for c in $(expand "$CORES"); do
    echo "  cpu $c still has $(ps -eLo psr= --no-headers | awk -v c="$c" '$1==c' | wc -l) threads"
done
echo "  (per-cpu kernel threads cannot be moved without isolcpus at boot)"
