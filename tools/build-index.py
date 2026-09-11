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

    tools  for the tool-collection front-end, which offers a decrypt /
          re-encrypt / fix-checksum action per game rather than a code list:
          {"generated": "...", "counts": {...},
           "tools": [["PS3", "BLUS30810", "Silent Hill: HD Collection",
                      "cde", "SAVEDATA.DAT"], ...]}
          The fourth field is what the patch can do — "d" decrypt, "e"
          re-encrypt, "c" checksum, "z" the patch uses offzip — the fifth is
          the file name(s) the user should supply, and the sixth is 1 when
          verify-tools.mjs has proved this patch against a real save.

          --verified=FILE  reads that proof (tools/verify-tools.mjs --out)
          --verified-only  emits only proven entries, for a release that
                           promises nothing it has not run

    modules  the names of the Python helper modules, for the web front-end:
          {"generated": "...", "modules": ["berseria.py", ...]}
          The page fetches these from a CDN on demand rather than carrying
          them in the wasm module, so it needs to know what to ask for. Tiny
          and separate from the patch index, which is 100x bigger and only
          loaded when someone opens the browser.
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


# Classifying a required code by its TITLE, because the body cannot do it
# alone: Resident Evil 2/4 Remake's decrypt code runs `encrypt blowfish_cbc`
# over its header as part of unwrapping it, so "contains an encrypt op" does
# not mean "is the encrypt step".
#
# A leading marker is stripped first ([PYTHON:...], DEFAULT:, and friends),
# and the checksum pattern is deliberately wide: the database spells that step
# "Update ADD", "Init SDBM", "Update XOR", "Update MD2", "Get EAChecksum",
# "Update csum" and plain "Update" among others. Across all 2240 patches this
# leaves exactly one title unclassified ("Read Encryption KEY.DAT", a setup
# step that loads a key into a variable), which is harmless: unclassified
# codes are still required, and still run in their place in the chain.
CODE_MARKER = re.compile(r"^\s*(?:\[)?(?:python|sw|bsd|default|info)\s*:\s*", re.I)
CODE_DECRYPT = re.compile(r"\b(decrypt|decode|decompress|unpack|extract|inflate|unzip)\b", re.I)
CODE_ENCRYPT = re.compile(r"\b(encrypt|encode|compress|repack|pack|deflate|zip)\b", re.I)
CODE_CHECKSUM = re.compile(
    r"(checksum|csum|\bcrc\w*|\bsha\w*|\bmd\d|\bhash|digest|adler\w*|fletcher\w*|murmur\w*"
    r"|\bdwadd|\bwadd|\bqwadd|\badd\b|\bxor\b|\bsdbm|\bfnv|\bjhash|\bdjb2|signature"
    r"|\bupdate\b|\bfix\b|\binit\b|\bcalculate\b|\bget\b)", re.I)

CODE_HEADING = re.compile(r"\[(.+)\]")
BODY_HAS_CRYPT = re.compile(r"(?im)^\s*(decrypt|encrypt)\s+\w")


def classify_code(title):
    """'d' decrypt, 'e' re-encrypt, 'c' checksum, or '' when it is neither."""
    title = CODE_MARKER.sub("", title).strip()
    if CODE_DECRYPT.search(title):
        return "d"
    if CODE_ENCRYPT.search(title):
        return "e"
    if CODE_CHECKSUM.search(title):
        return "c"
    return ""


def scan_tool_codes(path):
    """(kinds, files) for one patch, or (None, None) if it has no tool codes.

    A code counts when it is marked "(Required)" or carries a decrypt/encrypt
    op — the two ways the database says "this step is not optional".

    Deliberately NOT grouped by the patch's ':file' targets, for two reasons.
    The markers are not reliable: Metal Gear Solid 2 HD scopes its
    "Encrypt DATA.BIN" code under ':MASTER.BIN', and Call of Duty: Black Ops
    puts "Encrypt GPAD0_CM.PRF" under ':GPAD0_SP.PRF', so splitting on them
    invents 17 entries that can decrypt but never re-encrypt. And it would be
    splitting on something the front-end ignores anyway: it applies every
    selected code to the one file the user supplied, overriding the code's own
    target (apctl_apply's target_file argument). Per patch, every decrypt in
    the database has a matching encrypt.
    """
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
    except OSError:
        return None, None
    for encoding in ("utf-8", "cp1252"):
        try:
            text = raw.decode(encoding)
            break
        except UnicodeDecodeError:
            continue
    else:
        return None, None

    kinds, files = set(), []
    title, body, target = None, [], ""

    def take():
        if title is None:
            return
        joined = "\n".join(body)
        if "(required)" not in title.lower() and not BODY_HAS_CRYPT.search(joined):
            return
        kind = classify_code(title)
        if kind:
            kinds.add(kind)
        name = re.split(r"[\\/]", target)[-1].strip()
        if name and not name.lower().startswith("~extracted") and name not in files:
            files.append(name)

    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith(":") and not stripped.startswith("::"):
            target = stripped[1:].strip()
            if target.lower().startswith("~extracted"):
                kinds.add("z")
            continue
        heading = CODE_HEADING.fullmatch(stripped)
        if heading and not stripped.lower().startswith("[group"):
            take()
            title, body = heading.group(1), []
        elif title is not None:
            body.append(line)
    take()

    if not kinds - {"z"}:
        return None, None
    return "".join(sorted(kinds)), files


