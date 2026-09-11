# Apollo Save Patcher

Desktop and web front-ends for the [Apollo save-patch engine](https://github.com/bucanero/apollo-lib).

Both apply Apollo `.savepatch` files — Save Wizard codes, BSD scripts and Python
scripts — to save data on a computer. Neither reimplements any patch logic: they
drive the same `libapollo` engine the console apps use.

| Front-end | What it is |
|-----------|------------|
| [`gui/`](gui/README.md) | Native desktop app (Dear ImGui + GLFW) for Windows, macOS and Linux — with the patch database bundled offline |
| [`web/`](web/README.md) | The engine compiled to WebAssembly, running in a browser tab — the patcher, with the patch database searchable in-page, plus a **tools page** offering one decrypt / re-encrypt pair per game |

The command-line tools (`patcher`, `dumper`) and the engine itself live in
[apollo-lib](https://github.com/bucanero/apollo-lib).

## Layout

```
core/     apollo_ctrl.[ch] — stdio-free engine facade, shared by both front-ends
                             (incl. the PS3 big-endian guess both apply)
          patchdb.[ch]     — reads the bundled patch database (apollo-patches.zip)
gui/      Dear ImGui desktop app
web/      WebAssembly build + static site
tools/    build-index.py   — patch index, for both front-ends; also the
                             tool catalog (--format=tools), see below
          verify-tools.mjs — proves each catalogued tool against a real save
          make-bundle.py   — apollo-patches.zip, for the desktop app
```

Both front-ends let you search the ~2240 patches in
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

1148 of the 2240 patches qualify (540 distinct games): 906 can fix a checksum,
242 can decrypt and re-encrypt, and 43 of those go through offzip. 15KB
gzipped. `--verified=FILE` marks the ones `verify-tools.mjs` has proved against
a real save — 107 today, across 51 tools — and `--verified-only` emits nothing else, which is
what the web tools page ships, so it promises only what has been run. Each row is `[platform, title_id, name, kinds, files]`, where `kinds`
holds `d` decrypt, `e` re-encrypt, `c` checksum, `z` offzip, and `files` names
what the user should supply.

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
  games have a tool, and what kind" — the question you cannot ask 2240 files
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
searchable database.

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
- `apollo_patcher_gui` — the desktop app (macOS: `build/gui/apollo_patcher_gui.app`)
- `apollo_ctrl_test` — headless lister, proves parity with `patcher <file>`
- `-DAPOLLO_BUILD_GUI=OFF` builds only the engine + headless test (no GL needed)

### Web

See [web/README.md](web/README.md).

## License

[Apollo Save Tool](https://github.com/bucanero/apollo-lib) - Copyright (C) 2020-2026 [Damian Parrino](https://twitter.com/dparrino)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
