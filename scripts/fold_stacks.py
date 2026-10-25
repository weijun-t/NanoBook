"""fold_stacks.py: turns perf samples into the "folded stacks" a flame graph is drawn from.

What it does: Reads the text of `perf script -F ip,dso --show-mmap-events` (recorded with
  --call-graph fp) and the binary that was profiled, and writes one line per distinct stack:
  "outermost;...;innermost COUNT".
  Usage: python3 -I scripts/fold_stacks.py PERF_SCRIPT.txt BINARY --out FOLDED.txt

Why it exists: A flame graph needs each sample's chain of functions, but in a -O3 build most
  functions are inlined, so the real call stack holds only the few that weren't. The inlined
  ones are known only to the debug info. perf can't read that on WSL2: its perf is built without
  libelf or libdw (`perf version --build-options`), so it can't name any function. addr2line
  reads the debug info itself, so this works the same on WSL2 and on bare metal (polish design
  §6.1). Phase 6's profile used the same idea for the leaf address only.

How it works:
  - Each sample is a block of addresses, leaf first, then each caller's return address (the
    frame-pointer walk the kernel did when the sample was taken); a blank line ends a block.
  - The mmap events say where each file's code was loaded: [start(length) @ file offset].
    An address becomes a file offset (ip - start + offset), then an address in the binary
    (offset - segment offset + segment address, from `readelf -lW`'s executable LOAD).
  - A return address points just after its call, possibly at the next source line, so callers
    are looked up at address - 1 (inside the call instruction); the leaf as it is.
  - Stacks are counted first (a Counter), and addr2line runs once over the distinct addresses
    of all of them: O(samples x stack depth) to read, plus one addr2line pass.
  - `addr2line -f -i -C -a` prints each address's inlined frames, innermost first; reversed,
    they replace that one frame, so a stack reads main -> ... -> the innermost inlined function.
  - Names are shortened to Class::function (return types, template arguments, parameter
    lists, clone suffixes and the nanobook/std namespaces dropped) so the boxes are readable. Frames in other files become one
    box per file ([libc.so.6], mostly memmove); kernel addresses one [kernel] box; garbage
    frames ([unknown], from a function without a frame pointer) are dropped.

Data structures: Counter {stack as a tuple of (address, file): samples}; dict {address: frames}
  from addr2line; Counter {folded stack: samples} for the output.

Trade-offs: Frame-pointer stacks need the binary built with -fno-omit-frame-pointer (one register
  fewer, a push and pop per call): flamegraph.sh reports how many instructions that changed.
  A function without a frame pointer (a libc leaf such as memmove) hides its direct caller:
  the box above it is its caller's caller.
"""

import argparse
import re
import subprocess
from collections import Counter
from pathlib import Path

MMAP = re.compile(r"PERF_RECORD_MMAP2? .*\[0x([0-9a-f]+)\(0x([0-9a-f]+)\) @ (0x[0-9a-f]+|0) .*\]: "
                  r"\S+ (.+)$")
SAMPLE = re.compile(r"^\s*([0-9a-f]+) \((.*)\)$")
KERNEL_START = 0xFFFF_8000_0000_0000
# C++ operator symbols, longest first, so "operator<=><char>" is read as "operator<=>".
OPERATORS = sorted("<=> () [] <<= >>= ->* << >> == != <= >= && || ++ -- += -= *= /= %= &= |= ^= -> "
                   "< > = ! + - * / % & | ^ ~ ,".split(), key=len, reverse=True)
DROPPED_NAMESPACES = re.compile(r"\b(nanobook|fast|itch|pipeline|support|detail|std)::")


def executable_segment(binary: Path) -> tuple[int, int]:
    """(file offset, virtual address) of the binary's executable LOAD segment."""
    out = subprocess.run(["readelf", "-lW", str(binary)], capture_output=True, text=True,
                         check=True).stdout
    for line in out.splitlines():
        fields = line.split()
        # LOAD Offset VirtAddr PhysAddr FileSiz MemSiz Flags... Align, flags like "R E"
        if fields and fields[0] == "LOAD" and "E" in fields[6:-1]:
            return int(fields[1], 16), int(fields[2], 16)
    raise SystemExit(f"no executable LOAD segment in {binary}")


def strip_brackets(name: str, open_: str, close: str) -> str:
    """Removes every balanced open_ ... close group (nested ones too)."""
    out, depth = [], 0
    for ch in name:
        if ch == open_:
            depth += 1
        elif ch == close and depth:
            depth -= 1
        elif not depth:
            out.append(ch)
    return "".join(out)


