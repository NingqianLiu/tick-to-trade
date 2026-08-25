#!/usr/bin/env python3

import glob
import os
import sys

def spread(text):
    out = set()
    for part in text.split(","):
        if not part:
            continue
        ends = part.split("-")
        out.update(range(int(ends[0]), int(ends[-1]) + 1))
    return out

def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    held = spread(argv[1])
    free = ",".join(str(c) for c in sorted(set(range(os.cpu_count())) - held))
    moved, stuck = 0, []
    for d in sorted(glob.glob("/proc/irq/[0-9]*")):
        try:
            before = open(d + "/effective_affinity_list").read().strip()
        except OSError:
            continue
        if not before or not (spread(before) & held):
            continue
        try:
            with open(d + "/smp_affinity_list", "w") as f:
                f.write(free)
        except OSError:
            pass
        after = open(d + "/effective_affinity_list").read().strip()
        if after == before:
            stuck.append(os.path.basename(d))
        else:
            moved += 1
    print(f"  interrupts moved off the reserved cores: {moved}")
    print(f"  the kernel would not move: {len(stuck)}")
    fired = {}
    for line in open("/proc/interrupts"):
        head, _, rest = line.partition(":")
        n = head.strip()
        if n in stuck:
            fired[n] = sum(int(x) for x in rest.split() if x.isdigit())
    hot = {n: c for n, c in fired.items() if c > 0}
    print(f"  of those, ever fired: {len(hot)}" + (f"  {hot}" if hot else ""))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
