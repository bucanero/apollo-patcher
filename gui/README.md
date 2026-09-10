# Apollo Save Patcher — desktop app

A cross-platform (Windows / macOS / Linux) graphical front-end for the Apollo
save-patch engine, built on **Dear ImGui + GLFW/OpenGL3**.

It does not reimplement any patch logic: it wraps the existing `libapollo`
engine and drives the same functions the CLI does
(`apollo_load_code_list`, `apollo_apply_code`).

## Architecture

```
  src/main.cpp                     Dear ImGui UI (file pickers, code list, option combos, log)
  src/imgui_memory_editor.h        hex editor, vendored from ocornut/imgui_club (MIT)
  ../core/apollo_ctrl.[ch]         stdio-free facade over libapollo — shared with the web front-end
  ../core/patchdb.[ch]             reads apollo-patches.zip (the bundled database)
  ../../apollo-lib/source/*.c      libapollo engine (unchanged)
  ../../apollo-lib/mbedtls-2.16.12 crypto backend (libmbedcrypto), same as the CLI
```

The engine lives in a separate [apollo-lib](https://github.com/bucanero/apollo-lib)
checkout — see the [top-level README](../README.md) for the expected layout.

`apollo_ctrl` replaces the CLI's three UI-bound pieces with callbacks/data:

| CLI (`patcher.c` in apollo-lib) | GUI equivalent                               |
|------------------------------|-------------------------------------------------|
| `printf` / `dbglogger_log`   | `apctl_set_log_sink()` → log panel             |
| `scanf` in `get_user_options`| `apctl_opt_set_selected()` ← combo boxes       |
| `is_active_code` arg parsing | per-row checkboxes                              |

MicroPython `print()` output is also routed to the log panel (the engine is
built **without** `-DAPOLLO_CLI`, so `dbglogger_printf` goes to the sink).

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

**Find a game...** (or Ctrl+F) searches the ~2240 patches from
[apollo-patches](https://github.com/bucanero/apollo-patches) by game name or
title ID and loads the one you pick — the same flow as the web front-end, but
offline. CI builds the database into `apollo-patches.zip` and ships it in the
artifact (`tools/make-bundle.py`).

Where it looks, in order: `$APOLLO_PATCHES_ZIP`, next to the executable,
`../Resources/` (so a macOS `.app` is self-contained), then the working
directory. Without it the app still opens `.savepatch` files by hand; the
browser just explains what is missing.

Patches are read straight out of the zip, so there is nothing to unpack. The
Python helper modules are the exception — MicroPython's `import` goes through
`stat()`/`open()` on real paths — so on startup they are extracted to the
per-user cache directory and the engine is pointed at it with
`apctl_set_data_path()`. Before this the GUI passed no host callback at all, so
`DATA_PATH` was empty and a Python code that imported a helper module only
worked if the app happened to be launched from a directory containing
`python/`.

`index.tsv` inside the zip carries the game names, generated by
`tools/build-index.py` — the same script that builds the web front-end's index,
including the Windows-1252 fallback that 245 of the patch files need. Without it
the app would have to inflate 2240 entries at startup just to read their second
line.

## Viewing and editing data

- **View / edit data** (next to the target picker) opens the save file in a hex
  editor — `src/imgui_memory_editor.h`, vendored from
  [imgui_club](https://github.com/ocornut/imgui_club). It works on a copy held
  in memory and writes back only when asked, so a mistyped byte costs nothing
  until committed; a `.bak` is kept first, like the patch path does. The buffer
  is re-read every time the window is opened, because applying codes rewrites
  the file underneath it.
- **View** (per code row) opens that code's body — and it is editable. Save
  changes replaces the body the engine will run; *Revert to file* puts the
  patch's own text back, and the row carries a `*` while it differs. The edit
  lives in the session only: the `.savepatch` is never rewritten and closing
  the patch drops it. Applying works on a copy of the body, so an edit is not
  consumed by applying it and Apply stays repeatable.

  Two things an edit cannot do, both settled at parse time: change the code's
  **type** (an edited Save Wizard code is still read as Save Wizard), and
  rename a **`{TAG}`** — the engine writes an option's value over the tag in
  place, at the tag's own length, so a tag that has been retyped or deleted
  stops resolving. The window says so when a placeholder goes missing, since
  nothing else would until the patch misbehaved.
- **View patch file** shows the `.savepatch` as text. Worth having: parsing
  keeps only the codes, so author comments, credits, `[INFO:]` notes and the
  target-file lines are invisible otherwise. Carriage returns are stripped for
  display — most patches are CRLF and ImGui has no glyph for CR.
- **Big-endian mode** is set on load. PS3 is the only big-endian platform
  Apollo covers, and although the engine accepts a per-code `[BE:...]` header,
  no patch in the database uses one — so every PS3 patch relied on the user
  knowing to tick the box.

  `apctl_is_big_endian_for()` decides, shared by both front-ends. A patch picked
  from the database carries its **platform tag**, which is simply the directory
  it lives in, so it is authoritative and needs no guessing — a PS3 title with a
  prefix nobody has catalogued yet still comes out right. A loose file has no
  directory, so it falls back to matching the known PS3 title-ID prefixes
  against the file name and then the patch's own first lines. Still a checkbox,
  so it can be overridden.

## Features

- Native file pickers via header-only
  [portable-file-dialogs](https://github.com/samhocevar/portable-file-dialogs),
  vendored as a single header at [src/portable-file-dialogs.h](src/portable-file-dialogs.h).
  On macOS/Linux it shells out to `osascript`/`zenity` (a separate process),
  which sidesteps the in-process `NSOpenPanel` breakage caused by GLFW's Cocoa
  init — so no custom helper binary or bundling is needed. Windows uses the
  Win32 dialog API directly. Patch files are filtered to `*.savepatch`.
- **Apply is blocked** while any checked code has an unfilled required option:
  the offending combo boxes and an "(required)" tag turn red, and the button is
  disabled until every selection is made.
- **Big-endian mode** checkbox: selects the byte order the engine uses for save
  data (PS3 / Xbox 360 / Wii saves), the equivalent of the `patcher` CLI's
  `-b`/`--big-endian` flag. It calls `apollo_set_endianness()` before each code
  is applied, so a single build handles both byte orders — no separate
  big-endian binary.

## Known caveats / TODO
- **Linux dialogs:** portable-file-dialogs needs a dialog helper present at
  runtime (`zenity`, `kdialog`, `matedialog`, or `qarma`).
- **OpenGL / GPU-less & RDP hosts:** the app uses Dear ImGui's fixed-function
  `imgui_impl_opengl2` backend with a legacy context **on all platforms** (needs
  only OpenGL 1.1 — one code path everywhere; this 2D tool has no use for modern
  GL). On **Windows** it uses the **system OpenGL driver** by default (hardware
  when present). Some hosts have no usable OpenGL — **Remote Desktop exposes no
  WGL OpenGL at all**, and so do some VMs / GPU-less machines — and there the
  window can't be created (you'll get a message box saying so).

  **Software-OpenGL fallback (drop-in).** The Windows build ships a self-contained
  Mesa software renderer at **`softgl\opengl32.dll`** next to the exe. It is *not*
  used by default (it's in a subfolder), so the app uses the system GPU driver. If
  you hit the OpenGL error, **copy `softgl\opengl32.dll` up into the same folder
  as `apollo_patcher_gui.exe`** and relaunch — GLFW loads `opengl32.dll` from the
  exe's own directory first, so it then renders in software (presented via GDI,
  which works over RDP); `GALLIUM_DRIVER=llvmpipe` is set in `main` to force it.
  The bundled DLL is Mesa **17.2.6** — old enough to be a single self-contained
  file, and verified working on Windows 7, both x86 and x64. macOS/Linux never
  need this.

## Credits / third-party

- [portable-file-dialogs](https://github.com/samhocevar/portable-file-dialogs)
  by Sam Hocevar — native file dialogs (WTFPL). Vendored at
  `src/portable-file-dialogs.h`; update by replacing that file from upstream.
  Carries one **LOCAL PATCH** (search that string in the file): a 32-bit-only
  fix in `pfd::notify` where a capture-less lambda wouldn't bind to the
  `__stdcall ENUMRESNAMEPROC` on x86 — re-apply if you refresh the header.
- [Dear ImGui](https://github.com/ocornut/imgui) and
  [GLFW](https://github.com/glfw/glfw) — fetched at configure time via CMake.

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