def short_name(name: str) -> str:
    """'nanobook::fast::IdTable<8u>::find(nanobook::OrderId) const' -> 'IdTable::find'."""
    if name == "??":
        return "[unknown]"
    at = name.find("::operator")
    if at >= 0:  # 'ns::OrderPool::operator[](unsigned int)' -> 'OrderPool::operator[]'
        rest = name[at + 2 + len("operator"):]
        op = "operator" + next((o for o in OPERATORS if rest.startswith(o)), rest.split("(")[0])
        owner = short_name(name[:at]).split("::")
        owner = owner[-2:] if owner[-1].startswith("{") else owner[-1:]  # a lambda keeps its owner
        return "::".join([*owner, op]).replace(";", ",")
    for open_, close in ("<>", "()", "[]"):  # templates, parameters, [clone .isra.0]
        name = strip_brackets(name, open_, close)
    name = name.replace(" const", "").strip().split(" ")[-1]  # drop a return type
    parts = [p for p in DROPPED_NAMESPACES.sub("", name).split("::") if p]
    return "::".join(parts[-2:]).replace(";", ",") or "[unknown]"


def inline_chains(binary: Path, addresses: list[int]) -> dict[int, list[str]]:
    """{address: [outermost frame, ..., innermost frame]} via one addr2line process."""
    text = "\n".join(f"{a:x}" for a in addresses) + "\n"
    out = subprocess.run(["addr2line", "-f", "-i", "-C", "-a", "-e", str(binary)], input=text,
                         capture_output=True, text=True, check=True).stdout.splitlines()
    chains, current, i = {}, None, 0
    while i < len(out):
        line = out[i]
        if line.startswith("0x"):  # "-a": each address starts its block
            current = int(line, 16)
            chains[current] = []
            i += 1
            continue
        chains[current].append(short_name(line))  # function line; the next is file:line
        i += 2
    return {a: list(reversed(frames)) for a, frames in chains.items()}


def read_samples(path: Path) -> tuple[dict, Counter]:
    """The executable mappings {file: (start, length, offset)}, and Counter {stack: samples}."""
    maps, stacks, current = {}, Counter(), []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = MMAP.search(line)
            if m:
                if "x" in line.split("]: ")[1].split()[0]:  # executable mappings only
                    maps[m.group(4).strip()] = (int(m.group(1), 16), int(m.group(2), 16),
                                                int(m.group(3), 16))
                continue
            s = SAMPLE.match(line)
            if s:
                current.append((int(s.group(1), 16), s.group(2)))
            elif not line.strip() and current:
                stacks[tuple(current)] += 1
                current = []
    if current:
        stacks[tuple(current)] += 1
    return maps, stacks


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("perf_script", type=Path)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    binary = args.binary.resolve()
    seg_offset, seg_vaddr = executable_segment(binary)
    maps, stacks = read_samples(args.perf_script)
    own = next((name for name in maps if Path(name).resolve() == binary), None)

    def in_binary(ip: int, dso: str, leaf: bool) -> int | None:
        """The address inside the binary to look up, or None if the frame is elsewhere."""
        if dso != own:
            return None
        start, _, offset = maps[own]
        return ip - start + offset - seg_offset + seg_vaddr - (0 if leaf else 1)

    wanted = {a for stack in stacks for i, (ip, dso) in enumerate(stack)
              if (a := in_binary(ip, dso, i == 0)) is not None}
    chains = inline_chains(binary, sorted(wanted))

    folded, own_samples = Counter(), 0
    for stack, count in stacks.items():
        names = []
        for i in range(len(stack) - 1, -1, -1):  # outermost caller first
            ip, dso = stack[i]
            address = in_binary(ip, dso, i == 0)
            if address is not None:
                frames = chains[address]
            elif ip >= KERNEL_START or "kernel" in dso:
                frames = ["[kernel]"]
            elif dso in ("", "[unknown]"):
                frames = []  # a broken frame-pointer link, not a real caller
            else:
                frames = [f"[{Path(dso).name}]"]
            for frame in frames:
                if not (names and frame.startswith("[") and names[-1] == frame):
                    names.append(frame)  # one box for a run of kernel or library frames
        if names:
            folded[";".join(names)] += count
            own_samples += count if in_binary(*stack[0], True) is not None else 0
    total = sum(folded.values())
    if not total:
        raise SystemExit("no samples found: is this `perf script -F ip,dso --show-mmap-events`?")
    with open(args.out, "w", encoding="utf-8") as f:
        for stack, count in sorted(folded.items()):
            f.write(f"{stack} {count}\n")
    print(f"{total:,} samples, {own_samples / total * 100:.1f}% in {binary.name}, "
          f"{len(wanted):,} distinct addresses, {len(folded):,} stacks -> {args.out}")


if __name__ == "__main__":
    main()
