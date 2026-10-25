#!/usr/bin/env bash
# fetch_itch.sh: downloads and unpacks the Nasdaq day that every published replay number uses.
#
# What it does: Downloads 01302019.NASDAQ_ITCH50.gz (Wednesday 30 January 2019, 4.76 GB) from
#   Nasdaq's public sample site, checks its size, and unpacks it to
#   data/itch/01302019.NASDAQ_ITCH50/01302019.NASDAQ_ITCH50 (11.2 GB). Then it prints the
#   command that replays it.
#     scripts/fetch_itch.sh
#
# Why it exists: The "full" level of reproducing (polish design §0.2, §3.2) needs the same day
#   as the published numbers; this makes getting it one command. The file isn't in git: it's
#   far too big, and it's Nasdaq's to publish, not ours (.gitignore ignores /data/).
#
# How it works:
#   - Checks for 17 GB of free space first (4.8 GB download + 11.2 GB unpacked + margin).
#   - `curl -C -` resumes a broken download instead of starting again (the server accepts
#     byte ranges, checked 2026-10-25).
#   - Compares the size with the one Nasdaq's server reports for this file (4,764,426,091
#     bytes, docs/itch-design.md §2). Nasdaq publishes no checksum, so size is the check.
#   - Unpacks to a temporary name and renames at the end, so a half-unpacked file never sits
#     under the real name. The .gz is kept, so a damaged unpack can be redone offline.
#   - Skips any step already done, so running it twice is safe.
#
# Trade-offs: The download time depends on the reader's internet (about 6 minutes at 100 Mbit/s);
#   unpacking takes about a minute. 16 GB of disk while both files exist; delete the .gz after.

set -u

NAME=01302019.NASDAQ_ITCH50
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/$NAME.gz"
SIZE=4764426091

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR=$ROOT/data/itch
GZ=$DIR/$NAME.gz
FILE=$DIR/$NAME/$NAME
mkdir -p "$DIR/$NAME" || exit 1

if [ -f "$FILE" ]; then
    echo "already there: $FILE"
else
    free_gb=$(df --output=avail -BG "$DIR" | tail -1 | tr -dc 0-9)
    if [ "$free_gb" -lt 17 ]; then
        echo "need about 17 GB free in $DIR, have ${free_gb} GB" >&2
        exit 1
    fi
    if [ "$(stat -c %s "$GZ" 2> /dev/null || echo 0)" != "$SIZE" ]; then
        echo "downloading $URL (4.76 GB)"
        curl -fL -C - --retry 5 -o "$GZ" "$URL" || { echo "download failed (run again to resume)" >&2; exit 1; }
    fi
    got=$(stat -c %s "$GZ")
    if [ "$got" != "$SIZE" ]; then
        echo "wrong size: $got bytes, expected $SIZE (delete $GZ and run again)" >&2
        exit 1
    fi
    echo "unpacking to $FILE (11.2 GB)"
    gunzip -c "$GZ" > "$FILE.part" || { echo "unpack failed" >&2; rm -f "$FILE.part"; exit 1; }
    mv "$FILE.part" "$FILE"
fi

echo "next: scripts/reproduce.sh --itch $FILE"
