#!/usr/bin/env python3
"""
Build apollo-patches.zip — the patch database the desktop GUI browses.

    make-bundle.py <apollo-patches-dir> <output.zip>

Contents:
    index.tsv       generated here; the GUI reads this one entry to build its
                    browsable list instead of inflating 2200 files
    PS2/ PS3/ ...   the .savepatch files themselves
    python/         helper modules Python patches import

The GUI reads the .savepatch entries straight out of the zip, but the Python
modules have to reach a real filesystem: MicroPython's import goes through
stat()/open() (see micropy_import_stat in apollo-lib), so the app extracts
python/ to a cache directory on first use. They travel in the same zip so there
is only ever one file to ship.

~2240 patches, 9MB of text, ~2.8MB zipped.

This is Python rather than shell because the shell version needed `zip`,
`unzip`, `du` and a `python3` on PATH, and MSYS2 (the Windows CI job) does not
ship those by default. zipfile and sys.executable need nothing extra.
"""
import os
import subprocess
import sys
import zipfile

PLATFORMS = ["PS2", "PS3", "PS4", "PSP", "PSV"]

# A fixed timestamp for every entry: zip records mtimes, and a git checkout
# stamps them with the checkout time, so without this every CI run would produce
# a different archive for identical content. The archive is still not quite
# byte-reproducible — index.tsv carries its own build timestamp, so exactly one
# of the 2259 entries varies between runs — but the other 2258 do not, which is
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

    return names


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
            for name in names:
                store(zf, name, os.path.join(root, name))
    finally:
        if os.path.exists(index):
            os.remove(index)

    size_kb = (os.path.getsize(output) + 1023) // 1024
    print(f"{output}: {len(names) + 1} entries, {size_kb}KB")


if __name__ == "__main__":
    main(sys.argv)
