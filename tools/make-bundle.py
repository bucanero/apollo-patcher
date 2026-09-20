#!/usr/bin/env python3
"""
Build apollo-patches.zip — the patch database the desktop GUI browses.

    make-bundle.py <apollo-patches-dir> <output.zip>

Contents:
    index.tsv           generated here; the GUI reads this one entry to build
                        its browsable list instead of inflating 2200 files
    PS2/ PS3/ ...       the .savepatch files themselves
    python/             helper modules Python patches import
    PSP/gamekeys.txt    the PSP game-key database, so the desktop app can
    PS3/games.conf      unwrap a PSP or PS3 save's console encryption offline
    titles.tsv          generated here; game names by title ID, for the saves
                        whose own PARAM.SFO does not carry one

The GUI reads the .savepatch entries straight out of the zip, but the Python
modules have to reach a real filesystem: MicroPython's import goes through
stat()/open() (see micropy_import_stat in apollo-lib), so the app extracts
python/ to a cache directory on first use. They travel in the same zip so there
is only ever one file to ship.

~2250 patches, 8.9MB of text, plus 300KB of console key databases; ~2.8MB
zipped.

This is Python rather than shell because the shell version needed `zip`,
`unzip`, `du` and a `python3` on PATH, and MSYS2 (the Windows CI job) does not
ship those by default. zipfile and sys.executable need nothing extra.
"""
import os
import subprocess
import sys
import zipfile

PLATFORMS = ["PS2", "PS3", "PS4", "PSP", "PSV"]

#
# Game names by title ID -- (platform, file in the checkout, separator).
#
# apollo-patches ships four of these catalogues; these are the two the save
# browser can use. A PS3 or PS4 save names its own game in PARAM.SFO and needs
# no catalogue, and a VITA SAVE NAMES NOTHING AT ALL -- no TITLE_ID key, and a
# TITLE that is usually empty -- so without this a Vita save is listed under
# whatever the patch database happens to know, which is 123 titles against the
# 4581 here.
#
# ps1titleid.txt and ps2titleid.txt are left out deliberately: nothing browses
# a PS1 or PS2 save yet, and the two of them are another 250KB in the zip. Add
# them to this list the day something does -- but note they are Windows-1252,
# not UTF-8 like these two, so they need `encoding=` changed below.
#
TITLE_DBS = [
    ("PSP", "psptitleid.txt", " "),
    ("PSV", "psvtitleid.txt", "|"),
]

# A fixed timestamp for every entry: zip records mtimes, and a git checkout
# stamps them with the checkout time, so without this every CI run would produce
# a different archive for identical content. The archive is still not quite
# byte-reproducible — index.tsv carries its own build timestamp, so exactly one
# of the 2268 entries varies between runs — but the other 2267 do not, which is
# what makes two bundles worth diffing.
FIXED_DATE = (2020, 1, 1, 0, 0, 0)


def collect(root):
    """Archive-relative paths to store, in a stable order."""
    names = []

    for platform in PLATFORMS:
        directory = os.path.join(root, platform)
        if not os.path.isdir(directory):
            continue
        names += [f"{platform}/{e}" for e in sorted(os.listdir(directory))
                  if e.endswith(".savepatch")]

    py_dir = os.path.join(root, "python")
    if os.path.isdir(py_dir):
        names += [f"python/{e}" for e in sorted(os.listdir(py_dir))
                  if e.endswith(".py")]

    # The console key databases: PSP game keys (16KB) and PS3 secure file ids
    # (280KB, ~80KB in the zip). They are the only way the desktop app can take
    # a console's own encryption off a save without a network — the web page
    # fetches the same files from the CDN. Neither has a .savepatch extension,
    # so the sweep above misses them and they are named explicitly.
    for key_db in ("PSP/gamekeys.txt", "PS3/games.conf"):
        if os.path.isfile(os.path.join(root, key_db)):
            names.append(key_db)

    return names


def build_titles(root, path):
    """Normalise the title-ID catalogues into one platform/id/name table.

    Three files, three different separators, and ids that are sometimes a
    physical release's product code rather than a title ID. Sorting that out
    here rather than in C means the C side reads one obvious format, and means
    a catalogue that changes shape upstream breaks the build rather than the
    app.
    """
    rows = {}
    kept = 0

    for platform, name, sep in TITLE_DBS:
        source = os.path.join(root, name)
        if not os.path.isfile(source):
            continue
        with open(source, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line or sep not in line:
                    continue
                title_id, title = line.split(sep, 1)
                title_id = title_id.strip().upper()
                # A tab or a newline inside a name would split the record the
                # reader is about to parse, so they go. Neither appears today;
                # the point is that the format's one guarantee stays true
                # whatever upstream does to these files.
                title = title.strip().replace("\t", " ").replace("\r", " ")
                # Savedata title IDs are always nine characters. The handful of
                # shorter ones in these files are product codes for physical
                # releases ("FVGK0097"), which no save is ever named after.
                if len(title_id) != 9 or not title:
                    continue
                # First wins. The few repeats are "(Limited Edition)" variants
                # of a game already listed, and the plain name is the better
                # one to show.
                if rows.setdefault((platform, title_id), title) == title:
                    kept += 1

    with open(path, "w", encoding="utf-8", newline="\n") as out:
        for (platform, title_id), title in sorted(rows.items()):
            out.write(f"{platform}\t{title_id}\t{title}\n")
    return len(rows)


def main(argv):
    if len(argv) != 3:
        sys.exit(f"usage: {os.path.basename(argv[0])} <apollo-patches-dir> <output.zip>")

    root, output = argv[1], argv[2]
    here = os.path.dirname(os.path.abspath(argv[0]))

    if not os.path.isdir(os.path.join(root, "python")):
        sys.exit(f"{root} does not look like an apollo-patches checkout\n"
                 "clone it: git clone https://github.com/bucanero/apollo-patches")

    out_dir = os.path.dirname(os.path.abspath(output))
    os.makedirs(out_dir, exist_ok=True)

    # Generated beside the output, never inside the checkout: the source tree
    # may be read-only, and a build should not leave anything behind in it.
    #
    # sys.executable, not "python3": the interpreter running this script is by
    # definition present, whatever it happens to be called here.
    index = os.path.join(out_dir, "index.tsv")
    subprocess.run([sys.executable, os.path.join(here, "build-index.py"),
                    root, index, "--format=tsv"], check=True)

    titles = os.path.join(out_dir, "titles.tsv")
    title_count = build_titles(root, titles)

    def store(zf, arcname, path):
        info = zipfile.ZipInfo(arcname, date_time=FIXED_DATE)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        with open(path, "rb") as fh:
            zf.writestr(info, fh.read())

    try:
        names = collect(root)
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            store(zf, "index.tsv", index)
            store(zf, "titles.tsv", titles)
            for name in names:
                store(zf, name, os.path.join(root, name))
    finally:
        for generated in (index, titles):
            if os.path.exists(generated):
                os.remove(generated)

    size_kb = (os.path.getsize(output) + 1023) // 1024
    print(f"{output}: {len(names) + 2} entries, {size_kb}KB "
          f"({title_count} game names)")


if __name__ == "__main__":
    main(sys.argv)