def write_json(fh, doc):
    json.dump(doc, fh, ensure_ascii=False, separators=(",", ":"))
    fh.write("\n")


def write_modules(fh, doc):
    json.dump({"generated": doc["generated"], "modules": doc["modules"]},
              fh, ensure_ascii=False, separators=(",", ":"))
    fh.write("\n")


def write_tools(fh, doc):
    json.dump({"generated": doc["generated"], "source": doc["source"],
               "counts": doc["tool_counts"], "tools": doc["tools"]},
              fh, ensure_ascii=False, separators=(",", ":"))
    fh.write("\n")


def write_tsv(fh, doc):
    fh.write(f"# apollo-patches index\t{doc['generated']}\t{len(doc['patches'])}\n")
    for platform, title_id, name in doc["patches"]:
        # Tabs and newlines would break the format; neither occurs in the real
        # data, but a patch author could introduce one.
        clean = name.replace("\t", " ").replace("\r", "").replace("\n", " ")
        fh.write(f"{platform}\t{title_id}\t{clean}\n")


FORMATS = {"json": write_json, "tsv": write_tsv, "modules": write_modules,
           "tools": write_tools}


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = [a for a in argv[1:] if a.startswith("--")]

    fmt = "json"
    verified_path, verified_only = None, False
    for flag in flags:
        if flag.startswith("--format="):
            fmt = flag.split("=", 1)[1]
        elif flag.startswith("--verified="):
            verified_path = flag.split("=", 1)[1]
        elif flag == "--verified-only":
            verified_only = True
        else:
            sys.exit(f"unknown option: {flag}")
    if verified_only and not verified_path:
        sys.exit("--verified-only needs --verified=FILE")
    if fmt not in FORMATS:
        sys.exit(f"unknown format: {fmt} (want one of {', '.join(FORMATS)})")

    if len(args) != 2:
        sys.exit(f"usage: {os.path.basename(argv[0])} "
                 f"<apollo-patches-dir> <output> [--format=json|tsv|modules|tools]")

    root, out_path = args
    if not os.path.isdir(root):
        sys.exit(f"not a directory: {root}")

    rows, counts, skipped = [], {}, 0
    tools, tool_counts = [], {}

    # Which patches have actually been run against a real save. Nothing else
    # in this script can know that — it reads text, it does not decrypt
    # anything — so the claim comes from tools/verify-tools.mjs, which drives
    # the real engine and compares bytes.
    verified = set()
    if verified_path:
        with open(verified_path, encoding="utf-8") as fh:
            for entry in json.load(fh)["verified"]:
                verified.add((entry[0], entry[1]))
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

            # Only the tools catalog needs the file bodies; the other formats
            # get away with the second line, so do not read 2240 files twice.
            if fmt == "tools":
                kinds, files = scan_tool_codes(os.path.join(directory, entry))
                is_verified = (platform, title_id) in verified
                if kinds and (is_verified or not verified_only):
                    tools.append([platform, title_id, name, kinds,
                                  ",".join(files), 1 if is_verified else 0])
                    tool_counts[platform] = tool_counts.get(platform, 0) + 1

        if found:
            counts[platform] = found

    # Sorted by platform (in PLATFORMS order), then name, then id — so the
    # artifact is byte-reproducible from the same checkout.
    rows.sort(key=lambda r: (PLATFORMS.index(r[0]), r[2].lower(), r[1]))
    tools.sort(key=lambda r: (PLATFORMS.index(r[0]), r[2].lower(), r[1]))

    py_dir = os.path.join(root, "python")
    modules = sorted(e for e in os.listdir(py_dir) if e.endswith(".py")) \
        if os.path.isdir(py_dir) else []

    doc = {
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": "bucanero/apollo-patches",
        "counts": counts,
        "patches": rows,
        "modules": modules,
    }

    # Only for the tools format: write_json() dumps the whole document, so
    # adding these unconditionally would put empty "tools"/"tool_counts" keys
    # into patches.json for no reason.
    if fmt == "tools":
        doc["tools"] = tools
        doc["tool_counts"] = tool_counts

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as fh:
        FORMATS[fmt](fh, doc)

    if fmt == "modules":
        print(f"{out_path}: {len(modules)} python modules")
    elif fmt == "tools":
        detail = " ".join(f"{k}:{v}" for k, v in tool_counts.items())
        crypt = sum(1 for t in tools if "d" in t[3])
        nver = sum(1 for t in tools if t[5])
        note = " (verified only)" if verified_only else f", {nver} verified"
        print(f"{out_path}: {len(tools)} tools ({detail}); "
              f"{crypt} decrypt/re-encrypt, {len(tools) - crypt} checksum-only{note}")
    else:
        total = sum(counts.values())
        detail = " ".join(f"{k}:{v}" for k, v in counts.items())
        note = f", {skipped} without a game name" if skipped else ""
        print(f"{out_path}: {total} patches ({detail}){note}")


if __name__ == "__main__":
    main(sys.argv)
