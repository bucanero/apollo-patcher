# Apollo Save Patcher

Desktop and web front-ends for the [Apollo save-patch engine](https://github.com/bucanero/apollo-lib).

Both apply Apollo `.savepatch` files — Save Wizard codes, BSD scripts and Python
scripts — to save data on a computer. Neither reimplements any patch logic: they
drive the same `libapollo` engine the console apps use.

| Front-end | What it is |
|-----------|------------|
| [`gui/`](gui/README.md) | Native desktop app (Dear ImGui + GLFW) for Windows, macOS and Linux — with the patch database bundled offline |
| [`web/`](web/README.md) | The engine compiled to WebAssembly, running in a browser tab — the patcher, with the patch database searchable in-page, plus a **tools page** offering one decrypt / re-encrypt pair per game, and a **PSP savedata panel** for the console's own encryption |

The command-line tools (`patcher`, `dumper`) and the engine itself live in
[apollo-lib](https://github.com/bucanero/apollo-lib).

## PSP savedata

A PSP save is encrypted twice. The console wraps it with a per-title game key
before the game's own encryption is anywhere in the picture, and every PSP
patch in the database addresses only the inner layer — so a file copied
straight off a Memory Stick used to go into the patch engine and come back as
noise that looked like output.

`core/psp/` is that outer layer, vendored from
[apollo-psp](https://github.com/bucanero/apollo-psp) (`kirk_engine.c` and
`psp_decrypter.c`, at `17cb5ea`) and reworked into a buffer API with no stdio,
because there is no filesystem in a browser tab. The PARAM.SFO parsing is
bounds-checked against the length it is handed rather than trusting the offsets
inside the file — the difference between a save off your own console and one a
stranger put on the web.

It needs only mbedTLS's AES and SHA1, which both front-ends already link, so it
rides in the existing wasm module (about 12KB gzipped) rather than a second one.

Both front-ends reach it, and they reach it differently because one of them can
look around. The web page asks for `PARAM.SFO` and fetches the game key from
apollo-patches' `PSP/gamekeys.txt` over the CDN; the desktop app has the save
folder, so it *detects* the whole thing from the target you pick and reads the
same key database out of `apollo-patches.zip`, offline. The matching rule —
prefix against the save directory, longest entry wins — is one C function they
share, because the tie-break is load-bearing: the database holds both
`NPJJ30022` and `NPJJ30022GAME1`, with different keys.

`core/test_psp.c` pins it to the unmodified upstream implementation: the
known-answer digests come from apollo-psp's own code compiled for the host, so
a pass says the vendored copy is byte-faithful to what ships on the console
rather than merely self-consistent. The whole chain has been checked against a
real console save — the plaintext it produces matches what the reference
Monster Hunter decrypter validates by the game's own stored SHA-1.

The better long-term home for this is apollo-lib, shared with apollo-psp and
apollo-vita instead of copied a third time. That waits on apollo-vita finishing
its migration to mbedTLS, since its copy is still on polarSSL and that is the
only substantive difference between the two upstream versions.

## Layout

```
core/     apollo_ctrl.[ch] — stdio-free engine facade, shared by both front-ends
                             (incl. the PS3 big-endian guess both apply)
          patchdb.[ch]     — reads the bundled patch database (apollo-patches.zip)
          psp/             — the PSP's own savedata encryption, the layer below
                             any patch; vendored from apollo-psp, see above
          test_psp.c       — its known-answer vectors, taken from that upstream
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
a 2.7MB zip built by CI and works offline. A page is a download you make every
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
  save, or a PSP save folder
- `apollo_patches_bundle` — `apollo-patches.zip`, the database the app browses
  and the PSP game keys it needs. Built automatically when an `apollo-patches`
  checkout is present (see below) and copied into the app; skipped with a
  message when it is not, never fatal
- `apollo_ctrl_test` — headless lister, proves parity with `patcher <file>`
- `apollo_psp_test` — the PSP savedata checks; needs no sample save, and takes
  `--save DIR FILE KEY` to run a real one
- `-DAPOLLO_BUILD_GUI=OFF` builds only the engine + headless tests (no GL needed)

### Web

See [web/README.md](web/README.md).

## License

[Apollo Save Tool](https://github.com/bucanero/apollo-lib) - Copyright (C) 2020-2026 [Damian Parrino](https://twitter.com/dparrino)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
