#!/usr/bin/env python3
"""
Build the patch-database index the web front-end browses.

The database (bucanero/apollo-patches) is ~2200 .savepatch files whose game
name lives on the second line. Reading 2200 files is fine at build time and
impossible from a browser, so the index is generated here and shipped in dist/;
the page then fetches individual patches from a CDN on demand.

    build-index.py <apollo-patches-dir> <output.json>

Output is a compact, deterministically ordered document:

    {"generated": "...", "source": "...", "counts": {"PS3": 1799, ...},
     "patches": [["PS3", "BLUS30490", "3D Dot Game Heroes"], ...]}

Rows are arrays rather than objects purely for size: it saves ~40% on a file
every visitor who opens the browser downloads.
"""
import json
import os
import re
import sys
from datetime import datetime, timezone

PLATFORMS = ["PS3", "PS4", "PSV", "PSP", "PS2"]

# Some names carry a redundant platform tag ("PS4 Grand Theft Auto V") — the UI
# shows the platform in its own column, so drop it.
PLATFORM_TAG = re.compile(r"^(PS2|PS3|PS4|PSP|PSV|PS Vita|VITA)\b[\s:-]*", re.I)


def read_game_name(path):
    """Game name from the second line, or None if the file has no usable one.

    Two things to be careful about, both observed in the real database:
      - 245 files are Windows-1252, not UTF-8 (game names with ™ / ®), so a
        strict UTF-8 decode would drop them.
      - the leading ';' is a convention, not a guarantee, so strip it when
        present rather than requiring it.
    """
    try:
        with open(path, "rb") as fh:
            lines = fh.read(4096).split(b"\n")
    except OSError:
        return None
    if len(lines) < 2:
        return None

    raw = lines[1]
    for encoding in ("utf-8", "cp1252"):
        try:
            text = raw.decode(encoding)
            break
        except UnicodeDecodeError:
            continue
    else:
        return None

    name = text.strip().lstrip(";").strip()
    name = PLATFORM_TAG.sub("", name).strip()
    return name or None


def main(argv):
    if len(argv) != 3:
        sys.exit(f"usage: {os.path.basename(argv[0])} <apollo-patches-dir> <output.json>")

    root, out_path = argv[1], argv[2]
    if not os.path.isdir(root):
        sys.exit(f"not a directory: {root}")

    rows, counts, skipped = [], {}, 0
    for platform in PLATFORMS:
        directory = os.path.join(root, platform)
        if not os.path.isdir(directory):
            continue

        found = 0
        for entry in sorted(os.listdir(directory)):
            if not entry.endswith(".savepatch"):
                continue
            title_id = entry[: -len(".savepatch")]
            name = read_game_name(os.path.join(directory, entry))
            if not name:
                # Keep it listed: the title ID alone is still enough to find
                # and apply the patch.
                name = title_id
                skipped += 1
            rows.append([platform, title_id, name])
            found += 1

        if found:
            counts[platform] = found

    # Sorted by platform (in PLATFORMS order), then name, then id — so the
    # artifact is byte-reproducible from the same checkout.
    rows.sort(key=lambda r: (PLATFORMS.index(r[0]), r[2].lower(), r[1]))

    doc = {
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": "bucanero/apollo-patches",
        "counts": counts,
        "patches": rows,
    }

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, separators=(",", ":"))
        fh.write("\n")

    total = sum(counts.values())
    detail = " ".join(f"{k}:{v}" for k, v in counts.items())
    note = f", {skipped} without a game name" if skipped else ""
    print(f"{out_path}: {total} patches ({detail}){note}")


if __name__ == "__main__":
    main(sys.argv)
