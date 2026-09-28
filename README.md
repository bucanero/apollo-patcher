# Apollo Save Patcher

Desktop and web front-ends for the [Apollo save-patch engine](https://github.com/bucanero/apollo-lib).

Both apply Apollo `.savepatch` files — Save Wizard codes, BSD scripts and Python
scripts — to save data on a computer. Neither reimplements any patch logic: they
drive the same `libapollo` engine the console apps use.

| Front-end | What it is |
|-----------|------------|
| [`gui/`](gui/README.md) | Native desktop app (Dear ImGui + GLFW) for Windows, macOS and Linux — **opens on your saves**: point it at a folder, pick a game by name, patch it. The patch database is bundled offline |
| [`web/`](web/README.md) | The engine compiled to WebAssembly, running in a browser tab — the patcher, with the patch database searchable in-page and **PSP and PS3 savedata panels** for the consoles' own encryption, plus a **tools page** offering one decrypt / re-encrypt pair per game |

The command-line tools (`patcher`, `dumper`) and the engine itself live in
[apollo-lib](https://github.com/bucanero/apollo-lib).

## The consoles' own savedata encryption

A PSP or PS3 save is encrypted **twice**. The console wraps it with a key of its
own before the game's encryption is anywhere in the picture, and every patch in
the database addresses only the inner layer. So the console's wrapper has to
come off before any patch means anything: feed the engine a file copied straight
off a Memory Stick or a hard drive and it returns noise that looks like output.

Both layers live under `core/`, reworked into buffer APIs with no stdio because
there is no filesystem in a browser tab, and both parse their metadata
bounds-checked against the length they are handed rather than trusting the
offsets inside the file — the difference between a save off your own console and
one a stranger put on the web. Between them they need only mbedTLS's AES and
SHA1, which both front-ends already link, so they ride in the existing wasm
module rather than separate ones. Measured: the module is 317KB gzipped with
neither, 331KB with the PSP's, 341KB with both.

Both front-ends reach both layers, and they reach them differently because one
of them can look around. The web page asks for the metadata file and fetches the
key database from the CDN; the desktop app has the save folder, so it *detects*
the whole thing from the target you pick and reads the same database out of
`apollo-patches.zip`, offline. The lookup rules live in one C function each
front-end calls, because they are load-bearing in ways a second implementation
would get wrong.

### PSP — `core/psp/`

Vendored from [apollo-psp](https://github.com/bucanero/apollo-psp)
(`kirk_engine.c` and `psp_decrypter.c`, at `17cb5ea`). `PARAM.SFO` says which
files are wrapped and which of the console's modes was used; the per-title game
key comes from apollo-patches' `PSP/gamekeys.txt`, matched by prefix against the
save directory with the **longest entry winning** — the database holds both
`NPJJ30022` and `NPJJ30022GAME1`, with different keys.

`core/test_psp.c` pins it to the unmodified upstream implementation: the
known-answer digests come from apollo-psp's own code compiled for the host, so a
pass says the vendored copy is byte-faithful to what ships on the console rather
than merely self-consistent. The whole chain has been checked against a real
console save — the plaintext it produces matches what the reference Monster
Hunter decrypter validates by the game's own stored SHA-1.

### PS3 — `core/ps3/`

Derived from flatz's `pfdtool`, by way of
[pfd_sfo_tools](https://github.com/bucanero/pfd_sfo_tools) and
[apollo-ps3](https://github.com/bucanero/apollo-ps3). Those two are the same
file: apollo-ps3's copy swaps polarSSL for mbedTLS and drops every byte swap,
because the PS3 is big-endian and so is the format. This one runs little-endian
and links mbedTLS, so it takes the swaps from the first and the crypto calls
from the second.

The shape is not upstream's. `pfd_init()` takes a directory and rehashes every
file in it; a browser has no directory, and a patcher does not need one, because
only the file it just changed needs rehashing. So the API here is one file at a
time.

`PARAM.PFD` lists the protected files — and that list is the **authoritative**
answer to what is encrypted, better than any key database: a game that encrypts
nothing ships a PFD holding only `PARAM.SFO`. The per-file secure ID comes from
apollo-patches' `PS3/games.conf`, whose sections are keyed by **save directory**
names rather than title IDs, with the longest prefix winning and the first file
pattern in file order taking precedence. Both rules matter: DiRT 3 files
`BLUS30724` and `BLUS30724PROFILE` separately with different keys, and Devil May
Cry lists `DATA` before `*`.

`core/test_ps3.c` cross-checks against pfdtool itself — a separate
implementation with its own polarSSL, which reads the synthetic save this test
writes, reports every hash OK, and re-encrypts it to byte-identical output
including the `PARAM.PFD`. Its `--corpus` mode then walks a tree of real saves:
over 180 of them, every one parses, every one **re-signs to itself byte for
byte**, and every protected file's ciphertext round-trips.

### PS1 and PS2 — `core/psvcard.c`, `core/ps2/`

Not encryption. The opposite problem: neither console encrypted a save, and
neither produced a **file**. A PS1 or PS2 save lived in blocks on a memory card,
so there was nothing to copy off — which is why a scanner looking for a folder
with a `PARAM.SFO` in it has never seen one.

A save becomes a file when a PS3 exports it as a signed `.PSV` carrying the
save's whole memory-card directory. That container is what this reads and
writes, derived from
[apollo-ps4](https://github.com/bucanero/apollo-ps4)'s `psv_resign.c` and
`psv_ps2.c` — themselves `ps3-psvresigner` by @dots_tb, with the CBPS group.

The container is signed with HMAC-SHA1 under a key derived from the file's own
salt by AES-128, on a different schedule per console. So patching one means
extracting the file a code addresses, patching that, putting it back and
**re-signing** — a PS3 checks the signature on import, and a save that fails it
looks corrupt rather than merely unsigned.

The rebuild moves what comes after the edited file rather than laying the
container out afresh, which is not pedantry: of 2,641 real PS2 containers, two
leave a gap between files and nine carry a `displaySize` that is not the sum of
their contents. Recomputing both would quietly rewrite eleven saves it was only
asked to patch.

Two things that follow from the format and cost more than they look:

- **The title ID comes out of the container, and the container is right.** It
  can disagree with the folder a save was filed under: 2,646 of 2,647 agree, and
  the one that does not is a Final Fantasy Chronicles save, a two-in-one disc
  whose halves carry different IDs (`SLUS-01360` and `SLUS-01363`).
- **The icon has to be drawn.** Every other console here ships a PNG; a PS1
  save's icon is sixteen colours packed into its own first block, and a PS2
  save's is a textured **3D model** lit by parameters in `icon.sys`.
  `core/ps2/` is apollo-ps4's parser and software rasteriser, ported — no GL
  context, so it works in the wasm build too.

`core/test_psv.c` walks a tree of real containers. Over the 2,647 in
apollo-saves every one parses, every signature verifies, every one **rebuilds
byte for byte**, and every one rebuilds correctly with a file actually changed.
Its `--patch` mode runs the whole chain — extract, apply real codes from the
database, put back, re-sign — and checks that every *other* file in the
container came through untouched.

Note that `.PSV` here is a **file format holding a PS1 or PS2 save**, and has
nothing to do with `PSV` the platform tag, which is the PS Vita. The two share
four letters and nothing else, and the ambiguity is the kind that compiles, so
the code refuses to rely on context:

- the console's enumerator is **`ASAVE_PSVITA`**, spelled out, never `ASAVE_PSV`
- everything about the container is **`apsvc_`/`psvcard`**, never plain `psv`

Two things keep the bare spelling because they name the file format itself
rather than either concept: the `.psv` extension, and `asave_platform_name()`,
which still answers `"PSV"` for the Vita — that string is the key the patch
database is sorted by, so it lives on disk and cannot be renamed.

### Which console a save is written *for*

Both front-ends have a **Settings** panel naming the console a save is written
back for. Both values are optional, both change only what is WRITTEN, and
leaving them blank keeps whatever a save already says — which is what patching
one in place wants.

- **PSP, Fuse ID.** Savedata modes 4 and 6 derive two `PARAM.SFO` hashes from
  the console's own fuse. A PSP loads a save whose values differ, so this only
  matters for reproducing one console's output byte for byte.

PS1 and PS2 have no such setting, and no account fields either — neither
console had the concept.
- **PS3, console ID (IDPS).** Inside `PARAM.PFD`, one of `PARAM.SFO`'s four
  hashes is keyed by the IDPS of a single machine — that is what binds a save to
  a console. Name one and both front-ends offer to **re-bind** a save to it.

Re-binding is the `PARAM.PFD` half of moving a save between consoles. A save
also carries account fields in its own `PARAM.SFO`, and those are not touched.

### The corpus

Most of the numbers in this repo's docs — how many saves carry an account, what
a `CATEGORY` may be, which PNG shapes a decoder has to handle — come from one
place: the
**[apollo-saves](https://github.com/bucanero/apollo-saves) database**, checked
out beside this repo. At the time of measuring that is **4,834 archives**
across PS1, PS2, PS3, PS4, PSP and Vita, holding **2,648 `PARAM.SFO` files**,
**2,566 identified saves**, **2,647 `.PSV` containers** and **5,047 PNGs**.

Two things make it usable as a test corpus rather than a pile of zips:

- The saves are **real**, written by real consoles for real games, so they
  carry the cases nobody thinks to write a fixture for — a zero-byte
  `param.sfo`, a 14-byte file beginning `LOCA`, an icon whose `IDAT` fails its
  own CRC, 41 `.png` files that are encrypted Vita thumbnails, macOS
  `__MACOSX/._ICON0.PNG` stubs, PS3 saves filed under a Vita title, 80 DLC
  folders shaped exactly like PS4 saves.
- It is **big enough for a claim to fail**. Several claims in these docs did:
  "all real icons are non-interlaced" (12 are not), "the two PS3 account fields
  always agree" (42 do not), "a `.PSV`'s files are contiguous" (two leave a
  gap) and "its `displaySize` is the sum of them" (nine are not). All had been
  measured honestly against a smaller sample and were simply wrong at scale.

Reading it needs one thing beyond Python's `zipfile`: two archives use
**Deflate64**, which it does not implement, so any sweep should fall back to
`unzip` for those. With that in place nothing in the database fails to open.

A corpus this size also punishes a sloppy audit. Checking a PNG's chunk walk
before its image data reports five files as truncated when their pixels are
perfectly good and only the `IEND` marker is mangled; and 544 files carry bytes
after `IEND`, which a conformant decoder ignores. Measure the thing that
matters — can the image be recovered — before the thing that does not.

Nothing here is checked in, and nothing in the build depends on it — it is a
measuring stick, not a dependency. `apollo_save_test --scan`, `--accounts` and
`--icon`, and `apollo_patcher_gui --scan`, are the tools for pointing at it.

### Where this belongs

The better long-term home for both is apollo-lib, shared with apollo-psp,
apollo-ps3 and apollo-vita instead of copied a third time. That waits on
apollo-vita finishing its migration to mbedTLS, since its copy is still on
polarSSL and that is the only substantive difference between the upstream
versions.

## Layout

```
core/     apollo_ctrl.[ch] — stdio-free engine facade, shared by both front-ends
                             (incl. the PS3 big-endian guess both apply)
          patchdb.[ch]     — reads the bundled patch database (apollo-patches.zip)
          sfo.[ch]         — the PARAM.SFO container, which every console since
                             the PSP writes identically; one parser for all,
                             and the PS4/Vita account ID read and assigned
          saveinfo.[ch]    — ...and which console wrote a given one, for which
                             game. What lets the desktop app be pointed at a
                             folder of saves and produce a list, including the
                             title-ID catalogue that names a Vita save (which
                             carries no name of its own)
          png.[ch]         — a save's ICON0.PNG, decoded to RGBA. Small enough
                             to write rather than vendor: of 5,560 real files
                             checked, every PNG is 8-bit and all but 12 are
                             non-interlaced
          psvcard.[ch]     — the .PSV container, which is the only way a PS1 or
                             PS2 save reaches a computer at all: read, rebuilt
                             around an edited file, and re-signed
          shiftjis.[ch]    — ...and the Shift-JIS those saves name themselves in
          mcicon.[ch]      — ...and their icons, which are not pictures until
                             something makes one
          psp/             — the PSP's own savedata encryption, the layer below
                             any patch; vendored from apollo-psp, see above
          ps3/             — the PS3's, the same layer one console up; derived
                             from pfdtool, see above
          ps2/             — the PS2's 3D save icon: a textured model and a
                             software rasteriser for it, ported from apollo-ps4
          test_psp.c       — the PSP's known-answer vectors, from that upstream
          test_ps3.c       — the PS3's, cross-checked against pfdtool, plus a
                             --corpus mode for a folder of real saves
          test_save.c      — bounds on the SFO parser every front-end shares,
                             and the per-console identification over it
          test_psv.c       — the .PSV container: bounds, and --corpus/--patch
                             modes that re-sign, rebuild and patch real ones
gui/      Dear ImGui desktop app
web/      WebAssembly build + static site
tools/    build-index.py   — patch index, for both front-ends; also the
                             tool catalog (--format=tools), see below
          verify-tools.mjs — proves each catalogued tool against a real save
          make-bundle.py   — apollo-patches.zip, for the desktop app
          make-font.py     — the hex editor's fixed-pitch font, as a header
```

Both front-ends let you search the ~2250 patches in
[apollo-patches](https://github.com/bucanero/apollo-patches) by game name or
title ID, so nobody has to go hunting for a `.savepatch` first. They get there
differently, on purpose: the web page fetches patches and Python helper modules
from a CDN as it needs them, while the desktop app carries the whole database in
a 2.8MB zip built by CI and works offline. A page is a download you make every
visit; an app is one you keep.

`core/apollo_ctrl.c` is the only code that adapts libapollo's data model for a
UI. It replaces the CLI's three interactive pieces with callbacks and data:

| CLI (`patcher.c`)              | Front-end equivalent                    |
|--------------------------------|-----------------------------------------|
| `printf` / `dbglogger_log`     | `apctl_set_log_sink()` → log panel      |
| `scanf` in `get_user_options`  | `apctl_opt_set_selected()` ← dropdowns  |
| `is_active_code` arg parsing   | per-row checkboxes                      |

## The tool catalog

`build-index.py --format=tools` emits a second, smaller index describing what
each patch can *do* rather than what codes it contains — the input for a
front-end that offers "decrypt this save" / "put it back" / "fix the checksum"
per game instead of a code list.

```bash
python3 tools/build-index.py /path/to/apollo-patches tools.json --format=tools
```

1156 of the 2247 patches qualify, covering 509 distinct games once a game's
regions are folded together: 913 are checksum-only, 243 can decrypt and
re-encrypt, and 43 of those go through offzip. 15KB gzipped.
`--verified=FILE` marks the ones `verify-tools.mjs` has proved against a real
save — 187 today, across 79 distinct code chains — and `--verified-only` emits
nothing else, which is what the web tools page ships, so it promises only what
has been run. Each row is `[platform, title_id, name, kinds, files]`, where
`kinds` holds `d` decrypt, `e` re-encrypt, `c` checksum, `z` offzip, and
`files` names what the user should supply.

Two things it deliberately does not do:

- **It does not split by the patch's `:file` targets.** Those markers are not
  dependable — Metal Gear Solid 2 HD files its "Encrypt DATA.BIN" code under
  `:MASTER.BIN`, Call of Duty: Black Ops puts "Encrypt GPAD0_CM.PRF" under
  `:GPAD0_SP.PRF` — and splitting on them invents 17 entries that can decrypt
  but never re-encrypt. It would also be splitting on something the front-end
  overrides anyway, since it applies every selected code to the single file the
  user supplied. Taken per patch, every decrypt in the database has a matching
  encrypt.
- **It does not decide which codes to run.** The engine stays the source of
  truth for that: the page opens the patch, reads the real code list back, and
  applies the required ones in file order. The catalog only answers "which
  games have a tool, and what kind" — the question you cannot ask 2247 files
  from a browser.

## Building

The engine is not vendored here. Clone [apollo-lib](https://github.com/bucanero/apollo-lib)
next to this repository:

```bash
git clone https://github.com/bucanero/apollo-lib
git clone https://github.com/bucanero/apollo-patcher
git clone https://github.com/bucanero/apollo-patches   # web build only
```

Both front-ends need that third clone: the web build embeds the patch
database's `python/` helper modules, and the desktop build bundles the whole
database. Neither is required to *compile* — only to produce a release with a
searchable database. The desktop build finds it the same two ways it finds
apollo-lib, or takes `-DAPOLLO_PATCHES=/path/to/apollo-patches`.

Both build systems look for it in two places: `./apollo-lib` inside this repo
first (which is what CI produces, and where a submodule would sit), then
`../apollo-lib` as above. Point at a checkout elsewhere with
`-DAPOLLO_ROOT=/path/to/apollo-lib` (CMake) or `APOLLO_LIB=/path/to/apollo-lib`
(the web Makefile).

### Desktop GUI

Needs CMake ≥ 3.16, a C/C++ toolchain, zlib, OpenGL, and a built
`libmbedcrypto` in the apollo-lib checkout:

```bash
cd ../apollo-lib/mbedtls-2.16.12/library && make static && \
  mkdir -p ../build/library && cp libmbedcrypto.a ../build/library && cd -
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # fetches Dear ImGui + GLFW
cmake --build build -j
```

Targets:
- `apollo_patcher_gui` — the desktop app (macOS: `build/gui/apollo_patcher_gui.app`).
  Takes files on the command line, or dropped on its window: a `.savepatch`, a
  save, or a save folder. `--scan DIR` lists the saves under a folder without
  opening a window, and `--open PATH` reports where one path would land
- `apollo_patches_bundle` — `apollo-patches.zip`, the database the app browses
  and the console key databases it needs. Built automatically when an
  `apollo-patches` checkout is present (see below) and copied into the app;
  skipped with a message when it is not, never fatal
- `apollo_ctrl_test` — headless lister, proves parity with `patcher <file>`
- `apollo_psp_test` — the PSP savedata checks; needs no sample save, and takes
  `--save DIR FILE KEY` to run a real one
- `apollo_ps3_test` — the PS3 savedata checks, cross-checked against pfdtool.
  Needs no sample save either; `--corpus DIR PS3/games.conf` walks a folder of
  real ones and checks every hash a console wrote against one recomputed here
- `apollo_save_test` — the `PARAM.SFO` parser every front-end shares, and the
  identification over it: which console wrote a save and for which game.
  `--sfo root|sce FILE` reports on a real one, `--scan DIR` on a whole folder,
  `--accounts DIR` prints the PS4/Vita account each save names instead of its
  title, and `--icon FILE` decodes one save icon and prints its size and
  checksum. The last two exist so a whole tree of real saves can be diffed
  against an independent reader — see [the corpus](#the-corpus)
- `-DAPOLLO_FONT_FILE=...` ship a different font from the one vendored at
  `gui/assets/fonts/`. See [gui/README.md](gui/README.md#the-font)
- `-DAPOLLO_BUILD_GUI=OFF` builds only the engine + headless tests (no GL needed)

### Web

See [web/README.md](web/README.md).

## License

[Apollo Save Tool](https://github.com/bucanero/apollo-lib) - Copyright (C) 2020-2026 [Damian Parrino](https://twitter.com/dparrino)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
