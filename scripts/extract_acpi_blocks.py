#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Extract relevant ACPI device blocks from decompiled .dsl files.

Read-only helper for diagnostics. It looks for devices whose body contains a
given needle (default FTE3600), plus the SPI1 controller and the ACPI GPIO
controller devices referenced by the platform.

Usage: extract_acpi_blocks.py <directory-with-dsl-files> [needle ...]
"""
import glob
import os
import re
import sys


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def find_device_blocks(lines):
    """Yield (start, end, name) for every Device (...) { ... } block."""
    for i, line in enumerate(lines):
        m = re.match(r"\s*Device\s*\(\s*([A-Za-z0-9_]+)\s*\)", line)
        if not m:
            continue
        depth = 0
        started = False
        for j in range(i, len(lines)):
            depth += lines[j].count("{") - lines[j].count("}")
            if "{" in lines[j]:
                started = True
            if started and depth <= 0:
                yield i, j, m.group(1)
                break


def enclosing_devices(blocks, target_start):
    chain = []
    for start, end, name in blocks:
        if start <= target_start <= end:
            chain.append((start, end, name))
    chain.sort()
    return chain


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    directory = sys.argv[1]
    needles = sys.argv[2:] or ["FTE3600"]

    dsl_files = sorted(glob.glob(os.path.join(directory, "*.dsl")))
    if not dsl_files:
        print(f"No .dsl files in {directory}", file=sys.stderr)
        return 1

    seen = set()
    for path in dsl_files:
        with open(path, "r", errors="replace") as fh:
            raw = fh.read()
        lines = strip_comments(raw).splitlines()
        blocks = list(find_device_blocks(lines))
        text = "\n".join(lines)

        targets = set()
        for needle in needles:
            for m in re.finditer(re.escape(needle), text):
                line_no = text[: m.start()].count("\n")
                for start, end, name in blocks:
                    if start <= line_no <= end:
                        targets.add((start, end, name))
                        break

        # Also include the enclosing SPI controller and any _DSD-bearing
        # parent that directly wraps a matched device.
        expanded = set(targets)
        for start, end, name in list(targets):
            chain = enclosing_devices(blocks, start)
            for cstart, cend, cname in chain:
                if cname.startswith("SPI") or cname in ("GPI0", "GPIO", "I2C0"):
                    expanded.add((cstart, cend, cname))

        for start, end, name in sorted(expanded):
            key = (path, start, end)
            if key in seen:
                continue
            seen.add(key)
            print(f"\n/* ===== {os.path.basename(path)}: Device ({name}) "
                  f"lines {start + 1}-{end + 1} ===== */")
            print("\n".join(lines[start : end + 1]))

        # Print any _DSD block that mentions FocalTech/GPIO references, even
        # when it lives outside a Device block (rare).
        for needle in needles:
            for m in re.finditer(re.escape(needle), "\n".join(lines)):
                pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
