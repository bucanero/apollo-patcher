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
  ../core/psvcard.[ch]             the .PSV container holding a PS1 or PS2 save
  ../core/mcicon.[ch], ../core/ps2/  ...and its icon, which has to be rendered
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

Besides the patches, the zip carries three generated or copied tables:
`index.tsv` (game names per patch), the two console key databases
(`PSP/gamekeys.txt`, `PS3/games.conf`), and **`titles.tsv`** — 8783 game names
by title ID, normalised by `make-bundle.py` from apollo-patches'
`psptitleid.txt` and `psvtitleid.txt`. That last one is what names a save the
patch database has never heard of; see [Browsing saves](#browsing-saves).
It costs about 110KB in the zip.

`ps1titleid.txt` and `ps2titleid.txt` are deliberately left out — nothing
browses a PS1 or PS2 save yet and they are another 250KB. `TITLE_DBS` in
`make-bundle.py` is where to add them, with the note there that those two are
Windows-1252 rather than UTF-8.

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
it survives a rename. A PS4 or Vita save's folder is named after the *slot*
and carries no title ID at all, so the saves screen passes the one it read
from `sce_sys/param.sfo` along with the file — see
[Browsing saves](#browsing-saves).

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

## Two screens

The app opens on **your saves**, not on a file picker.

| Screen | | |
|---|---|---|
| **Saves** | Ctrl+B | The list. Point it at a folder and every save underneath turns up, by game. |
| **Patcher** | Ctrl+P | One save: its icon, which of its files is the target, the console's encryption, the codes, Apply. |

Picking a save goes to the patcher; **< Saves** comes back. Going back closes
nothing — the patch stays open, so you can look at the list and return.

There is no third screen for driving files by hand, because everything after
*"which file, and which patch"* is the same work: the code list, the option
dropdowns, Apply, the log, the hex editor. **File ▸ Advanced** is a *door* into
the patcher screen rather than a room of its own — it swaps the save header for
**Open .savepatch...** and **Choose target...** so a loose file or a patch of
your own can be driven by hand. Everything below is identical either way.

A screen swap rather than a pop-up, for a concrete reason: an ImGui modal
*"blocks every interaction behind the window"*, and the hex editor and the
per-code editors are windows behind it. As a modal, the patcher would have made
its own hex editor unreachable.

### Browsing saves

Point it at wherever the saves are — a memory stick, a folder pulled off a PS3's
hard drive, a USB stick of PS4 exports — and it finds every save underneath and
lists them by game, slot, console, title ID, and whether the database has codes.
Pick one and the target, the game key, the byte order and the codes all follow.
*Find a game* starts from the patch database instead, which is what you want
when the save is not on this machine yet.

Finding them is the same question on four of the six consoles and has the same
answer: **a save is a folder with a `PARAM.SFO` in it**. Where that SFO sits is
itself the first half of the identification:

| Found at | Console | Encrypted by the console |
|----------|---------|--------------------------|
| `<save>/PARAM.SFO`, with `SAVEDATA_PARAMS` | PSP | yes, per-title game key |
| `<save>/PARAM.SFO`, without | PS3 | yes, `PARAM.PFD` |
| `<save>/sce_sys/param.sfo`, with `TITLE_ID` | PS4 | no |
| `<save>/sce_sys/param.sfo`, without | Vita | no |

**PS1 and PS2 are neither a folder nor an SFO.** Those consoles kept saves in
memory-card blocks and wrote no `PARAM.SFO` at all — the format postdates them —
so a save only becomes a file when a PS3 exports it as a signed `.PSV`
container. The walk therefore looks at files as well as folders, and a `.PSV` is
identified from what is inside it: the save's own memory-card directory name,
its file list, and the name the console's save list showed. See
[the top-level README](../README.md) for the container itself.

The second half — which game — is where the consoles stop agreeing, and is
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
  **nameless** and has to be looked up by title ID.

For that last case the name comes from elsewhere, in this order:

1. **the save's own `PARAM.SFO`** — what the console itself shows, so it wins
   whenever there is one.
2. **`titles.tsv`** in the bundle, which `make-bundle.py` normalises from
   apollo-patches' `psptitleid.txt` and `psvtitleid.txt`: 8783 games by title
   ID, 4581 of them Vita.
3. **the patch database**, which names only the games it has patches for — 123
   Vita titles.

The catalogue sits above the patch database on purpose. It is a catalogue,
keyed exactly by title ID, where a patch's name is whatever its author wrote on
the file's second line and carries region suffixes and inconsistencies. More to
the point it covers 37× as many Vita games, so a save for a game nobody has
written codes for is still listed under its real name instead of `SLOT0`.

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
- **Neither is DLC.** `CATEGORY` says what an SFO *describes*, and exactly one
  value per console means savedata — `MS` on a PSP, `SD` on a PS3, `sd` on a
  PS4 or Vita. Everything else is something else: `ac` is add-on content, `gd`
  is game data.

  This matters most for the Vita, whose DLC folders carry their own
  `sce_sys/param.sfo` **with a `TITLE_ID` in it** — and a `TITLE_ID` beside a
  Vita-shaped SFO is the single thing that otherwise distinguishes a PS4 save
  from a Vita one. Run over the
  [apollo-saves](https://github.com/bucanero/apollo-saves) database, 80 DLC
  folders across 7 archives were being listed as PS4 saves. All 2,566 real
  saves in it carry one of the three savedata categories and nothing else
  does.

  The comparison ignores case, since the same two letters are upper on a PS3
  and lower on a PS4. A file with **no** `CATEGORY` falls through to the
  key-set tests rather than being refused — every real save measured has one,
  but refusing a save over a key it merely omits would be a worse failure than
  the one this fixes.
- **It runs on its own thread.** A memory stick scans in well under a second,
  but nothing stops somebody choosing their home directory, and a window that
  freezes for a minute looks broken rather than busy. There is a Stop button,
  and quitting mid-scan cancels rather than waiting for the disk.

The folder is remembered in the settings file and re-scanned in the background
at startup, so the list is there the next time rather than asking again. With no
folder set, the saves screen says what to do instead of showing an empty table.

Going back to the list never loses anything, and opening a *different* save is
the one place work could go — it closes the patch that holds any edited code
bodies. `apctl_code_is_edited()` makes that detectable, so it is asked rather
than assumed.

The window opens at 1180x820, clamped to the monitor's work area so the
default cannot land partly off a smaller screen. The width matters because a
modal cannot be wider than the window around it — ImGui clips it rather than
growing the window — so the patch database browser and the rest size to what
the window gives them, and shrink with it rather than losing an edge.

### Which file inside the save

A save is a folder, and a patch addresses one file in it. The **File** dropdown
on the patcher screen is where that is chosen; the right one is starred, and for
the two encrypted consoles it is not a guess: the console's
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

A PS1 or PS2 save's files are not on disk — they are inside the `.PSV`, and the
dropdown lists them from the container with their sizes exactly as it lists a
folder's. Choosing one **extracts** it to a scratch copy under the app's cache
directory, which is what the hex editor, the code viewers and the patch engine
then see; applying puts it back and re-signs the container. Nothing above that
line knows a container is involved, which is the point.

The starred suggestion skips a memory-card save's own presentation — `icon.sys`
and the `.ico` files, which every one of the 2,641 real PS2 containers carries
and which no code has ever addressed — and offers the largest of what is left.
A PS1 container holds exactly one file, its whole memory-card block, so there is
nothing to choose.

### The icon

The picture the console's own save list shows — `ICON0.PNG` beside the data
files on a PSP or PS3, `sce_sys/icon0.png` on a PS4 or Vita — appears beside
the game's name on the patcher screen, and in the hover panel on the list.
Both spellings of each are tried, because the case is per console and only
some filesystems care: a save copied to a Mac and then opened on Linux is the
one that would otherwise lose it.

**PS1 and PS2 saves have no such file**, and their icons are drawn rather than
decoded. A PS1 save's is sixteen colours packed four bits to a pixel inside its
own first block, 16x16, with up to three animation frames; a PS2 save's is a
textured **3D model** — one to three of them, named by `icon.sys`, which also
supplies the three directional lights and the ambient term. `core/ps2/` is
apollo-ps4's parser and software rasteriser, ported, so this needs no GL context
of its own.

**A PS2 icon animates.** It is a model with up to eight morph targets, and the
app plays them: the pose is re-rendered every frame, interpolating between
shapes on the loop the file itself states. Most icons do not move — 1,898 of
the 2,345 in the save database carry a single shape — so `animated` is checked
first and the common case costs nothing.

**Hovering the icon on the patcher screen shows it bigger**, still animating,
at 256px against a transparent background. That is the closest this gets to
what the save looked like on a television, and it is rendered only while the
pointer is actually on it.

All of it happens on the CPU and arrives as an ordinary texture, so it needs no
depth buffer, no shaders and no second GL context — which matters, because the
floor here is OpenGL 1.1 and one supported configuration is a software
rasteriser over Remote Desktop. Both render sizes are powers of two for the
same reason. It is affordable: 128px at 4x supersampling measures 1.4ms against
a 16ms frame, and 256px 4.8ms.

**An icon is drawn whole or not at all.** Either the 3D model reads, or — when
its geometry does not fit its file but the texture survived intact — that
texture is shown flat. There is no third option, and in particular no partial
model.

That was tried, and it was the wrong answer. The one damaged icon in the save
database declares 1,770 vertices in a file with room for 1,159; drawing the
two thirds that survive produces a clean, confident silhouette that tells the
person looking at it that nothing is the matter. Something truncated that file,
and it was under no obligation to stop at the icon. So the app says so instead,
in colour, on the patcher screen and in the list's hover panel: *this save's
icon is damaged, and the save data may be too*.

Measured over the 2,647 real containers: **2,563 icons render** as models, 69
saves carry none at all, and **15 are damaged** — all the same Action Replay
MAX file. None fall back to a flat texture, because that file's texture is in
the missing third; the fallback is covered by a built fixture in
`core/test_psv.c` rather than by anything real, which is worth knowing.

It is decoded when the selection changes, not during the scan. A folder of 176
saves is 176 PNGs, and uploading all of them would be 40MB of texture for a
list that shows one at a time; re-decoding a 320x176 image on each click costs
a fraction of a millisecond.

`core/png.c` does the decoding rather than a vendored library, because the job
is small and the input is narrow. Measured over **every `.png` in the
[apollo-saves](https://github.com/bucanero/apollo-saves) database** — all
5,560 of them, entry art and everything inside the archives, across all six
consoles:

| | files | |
|---|---|---|
| 8-bit RGB | 4,316 | |
| 8-bit RGBA | 940 | |
| 8-bit palette | 237 | |
| 8-bit RGB, **interlaced** | 12 | refused |
| not a PNG at all | 55 | |

Every real PNG among them is **8 bits per channel** — not one 16-bit, 1-, 2-
or 4-bit file in 5,505 — so the whole non-interlaced format is handled anyway
(palettes, greyscale, 1/2/4/16-bit, `tRNS`), each a few lines. Adam7
interlacing is refused outright, being the one thing in the format that would
double the size of that file.

**5,492 of the 5,560 decode.** The 68 refused break down as:

| | files | |
|---|---|---|
| not PNG data | 41 | Vita thumbnails archived without decrypting — entropy 8.00 |
| macOS AppleDouble stubs | 13 | `__MACOSX/._ICON0.PNG`, not icons at all |
| interlaced | 12 | valid; see above |
| `IDAT` fails its CRC and will not inflate | 1 | `PS3/NPUB30720/00000001.zip`'s copy |
| zero-byte | 1 | |

So **three are genuinely lost images** and the rest are either not pictures or
not supported. An independent decoder refuses the same ones. Six are an
`ICON0.PNG` the app would actually show — which is why a failed icon is a
caption and not an error: the row still lists, and says *"(the icon is …)"*.

Two near-misses worth recording, because both looked worse than they were.
Five files have a complete, CRC-clean `IDAT` and a mangled 12-byte tail
reading `HEND` rather than `IEND`; they decode everywhere, since `IEND` carries
no pixels. And 544 have bytes appended *after* `IEND`, which is what a
conformant decoder is supposed to ignore. Neither is damage, and an audit that
checks the chunk walk before the image data will report both as truncation.
zlib
was already linked for the engine's own use.

The texture is **padded to a power of two** and drawn with UVs that cut the
padding back off. OpenGL 1.1 is the floor this app targets — it is exactly what
Microsoft's software renderer offers on a GPU-less or Remote Desktop host — and
1.1 takes only power-of-two textures. No save icon is one: they are 320x176,
228x128, 144x80. Without the padding the icon would silently fail to appear on
precisely the machines least able to explain why.

### Checking a folder from a terminal

`--scan` runs the same walk with no window, which is how the browser's
behaviour is tested — and is useful on its own for "what does this card
actually hold":

```bash
apollo_patcher_gui --scan /Volumes/PSP
```

Add a save's number to open it as well — the same call the list makes — which
reports the save, its files, the target, the patch, the code count, the byte
order and the state of the encryption layer. What it prints is what the patcher
screen would be showing had you clicked it:

```bash
apollo_patcher_gui --scan /Volumes/PSP 2
```

`--open` takes one path the way a **dropped file** does and reports the same,
which is how that path is tested:

```bash
apollo_patcher_gui --open /Volumes/PS3/PS3/SAVEDATA/BLUS30917-AUTOSAVE
```

A dropped save **folder** goes through the browser's own identification, so it
arrives named, iconned, with its files listed and its codes loaded — the same
thing as one picked from the list. A loose file has no save behind it, so the
patcher shows the pickers instead of a save header.

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

## The font

Game names are whatever the game wrote, and they are not ASCII. Measured over
every name and slot label in the [apollo-saves](https://github.com/bucanero/apollo-saves)
database — **6,243 strings** out of 2,648 `PARAM.SFO` files — **1,616 of them
(26%) contain a character outside ASCII**: 877 distinct codepoints, 14,768
occurrences. The distribution is not what a Latin-1 font would suggest:

| | occurrences |
|---|---|
| Kana | **7,627** |
| CJK ideographs | 3,066 |
| Fullwidth forms | 1,615 |
| Ideographic space and CJK punctuation | 1,246 |
| Latin-1 supplement (`®`, accented Latin) | 429 |
| `™` and other letterlike | 284 |
| Cyrillic, Greek, symbols (`★ ♪ ♂ ⚙ ↑`), Arabic | ~500 |

Japanese is not a footnote here, it is the bulk of it — a quarter of every
name in the database needs more than Latin-1.

ImGui's built-in ProggyClean covers U+0020–00FF and nothing else, so `®` drew
fine and `™` drew as `?`. The app therefore **ships a font**:

1. **`$APOLLO_FONT`** — a `.ttf` or `.otf` named outright, for anyone who
   wants a different face.
2. **`NotoSansJP-Medium.otf` beside the app** — `Contents/Resources` for a
   macOS `.app`, next to the executable elsewhere. The same two places the
   patch database is looked for, and what happens in practice.
3. **ImGui's own**, as a safety net that should never fire. Reaching it means
   the copy beside the app is missing, so the log says so rather than quietly
   looking wrong.

There is deliberately **no system-font tier**. Falling back to a per-OS list
(Arial, Segoe UI, DejaVu) would work, but the app would then look different on
every machine — and since every column width comes from `CalcTextSize`, even
its proportions would move. It would also hide a broken build behind something
that looked almost right. One vendored font is one appearance and one tested
path.

Asked of the built atlas, one codepoint at a time, Noto Sans JP with the ranges
the app bakes has a glyph for **827 of the 877** — and weighted by how often
each actually turns up, **14,596 of 14,768 characters, or 98.84%**.

The 50 it misses are worth naming, because each is a deliberate limit rather
than an oversight:

| missing | codepoints | occurrences | why |
|---|---|---|---|
| rare CJK ideographs | 28 | 109 | outside ImGui's common-use Japanese set; the full range would balloon the atlas |
| Arabic | 11 | 12 | see below |
| enclosed alphanumerics (`ⓒ ⑴`), geometric shapes (`■`) | 4 | 21 | ranges not baked — two lines would close it |
| an emoji, a variation selector, four others | 7 | 30 | not UI-font material |

**Arabic is left out on purpose.** ImGui does no bidirectional layout and no
contextual shaping, so baking the glyphs would draw Arabic left-to-right in
isolated letterforms — confidently wrong rather than visibly missing. Twelve
characters in the whole database is not worth a wrong answer.

The Japanese range is requested whichever font is found, because asking costs
nothing when the font has no such glyphs — Arial comes out at the same 1015
glyphs and 0.2MB either way. So there is no need to ask a font what it
contains before asking it for something.

The font is **vendored** at `gui/assets/fonts/`, with its SIL Open Font
License beside it (redistribution requires it). That is a deliberate exception
to this repo's habit of finding siblings rather than copying them: `apollo-lib`
and `apollo-patches` are code and data you want to update independently, where
this is a frozen asset the app needs in order to render correctly. It is the
same file `apollo-psp` ships.

`-DAPOLLO_FONT_FILE=` ships a different face instead. A build with no font
still succeeds — with a CMake warning, because the checkout is then incomplete
— and CI treats a missing font in the artifact as an error.

Text is drawn at **20px** rather than ProggyClean's 13: a bitmap font at its
design size is crisp where an outline font at 13 is muddy, and this app is read
more than it is clicked. Every column width comes from `CalcTextSize`, so the
layout follows the size rather than having to be retuned for it.

Size is what drives the atlas, not the glyph count, and it is a step rather
than a slope: the same ~4000 glyphs fit 1024×1024 up to and including 16px and
need 1024×2048 from 17 — 1MB against 2MB of alpha texture. Having paid that,
20px costs no more than 18.

### The second font, for the hex editor

Noto is the right font for names and the wrong one for a hex dump. The memory
editor sizes its entire grid from `CalcTextSize("F").x` and then draws every
other character in that one cell, which only holds if the advances match. In
Noto at 20px they do not:

| | advance |
|---|---|
| `0`–`9` | 7.87 — tabular, so decimal lines up |
| `A`–`F` | 7.82 (`F`) to 9.65 (`D`) |
| `i` / `W` | 3.96 / 12.35 |

So hex columns drift and the ASCII pane stops lining up with the bytes above
it. The fix is a fixed-pitch font, and the one already in the family is the
**10×20 console raster font** that `apollo-ps3`, `apollo-ps4` and
`apollo-vita` draw with, from
[idispatch/raster-fonts](https://github.com/idispatch/raster-fonts).

It was picked over a monospace outline face (Cousine, which ImGui ships, comes
out at 10.59×20 — near enough the same box) for three reasons. A bitmap is
drawn rather than rasterised, so it is crisp at exactly the small dense sizes
where antialiasing does the most damage. Its advance is an integer 10, so 16
columns of hex land on integer pixels instead of accumulating a fraction. And
the app already renders at a 20px line height, so a 20px cell sits in the same
rhythm as everything around it.

`tools/make-font.py` converts the upstream file — 180KB of commented C
for 10KB of bitmap — into `gui/src/font10x20.h`, keeping only the 95 printable
ASCII glyphs at 3,800 bytes. The parser matches each byte pair together with
the bit-pattern comment beside it and fails if the two disagree, so a parse
that drifted would have to drift in both at once. The codes above `0x7E` are
**deliberately dropped**: they are CP437 box drawing, which is not what
Unicode U+0080–00FF means, so registering them would draw the wrong character
rather than none.

ImGui has no notion of a bitmap font, so `load_mono_font()` goes the long way
round: a font whose own glyph range is a single character nobody draws
(U+0020, from ProggyClean), with the 95 glyphs added as **custom atlas
rectangles** and their pixels written in by hand after packing. Custom rects
are registered last and the lookup table takes the last glyph for a codepoint,
so nothing of ProggyClean shows. A custom-rect glyph sits at its
`GlyphOffset`, which defaults to `(0,0)` — the top-left of the text line — so
a full-height 20px cell lands on a 20px line with no nudging. `Build()` is
called explicitly rather than left to the backend, because the pixels have to
be written before anything reads the texture; the GL backend then asks for
RGBA32, which converts from that same alpha buffer and only rebuilds if it is
absent.

The whole thing costs 95 glyphs, 19,000 atlas pixels and no change to the
atlas size, which stays 1024×2048.

### Where it is used, and where it isn't

Being ASCII-only, the font cannot simply replace Noto everywhere a fixed pitch
would help. The savepatch code viewers and the raw patch view have the same
column drift as the hex editor, but of the 2,247 files in the patch database
**246 are not valid UTF-8 at all** (CP1252 quotes and dashes, which ImGui
already draws as `?` whatever the font) and another 82 carry curly quotes,
accented Latin, `™`, katakana, CJK or fullwidth forms. A blanket switch would
trade a cosmetic problem for rows of `?`.

So the choice is made per piece of text, by `is_plain_ascii()` — every byte
printable ASCII, with tab, newline and carriage return allowed through as
layout. Measured over the database:

| | fixed pitch | falls back to Noto |
|---|---|---|
| code bodies | **80,092 of 80,094** (100.0%) | 2 |
| whole patch files | 1,919 of 2,247 (85.4%) | 328 |

The split is not a coincidence: non-ASCII in a `.savepatch` lives almost
entirely in **names and author comments**, which the code viewer does not
show. So in practice every code body gets the fixed-pitch font, the raw view
gets it for six files in seven, and nothing anywhere degrades to `?`.

The answer is **cached**, not asked per frame — a patch file runs to 430KB and
a single body to tens of KB. `AppState::CodeEdit::set()` recomputes it on
every assignment, and an edit re-asks, because pasted text can bring in a
character the font has no glyph for. The push and the pop read one local so
the font stack stays balanced even when an edit flips the answer mid-widget.

`is_plain_ascii()` deliberately asks about the **text and not the font**, and
never looks at `g_mono`. An earlier version checked the font there and got it
wrong in a way worth recording: a patch named on the command line, dropped on
the Dock or opened from Finder is loaded by the argument loop, which runs
*before* the window and its fonts exist — so `g_mono` was still null, every
such patch latched to "cannot draw", and the raw view stayed proportional for
the rest of the run. The font is checked where it is pushed instead.

The hex editor's own window is the one place the font is pushed around part of
a window rather than all of it: the path and file name above the grid can hold
anything, so only the grid gets it. All three of these windows also had their
identities fixed while this was going in — see
[One window, one identity](#one-window-one-identity).

### Fitting the hex window

The hex window used to open at a hard-coded 700×520 and cut off the right-hand
ASCII pane. Under Noto the editor was laying its grid out on
`CalcTextSize("F").x + 1` = 8.82px cells while a `W` drew 12.35px wide, so the
text spilled past the width the editor thought it needed — 700 looked like
plenty and wasn't.

With a fixed pitch the arithmetic is exact, so the window is now sized from
`MemoryEditor::CalcSizes()` rather than guessed at. Measured, with the address
column growing as the file does:

| file | address digits | width |
|---|---|---|
| 64 B | 2 | 716 |
| 2 KB | 3 | 727 |
| 64 KB | 4 | 738 |
| 4 MB | 6 | 760 |

The width has to be measured **with the fixed-pitch font pushed and before
`Begin()`**, since `SetNextWindowSize` applies to the next window rather than
the current one. Height follows the file, clamped to 8–24 rows so a 64-byte
`PARAM.SFO` does not get the same window as a 4MB save, and both dimensions
are clamped to the viewport.

Refitting happens on `ImGuiCond_FirstUseEver`, plus once more whenever a
**different** file is loaded — `hex_load()` compares the path and sets
`hex_fit`. Reloading the same file from disk deliberately does not refit, so a
size the user chose survives.

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
  The patcher screen therefore always says which order is in effect and why, in
  amber when a forced one disagrees with the patch you have open, and the log
  repeats it at Apply.

The other two name the console a save is written *for*. Both change only what
is WRITTEN, and leaving them blank keeps whatever a save already says — which
is what patching one in place wants.

- **PSP, Fuse ID** (16 hex digits). Savedata modes 4 and 6 derive two
  `PARAM.SFO` hashes from the console's own fuse. A PSP loads a save whose
  values differ, so this only matters for reproducing one console's output byte
  for byte.
- **PS3, account ID** (16 hex digits) — your PSN account, and **usually the
  one to reach for**. It is written into the save's own `PARAM.SFO`, so the
  save loads on *any* PS3 that account has signed in to rather than on one
  machine. Name one and the PS3 section offers **Sign to your account**.
- **PS3, console ID / IDPS** (32 hex digits, plus a user number). Inside
  `PARAM.PFD`, one of `PARAM.SFO`'s four hashes is keyed by the IDPS of a single
  machine — that is what binds a save to a console. Name one and the PS3 section
  offers **Re-bind to your console**, which rewrites that hash and re-signs the
  database around it. The user number reaches only a trophy folder's hashes.

Both hex fields are all-or-nothing: a half-typed value is not "no value", it is
one that would bind a save to the wrong machine, so Save stays disabled until
each is empty or complete. The byte order has nothing to validate, so it takes
effect and is saved the moment it is picked.

The two are independent and both can be used. Re-binding is the `PARAM.PFD`
half of moving a save; signing to an account is the `PARAM.SFO` half.

**Signing to an account** writes the ID into *both* places `PARAM.SFO` keeps
it — the `ACCOUNT_ID` field and the copy at offset `0x30` inside the binary
`PARAMS` blob. Writing one and not the other would leave the save disagreeing
with itself, and saves in that state are not hypothetical: across the **632
PS3 saves** in apollo-saves the two fields agree 587 times and **disagree 42**.

| | |
|---|---|
| `ACCOUNT_ID` zeroed, `PARAMS` still names one | 28 |
| `PARAMS` zeroed, `ACCOUNT_ID` still names one | 13 |
| two genuinely different accounts | 1 |

Forty-one of the 42 are half-unsigned — a tool cleared one field and left the
other. Which is exactly what writing both prevents.

**Reading** them is the other way round, and deliberately asymmetric:
`apfd_sfo_account_id()` takes `ACCOUNT_ID` whenever the key is present, and
falls back to the `PARAMS` copy only when it is missing or too short — *not*
when it is present and zeroed. So a save whose `ACCOUNT_ID` has been cleared
reports no owner even though `PARAMS` still holds the old one. That is the
right answer: `ACCOUNT_ID` is the documented field, `PARAMS+0x30` is a blob
offset recovered by reverse engineering, and on a save somebody unsigned for
sharing the leftover in `PARAMS` is a remnant rather than a claim of
ownership. Reporting it as the owner would put a stranger's name against 28 of
these saves. The PFD's hash of `PARAM.SFO` is taken over those bytes, so it is
recomputed and the database re-signed in the same action — stopping half way
leaves a save that will not load at all.

`PARAM.SFO` is written before `PARAM.PFD` deliberately: if the second write
fails, the save is one **Resign PARAM.PFD** away from correct rather than
silently mismatched, and the log says so.

**Which account a save is signed to** is read during the scan, for every save
on a console that has the concept, and shown in three places: an **Owner**
column in the save list, the hover panel, and the patcher screen's save
header. The column appears only once
an account is named in Settings — which is exactly when the question has an
answer worth a column.

Its cells carry a **check mark** when the save is yours and nothing when it is
not — the same tick the View menu puts beside *Saves* and *Patcher*, drawn by
`draw_check_mark()` with `MenuItemEx()`'s own sizing copied so the two cannot
drift apart. Marking only the matches keeps the column scannable: what the eye
runs down it for is the saves that **are** yours, and a word in every row would
bury those among the rest. Whose a save is instead — and whether it names an
account at all — is in the hover panel, which reads *"Signed to account … —
yours"* or *"— not yours"*.

It is read during the scan rather than when a save is opened because that is
when the question gets asked: *which of these are mine?* Reading it at open
time would have missed every save whose game encrypts nothing, since the PS3
section that would have shown it is hidden for those.

`--scan` and `--open` report it too, as `account: … (yours)`. Getting that
working turned up a separate bug: both modes returned from the argument loop
**before** `settings_load()` ran, so they answered every question against a
blank configuration — no account, so never "yours", and no console ID either,
so the console layer looked unavailable when it was configured. A file named
on the command line or handed over by Finder is opened in that same loop, so
it had the same problem, and the same save opened a second later through the
browser would behave differently. Settings are now loaded before the loop.

#### The same number, written two ways

The PSN account ID is one 64-bit number and it is the same number on every
console that carries it, which is why **one** field in Settings serves all of
them. What differs is how a save writes it down:

| | key | stored as | read by |
|---|---|---|---|
| **PS3** | `ACCOUNT_ID`, and again inside `PARAMS` at `+0x30` | **16 bytes** of ASCII hex | `apfd_sfo_account_id()` |
| **PS4** | `ACCOUNT_ID` in `sce_sys/param.sfo` | **8 raw bytes**, little-endian | `asfo_account_id()` |
| **Vita** | the same | the same | the same |
| **PSP** | — | no account in `PARAM.SFO` | never asked |
| **PS1, PS2** | — | no such concept | never asked |

The trap is that a PS3 and a PS4 use the **same key with the same binary
format code** and mean entirely different things by it. Only the length tells
them apart, so each reader checks it and returns an error on the other's file
rather than reading a number that is not there — `asfo_account_id()` on a PS3
save gives `ASFO_ERR_FORMAT`, not a plausible-looking 64 bits of ASCII. Both
directions are covered in `test_save.c`.

An `ACCOUNT_ID` of **zero** is left as "no account" rather than reported as an
owner. It is what a decrypted or shared save usually carries, and treating it
as an owner would mark every one of those as somebody else's.

#### Measured against the save database

The whole of [apollo-saves](https://github.com/bucanero/apollo-saves) —
**4,834 archives** across PS1, PS2, PS3, PS4, PSP and Vita — with every
`PARAM.SFO` read straight out of its zip, giving **2,648** of them. Each was
classified by this reader and by an independent one written from the format
spec. The two agree on **all 2,648**:

| | files | |
|---|---|---|
| 16-byte ASCII (PS3) | **632** | 321 name an account, 311 are all-zero |
| 8-byte binary (PS4, Vita) | **672** | 624 name an account across 155 distinct ones, 48 are zero |
| no `ACCOUNT_ID` | 1,342 | PSP saves, and application/DLC SFOs |
| rejected as malformed | 2 | |

Reading the archives needs one thing beyond Python's `zipfile`: two use
**Deflate64**, which it does not implement, so the sweep falls back to `unzip`
for those. With that in place nothing in the database failed to open.

Three rows are worth drawing out, none of which was in the synthetic tests:

**Ten of the 16-byte ones were PS3 saves living inside a Vita archive** — a
PS1 Classic (`NPEB01899`) filed under `PSV/PCSB00560`, carrying a PS3-format
`PARAM.SFO` at its root. Precisely the collision the length check exists for,
sitting in the real corpus rather than only in a test. (They have since been
moved to `PS3/NPEB01899`, but the case they proved stands: nothing about a
directory's name can be trusted to say which console wrote what is in it.)

**Zero is common and means nobody** — 311 PS3 and 48 PS4/Vita saves carry it.
A save decrypted or unsigned for sharing usually does. Treating it as an owner
would mark every one of them as somebody else's, so it reads as "no account",
and the writer refuses to store it.

**The two rejected files are not PARAM.SFOs at all**: a zero-byte one
(`PSV/PCSE00638`), and a 14-byte file beginning `LOCA` rather than `\0PSF`
(`PSP/UCUS98640`). Both are refused on the magic and the length before any
offset in them is followed.

Run through the app itself rather than the reader alone, the same tree gives
2,566 saves — 1,262 PSP, 632 PS3, 339 PS4, 333 Vita — and the accounts
reconcile exactly with the table above.

#### Assigning one

**PS3** goes through `ps3_account_resign()`, which must also rewrite
`PARAM.PFD` so its hash of `PARAM.SFO` still matches — see above for why that
is one action and not two buttons.

**PS4 and Vita** go through `sfo_account_resign()`, which is very much
simpler: these are decrypted saves with the console's own layer already off,
so there is no signature to keep in step. `asfo_set_account_id()` rewrites the
value at its own eight bytes, so the file neither grows nor moves. Verified on
real saves: the file stays 2,728 bytes and **exactly eight contiguous bytes
change**, the `ACCOUNT_ID` and nothing else.

That is also why there is no `.bak` for it — an eight-byte overwrite at a
known offset is undone by signing the save back, and a stray `param.sfo.bak`
inside `sce_sys` is worse than the thing it guards against.

The button lives in a different place per console, and deliberately: a PS3
save has a whole section below the header for its encryption layer, and the
button belongs there with the rest of it. A PS4 or Vita save has no such
section, because nothing about it is encrypted, so its one button sits in the
header beside the account line.

Re-signing updates **both** copies of the save — the open one and the list's,
which are separate because a rescan re-sorts the list and a reference into it
would dangle. `note_account_change()` does that, so the Owner column ticks at
once instead of waiting for a rescan.

The desktop updates the account line after signing, because the files really
are on disk by then. The web front-end's PS3 panel deliberately does not:
there the bytes are only offered for download, and the save has not changed
until you save them.

That panel is the whole of the web front-end's involvement. It handles one
file at a time and only for PSP and PS3 — the two consoles with an encryption
layer worth a panel — so there is no PS4 or Vita side of it to extend.

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

Files on the command line are the other way in, alongside the
[saves screen](#two-screens), and they land on the patcher screen. A
`.savepatch` is opened as the patch; anything else is opened as the save to
patch, which pulls in everything that follows — the PSP game key, and that
game's patch from the database. Arguments are taken in the order given, so an
explicit patch beats the one a save's title ID would have auto-loaded,
whichever way round they are written. `--help` prints this and exits;
`--scan` and `--open` are described [above](#checking-a-folder-from-a-terminal).

**A folder works too**, which is the obvious thing to drag for a console save:

```bash
apollo_patcher_gui /Volumes/PSP/PSP/SAVEDATA/ULUS10391
```

It goes through the same identification the save list uses, so it arrives
**named, iconned, with its files listed** and Monster Hunter Freedom Unite's
patch loaded — the same thing as picking it from the list. A folder that is not
a save says so rather than becoming a target nothing can read, and neither does
a path that is not there.

Files can also be **dropped on the window**, which takes the same route.

### From Finder, on macOS

Double-clicking a `.savepatch`, dropping a save on the **Dock icon**, and
**Open With** are all the same mechanism — `kAEOpenDocuments` — and all three
work. A `.app` launched from Finder is handed its documents through Apple
Events rather than `argv`, so this takes two halves, and neither is any use
alone:

- **`Info.plist.in`** claims the types. macOS sends these events only for
  types an app has claimed, so without this the Dock icon would not even
  highlight when a save was dragged over it. `.savepatch` is claimed at
  `Owner` rank, since nothing else opens those; `public.data` and
  `public.folder` — a save is any file, or a folder — at **`Alternate`**,
  deliberately, so the app turns up in *Open With* and accepts Dock drops
  without volunteering to become the default application for every file on
  the machine.
- **`src/macos_open_docs.mm`** receives them. GLFW installs its own
  `NSApplication` delegate and does not implement `application:openFiles:`,
  so rather than subclass or swizzle it this registers with the Apple Event
  manager directly, which GLFW leaves alone. It is registered before the
  window exists, because a double-click launch sends the event almost
  immediately.

Each path goes to the same `open_path()` the drop handler and the command line
use, so a save folder opened from Finder arrives identified — named, iconned,
files listed, codes loaded — exactly as one picked from the list.

Windows and Linux file associations go through `argv`, which already worked.

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

### One window, one identity

ImGui hashes a window's whole name, so `"Save data: foo *##hexedit"` and
`"Save data: foo##hexedit"` were two different windows: the hex editor jumped
back to its default position and size the moment a byte was edited, and the
code viewers did the same on their first edit. `###` restarts the hash, so the
varying part — file name, code name, the `*` dirty marker — stays out of the
identity. The three windows this affects are the hex editor, the raw patch
view and the per-code viewers (`###viewer%d`, one identity each).

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
  flag. Detected per patch by default and overridable in Settings; the patcher
  screen shows which order is in effect and why. It calls
  `apollo_set_endianness()` before each code is applied, so a single build
  handles both byte orders — no separate big-endian binary. See
  [Settings](#settings).

- **PSP and PS3 savedata**: the consoles' own encryption, detected from the
  save folder and taken off and put back around Apply, plus a Settings panel
  naming the console a save is written for. See
  [PSP and PS3 saves](#psp-and-ps3-saves)
  above.
- **PS1 and PS2 savedata**: those consoles wrote no files at all, so their
  saves arrive inside a signed `.PSV` container. Scanned, listed and patched
  like any other — the file a code addresses is lifted out, patched, put back,
  and the container **re-signed** — with the icon rendered from the save
  itself. See [Browsing saves](#browsing-saves).
- **Two screens**: the app opens on your saves, not on a file picker. Pick one
  and the patcher screen has its target, key, byte order and codes ready.
  **File ▸ Advanced** drives a loose file or your own `.savepatch` by hand.
  See [Two screens](#two-screens).
- **The patch finds itself**: choosing a target looks its title ID up in the
  bundled database and loads that game's patch, or offers it when one is
  already open.
- **Files on the command line, dropped on the window, and opened from Finder**
  (double-click, Dock icon, *Open With* — macOS), including a save folder,
  which is identified exactly as one picked from the list. See
  [Opening things](#opening-things).
- **Save icons**, decoded by `core/png.c` and shown beside the game's name —
  or, for a PS1 or PS2 save, rendered by `core/mcicon.c` and `core/ps2/`, since
  those carry no picture to decode.
  See [The icon](#the-icon).
- **A vendored font** (Noto Sans JP), so game names keep their trade mark
  signs, curly quotes and Japanese titles. See [The font](#the-font).

## Known caveats / TODO
- **PS1 saves cannot change size.** A PS1 memory-card save occupies whole
  blocks, and the container stores it as one of them, so a code that grew or
  shrank the data would produce something a PS3 will not import. Such a code is
  refused with that reason rather than written back. PS2 saves have a real file
  table and may change size freely.
- **PS1 icons do not animate**, though the format allows up to three frames:
  not one of the six PS1 saves in the database uses more than one, so there is
  nothing to play. PS2 icons do animate.
- **PS1 and PS2 patch coverage is thin** — not a limitation of the app but of
  the database, which has two PS2 patches and no PS1 directory at all. Browsing,
  identifying, icons and re-signing work for every save regardless.
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
  which works over RDP); `main` sets `GALLIUM_DRIVER=llvmpipe` to select it,
  unless you have set that variable yourself.
  The bundled DLL is Mesa **17.2.6** — old enough to be a single self-contained
  file. macOS/Linux never need this.

  **The 64-bit software renderer needs a CPU with AVX.** That is not by
  design: every mesa-dist-win x64 build before Mesa 22.0 carries the `swr`
  driver, which
  [leaks AVX into common code](https://github.com/pal1000/mesa-dist-win#known-issues).
  On an older CPU — a Pentium 4 has SSE3 and no more — the app dies at launch
  with `0xC000001D`, `STATUS_ILLEGAL_INSTRUCTION`, before a window appears.

  **Use the 32-bit build on such a machine.** The AVX leak is x64-only, so the
  x86 artifact's `softgl` is unaffected, and a 32-bit app runs perfectly well
  on 64-bit Windows. That is the supported answer, deliberately — and a tested
  one: on a Windows 7 x64 host with a Pentium 4 (SSE3, no AVX), the x64
  software renderer dies at launch with `0xC000001D` and the x86 one runs
  normally. The same host runs the x64 build fine on its GPU driver, which is
  what narrows it to the software renderer rather than the app.

  `GALLIUM_DRIVER` is only ever *suggested* by the app, never overwritten, so
  `set GALLIUM_DRIVER=softpipe` does take effect if you ever need it — it just
  cannot help with this particular failure.

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
  `assets/fonts/` under the SIL Open Font License. See [The font](#the-font).
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
