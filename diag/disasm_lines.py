#!/usr/bin/env python3
"""Show what a range of source lines compiled into.

Usage
    ./disasm_lines.py <executable or .o> <source path> <first line> [line count]

    Example: see what src/foo.cpp lines 1994-1998 compiled into
        diag/disasm_lines.py  build/trader ./src/tools/trader.cpp 1994 4

    Leave the line count out and it looks at one line. The source path does two
    jobs: its file name is matched against the binary, and the lines themselves
    are printed next to the instructions for comparison.

    Compile with -g. All -g does is store one extra table saying which source
    line each instruction came from. It does not change the generated machine
    code, so it can stay on and it does not affect a performance measurement.

Options
    --strict        show only instructions tagged with these exact lines. By
                    default the tagged instructions fix a range of addresses and
                    everything inside that range is shown, which is what these
                    lines really execute once code is inlined in from headers.
    --mark <regex>  put a > next to every instruction matching the regex
    --loops         also show each loop on its own
    --brief         counts only, no instruction listing
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

# Decide whether an instruction touches memory: any operand written as
# (%register) or (%base,%index,scale) counts.
MEM_OPERAND = re.compile(r'\((%[a-z0-9]+)?(?:,%[a-z0-9]+,\d)?\)')
# These do not touch memory even when they carry brackets:
#   lea    only computes an address, it never fetches
#   nop    does nothing, it is there for padding
#   jumps  the brackets hold a jump target, not a data address
NOT_MEMORY = ('lea', 'nop', 'nopw', 'nopl', 'nopq', 'cs', 'data16', 'xchg', 'endbr64')
SRC_SUFFIX = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.hxx', '.inc')


def have(tool):
    """Check that an external command exists, and say how to install it."""
    if shutil.which(tool):
        return True
    print(f"Cannot find the command {tool}. It ships in the binutils package:")
    print("    Debian / Ubuntu :  sudo apt install binutils")
    print("    RHEL / Fedora   :  sudo dnf install binutils")
    return False


def build_hint():
    """Every kind of "not found" ends up here, so the rebuild command is said once."""
    print()
    print("  To rebuild, use one of these (put your own file name in):")
    print("      object file :  g++ -std=c++17 -O3 -march=native -g -c yours.cpp -o /tmp/yours.o")
    print("      executable  :  g++ -std=c++17 -O3 -march=native -g   yours.cpp -o /tmp/yours")
    print()
    print("  With cmake, add -g to the whole project:")
    print("      cmake -S . -B build -DCMAKE_CXX_FLAGS=-g && cmake --build build")
    print("      Only CMAKE_BUILD_TYPE=Debug carries -g by default; Release does not")
    print()
    print("  -g only stores one extra table saying which source line each instruction")
    print("  came from. It does not change the machine code, so it can stay on and it")
    print("  does not affect a performance measurement. Strip it when you ship.")


def disassemble(binary):
    """Disassemble, and return [(address, instruction text, source file, line)].

    The objdump options:
      -d                 disassemble the code section
      -C                 undo C++ name mangling
      --no-show-raw-insn leave out the hex machine code, keep only the assembly
      -l                 insert "this came from file X line N" between instructions
                         (needs -g at compile time)
    """
    cmd = ["objdump", "-d", "-C", "--no-show-raw-insn", "-l", binary]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(f"objdump cannot read this file:\n{out.stderr.strip()}")
        return None
    insns, f, ln = [], None, None
    for raw in out.stdout.split('\n'):
        line = raw.rstrip()
        m = re.match(r'^(\S+):(\d+)', line.strip())
        if m and m.group(1).endswith(SRC_SUFFIX):
            f, ln = m.group(1), int(m.group(2))
            continue
        m = re.match(r'^\s+([0-9a-f]+):\s+(.*)$', line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2).strip(), f, ln))
    return insns


def is_memory(text):
    """Does this instruction touch memory."""
    parts = text.split()
    op = parts[0] if parts else ''
    if op.startswith('j') or op in NOT_MEMORY:
        return False
    return bool(MEM_OPERAND.search(text))


def find_loops(insns):
    """Find every loop.

    The test: a branch that jumps backwards, meaning its target address is lower
    than the branch itself. Everything from the target up to the branch is one
    pass through the loop body.
    """
    by_addr = {a: i for i, (a, _, _, _) in enumerate(insns)}
    loops = []
    for i, (addr, text, _, _) in enumerate(insns):
        m = re.match(r'^j\w+\s+([0-9a-f]+)', text)
        if not m:
            continue
        tgt = int(m.group(1), 16)
        if tgt > addr or tgt not in by_addr:
            continue
        loops.append(insns[by_addr[tgt]:i + 1])
    return loops


def listing(body, mark_re, src, indent="      "):
    """Print the instructions, with the source line printed above each group that
    came from the same line."""
    last = None
    for addr, text, f, ln in body:
        if ln is not None and (f, ln) != last:
            code = src.line(f, ln) if src else None
            code = code.strip() if code else ''
            # Print only the last part of the path, but look the source up by the
            # full path, so two files with the same base name never get mixed up
            print(f"{indent}---- {os.path.basename(f)}:{ln}" + (f"   {code}" if code else ""))
            last = (f, ln)
        if mark_re is not None and mark_re.search(text):
            mark = ' >'
        elif is_memory(text):
            mark = ' .'
        else:
            mark = '  '
        print(f"{indent}  {addr:>8x}: {text:<44s}{mark}")


def counts(body, mark_re):
    """One line saying how big this run of instructions is."""
    mem = sum(1 for x in body if is_memory(x[1]))
    out = f"{len(body)} instructions, {mem} touch memory"
    if mark_re is not None:
        out += f", {sum(1 for x in body if mark_re.search(x[1]))} match --mark"
    return out


def pick_by_lines(binary, srcname, first, count, strict):
    """Pick the stretch of addresses to show, given a range of source lines."""
    insns = disassemble(binary)
    if insns is None:
        return None
    last = first + count - 1
    hit = [x for x in insns if x[2] and x[2].endswith(srcname) and x[3] and first <= x[3] <= last]
    if not hit:
        seen_files = sorted({x[2] for x in insns if x[2]})
        any_line = any(x[3] is not None for x in insns)
        print(f"No instructions for {srcname} lines {first}-{last} in {binary}.")
        print()
        if not any_line:
            print("  This file carries no line information at all, so it was built")
            print("  without -g. That is by far the most common cause.")
        elif not any(f.endswith(srcname) for f in seen_files):
            print(f"  There is line information, but no file called {srcname}. Typo in the name?")
            print(f"  This binary mentions {len(seen_files)} source files; the first few are:")
            for f in seen_files[:10]:
                print(f"       {f}")
        else:
            got = sorted({x[3] for x in insns if x[2] and x[2].endswith(srcname) and x[3]})
            print(f"  {srcname} has line information, but not for these lines. Either:")
            print("       they were optimised away (dead code, or folded into a constant)")
            print("       they are only declarations, comments or braces, which emit nothing")
            if got:
                print(f"     Lines with instructions run from {got[0]} to {got[-1]},"
                      f" {len(got)} distinct lines in all")
        build_hint()
        return None
    lo, hi = min(x[0] for x in hit), max(x[0] for x in hit) + 1
    if strict:
        return lo, hi, hit, True
    return lo, hi, [x for x in insns if lo <= x[0] < hi], False


class SourceCache:
    """Cache source files, one entry per file.

    A stretch of machine code can come from several files at once: a function
    inlined from a header is tagged with that header's name. So each file has to
    be read separately, and one file's text must never be used to answer another
    file's line number.
    """

    def __init__(self, anchor):
        # The anchor is the source path the user gave. It is used to guess where
        # the other files might be.
        self.anchor = anchor
        self.anchor_dir = os.path.dirname(os.path.abspath(anchor)) if anchor else ''
        self.files = {}          # full path -> {line number: text}, or None if unreadable

    def _candidates(self, path):
        """Where a file might be: the path recorded at compile time, or somewhere
        near the path the user gave."""
        yield path
        base = os.path.basename(path)
        if self.anchor and os.path.basename(self.anchor) == base:
            yield self.anchor
        if self.anchor_dir:
            yield os.path.join(self.anchor_dir, base)
            yield os.path.join(self.anchor_dir, '..', base)
        yield base

    def line(self, path, num):
        """Fetch one line of one file. Return None if it cannot be read, in which
        case the caller prints the line number without the text."""
        if path not in self.files:
            self.files[path] = None
            for cand in self._candidates(path):
                try:
                    with open(cand) as f:
                        self.files[path] = {i: l.rstrip('\n') for i, l in enumerate(f, 1)}
                    break
                except OSError:
                    continue
        table = self.files[path]
        return None if table is None else table.get(num)

    def missing(self):
        """Which files were not found, so the end of the report can say so and the
        user does not think the tool dropped something."""
        return sorted(os.path.basename(k) for k, v in self.files.items() if v is None)


def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument('binary', nargs='?')
    ap.add_argument('source', nargs='?')
    ap.add_argument('first', nargs='?', type=int)
    ap.add_argument('count', nargs='?', type=int, default=1)
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--mark')
    ap.add_argument('--loops', action='store_true')
    ap.add_argument('--brief', action='store_true')
    ap.add_argument('-h', '--help', action='store_true')
    a = ap.parse_args()

    if a.help or not a.binary or not a.source or a.first is None:
        print(__doc__)
        return 0
    if not have('objdump'):
        return 2
    try:
        open(a.binary, 'rb').close()
    except OSError as e:
        print(f"Cannot open {a.binary}: {e}")
        build_hint()
        return 2

    # The source path does two jobs: its base name is matched against the binary,
    # and the file itself is read so the source can be printed alongside
    srcname = os.path.basename(a.source)
    src = SourceCache(a.source)
    mark_re = re.compile(a.mark) if a.mark else None

    picked = pick_by_lines(a.binary, srcname, a.first, a.count, a.strict)
    if picked is None:
        return 1
    lo, hi, insns, strict = picked
    last = a.first + a.count - 1

    how = "only instructions tagged with these lines" if strict \
        else "every instruction in the address range these lines cover"
    print(f"\n=== {srcname}:{a.first}-{last}   0x{lo:x}-0x{hi:x} ===")
    print(f"    {how}")
    print(f"    {counts(insns, mark_re)}")
    if mark_re is not None:
        print("    marks:  > matches --mark,  . touches memory")
    else:
        print("    marks:  . touches memory")
    if not a.brief:
        print()
        listing(insns, mark_re, src)

    loops = find_loops(insns)
    if a.loops:
        print()
        if not loops:
            print("  No branch jumps backwards here, so there is no loop.")
            print("  If you are sure the source has one, there are three possibilities:")
            print("    1 the compiler unrolled it away, leaving straight-line code")
            print("    2 the loop body sits in another function that was not inlined")
            print("    3 the line range covers only part of the loop; widen it")
        for k, body in enumerate(loops):
            print(f"  [loop {k + 1}]  0x{body[0][0]:x}-0x{body[-1][0]:x}   {counts(body, mark_re)}")
            if not a.brief:
                listing(body, mark_re, src, indent="        ")
                print()
    elif loops:
        print()
        print(f"    ({len(loops)} loop(s) in this range; --loops shows each one on its own)")

    miss = src.missing()
    if miss:
        print()
        print("    Source not found for these files, so their lines were printed")
        print("    without their text:")
        print("       " + ", ".join(miss))
        print("       They are code that was inlined in. To see the text, run this tool")
        print("       from their directory, or put them next to the source you passed.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
