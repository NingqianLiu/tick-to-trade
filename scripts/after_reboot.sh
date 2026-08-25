#!/bin/bash

set -uo pipefail
cd "$(dirname "$0")/.."

echo "=== what the kernel was given ==="
cat /proc/cmdline
echo
printf '%-24s %s\n' "isolated cpus"  "$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo '(none)')"
printf '%-24s %s\n' "nohz_full cpus" "$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo '(none)')"
printf '%-24s %s\n' "2M huge pages"  "$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)"
printf '%-24s %s\n' "1G huge pages"  "$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages 2>/dev/null || echo '-')"
printf '%-24s %s\n' "iommu mode" "$(cat /sys/kernel/iommu_groups/0/type 2>/dev/null || echo '(no groups; passthrough)')"
printf '%-24s %s\n' "threading" "$(cat /sys/devices/system/cpu/smt/control)"

echo
echo "=== putting back what the restart took ==="
./scripts/netns_setup.sh || { echo "namespace failed" >&2; exit 1; }
CORES=${CORES:-40-43,52-95} REST=${REST:-0-39,44-51} ./scripts/isolate_cores.sh on
for slice in system.slice user.slice; do
    systemctl set-property "$slice" AllowedCPUs="${REST:-0-39,44-51}" 2>/dev/null || true
done

echo off > /sys/devices/system/cpu/smt/control 2>/dev/null
sleep 1

for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$g" 2>/dev/null
done
echo 0 > /proc/sys/kernel/numa_balancing
echo 0 > /proc/sys/kernel/nmi_watchdog
echo 0 > /proc/sys/kernel/watchdog
echo 60 > /proc/sys/vm/stat_interval
swapoff -a
echo never > /sys/kernel/mm/transparent_hugepage/enabled
echo never > /sys/kernel/mm/transparent_hugepage/defrag

./scripts/move_irqs.py "$(cat /sys/fs/cgroup/hft/cpuset.cpus)"

echo
echo "=== check ==="
ip netns list
printf '%-24s %s\n' "reserved cores" "$(cat /sys/fs/cgroup/hft/cpuset.cpus)"
printf '%-24s %s\n' "everything else" "$(cat /sys/fs/cgroup/system.slice/cpuset.cpus)"
printf '%-24s %s\n' "governor" "$(cat /sys/devices/system/cpu/cpu52/cpufreq/scaling_governor)"
printf '%-24s %s\n' "numa balancing" "$(cat /proc/sys/kernel/numa_balancing)"
printf '%-24s %s\n' "transparent huge" "$(cat /sys/kernel/mm/transparent_hugepage/enabled)"
printf '%-24s %s\n' "threading" "$(cat /sys/devices/system/cpu/smt/control)"
printf '%-24s %s\n' "swap" "$(swapon --show --noheadings | wc -l) devices"
echo "run the consumer with:  ./scripts/isolate_cores.sh run -- ... --cpu-base 52"
