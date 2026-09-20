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
  ../core/psp/                     the PSP's own savedata encryption, below any patch
  ../core/ps3/                     the PS3's, the same layer one console up
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

**Find a game...** (or Ctrl+F) searches the ~2250 patches from
[apollo-patches](https://github.com/bucanero/apollo-patches) by game name or
title ID and loads the one you pick — the same flow as the web front-end, but
offline.

The database is `apollo-patches.zip`, built by `tools/make-bundle.py`. **The
CMake build makes it for you**, from the same `apollo-patches` checkout the web
build uses (`./apollo-patches`, then `../apollo-patches`, or
`-DAPOLLO_PATCHES=...`), and copies it into the app — `Contents/Resources` for
a macOS `.app`, beside the executable elsewhere. It lands at
`build/apollo-patches.zip` too, which is next to `apollo_ctrl_test`, so
`--db` works with no environment variable.

It is optional and never fails a build: with no patch checkout, or no Python,
configuring says what is missing and carries on. The app then opens
`.savepatch` files by hand as before, and the browser explains itself.

Rebuilds are tracked — the glob over the patch files is `CONFIGURE_DEPENDS`, so
pulling new patches rebuilds the zip and refreshes the app's copy on the next
build. A build with nothing changed does neither.

Where it looks, in order: `$APOLLO_PATCHES_ZIP`, next to the executable,
`../Resources/` (so a macOS `.app` is self-contained), then the working
directory.

### The patch finds itself

Every title ID in the database is exactly nine characters, on every platform,
and a save folder is named after it with an optional suffix — `ULUS10391`,
`ULJM05500DATA00`, `UCUS98751_DATA01`. So **choosing a target usually identifies
the game**: the first nine characters of the folder are looked up in the
database, and for a PSP save the `SAVEDATA_DIRECTORY` out of `PARAM.SFO` is
preferred over the folder on disk, since that is what the console recorded and
it survives a rename.

With no patch open it loads outright. With one already open it only *offers* —
a line naming the game and a button — because loading closes the current
session along with any edited code bodies in it, and discarding somebody's work
to be helpful is not a trade worth making.

An exact match against a real title ID is the whole guard, which is what keeps
it quiet: across the 792 real PSP save folders in
[apollo-saves](https://github.com/bucanero/apollo-saves) it matches 23 and
nothing else — `Brave_Story_New_Traveler` and a folder called `Downloads` name
no game, and neither does `Copy of ULUS101890001`, since the id has to be at
the front. Those open a patch by hand as before.

Patches are read straight out of the zip, so there is nothing to unpack. The
Python helper modules are the exception — MicroPython's `import` goes through
`stat()`/`open()` on real paths — so on startup they are extracted to the
per-user cache directory and the engine is pointed at it with
`apctl_set_data_path()`. Without that call `DATA_PATH` is empty and the engine
resolves `python/` against the process's working directory, so a Python code
that imports a helper module would work only when the app happened to be
launched from the right place.

`index.tsv` inside the zip carries the game names, generated by
`tools/build-index.py` — the same script that builds the web front-end's index,
including the Windows-1252 fallback that 245 of the patch files need. Without it
the app would have to inflate 2247 entries at startup just to read their second
line.

## Browsing saves

**Browse saves...** (Ctrl+B, or File ▸ Browse saves...) is the way in. Point it
at wherever the saves are — a memory stick, a folder pulled off a PS3's hard
drive, a USB stick of PS4 exports — and it finds every save underneath and
lists them by game. Pick one and the target, the game key, the byte order and
the codes all follow. The alternative, *Find a game*, starts from the patch
database instead, which is what you want when the save is not on this machine
yet.

Finding them is the same question on all four consoles and has the same answer:
**a save is a folder with a `PARAM.SFO` in it**. Where that SFO sits is itself
the first half of the identification:

| Found at | Console | Encrypted by the console |
|----------|---------|--------------------------|
| `<save>/PARAM.SFO`, with `SAVEDATA_PARAMS` | PSP | yes, per-title game key |
| `<save>/PARAM.SFO`, without | PS3 | yes, `PARAM.PFD` |
| `<save>/sce_sys/param.sfo`, with `TITLE_ID` | PS4 | no |
| `<save>/sce_sys/param.sfo`, without | Vita | no |

The second half — which game — is where the four consoles stop agreeing, and is
the reason `core/saveinfo.c` exists rather than the app reading three keys and
guessing:

- **PSP and PS3** name no title ID at all. It is the first nine characters of
  `SAVEDATA_DIRECTORY` (`ULUS10391DATA00`, `BLUS30917-AUTOSAVE`), and the name
  is `TITLE`.
- **PS4** says `TITLE_ID` outright, and `MAINTITLE` is the game's real name —
  the only one of the four that stores it plainly.
- **Vita** says neither. `TITLE` is usually empty and there is no `TITLE_ID`
  key; the title ID is nine bytes at `0x28` inside the binary `PARAMS` blob,
  with `PARENT_DIRECTORY` (`/PCSE00608`) as the fallback. So a Vita save is
  **nameless** until the patch database is asked about its title ID, which is
  where the name in the list comes from.

A title ID is believed only when it is exactly nine characters of upper-case
letters and digits. A folder somebody renamed still gets listed under its own
name — it just offers no codes, which is the right answer, because the wrong
game's codes would be worse than none.

This is also what makes PS4 and Vita saves work at all: their folder is named
after the *slot* (`SLOT0`, `JOJOASB.S`), so the folder-name lookup described
above finds nothing. The browser passes the title ID it read from the SFO
along with the file, and that is what the database is asked about.

### What the walk does and does not do

- **Depth 8, 40 000 folders.** A PS3's savedata sits five levels down
  (`dev_hdd0/home/00000001/savedata/<save>`), so eight is generous for anything
  pointed at a card or a drive. Both limits are *reported* when they are hit —
  "3 folders sit deeper than the scan goes" — because a save missing from the
  list with nothing said about it looks exactly like "you have no saves", and
  that is the one failure nobody can debug.
- **A folder that is a save is not descended into.** Nothing below a save is a
  save, and a PS4 save's `sce_sys` would otherwise be examined in its own right.
- **Symlinked folders are skipped.** A link pointing back up its own tree would
  otherwise be walked until the depth cap stopped it, listing the same saves
  several times over.
- **A game's own `PARAM.SFO` is not a save.** Discs and homebrew EBOOTs carry
  one and they are all over a memory card; requiring `SAVEDATA_DIRECTORY` is
  what keeps them out of the list.
- **It runs on its own thread.** A memory stick scans in well under a second,
  but nothing stops somebody choosing their home directory, and a window that
  freezes for a minute looks broken rather than busy. There is a Stop button,
  and quitting mid-scan cancels rather than waiting for the disk.

The folder is remembered in the settings file and re-scanned in the background
at startup, so the list is there the next time rather than asking again.

The browser is two panes, and a modal cannot be wider than the window around
it — ImGui clips it rather than growing the window — so the app's own window
decides how much room the file list gets. It opens at 1180x820 for that
reason, clamped to the monitor's work area so the default cannot land partly
off a smaller screen, and both modals shrink with the window rather than
losing an edge.

### Which file inside the save

A save is a folder, and a patch addresses one file in it. The right one is
starred, and for the two encrypted consoles it is not a guess: the console's
own metadata says which files it wrapped — `SAVEDATA_FILE_LIST` in a PSP's
`PARAM.SFO`, the entry table in a PS3's `PARAM.PFD` — and that is the same list
that answers "which file comes out as garbage if you patch it as-is". PS4 and
Vita saves have no such list, so the largest file stands in; their saves are one
big file plus, occasionally, a small index beside it.

Everything else in the folder is listed too, with its size, so an unusual save
can still be opened by hand. The console's own metadata and artwork —
`PARAM.SFO`, `PARAM.PFD`, `ICON0.PNG`, `ICON1.PAM`, `PIC1.PNG`, `SND0.AT3`,
`sce_sys/` — is left out: none of it is ever the target, and on a PS3 the
artwork is *inside* the encrypted list, so leaving it in would have
Assassin's Creed suggesting its animated icon.

The last word belongs to the patch. A `.savepatch` carries target-file lines
(`:OPTIONS.DAT`), and once the game's codes are loaded they are asked: if the
patch names exactly one file, that file is in this save, and it is not the one
that was opened, the target moves to it and the log says so. Only when the
suggestion was taken — a file picked by hand is left alone — and only when the
answer is unambiguous, since plenty of patches address two files and have no
opinion about which to start with.

Across 176 real PS3 saves, 119 of which have codes: 101 suggestions already
matched the patch's own target line, 3 were corrected by it, and 15 were left
alone because the patch named several files or named one this save does not
have.

### Checking a folder from a terminal

`--scan` runs the same walk with no window, which is how the browser's
behaviour is tested — and is useful on its own for "what does this card
actually hold":

```bash
apollo_patcher_gui --scan /Volumes/PSP
```

Add a save's number to have it opened as well, which reports the target, the
patch, the code count, the byte order and the state of the encryption layer —
everything the main window would be showing had you clicked it:

```bash
apollo_patcher_gui --scan /Volumes/PSP 2
```

## PSP and PS3 saves

A PSP or PS3 save is encrypted twice. The console wraps it with a key of its
own, and the game encrypts what is inside that — and every `.savepatch`
addresses only the inner layer. So the console's wrapper has to come off first:
feed the engine a file copied straight off a Memory Stick or a hard drive and it
returns noise that looks like output.

The desktop app has something the web page does not: the folder. So none of this
is asked for. **Choose a target, and if the console's metadata sits beside it
and lists that file, a section appears** with everything already filled in.

|             | PSP                        | PS3                          |
|-------------|----------------------------|------------------------------|
| metadata    | `PARAM.SFO`                | `PARAM.PFD`                  |
| what is wrapped | `SAVEDATA_FILE_LIST`   | the PFD's entry table        |
| the key     | per title, `PSP/gamekeys.txt` | per file, `PS3/games.conf` |
| keyed by    | the save directory         | the save directory *and* the file name |

Both key databases travel in `apollo-patches.zip`. No network: the web page
fetches the same files from a CDN, the app carries them.

The metadata is what decides, not the file name. A save folder holds
`ICON0.PNG` too, and a target the metadata does not name gets no section at all
rather than an offer to decrypt something that was never encrypted. On the PS3
that answer is especially reliable — a game that encrypts nothing ships a
`PARAM.PFD` listing only `PARAM.SFO`, so no key database has to be consulted to
find out.

- **The checkbox** — *unwrap before patching, and put it back after* — is the
  main event, and defaults to on. Apply then takes the console's layer off, runs
  the codes, puts it back, and rewrites the metadata with the file's new hash.
  The order is not symmetric and not negotiable: wrapping first would encrypt
  the ciphertext.
- **Decrypt only / Re-encrypt** are for the other case — opening a save in the
  hex editor, or repairing one that a failed run left decrypted. Decrypt only
  turns the checkbox off, so a file that is already plaintext is not unwrapped
  twice.
- **Resign** regenerates the metadata's own hashes alone, leaving every file as
  it is. It needs no key.
- **Re-bind to your console** (PS3) appears once a console ID is named in
  Settings. See below.

Without a key nothing is guessed: the buttons and the checkbox stay disabled
rather than handing back noise. For a game the database does not cover, type 32
hex digits — or, on the PSP, load a dumper's file (SGKeyDumper's 16 bytes, or
SGDeemer's 1536).

A PS3 save also gets a check the PSP cannot offer: whether `PARAM.PFD`'s
recorded hash still matches the file on disk. A mismatch says so in the section,
because a save that already disagrees was damaged before it got here and
patching would sign the damage into place. It is only asked when the file is
still the length the console left it — one you already decrypted cannot match,
and flagging that would be crying wolf.

If Apply cannot put the layer back, it says so and names the state the file is
actually in — decrypted on disk — rather than reporting a generic failure. The
re-wrap runs even when a code failed, because the alternative is leaving the
user with something the console cannot read and no obvious way back.

The crypto is `../core/psp/` and `../core/ps3/`; see the
[top-level README](../README.md#the-consoles-own-savedata-encryption).

## Settings

**File ▸ Settings…** holds how saves are read and written. Everything in it is
optional, and every default is the safe one.

- **Byte order** — *Auto*, *Big-endian* or *Little-endian*. Auto is right for
  every patch in the database: PS3 saves are big-endian, everything else Apollo
  covers is not, and the database's own platform tag says which a patch is.
  Force one only for a loose patch file for a console it does not cover.

  A forced order is remembered across runs and applies to every save, which is
  the point of it and also its only hazard — a forced big-endian left set will
  byte-reverse a PS4 or Vita save and hand back something that looks patched.
  The main window therefore always says which order is in effect and why, in
  amber when a forced one disagrees with the patch you have open, and the log
  repeats it at Apply.

The other two name the console a save is written *for*. Both change only what
is WRITTEN, and leaving them blank keeps whatever a save already says — which
is what patching one in place wants.

- **PSP, Fuse ID** (16 hex digits). Savedata modes 4 and 6 derive two
  `PARAM.SFO` hashes from the console's own fuse. A PSP loads a save whose
  values differ, so this only matters for reproducing one console's output byte
  for byte.
- **PS3, console ID / IDPS** (32 hex digits, plus a user number). Inside
  `PARAM.PFD`, one of `PARAM.SFO`'s four hashes is keyed by the IDPS of a single
  machine — that is what binds a save to a console. Name one and the PS3 section
  offers **Re-bind to your console**, which rewrites that hash and re-signs the
  database around it. The user number reaches only a trophy folder's hashes.

Both hex fields are all-or-nothing: a half-typed value is not "no value", it is
one that would bind a save to the wrong machine, so Save stays disabled until
each is empty or complete. The byte order has nothing to validate, so it takes
effect and is saved the moment it is picked.

Re-binding is the `PARAM.PFD` half of moving a save between consoles. A save
also carries account fields in its own `PARAM.SFO`, and those are not touched.

The saves folder is in that file too, though nothing in this dialog sets it:
choosing one in the save browser writes it there straight away. It is the one
setting somebody changes by *using* the app, and having to re-find a memory
stick every launch is the thing the browser exists to stop.

Settings live in the user's config directory —
`~/Library/Application Support/apollo-patcher/settings.txt` on macOS,
`$XDG_CONFIG_HOME/apollo-patcher/` on Linux, `%APPDATA%\apollo-patcher\` on
Windows — because they describe the person's console rather than this copy of
the program. It is the only file the app writes there; ImGui's own `.ini` is
deliberately off.

## Opening things

```bash
apollo_patcher_gui [FILE...]
```

Files on the command line are the other way in, alongside **Browse saves...**
above. A `.savepatch` is opened as the patch; anything else is opened as the
save to patch, which pulls in everything that follows from it — the PSP game
key, and that game's patch from the database. Arguments are taken in the order given, so
an explicit patch beats the one a save's title ID would have auto-loaded,
whichever way round they are written. `--help` prints this and exits, and
`--scan DIR [N]` lists the saves under a folder without opening a window (see
[Browsing saves](#browsing-saves)).

**A folder works too**, which is the obvious thing to drag for either console's
save: its `PARAM.SFO` or `PARAM.PFD` already says which files the console
encrypted, and the first of those becomes the target. So

```bash
apollo_patcher_gui /Volumes/PSP/PSP/SAVEDATA/ULUS10391
```

opens the save, finds its key, and loads Monster Hunter Freedom Unite's patch —
from the folder alone, and a PS3 folder takes the same route. A folder that is
neither says so rather than becoming a target nothing can read.

Files can also be **dropped on the window**, which takes the same route. Note
that on macOS a `.app` launched from Finder is handed documents through Apple
Events rather than `argv`, so dragging onto the Dock icon is not the same
gesture and is not wired up — the command line covers a terminal, a script and
a Windows/Linux file association, and the drop handler covers the window.

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

  **Runs as** in the same window picks the interpreter — Save Wizard, BSD or
  Python — and takes effect immediately, since a wrongly-typed code is often
  the whole problem and has nothing to type. The loader takes the type from a
  `[SW:…]` / `[BSD:…]` / `[PYTHON:…]` title prefix when there is one, and
  otherwise from the shape of the body (Save Wizard only when *every* line is
  exactly `XXXXXXXX YYYYYYYY`) — so one mistyped line is enough to land a code
  on the wrong interpreter, and this is how it gets corrected. It counts as an
  edit, *Revert to file* puts the declared type back with the body, and saving
  the patch writes the choice into the title.

  What an edit cannot do is rename a **`{TAG}`** — the engine writes an
  option's value over the tag in place, at the tag's own length, so a tag that
  has been retyped or deleted stops resolving. The window says so when a
  placeholder goes missing, since nothing else would until the patch
  misbehaved.
- **Save patch file…** (also File ▸ Save .savepatch as…, Ctrl+S) writes the
  patch back out with your edits in it, so a hand-modified code can be kept or
  shared. The engine splices the edits into the *original* bytes instead of
  regenerating the file from its parse, so comments, credits, `:file` lines and
  option blocks come through untouched — the parse keeps codes and drops all of
  that. It then re-reads the file it just built and logs any code that would
  come back different. A forced type is written as a title prefix (`[SW:…]`,
  `[BSD:…]`, `[PYTHON:…]`) and does survive — but a title carries only one
  marker, so a code already flagged `[DEFAULT:…]` or `[INFO:…]` has no room to
  state one, and that is what gets reported.
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
- **Byte order**, the equivalent of the `patcher` CLI's `-b`/`--big-endian`
  flag. Detected per patch by default and overridable in Settings; the main
  window shows which order is in effect and why. It calls
  `apollo_set_endianness()` before each code is applied, so a single build
  handles both byte orders — no separate big-endian binary. See
  [Settings](#settings).

- **PSP and PS3 savedata**: the consoles' own encryption, detected from the
  save folder and taken off and put back around Apply, plus a Settings panel
  naming the console a save is written for. See
  [PSP and PS3 saves](#psp-and-ps3-saves)
  above.
- **Browse saves**: point the app at a folder and it finds every PSP, PS3, PS4
  and Vita save under it, lists them by game, and opens the one you pick —
  target, key, byte order and codes together. See
  [Browsing saves](#browsing-saves).
- **The patch finds itself**: choosing a target looks its title ID up in the
  bundled database and loads that game's patch, or offers it when one is
  already open.
- **Files on the command line and dropped on the window**, including a save
  folder. See [Opening things](#opening-things).

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
