#!/bin/sh
#
# Build apollo-patches.zip — the patch database the desktop GUI browses.
#
#   make-bundle.sh <apollo-patches-dir> <output.zip>
#
# Contents:
#   index.tsv           generated here; the GUI reads this one entry to build
#                       its browsable list instead of inflating 2200 files
#   PS2/ PS3/ ...       the .savepatch files themselves
#   python/             helper modules Python patches import
#
# The GUI reads the .savepatch entries straight out of the zip, but the Python
# modules have to reach a real filesystem: MicroPython's import goes through
# stat()/open() (see micropy_import_stat in apollo-lib), so the app extracts
# python/ to a cache directory on first use. They travel in the same zip so
# there is only ever one file to ship.
#
# ~2240 patches, 9MB of text, ~2.8MB zipped.
set -eu

if [ $# -ne 2 ]; then
    echo "usage: $(basename "$0") <apollo-patches-dir> <output.zip>" >&2
    exit 2
fi

patches=$1
output=$2
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ ! -d "$patches/python" ]; then
    echo "$patches does not look like an apollo-patches checkout" >&2
    echo "clone it: git clone https://github.com/bucanero/apollo-patches" >&2
    exit 2
fi

command -v zip >/dev/null 2>&1 || { echo "zip is not installed" >&2; exit 2; }

# Absolute, so the `cd` below cannot break it.
case $output in
    /*) out_abs=$output ;;
    *)  out_abs=$(pwd)/$output ;;
esac
mkdir -p "$(dirname "$out_abs")"

python3 "$here/build-index.py" "$patches" "$patches/index.tsv" --format=tsv

# Built from inside the checkout so entry names are "PS3/BLUS30490.savepatch"
# rather than carrying the checkout path. -X drops extra file attributes, which
# keeps the archive reproducible across machines.
rm -f "$out_abs"
( cd "$patches" && zip -q -r -9 -X "$out_abs" \
    index.tsv \
    PS2 PS3 PS4 PSP PSV python \
    -i 'index.tsv' '*.savepatch' 'python/*.py' )

rm -f "$patches/index.tsv"

size=$(du -k "$out_abs" | cut -f1)
count=$(unzip -l "$out_abs" | tail -1 | awk '{print $2}')
echo "$output: $count entries, ${size}KB"
