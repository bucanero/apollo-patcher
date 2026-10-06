# Changelog

Notable changes to the Apollo Save Patcher front-ends. The patch engine has its
own history in [apollo-lib](https://github.com/bucanero/apollo-lib), and the
patch data its own in [apollo-patches](https://github.com/bucanero/apollo-patches).

This file follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 2.5.0 — unreleased

The first release of this repository. The desktop GUI was split out of
apollo-lib and a WebAssembly front-end was added beside it, so everything below
is new *here* and is grouped by area rather than by Added / Changed / Fixed.
Built against apollo-lib 3.0.0.

### Desktop app

A native Dear ImGui + GLFW application for Windows, macOS and Linux.

- **Saves list.** Point it at a folder and it finds the saves underneath, then
  filters by console, by name or title ID, and by whether the patch database
  has codes. Each row carries the game, its slot, the console, the title ID and
  — when an account is configured — whether the save is yours. Hovering a row
  opens a panel with the save's own icon.
- **Patcher screen.** The save's icon and name, the target file, and the code
  list with its groups, options and per-code notes. Ticking a code pulls in any
  code it requires; codes the patch marks `[DEFAULT:]` arrive ticked. Each code
  shows the interpreter it runs under — Save Wizard, Python or BSD — as a small
  picture rather than a pair of letters.
- **Whole-save actions**, in a grid at the top of the save header: decrypt or
  re-encrypt every file the console wrapped, resign `PARAM.SFO` / `PARAM.PFD`,
  sign the save to your account, and re-bind it to your console. Each reports
  what it did in a message box rather than only in the log.
- **Applying runs on its own thread**, behind a dialog that names the code
  being applied and counts them off. A code that recompresses a save takes
  real time on an older machine, and a single-threaded apply painted nothing
  while it worked — long enough for Windows to grey the window out. The same
  dialog then shows the result.
- **View and edit data.** A hex editor over the target file, the raw
  `.savepatch` text, and per-code editing, with the result writable back out as
  a `.savepatch`.
- **Settings** in tabs — **General** for the PSN account ID and the byte-order
  override, then **PSP**, **PS3** and **PS4** for each console's own identity,
  so only the console a save is going to has to be filled in. A tab holding a
  half-typed value is marked, because the warning is otherwise about a field
  on a page you cannot see.
- **Opening things directly**: native file pickers, drag and drop onto the
  window, and `.savepatch` / save-folder arguments on the command line. On
  macOS the app registers for `.savepatch` documents from Finder.
- **Help menu** with the user guide, the project page and an About box.
- A **launch screen** in its own window, before the main one appears: the app
  icon, name and version, while the patch database opens and the previous
  folder is scanned behind it. A click or a key dismisses it early.
- **Fonts**: Noto Sans JP covering Latin, Greek, Cyrillic and Japanese, plus a
  10×20 raster font for the hex and code views.
- **Windows** builds static with MinGW and ships a software OpenGL fallback for
  machines with no usable driver — Remote Desktop and some VMs.

### Web front-end

- The engine compiled to **wasm32**, running entirely in the browser tab. No
  server, no upload, no bundler; engine calls run in a Web Worker and the save
  is patched inside Emscripten's in-memory filesystem.
- The **patch database is searchable in-page**.
- **PSP and PS3 savedata panels** for the consoles' own encryption.
- A **tools page** offering one decrypt / re-encrypt pair per game.
- Python helper modules are fetched from a CDN on demand rather than embedded.

### Console save support

Six consoles, with the console's own encryption handled where there is any:

| | a save is | the console's layer | the icon |
|---|---|---|---|
| PS1 | a memory-card block inside a `.PSV` | none | 16 colours in the save's own block |
| PS2 | memory-card files inside a `.PSV` | none | a textured 3D model |
| PSP | a folder with `PARAM.SFO` | encrypted, per-title key | `ICON0.PNG` |
| PS3 | a folder with `PARAM.SFO` | encrypted, per-file key in `PARAM.PFD` | `ICON0.PNG` |
| PS4 | a folder with `sce_sys/param.sfo` | none | `sce_sys/icon0.png` |
| Vita | a folder with `sce_sys/param.sfo` | none | `sce_sys/icon0.png` |

- **PSP**: KIRK decryption, per-title game keys, `PARAM.SFO` hash rewriting,
  and the fuse ID for the games that are console-locked.
- **PS3**: `PARAM.PFD` parsing and re-signing, per-file secure IDs, account-ID
  re-signing, console re-binding, and a hash check over the result.
- **PS4**: the full `param.sfo` re-sign apollo-ps4 performs on the console —
  `ACCOUNT_ID`, the `PARAMS` hash of the console's OpenPSID (HMAC-SHA256),
  the console-local user ID, and the title-ID copy. The account alone is
  enough for most saves, since a PS4 checks the console hash only for a save
  naming none; supply an OpenPSID and the save satisfies either check. Strictly
  PS4-gated — a PS3 keeps its account where the PS4 keeps that hash, and a Vita
  its title ID.
- **Vita**: the account ID, which is all apollo-vita writes too.
- **PS1 and PS2**: the signed `.PSV` container, which is how those saves reach
  a computer at all — also written by
  [apollo-ps2](https://github.com/bucanero/apollo-ps2) and
  [ps2vmc-tool](https://github.com/bucanero/ps2vmc-tool).
- **Icons** that are not pictures are drawn rather than decoded: the PS1's
  sixteen-colour block, and the PS2's 3D model, software-rasterised with its
  animation and its turntable rotation.
- Metadata is parsed **bounds-checked against the length handed in** rather
  than trusting the offsets inside the file.

### Patch database

- **Bundled offline** with the desktop app: 2,249 patches — PS3 1,801, PS4 253,
  Vita 123, PSP 70, PS2 2.
- A **browser** over it with search and a per-console filter (`Ctrl+F`).
- Byte order comes from the **database's platform** rather than being guessed
  from the title ID.

### Documentation

- A task-shaped **[user guide](docs/user-guide.md)**, also published at
  [bucanero.github.io/apollo-patcher/guide.html](https://bucanero.github.io/apollo-patcher/guide.html)
  and reachable from the app's Help menu.
- **Maintainer references** under [`docs/internals/`](docs/internals/): save
  data, save icons, text and fonts, the front-ends, and the real-save corpus
  most numbers in the docs are measured over.
- The three READMEs are orientation only; the implementation detail moved into
  the references above.
