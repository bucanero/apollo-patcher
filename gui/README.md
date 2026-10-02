# Apollo Save Patcher — desktop app

A cross-platform (Windows / macOS / Linux) graphical front-end for the Apollo
save-patch engine, built on **Dear ImGui + GLFW/OpenGL3**.

It does not reimplement any patch logic: it wraps the existing `libapollo`
engine and drives the same functions the CLI does
(`apollo_load_code_list`, `apollo_apply_code`).

## Documentation

| | |
|---|---|
| **[User guide](../docs/user-guide.md)** | Using the app — start here if you are not changing it |
| [The front-ends](../docs/internals/frontends.md) | The screens, the save walk, settings, opening files, the hex views |
| [Save data](../docs/internals/savedata.md) | What a save is per console, and the encryption under it |
| [Save icons](../docs/internals/icons.md) | Decoding and rendering them |
| [Text](../docs/internals/text.md) | The two fonts, and what they cover |

This file is the build: how to compile it, package it, and where the bundled
database and the vendored third-party pieces come from.

## Architecture

```
  src/main.cpp                     Dear ImGui UI (file pickers, code list, option combos, log)
  src/imgui_memory_editor.h        hex editor, vendored from ocornut/imgui_club (MIT)
  ../core/apollo_ctrl.[ch]         stdio-free facade over libapollo — shared with the web front-end
  ../core/patchdb.[ch]             reads apollo-patches.zip (the bundled database)
  ../core/psp/                     the PSP's own savedata encryption, below any patch
  ../core/ps3/                     the PS3's, the same layer one console up
  ../core/psvcard.[ch]             the .PSV container holding a PS1 or PS2 save
  ../core/mcicon.[ch], ../core/ps2/  ...and its icon, which has to be rendered
  ../../apollo-lib/source/*.c      libapollo engine (unchanged)
  ../../apollo-lib/mbedtls-2.16.12 crypto backend (libmbedcrypto), same as the CLI
```

The engine lives in a separate [apollo-lib](https://github.com/bucanero/apollo-lib)
checkout — see the [top-level README](../README.md) for the expected layout.

`apollo_ctrl` replaces the CLI's three UI-bound pieces with callbacks and data
— see [the engine facade](../docs/internals/frontends.md#the-engine-facade).

## Build

Prerequisites: CMake ≥ 3.16, a C/C++ toolchain, zlib, OpenGL, and a built
`libmbedcrypto` (the CI already builds it):

```bash
cd ../apollo-lib/mbedtls-2.16.12/library && make static && \
  mkdir -p ../build/library && cp libmbedcrypto.a ../build/library && cd -
```

Then, from the **repository root** (the CMake project lives there, so that the
engine target can be shared):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # fetches Dear ImGui + GLFW
cmake --build build -j
```

Targets:
- `apollo_patcher_gui` — the GUI app (macOS: `build/gui/apollo_patcher_gui.app`)
- `apollo_ctrl_test`   — headless lister, proves parity with `patcher <file>`
- `-DAPOLLO_BUILD_GUI=OFF` builds only the engine + headless test (no GL needed)

## Packaging (per OS)

| OS      | Tooling                                             |
|---------|-----------------------------------------------------|
| macOS   | `MACOSX_BUNDLE` → `.app`, `cpack -G DragNDrop` (dmg) |
| Windows | MSYS2/MinGW (same as `build-win.yml`), `cpack -G NSIS` |
| Linux   | `cpack -G AppImage` / `.deb`                         |

CI is already wired: `.github/workflows/build.yml` (macOS + Linux) and
`build-win.yml` build the GUI and upload it as an
`apollo-gui-<sha>-<os>` artifact (`.app` on macOS, `apollo_patcher_gui.exe`
elsewhere). Turn those artifacts into installers with CPack when you're ready.

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

## The bundled patch database

**Find a game...** (or Ctrl+F) searches the ~2250 patches from
[apollo-patches](https://github.com/bucanero/apollo-patches) by game name or
title ID and loads the one you pick — the same flow as the web front-end, but
offline.

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

## Known limitations that affect packaging

Both of these are in the [user guide](../docs/user-guide.md#when-something-goes-wrong)
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

## Credits / third-party

- [portable-file-dialogs](https://github.com/samhocevar/portable-file-dialogs)
  by Sam Hocevar — native file dialogs (WTFPL). Vendored at
  `src/portable-file-dialogs.h`; update by replacing that file from upstream.
  Carries one **LOCAL PATCH** (search that string in the file): a 32-bit-only
  fix in `pfd::notify` where a capture-less lambda wouldn't bind to the
  `__stdcall ENUMRESNAMEPROC` on x86 — re-apply if you refresh the header.
- [Dear ImGui](https://github.com/ocornut/imgui) and
  [GLFW](https://github.com/glfw/glfw) — fetched at configure time via CMake.
- [Noto Sans JP](https://fonts.google.com/noto) — the UI font, vendored at
  `assets/fonts/` under the SIL Open Font License. See [Text](../docs/internals/text.md#the-ui-font).
- [raster-fonts](https://github.com/idispatch/raster-fonts) by idispatch — the
  10×20 console font the hex editor and code viewers draw with, the same one
  `apollo-ps3`, `apollo-ps4` and `apollo-vita` use. Converted to
  `src/font10x20.h` by `tools/make-font.py`; regenerate from upstream's
  `font-10x20.c` rather than editing the header.

## App icon

Source art: `assets/icon.png`. Derived files (checked in; the app needs no
image decoder at runtime):
- `assets/icon.icns` — macOS bundle icon (Dock/Finder), wired via CMake
  `MACOSX_BUNDLE_ICON_FILE`. Regenerate from the PNG with `sips`/`iconutil`.
- `src/icon_rgba_z.h` — the icon **pre-decoded to 256×256 RGBA and zlib-deflated**
  (262 KB → ~51 KB). Inflated at startup with zlib (already linked) and passed to
  `glfwSetWindowIcon()` for the Windows/Linux title-bar & taskbar (no-op on
  macOS — the whole path is behind `#ifndef __APPLE__`). Regenerate by resizing
  `assets/icon.png` to 256×256, decoding to raw RGBA, `compress2()`-ing at
  `Z_BEST_COMPRESSION`, and `xxd -i`-ing the deflated bytes into this header.
