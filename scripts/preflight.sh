#!/bin/bash
set -u
fail=0
chk () { local got=$1 want=$2 name=$3
    if [[ "$got" == "$want" ]]; then echo "  ok   $name = $got"
    else echo "  BAD  $name = $got (want $want)"; fail=1; fi }
chkin () { local got=$1 want=$2 name=$3
    if [[ "$got" == *"$want"* ]]; then echo "  ok   $name = $got"
    else echo "  BAD  $name = $got (want it to contain $want)"; fail=1; fi }
chk "$(cat /sys/devices/system/cpu/smt/control)" off "hyperthreading"
chkin "$(cat /sys/kernel/mm/transparent_hugepage/enabled)" "[never]" "transparent huge pages"
chk "$(cat /proc/sys/kernel/numa_balancing)" 0 "numa_balancing"
chk "$(cat /proc/sys/kernel/watchdog)" 0 "watchdog"
chk "$(cat /proc/sys/kernel/nmi_watchdog)" 0 "nmi_watchdog"
chk "$(swapon --show --noheadings | wc -l)" 0 "swap areas"
chk "$(cat /sys/fs/cgroup/hft/cpuset.cpus 2>/dev/null)" "40-43,52-95" "hft cpuset"
chk "$(cat /sys/devices/system/cpu/cpu52/cpuidle/state2/disable)" 1 "C2 disabled"
chk "$(cat /sys/devices/system/cpu/cpu52/cpufreq/scaling_min_freq)" 2600000 "minimum clock"
chk "$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null)" 0 "boost disabled"
chk "$(cat /sys/devices/system/machinecheck/machinecheck0/check_interval)" 0 "machine check polling"
chk "$(ethtool -a enp129s0f0 2>/dev/null | grep '^RX:' | awk '{print $2}')" off "flow control"
chk "$(cat /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages)" 0 "huge pages reserved"
chk "$(ip netns exec trader ip neigh show 2>/dev/null | grep -c 10.9.9.1)" 1 "peer in the ARP table"
exit $fail
