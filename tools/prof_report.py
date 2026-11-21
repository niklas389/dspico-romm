#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Resolves sampling-profiler output (romm/prof-*.bin) against the ELF and map
# file of the exact build that produced it.
#
# Usage: tools/prof_report.py <samples.bin> <build/NAME.elf> [build/NAME.map]

import bisect
import collections
import re
import struct
import subprocess
import sys

NM = "/opt/wonderful/toolchain/gcc-arm-none-eabi/bin/arm-none-eabi-nm"
OBJDUMP = "/opt/wonderful/toolchain/gcc-arm-none-eabi/bin/arm-none-eabi-objdump"


def load_samples(path):
    data = open(path, "rb").read()
    (n,) = struct.unpack_from("<I", data, 0)
    return list(struct.unpack_from("<%dI" % n, data, 4))


def load_symbols(elf):
    out = subprocess.run([NM, "-n", "-S", "--defined-only", elf],
                         capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4:
            addr, size, kind, name = parts
            size = int(size, 16)
        elif len(parts) == 3:
            addr, kind, name = parts
            size = 0
        else:
            continue
        if kind.lower() not in ("t", "w"):
            continue
        syms.append((int(addr, 16), size, name))
    syms.sort()
    return syms


def wfi_addresses(elf):
    # Instruction "mcr p15,0,rX,c7,c0,4" = wait for interrupt (CPU halted).
    out = subprocess.run([OBJDUMP, "-d", elf], capture_output=True, text=True,
                         check=True).stdout
    addrs = set()
    for line in out.splitlines():
        if re.search(r"\bmcr\s+15, 0, r\d+, cr7, cr0, \{4\}", line):
            addrs.add(int(line.split(":")[0], 16))
    return addrs


def load_map_objects(mapfile):
    # Returns sorted list of (start, size, archive/object) for .text sections.
    ranges = []
    cur = None
    rx = re.compile(r"^\s*\.(?:text|itcm)\S*\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)")
    rx_name = re.compile(r"^\s*\.(?:text|itcm)\S*\s*$")
    rx_cont = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)")
    pending = False
    for line in open(mapfile, errors="replace"):
        m = rx.match(line)
        if m:
            ranges.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
            pending = False
            continue
        if rx_name.match(line):
            pending = True
            continue
        if pending:
            m = rx_cont.match(line)
            if m:
                ranges.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
            pending = False
    ranges = [r for r in ranges if r[1] > 0]
    ranges.sort()
    return ranges


def short_lib(path):
    m = re.search(r"lib(\w+?)\.a\((.+)\)", path)
    if m:
        return m.group(1), m.group(2)
    return "app", path.split("/")[-1]


def main():
    samples = load_samples(sys.argv[1])
    syms = load_symbols(sys.argv[2])
    objs = load_map_objects(sys.argv[3]) if len(sys.argv) > 3 else []
    wfi = wfi_addresses(sys.argv[2])
    starts = [s[0] for s in syms]
    ostarts = [o[0] for o in objs]

    funcs = collections.Counter()
    libs = collections.Counter()
    idle = 0
    for pc in samples:
        pc &= ~1
        if any(0 <= pc - a <= 8 for a in wfi):
            idle += 1
            funcs["<idle: CPU halted>"] += 1
            libs["<idle>"] += 1
            continue
        i = bisect.bisect_right(starts, pc) - 1
        name = syms[i][2] if i >= 0 else "?"
        if i >= 0 and syms[i][1] and pc >= syms[i][0] + syms[i][1]:
            name = "?%08x" % pc
        funcs[name] += 1
        j = bisect.bisect_right(ostarts, pc) - 1
        if j >= 0 and pc < objs[j][0] + objs[j][1]:
            lib, _ = short_lib(objs[j][2])
        else:
            lib = "?"
        libs[lib] += 1

    total = len(samples)
    print("samples: %d (%.1f s at 511 Hz)" % (total, total / 511.0))
    print("\nby library:")
    for k, v in libs.most_common():
        print("  %5.1f%%  %s" % (100.0 * v / total, k))
    print("\ntop functions:")
    for k, v in funcs.most_common(40):
        print("  %5.1f%%  %s" % (100.0 * v / total, k))


if __name__ == "__main__":
    main()
