#!/usr/bin/env python3
"""
Build the patch-database index that both front-ends browse.

The database (bucanero/apollo-patches) is ~2200 .savepatch files whose game
name lives on the second line. Reading them all is fine at build time and
unreasonable at run time, so the index is generated here.

    build-index.py <apollo-patches-dir> <output> [--format json|tsv]

    json  for the web front-end, fetched by the page (default):
          {"generated": "...", "source": "...", "counts": {"PS3": 1799, ...},
           "patches": [["PS3", "BLUS30490", "3D Dot Game Heroes"], ...]}
          Rows are arrays rather than objects purely for size — it saves ~40%
          on a file every visitor who opens the browser downloads.

    tsv   for the desktop GUI, stored inside apollo-patches.zip as index.tsv:
          platform<TAB>title_id<TAB>name, one per line, after a "#" header.
          A line-oriented format so the GUI needs no JSON parser.
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


def write_json(fh, doc):
    json.dump(doc, fh, ensure_ascii=False, separators=(",", ":"))
    fh.write("\n")


def write_tsv(fh, doc):
    fh.write(f"# apollo-patches index\t{doc['generated']}\t{len(doc['patches'])}\n")
    for platform, title_id, name in doc["patches"]:
        # Tabs and newlines would break the format; neither occurs in the real
        # data, but a patch author could introduce one.
        clean = name.replace("\t", " ").replace("\r", "").replace("\n", " ")
        fh.write(f"{platform}\t{title_id}\t{clean}\n")


FORMATS = {"json": write_json, "tsv": write_tsv}


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = [a for a in argv[1:] if a.startswith("--")]

    fmt = "json"
    for flag in flags:
        if flag.startswith("--format="):
            fmt = flag.split("=", 1)[1]
        else:
            sys.exit(f"unknown option: {flag}")
    if fmt not in FORMATS:
        sys.exit(f"unknown format: {fmt} (want one of {', '.join(FORMATS)})")

    if len(args) != 2:
        sys.exit(f"usage: {os.path.basename(argv[0])} "
                 f"<apollo-patches-dir> <output> [--format=json|tsv]")

    root, out_path = args
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
        FORMATS[fmt](fh, doc)

    total = sum(counts.values())
    detail = " ".join(f"{k}:{v}" for k, v in counts.items())
    note = f", {skipped} without a game name" if skipped else ""
    print(f"{out_path}: {total} patches ({detail}){note}")


if __name__ == "__main__":
    main(sys.argv)
