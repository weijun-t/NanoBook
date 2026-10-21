"""itch_slice.py: copies an ITCH 5.0 day from its start up to a chosen time of day.

What it does: Writes every message of ITCH_FILE before the first one stamped at or after --until
  (HH:MM:SS) to OUT, and prints how many messages and bytes that is and the last one's time.
  Usage: python3 scripts/itch_slice.py ITCH_FILE OUT --until 12:00:00

Why it exists: Profile-guided optimisation must not train on the data it is measured on
  (optimise design E4): scripts/build_pgo.sh trains on the day up to 12:00 and the measurement
  replays the whole day. A slice from the start, not from the middle, keeps every stock directory
  message and every order the later messages refer to, so the books build exactly as on the day.

How it works: Walks the file message by message through a memory map: each message is a 2-byte
  big-endian length, then the message; its time (nanoseconds since midnight) is the 6-byte
  big-endian field 5 bytes into the message (after type, locate and tracking number). Stops at
  the first time >= --until and copies the bytes before it in one write. O(messages); about two
  minutes for the morning of a full day (pure Python, a one-off).

Trade-offs: Messages are in time order on the feed, so the first late one ends the slice. A
  truncated last message ends the walk too.
"""

import argparse
import mmap
import sys
from pathlib import Path


def nanoseconds(hhmmss: str) -> int:
    hours, minutes, seconds = (int(part) for part in hhmmss.split(":"))
    return ((hours * 60 + minutes) * 60 + seconds) * 1_000_000_000


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("itch_file", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--until", required=True, help="HH:MM:SS, exclusive")
    args = parser.parse_args()
    until = nanoseconds(args.until)

    with open(args.itch_file, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        size = len(m)
        pos = 0
        count = 0
        last = 0
        while pos + 2 <= size:
            length = (m[pos] << 8) | m[pos + 1]
            if pos + 2 + length > size or length < 11:
                break  # truncated (or not a stamped message): the file's end
            stamp = int.from_bytes(m[pos + 7 : pos + 13], "big")
            if stamp >= until:
                break
            last = stamp
            pos += 2 + length
            count += 1
        args.out.write_bytes(m[:pos])

    seconds = last // 1_000_000_000
    print(f"wrote {args.out}: {count:,} messages, {pos:,} bytes, last at "
          f"{seconds // 3600:02}:{seconds // 60 % 60:02}:{seconds % 60:02}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
