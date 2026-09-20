# Apollo Save Patcher

Desktop and web front-ends for the [Apollo save-patch engine](https://github.com/bucanero/apollo-lib).

Both apply Apollo `.savepatch` files — Save Wizard codes, BSD scripts and Python
scripts — to save data on a computer. Neither reimplements any patch logic: they
drive the same `libapollo` engine the console apps use.

| Front-end | What it is |
|-----------|------------|
| [`gui/`](gui/README.md) | Native desktop app (Dear ImGui + GLFW) for Windows, macOS and Linux — **point it at a folder of saves and pick a game by name**, with the patch database bundled offline |
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

### Which console a save is written *for*

Both front-ends have a **Settings** panel naming the console a save is written
back for. Both values are optional, both change only what is WRITTEN, and
leaving them blank keeps whatever a save already says — which is what patching
one in place wants.

- **PSP, Fuse ID.** Savedata modes 4 and 6 derive two `PARAM.SFO` hashes from
  the console's own fuse. A PSP loads a save whose values differ, so this only
  matters for reproducing one console's output byte for byte.
- **PS3, console ID (IDPS).** Inside `PARAM.PFD`, one of `PARAM.SFO`'s four
  hashes is keyed by the IDPS of a single machine — that is what binds a save to
  a console. Name one and both front-ends offer to **re-bind** a save to it.

Re-binding is the `PARAM.PFD` half of moving a save between consoles. A save
also carries account fields in its own `PARAM.SFO`, and those are not touched.

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
                             the PSP writes identically; one parser for all
          saveinfo.[ch]    — ...and which console wrote a given one, for which
                             game. What lets the desktop app be pointed at a
                             folder of saves and produce a list, including the
                             title-ID catalogue that names a Vita save (which
                             carries no name of its own)
          png.[ch]         — a save's ICON0.PNG, decoded to RGBA. Small enough
                             to write rather than vendor: all 185 real icons
                             checked are 8-bit and non-interlaced
          psp/             — the PSP's own savedata encryption, the layer below
                             any patch; vendored from apollo-psp, see above
          ps3/             — the PS3's, the same layer one console up; derived
                             from pfdtool, see above
          test_psp.c       — the PSP's known-answer vectors, from that upstream
          test_ps3.c       — the PS3's, cross-checked against pfdtool, plus a
                             --corpus mode for a folder of real saves
          test_save.c      — bounds on the SFO parser every front-end shares,
                             and the per-console identification over it
gui/      Dear ImGui desktop app
web/      WebAssembly build + static site
tools/    build-index.py   — patch index, for both front-ends; also the
                             tool catalog (--format=tools), see below
          verify-tools.mjs — proves each catalogued tool against a real save
          make-bundle.py   — apollo-patches.zip, for the desktop app
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
  save, or a PSP or PS3 save folder. `--scan DIR` lists the saves under a
  folder without opening a window
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
  `--icon FILE` decodes one save icon and prints its size and checksum
- `-DAPOLLO_BUILD_GUI=OFF` builds only the engine + headless tests (no GL needed)

### Web

See [web/README.md](web/README.md).

## License

[Apollo Save Tool](https://github.com/bucanero/apollo-lib) - Copyright (C) 2020-2026 [Damian Parrino](https://twitter.com/dparrino)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
