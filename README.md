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

## Documentation

| | |
|---|---|
| **[User guide](docs/user-guide.md)** | Using the desktop app: patching a save, the settings, and what to do when something goes wrong. Also published at [bucanero.github.io/apollo-patcher/guide.html](https://bucanero.github.io/apollo-patcher/guide.html) |
| [Save data](docs/internals/savedata.md) | What a save is per console, the encryption layers, the `.PSV` container, accounts |
| [Save icons](docs/internals/icons.md) | Decoding a PNG, and drawing the PS1 and PS2 icons that are not pictures |
| [Text](docs/internals/text.md) | The two fonts, what they cover, and the Shift-JIS underneath a PS2 title |
| [The front-ends](docs/internals/frontends.md) | What the desktop app and the web page each do with the engine |
| [The corpus](docs/internals/corpus.md) | The real-save database most numbers in these docs come from |

## What it handles

Six consoles, with almost nothing in common between them:

| | a save is | the console's own layer | the icon |
|---|---|---|---|
| **PS1** | a memory-card block, inside a `.PSV` | none | 16 colours in the save's own block |
| **PS2** | memory-card files, inside a `.PSV` | none | a textured 3D model |
| **PSP** | a folder with `PARAM.SFO` | encrypted, per-title key | `ICON0.PNG` |
| **PS3** | a folder with `PARAM.SFO` | encrypted, per-file key in `PARAM.PFD` | `ICON0.PNG` |
| **PS4** | a folder with `sce_sys/param.sfo` | none — already decrypted | `sce_sys/icon0.png` |
| **Vita** | a folder with `sce_sys/param.sfo` | none | `sce_sys/icon0.png` |

Two of those rows are the interesting ones. **PSP and PS3 saves are encrypted
twice** — the console wraps a save with a key of its own before the game's
encryption is anywhere in the picture, and every patch addresses only the inner
layer, so the wrapper has to come off first. **PS1 and PS2 saves are not files
at all** — those consoles kept saves in memory-card blocks, so a save only
reaches a computer inside a signed `.PSV` container — the PS3's export
format, also written by [apollo-ps2](https://github.com/bucanero/apollo-ps2)
and [ps2vmc-tool](https://github.com/bucanero/ps2vmc-tool).

Both problems live under `core/`, reworked into buffer APIs with no stdio
because there is no filesystem in a browser tab, and both parse their metadata
bounds-checked against the length they are handed rather than trusting the
offsets inside the file — the difference between a save off your own console
and one a stranger put on the web.

[docs/internals/savedata.md](docs/internals/savedata.md) has the detail.

> **A naming collision worth knowing before reading the code.** `.PSV` is a
> **file format holding a PS1 or PS2 save**, and `PSV` is the **platform tag for
> the PS Vita**. They share four letters and nothing else, so the console's
> enumerator is spelled out as `ASAVE_PSVITA` and everything about the container
> is `apsvc_`/`psvcard`.

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
          png.[ch]         — a save's ICON0.PNG, decoded to RGBA
          psvcard.[ch]     — the .PSV container, which is the only way a PS1 or
                             PS2 save reaches a computer at all: read, rebuilt
                             around an edited file, and re-signed
          shiftjis.[ch]    — ...and the Shift-JIS those saves name themselves in
          mcicon.[ch]      — ...and their icons, which are not pictures until
                             something makes one
          psp/             — the PSP's own savedata encryption, the layer below
                             any patch; vendored from apollo-psp
          ps3/             — the PS3's, the same layer one console up; derived
                             from pfdtool
          ps2/             — the PS2's 3D save icon: a textured model and a
                             software rasteriser for it, ported from apollo-ps4
          test_psp.c       — the PSP's known-answer vectors, from that upstream
          test_ps3.c       — the PS3's, cross-checked against pfdtool, plus a
                             --corpus mode for a folder of real saves
          test_save.c      — bounds on the SFO parser every front-end shares,
                             and the per-console identification over it
          test_psv.c       — the .PSV container: bounds, and --corpus/--patch
                             modes that re-sign, rebuild and patch real ones
docs/     user-guide.md    — using the desktop app
          internals/       — how and why the above works the way it does
gui/      Dear ImGui desktop app
web/      WebAssembly build + static site
tools/    build-index.py   — patch index, for both front-ends; also the
                             tool catalog (--format=tools)
          verify-tools.mjs — proves each catalogued tool against a real save
          make-bundle.py   — apollo-patches.zip, for the desktop app
          make-font.py     — the hex editor's fixed-pitch font, as a header
          make-guide.py    — docs/user-guide.md, rendered onto the web site
```

Both front-ends let you search the ~2250 patches in
[apollo-patches](https://github.com/bucanero/apollo-patches) by game name or
title ID, so nobody has to go hunting for a `.savepatch` first. They get there
differently, on purpose: the web page fetches patches and Python helper modules
from a CDN as it needs them, while the desktop app carries the whole database in
a zip built by CI and works offline. A page is a download you make every visit;
an app is one you keep.

## Building

The engine is not vendored here. Clone [apollo-lib](https://github.com/bucanero/apollo-lib)
next to this repository:

```bash
git clone https://github.com/bucanero/apollo-lib
git clone https://github.com/bucanero/apollo-patcher
git clone https://github.com/bucanero/apollo-patches   # for a release build
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
  `apollo-patches` checkout is present and copied into the app; skipped with a
  message when it is not, never fatal
- `apollo_ctrl_test` — headless lister, proves parity with `patcher <file>`
- `apollo_psp_test` — the PSP savedata checks; needs no sample save, and takes
  `--save DIR FILE KEY` to run a real one
- `apollo_ps3_test` — the PS3 savedata checks, cross-checked against pfdtool.
  Needs no sample save either; `--corpus DIR PS3/games.conf` walks a folder of
  real ones and checks every hash a console wrote against one recomputed here
- `apollo_save_test` — the `PARAM.SFO` parser every front-end shares, and the
  identification over it: which console wrote a save and for which game
- `apollo_psv_test` — the `.PSV` container: bounds, signatures, and
  `--corpus`/`--patch` modes over a folder of real ones
- `-DAPOLLO_FONT_FILE=...` ship a different font from the one vendored at
  `gui/assets/fonts/`. See [docs/internals/text.md](docs/internals/text.md)
- `-DAPOLLO_BUILD_GUI=OFF` builds only the engine + headless tests (no GL needed)

The test binaries' `--corpus`, `--scan` and `--accounts` modes all expect a
checkout of the real-save database — see
[docs/internals/corpus.md](docs/internals/corpus.md).

### Web

See [web/README.md](web/README.md).

### The tool catalog

`build-index.py --format=tools` emits a second, smaller index describing what
each patch can *do* rather than what codes it contains — the input for the web
tools page:

```bash
python3 tools/build-index.py /path/to/apollo-patches tools.json --format=tools
```

See [docs/internals/frontends.md](docs/internals/frontends.md#the-tool-catalog)
for what it does and does not promise.

## License

[Apollo Save Tool](https://github.com/bucanero/apollo-lib) - Copyright (C) 2020-2026 [Damian Parrino](https://twitter.com/dparrino)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
