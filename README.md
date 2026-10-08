# Apollo Save Patcher

Desktop and web front-ends for the [Apollo save-patch engine](https://github.com/bucanero/apollo-lib).

Both apply Apollo `.savepatch` files — Save Wizard codes, BSD scripts and Python
scripts — to save data on a computer. Neither reimplements any patch logic: they
drive the same `libapollo` engine the console apps use.

| Front-end | What it is |
|-----------|------------|
| [`gui/`](#desktop-gui) | Native desktop app (Dear ImGui + GLFW) for Windows, macOS and Linux — **opens on your saves**: point it at a folder, pick a game by name, patch it. The patch database is bundled offline |
| [`web/`](web/README.md) | The engine compiled to WebAssembly, running in a browser tab — the patcher, with the patch database searchable in-page and **PSP and PS3 savedata panels** for the consoles' own encryption, plus a **tools page** offering one decrypt / re-encrypt pair per game |

The command-line tools (`patcher`, `dumper`) and the engine itself live in
[apollo-lib](https://github.com/bucanero/apollo-lib).

![The desktop app's saves list, showing PS3 saves by game with their slot,
console, title ID and whether the patch database has codes, and a hover panel
detailing one save.](docs/images/saves-list.webp)

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
gui/      src/main.cpp     — Dear ImGui desktop app (file pickers, save list,
                             code list, option combos, log)
          src/imgui_memory_editor.h — hex editor, vendored from
                             ocornut/imgui_club (MIT)
          assets/          — the app icon and the vendored UI font
          cmake/           — the installers (packaging.cmake), and the mingw-w64
                             toolchain for the 32-bit Windows build
          app.rc.in        — the Windows executable's icon and version block
          io.github.bucanero.apollo_patcher.desktop, .appdata.xml
                         — the Linux desktop entry and AppStream metadata
web/      WebAssembly build + static site
tools/    build-index.py   — patch index, for both front-ends; also the
                             tool catalog (--format=tools)
          verify-tools.mjs — proves each catalogued tool against a real save
          make-bundle.py   — apollo-patches.zip, for the desktop app
          make-font.py     — the hex editor's fixed-pitch font, as a header
          make-guide.py    — docs/user-guide.md, rendered onto the web site
          make-ico.py      — gui/assets/icon.ico, from PNGs
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

#### Packaging

Installers come from CPack, configured in `gui/cmake/packaging.cmake`. Run it
from the build directory after a Release build:

| OS      | command | produces |
|---------|---------|----------|
| macOS   | `cpack -G DragNDrop` | `ApolloSavePatcher-<ver>-macOS.dmg`: the app, renamed `Apollo Save Patcher.app` and signed ad hoc, plus an `Applications` link |
| Windows | `cpack -G NSIS` (needs `makensis`) | `ApolloSavePatcher-<ver>-windows-<x64\|x86>-setup.exe`: Program Files, a Start menu entry, `.savepatch` opening in the app, and an uninstaller |
| Linux   | `cmake --install build --prefix AppDir/usr`, then [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy) `--appdir AppDir --output appimage` | `ApolloSavePatcher-<ver>-linux-x86_64.AppImage` |

The packages land in `build/packages/`. The version is `project(VERSION)` in
the root `CMakeLists.txt`. None of them are signed, and the
[user guide](docs/user-guide.md#the-first-time-you-open-it) tells people how to
get past the warning that causes.

The Windows installer carries the [software OpenGL fallback](#known-limitations-that-affect-packaging)
when configure is given one with `-DAPOLLO_SOFTGL_DLL=path/to/opengl32.dll`.
It always goes into `softgl\`. An optional component, unticked by default,
also puts it beside the `.exe`. Without that component, someone on Remote
Desktop would need administrator rights to copy it into Program Files
themselves.

In CI, `build.yml` and `build-win.yml` build and test on every push and
upload the plain builds as `apollo-gui-<sha>-<os>`. They make no installers.
The installers come from `.github/workflows/release.yml`, which runs only when
started by hand (**Actions ▸ Release installers ▸ Run workflow**). It builds
all four in parallel (the Linux one on Ubuntu 22.04, to keep the AppImage's
glibc floor low) and gathers them, with a `SHA256SUMS.txt`, into one artifact
named `ApolloSavePatcher-<version>`. Attach that artifact's files to a GitHub
Release.

`gui/assets/icon.ico` (the `.exe`'s and the installer's icon, bound into the
executable by `gui/app.rc.in` along with its version block) and
`gui/assets/icon-256.png` (the AppImage's) are derived from `icon.png`, like
the other icon files below.

**Windows architectures.** The `msys` job (MSYS2 MINGW64) produces the **x64**
build. The **x86 (32-bit)** build is cross-compiled on Linux with mingw-w64
(`win32-cross` job) using the toolchain file `gui/cmake/mingw-i686.cmake` —
MSYS2 dropped its 32-bit toolchain, so its MINGW32 target silently emitted x64
binaries. To reproduce the 32-bit build locally on Linux:

```bash
sudo apt-get install -y gcc-mingw-w64-i686 g++-mingw-w64-i686 libz-mingw-w64-dev ninja-build
# build mbedcrypto into ../apollo-lib/mbedtls-2.16.12/build/library with the same
# toolchain file, then from the repository root:
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/gui/cmake/mingw-i686.cmake
cmake --build build
```

#### The bundled patch database

The database is `apollo-patches.zip` (3.3MB, 2269 entries), built by
`tools/make-bundle.py`. **The CMake build makes it for you**, from the same
`apollo-patches` checkout the web build uses (`./apollo-patches`, then
`../apollo-patches`, or `-DAPOLLO_PATCHES=...`), and copies it into the app —
`Contents/Resources` for a macOS `.app`, beside the executable elsewhere. It
lands at `build/apollo-patches.zip` too, which is next to `apollo_ctrl_test`,
so `--db` works with no environment variable.

It is optional and never fails a build: with no patch checkout, or no Python,
configuring says what is missing and carries on. The app then opens
`.savepatch` files by hand as before, and the browser explains itself.

Rebuilds are tracked — the glob over the patch files is `CONFIGURE_DEPENDS`, so
pulling new patches rebuilds the zip and refreshes the app's copy on the next
build. A build with nothing changed does neither.

Besides the patches, the zip carries three generated or copied tables:
`index.tsv` (game names per patch), the two console key databases
(`PSP/gamekeys.txt`, `PS3/games.conf`), and **`titles.tsv`** — game names by
title ID, normalised by `make-bundle.py` from apollo-patches' four title
catalogues. That last one is what names a save the patch database has never
heard of:

| source | platform | titles |
|---|---|---|
| `psptitleid.txt` | PSP | 4,306 |
| `psvtitleid.txt` | PSV (Vita) | 4,477 |
| `ps1titleid.txt` | PS1 | 10,048 |
| `ps2titleid.txt` | PS2 | 14,339 |

33,170 rows, about 387KB compressed. `TITLE_DBS` in `make-bundle.py` is the
list, and carries a per-file encoding because the PS1 and PS2 catalogues are
Windows-1252 rather than UTF-8.

Where it looks, in order: `$APOLLO_PATCHES_ZIP`, next to the executable,
`../Resources/` (so a macOS `.app` is self-contained), then the working
directory.

#### Known limitations that affect packaging

These are in the [user guide](docs/user-guide.md#when-something-goes-wrong)
in the form a user needs them, but they shape what has to ship:

- **The Windows artifacts carry a software OpenGL fallback** at
  `softgl\opengl32.dll` — Mesa 17.2.6, old enough to be a single
  self-contained file. It is deliberately in a subfolder so the app uses the
  system GPU driver by default; a user with no usable OpenGL (Remote Desktop
  exposes none at all) copies it up beside the exe. `main` sets
  `GALLIUM_DRIVER=llvmpipe` to select it, unless the user has set that variable
  themselves.
- **The x64 software renderer needs AVX**, which is not by design: every
  mesa-dist-win x64 build before Mesa 22.0 carries the `swr` driver, which
  [leaks AVX into common code](https://github.com/pal1000/mesa-dist-win#known-issues).
  On an older CPU the app dies at launch with `0xC000001D` before a window
  appears. The x86 artifact is unaffected, which is why **both architectures
  are shipped** and the 32-bit one is the supported answer for such a machine.
  Tested: on a Windows 7 x64 host with a Pentium 4, the x64 software renderer
  dies and the x86 one runs normally, while the same host runs the x64 build
  fine on its GPU driver.
- **Linux file dialogs** need `zenity`, `kdialog`, `matedialog` or `qarma`
  present at runtime.

#### App icon

Source art: `gui/assets/icon.png`. Derived files (checked in; the app needs no
image decoder at runtime):
- `gui/assets/icon.icns` — macOS bundle icon (Dock/Finder), wired via CMake
  `MACOSX_BUNDLE_ICON_FILE`. Regenerate from the PNG with `sips`/`iconutil`.
- `gui/src/icon_rgba_z.h` — the icon **pre-decoded to 256×256 RGBA and
  zlib-deflated** (262 KB → ~51 KB). Inflated at startup with zlib (already
  linked) and passed to `glfwSetWindowIcon()` for the Windows/Linux title-bar &
  taskbar (no-op on macOS — the whole path is behind `#ifndef __APPLE__`).
  Regenerate by resizing `gui/assets/icon.png` to 256×256, decoding to raw
  RGBA, `compress2()`-ing at `Z_BEST_COMPRESSION`, and `xxd -i`-ing the
  deflated bytes into this header.
- `gui/assets/icon.ico`: the Windows icon, 16 to 256px. Regenerate with
  `sips -z N N` (or any resizer) for each of 16, 24, 32, 48, 64, 128 and 256,
  then `tools/make-ico.py gui/assets/icon.ico icon-*.png`.
- `gui/assets/icon-256.png`: the Linux desktop icon, at the 256×256 size
  linuxdeploy accepts. `sips -z 256 256`.

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

## Credits

The consoles' own savedata encryption, the layer under every patch, rests on
other people's reverse engineering:

- **KIRK engine** by **Draan**, with help from coyotebean, Davee, hitchhikr,
  kgsws, liquidzigong, Mathieulh, Proxima and SilverSpring — an open-source
  implementation of the PSP's crypto engine (GPLv3). Vendored at `core/psp/`
  by way of [apollo-psp](https://github.com/bucanero/apollo-psp), and what
  takes the PSP's wrapper off a save.
- **pfdtool** by **flatz** — the PS3's `PARAM.PFD` format, its hashing and the
  console keys. `core/ps3/` is derived from it by way of
  [pfd_sfo_tools](https://github.com/bucanero/pfd_sfo_tools) and
  [apollo-ps3](https://github.com/bucanero/apollo-ps3).

Third-party pieces in the desktop app:

- [Dear ImGui](https://github.com/ocornut/imgui) and
  [GLFW](https://github.com/glfw/glfw) — fetched at configure time via CMake.
- [imgui_club](https://github.com/ocornut/imgui_club)'s memory editor — the
  hex editor, vendored at `gui/src/imgui_memory_editor.h` (MIT).
- [portable-file-dialogs](https://github.com/samhocevar/portable-file-dialogs)
  by Sam Hocevar — native file dialogs (WTFPL). Vendored at
  `gui/src/portable-file-dialogs.h`; update by replacing that file from
  upstream. Carries one **LOCAL PATCH** (search that string in the file): a
  32-bit-only fix in `pfd::notify` where a capture-less lambda wouldn't bind to
  the `__stdcall ENUMRESNAMEPROC` on x86 — re-apply if you refresh the header.
- [Noto Sans JP](https://fonts.google.com/noto) — the UI font, vendored at
  `gui/assets/fonts/` under the SIL Open Font License. See
  [Text](docs/internals/text.md#the-ui-font).
- [raster-fonts](https://github.com/idispatch/raster-fonts) by idispatch — the
  10×20 console font the hex editor and code viewers draw with, the same one
  `apollo-ps3`, `apollo-ps4` and `apollo-vita` use. Converted to
  `gui/src/font10x20.h` by `tools/make-font.py`; regenerate from upstream's
  `font-10x20.c` rather than editing the header.

## License

[Apollo Save Tool](https://github.com/bucanero/apollo-lib) - Copyright (C) 2020-2026 [Damian Parrino](https://twitter.com/dparrino)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
