# Apollo Save Patcher — web front-end

The Apollo engine compiled to WebAssembly, with a small static site around it.
Everything runs in the browser tab: the page reads your files locally, patches
them in memory and hands the result back as a download. There is no server, no
upload and no telemetry.

Published to GitHub Pages from `main` (see `.github/workflows/pages.yml`).

## Layout

```
src/apollo_wasm.c    Emscripten binding over core/apollo_ctrl.[ch]
../tools/            build-index.py generates the browsable patch index and
                     the tools catalog; verify-tools.mjs proves the latter
public/index.html    the patcher page
public/app.js        UI: state + DOM, no framework
public/tools.html    the tools page — one decrypt/re-encrypt pair per game
public/tools.js      its UI, on the same worker
public/psp.js        the PSP savedata panel — the console's own encryption,
                     which wraps a save below the game's. The PANEL is on the
                     patcher page; the shared calls beneath it also drive the
                     optional stage inside the tools page's game dialogs
public/ps3.js        the PS3's, the same layer one console up
public/settings.js   byte order, and which console a save is written FOR (the
                     PSP's Fuse ID, the PS3's console and account IDs). The dialog is on the
                     patcher page; the store is read by both
public/tools.css     its layout, on style.css's tokens
public/toolkit.js    which codes each action runs; shared with verify-tools.mjs
public/worker.js     owns the wasm module, runs every engine call
public/cdn.js        where the database is fetched from, shared by all
public/hexedit.js    hex editor, vendored from bucanero/ps2vmc-tool
public/style.css     light + dark
dist/                build output — exactly what gets published
```

## Two pages

`index.html` is the patcher: every patch in the database, the whole code list,
tick what you want. It assumes you know which codes you need, which is also why
the two **console savedata panels** and **Settings** live there — neither is
per-game, both want the save folder's metadata alongside the file, and a save
still wrapped in its console's own encryption is not something the two-click
page can sensibly offer.

`tools.html` is narrower on purpose. Most people arrive wanting one of two
things — open this save so an editor can read it, or put the edited one back —
and the patcher makes them assemble that themselves from a list where getting
the order wrong produces a save the game rejects. The tools page offers the two
buttons instead, and `toolkit.js` decides which codes each runs:

- **Decrypt** — the required codes up to the first checksum-or-encrypt step.
- **Re-encrypt** (or **Fix checksum**, when the patch has no crypto) — the rest,
  in file order. The order is load-bearing: Silent Hill 3 is decrypt, update
  the DWADD checksum, encrypt, and skipping the middle gives a save the game
  refuses. Crisis Core computes its checksum over the *ciphertext*, which is
  why its checksum code sits after the encrypt.

It lists only patches `make verify` has vouched for — see below. Everything
else about it is the patcher's machinery: same worker, same wasm, same CDN
fetch.

One card is one TOOL, not one patch. A game ships a patch per region and they
normally carry identical codes, so listing them separately buried the same tool
five times and left people unsure whether their region was covered. Patches are
folded by the chain group the verifier assigns — identical codes, so any member
can be the one the card loads — and the dialog names every title ID in the
group. Deliberately not folded by game name: Metal Gear Solid V keys per
region, so its PS3 releases are different tools and keep separate cards.

Card art comes from [apollo-saves](https://github.com/bucanero/apollo-saves) at
run time, by title ID. Coverage is partial, so the image removes itself when it
404s and the layout closes up; a group whose first region has no art is retried
against the others before giving up.

### The PSP and PS3 panels

On the **patcher** page, below the pickers. A save off either console is wrapped
twice — the console encrypts it with a key of its own before the game's
encryption is anywhere in the picture:

```
MHP2NDG.BIN (1,483,024 bytes)   PSP savedata encryption (KIRK + the game key)
  └─ MHP2NDG.BIN (1,483,008)    Monster Hunter's own — what a .savepatch undoes
       └─ plaintext
```

Every PSP and PS3 tool in the catalog operates on the *middle* layer, so a file
copied straight off a Memory Stick or a hard drive goes into the patch engine
and comes back as noise that looks like output unless the outer layer comes off
first. The panels are that outer layer, and they are deliberately not cards in
the grid — they are not per-game, and they work for any save at all, including
titles the patch database covers but the catalog does not, and saves with no
patch. The tools page carries a line pointing at them, since that is where
somebody holding a raw save is most likely to start.

Each wants two files — the console's metadata and the save itself — and works
out the rest:

|                 | PSP                          | PS3                            |
|-----------------|------------------------------|--------------------------------|
| metadata        | `PARAM.SFO`                  | `PARAM.PFD`                    |
| what is wrapped | `SAVEDATA_FILE_LIST`         | the PFD's entry table          |
| the key         | `PSP/gamekeys.txt` (16KB)    | `PS3/games.conf` (280KB)       |
| keyed by        | the save directory           | the save directory *and* the file name |
| fallback        | a dumper's file, or 32 hex digits | 32 hex digits             |

Both databases are fetched from the CDN like the patches are; `games.conf` is 23
times the size of the PSP's, so it is fetched when the panel opens rather than
at page load.

Two things differ enough to be worth stating:

- **The PS3 needs a save folder NAME**, because `games.conf` files its sections
  under save directories and `PARAM.PFD` carries no such string. Dropping the
  folder's `PARAM.SFO` in alongside fills the field; otherwise you type it.
- **The PS3 panel checks the file against the database.** `PARAM.PFD` records a
  hash per protected file, so the panel can say whether the one you loaded is
  still the one it describes — worth knowing before patching, since a save that
  already disagrees would have the damage signed into place.

Re-encrypting hands back **two** files, and both have to go into the save
folder: the data file, and the rewritten metadata. The file's own hash lives in
the metadata and the signatures over it are regenerated, so a save put back with
the old `PARAM.SFO` or `PARAM.PFD` does not load.

The engines are `core/psp/` and `core/ps3/`, compiled into the same wasm module
— between them they need only mbedTLS's AES and SHA1, which this module already
links, so they cost **23KB gzipped** rather than separate modules with their own
copies of the crypto, their own heaps and their own instantiates. Measured
against builds with each layer removed: 317KB with neither, 331KB with the
PSP's, 341KB with both.

### …and inside the game cards

The PSP and PS3 tools in the grid address the *inner* layer, so each of their
dialogs also carries the console one, as an optional stage: add the save
folder's `PARAM.SFO` or `PARAM.PFD` and Decrypt takes both layers off in one
press, while Re-encrypt puts both back and hands over the rewritten metadata
alongside the save. Leave it out and the tool behaves exactly as it always did,
which is right for a file that is already unwrapped.

The stage is deliberately not one of the patch's file slots. Those slots are the
patch's own targets and their indices are what `routeChain()` assigns codes to,
so a row for the metadata would shift every route by one; it is a stage wrapped
around the run, not another target. Only files the metadata names are wrapped —
a save folder holds `ICON0.PNG` too.

For a PS3 tool the save folder defaults to the tool's own title ID, which is
right for 8480 of `games.conf`'s 8510 section IDs. When it is not — DiRT 3 files
`BLUS30724` and `BLUS30724PROFILE` with different keys — the stage says so
rather than decrypting to noise: it verifies the loaded file against the hash
`PARAM.PFD` recorded, and warns when they disagree.

The order is the part worth stating, because it is not symmetric and getting it
backwards produces a file that looks plausible and that the game refuses:

```
opening a save    unwrap the console's layer, THEN run the patch's decrypt
putting it back   run the patch's encrypt, THEN wrap the console's layer
```

The `NATIVE` table in `tools.js` owns that rule, and the per-console differences
under it, so the panels, the game cards and `verify-tools.mjs` cannot drift
apart on it.

### Settings

The **Settings** button in the patcher page's header holds how saves are read
and written. The tools page has no such button: the only setting that reaches it
is the byte order, which it honours and reports but never needs to change, since
every patch it runs comes from the database and carries its own platform.
Everything in it is optional, and every default is the safe one.

- **Byte order** — *Auto*, *Big-endian* or *Little-endian*. Auto follows each
  patch, which is right for every patch in the database. A forced order is
  remembered across visits and applies to every save on both pages, so each says
  which order is in effect whenever one is forced — and says so in amber when it
  contradicts the patch that is open. The patcher page carries the same control
  inline, next to Apply, since it has no Settings dialog of its own.

The rest name the console or account a save is written *for*. All change only
what is WRITTEN, and leaving them blank keeps whatever a save already says.

- **PSP, Fuse ID.** Savedata modes 4 and 6 derive two `PARAM.SFO` hashes from
  the console's own fuse. A PSP loads a save whose values differ, so this only
  matters for reproducing one console's output byte for byte.
- **PS3, account ID (PSN).** 16 hex digits, and usually the one to reach for.
  It is written into the save's own `PARAM.SFO`, so the save loads on *any* PS3
  that account has signed in to rather than on one machine. Name one and the
  PS3 panel offers **Sign to your account**.
- **PS3, console ID (IDPS).** Inside `PARAM.PFD`, one of `PARAM.SFO`'s four
  hashes is keyed by the IDPS of a single machine. Name one and the PS3 panel
  offers **Re-bind to your console**, which rewrites that hash and re-signs the
  database around it.

The two PS3 settings are independent and both can be used. Signing to an
account writes the ID into *both* places `PARAM.SFO` keeps it — the
`ACCOUNT_ID` field and the copy at `0x30` inside the binary `PARAMS` blob —
and then recomputes `PARAM.PFD`'s hash of that file, so it hands back **two**
files. Keep both: a `PARAM.SFO` carrying a new account beside a `PARAM.PFD`
that still hashes the old one is a save that does not load.

The panel shows **which account the save is signed to now**, read from the
`PARAM.SFO` you supply, and marks it as yours when it matches Settings. It is
not updated after signing, on purpose: the page hands back bytes rather than
writing files, so the save has not changed until you save them.

The console is cleared across that update and put back afterwards, because
`apfd_update_file` re-binds `PARAM.SFO`'s console-keyed hashes whenever one is
named — ambient state rather than an argument — and signing to an account has
no business doing the console's job as well.

The Fuse ID and console ID live in the wasm module, which belongs to the
worker, so the page pushes them there on every change and once at start-up.
The account ID does not: it is passed with the call that uses it. `localStorage`
keeps all of them across visits, since they describe your console or account
rather than the save you happen to be holding.

## Verifying the tools

```bash
make verify SAMPLES=/path/to/save-decrypters
```

For each row of `../tools/verify-manifest.tsv` this opens the shipped
`.savepatch` in the real module, splits its codes with the same `toolkit.js`
the page uses, applies the decrypt half to a real encrypted save, and requires
the output to equal the reference plaintext byte for byte. The result is
`../tools/verified.json` — a committed input, so a clean checkout builds the
page without needing the saves — and `dist/tools.json` is generated from it
with `--verified-only`.

So the page does not promise anything that has not been run. 187 patches pass
today — 109 driven directly against a sample, 78 more inheriting that proof by
carrying a byte-identical chain — and they collapse to 109 cards (PS3 46, PS4
53, PSP 8, PS Vita 2); the catalog knows about 1156, and opening up the rest is
a matter of dropping `--verified-only` once there is evidence for them.

`verify-manifest.tsv`'s 8th column extends that to the rows whose console wraps
saves itself. A row marked `psp=<32 hex>` or `ps3=<32 hex>` has the suite wrap
the sample as a console would have stored it — a `PARAM.SFO` or `PARAM.PFD`
fixture plus the real key — and then requires the full two-stage chain to reach
the same plaintext a one-stage row reaches, and on the way back to reproduce
both the wrapped bytes and the rewritten metadata exactly, from a fresh fixture
rather than a carried-over one.

**54 of the 118 rows carry one**: 48 of the 52 PS3 rows and 6 of the 8 PSP.
`ps3=db` goes further and looks the key up in the real `PS3/games.conf` by title
ID and file name, through the same C the page calls — so each of those rows also
proves that lookup against all 1819 of its sections, which is coverage nothing
else in CI has.

The rows that carry none are right not to. Three PS3 titles — Uncharted 2
(`BCES00509`, `BCUS98123`) and [PROTOTYPE] (`BLES00269`) — are filed in
`games.conf` as `UNPROTECTEDGAME`, meaning the console encrypts nothing in them,
so there is no layer to prove; `BLUS31460` is not in that database at all, and
two PSP titles likewise. `ps3=db` FAILS rather than inventing a key, which is
why these read as absences rather than as passes.

That proves the *composition* — order, routing, both directions. It does not
re-prove the console layers themselves: `core/test_psp.c` and `core/test_ps3.c`
do that, against known-answer vectors cross-checked with the unmodified upstream
implementations, and against real console-written saves.

What it is really guarding is the seam between the engine and the patches,
which drifts silently: a patch written against a fixed engine keeps parsing
against an unfixed one and simply produces the wrong bytes. Four patches are
refused for exactly that reason, because `main` predates fixes that exist on
branches held back for the next console release:

- `PS3/BLES00450` + `PS3/BLUS30248` (Need for Speed: Undercover) decrypt with a
  range one block short, `pointer+0x5B`, where apollo-lib has needed
  `pointer+0x6B` since `63f334a`. Fixed on `nfs-undercover-range`.
- `PS3/NPUB30611` + `PS3/NPEB00686` (MGS: Peace Walker) call `DECRYPT mgs_pw`
  with no save-type argument, so they decrypt nothing. Fixed on `mgs-pw-psp`.

Those four are parked in the manifest rather than listed, because this page
fetches patches from `apollo-patches@main` at run time: what ships on a branch
does not reach a visitor. They go back in when the branches land — they already
pass against the branch content.

The module is rebuilt for node to run this (the shipped one is
`-sENVIRONMENT=worker`); only the JS bootstrap differs, and the target asserts
the `.wasm` is byte-identical to the shipped one.

## Building

Requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/) on
`PATH`, a clone of [apollo-patches](https://github.com/bucanero/apollo-patches)
(its `python/` directory gets embedded — see below), and a wasm build of
libapollo in the apollo-lib checkout:

```bash
cd ../../apollo-lib             # or ../apollo-lib for an in-tree checkout
make -f Makefile.wasm mbedtls    # once
make -f Makefile.wasm            # build-wasm/libapollo.a
cd -
make          # -> dist/
make serve    # build, then serve dist/ on http://localhost:8000
```

Both checkouts are found the same way `CMakeLists.txt` finds apollo-lib —
in-tree first, then a sibling clone. Override with
`make APOLLO_LIB=/path/to/apollo-lib APOLLO_PATCHES=/path/to/apollo-patches`.

## How it fits together

The engine is synchronous and some of it is slow — a Python code allocates a
64MB heap and runs a MicroPython interpreter — so all of it happens in a Web
Worker and the page stays responsive.

Files never touch a real filesystem. The `.savepatch` is parsed straight from
memory (`apctl_open_buffer`), and the save file is written into Emscripten's
in-memory filesystem so that `apollo_apply_code()` can read and write it the
same way it does on a console. Nothing in the engine needed changing.

`Apply` always starts from the bytes you loaded, so it is idempotent: what you
download reflects exactly the codes currently ticked, not an accumulation of
previous runs.

### Things worth knowing about the build

- **`--spill-pointers`, applied post-link** — not optional. MicroPython's GC
  scans the C stack conservatively, and under wasm that finds almost nothing:
  locals live in wasm locals rather than addressable memory, so a measured run
  had **1276 bytes** of shadow stack for the entire live VM call chain. Live
  objects go unseen, are swept, and the next free of one aborts the module with
  `assert(!"bad free")` — which takes out *every* Python patch, about 50 of
  them in the database.

  It cannot be passed as `-sBINARYEN_EXTRA_PASSES=--spill-pointers`, because
  that route is a catch-22: the pass locates the stack pointer **by name**, so
  without the name section wasm-opt stops with `Fatal: getStackSpace: failed to
  find the stack pointer` — and asking emcc for names with `-g` makes it report
  `running limited binaryen optimizations because DWARF info requested` and
  *silently drop* the extra passes. `--profiling-funcs` threads between the
  two: it keeps the name section without DWARF, so the full `-O3` Binaryen
  pipeline still runs, and `--strip-debug` removes the names again immediately
  after spilling.

  It costs real bytes: **920KB against 600KB, 317KB gzipped against 243KB.**
  That is the price of a MicroPython that does not corrupt saves.

- **`-sSTACK_SIZE=4MB`** — Emscripten's 64KB default is *below* MicroPython's
  own 40KB stack limit, so Python codes fail immediately without it.
- **No `--embed-file`** — Python codes `import` helper modules (`rijndael`,
  `umsgpack`, and per-game ones), and those are fetched at run time rather than
  built in. See below.
- **Interactive `{TAG}` options** start unset (the engine loads them as `-1`,
  meaning "not chosen"), so `Apply` stays blocked until every option group on a
  ticked code has a value — the same rule the desktop GUI enforces. Defaulting
  to the first value would silently choose a user profile or character slot on
  the player's behalf.

## The patch database browser

Users should not have to go and find a `.savepatch` first, so the page can
search the ~2250 patches in
[apollo-patches](https://github.com/bucanero/apollo-patches) by game name or
title ID and fetch the one they pick.

The split between build time and run time is deliberate:

- **The index is built in.** `../tools/build-index.py` reads the second line of
  every patch file (where the game name lives) and writes `dist/patches.json` —
  2247 rows, 24KB gzipped, fetched the first time the dialog opens. Reading
  2200 files is trivial here and impossible from a browser, and the GitHub API
  would neither give game names nor survive the rate limit.
- **The patches are fetched live**, from
  `cdn.jsdelivr.net/gh/bucanero/apollo-patches@main`, so a patch fixed upstream
  reaches users without redeploying this site. jsDelivr rather than
  `raw.githubusercontent.com`, which answers 503 to cross-origin requests from
  the Pages origin.

The consequence to keep in mind: a patch added upstream is not listed until this
site is rebuilt. A listed patch that has since been renamed 404s, which the
dialog reports while pointing at the drop zone as the fallback.

The same script builds the desktop app's index (as TSV, inside
`apollo-patches.zip`), so the parsing quirks below are handled once for both.

Two details in `build-index.py` that came from the real data: 245 patch files
are Windows-1252 rather than UTF-8 (game names with ™ / ®), so a strict decode
would drop them; and the leading `;` on the name line is a convention, not a
guarantee. The index's decoded name is also preferred over the engine's for
display, since the engine hands back raw bytes.

## Python helper modules

Fetched from the CDN on demand and written into `/python` in the in-memory
filesystem. libapollo resolves imports against `python` relative to the
filesystem root when no host callback is set, and MicroPython's import
`stat()`s real paths, so files written after startup work fine.

They were embedded with `--embed-file` at first. Measured, that cost **133KB
gzipped — 31% of the whole page** — while only 35 of 2247 patches import one. It
also left the page internally inconsistent: patches came live from the CDN while
the modules they import were frozen at build time, so a patch updated upstream
to use a new module raised a bare `ImportError`. Upstream history shows that is
a real pattern — new modules arrive together with new patches ("Add JoJo ASB
decrypter", "add jump force decrypter").

How it works now:

- `dist/python-modules.json` (300 bytes, generated) lists the module names, so
  nothing has to crawl a directory.
- The fetch starts when a patch containing **any** Python code is loaded, so it
  overlaps with the user reading the code list. `Apply` awaits it, and only when
  a ticked code is actually Python.
- The whole set is fetched, not the imports a code names, because the modules
  import each other (`umsgpack` pulls in `datetime`). Resolving that from
  outside would break the first time someone adds an import upstream. It costs
  little: 51 patches contain Python at all, and 16 of those import only
  built-ins.
- Nothing is written until every file has arrived. A partial set on disk would
  surface as an `ImportError` from inside a patch, which looks like a broken
  patch rather than a failed download. A failure is reported as one, and retried
  on the next Apply.

The desktop GUI deliberately does the opposite and bundles everything offline —
it is a download you keep, not one you make every visit.

## Hex editor

`public/hexedit.js` is vendored **unmodified** from
[ps2vmc-tool](https://github.com/bucanero/ps2vmc-tool) (`web-ps2/hexedit.js`),
so it can be refreshed from there. It ships its own dark stylesheet, injected
into `<head>` at run time; rather than fork the file, `style.css` maps its
palette onto this app's tokens with `.hx-bg`-prefixed rules — prefixed because
the injected `<style>` lands after our stylesheet and would otherwise win.

Loaded as a classic `<script>` (it is UMD, not an ES module) so it registers
`window.HexEdit` before the deferred module runs.

The loaded save is editable: committing edits replaces the bytes in memory and
retracts any previous result, since that output was produced from the bytes as
they were. The patched result is offered read-only — editing it would produce a
file no patch chain accounts for, and it is one Download away.

## Editing a code

The per-code **View** window is a text editor: *Save changes* replaces the body
the engine runs, *Revert to file* restores the patch's own, and the row gets an
`E` marker while the two differ. Saving also retracts any previous result — it
came out of the old body.

Session-only, deliberately. The `.savepatch` is never rewritten, and loading
another patch drops every edit; exporting a modified patch file would be a
different feature. Applying does not consume an edit either, because the engine
copies the body before it runs, so Apply stays as repeatable as it is for an
unedited patch.

**Runs as** picks the interpreter (Save Wizard / BSD / Python) and applies at
once rather than waiting for *Save changes*: the type and the text are separate
things, and a wrongly-typed code often has nothing to type. It matters more
than it sounds — without a `[SW:…]` / `[BSD:…]` / `[PYTHON:…]` prefix the type
comes from the shape of the body, so a single mistyped line makes a Save Wizard
code parse as BSD and fail, and this is the only way to correct it from here.

Switching a code *to* Python has to tell the worker, because the Python helper
modules are fetched on demand and that decision is made from the parsed types
at load time. `codeState()` refreshes `codeTypes` and starts the fetch, so a
patch that contained no Python when it loaded still gets the modules. The wire
argument is `codeType`, not `type`: the worker envelope already spends that
name on the message kind.

The engine, not the page, decides whether a code counts as edited — saving the
patch file's own text back is not an edit — so `setCodeText` answers with what
the engine holds afterwards and the marker follows that.

The one trap worth the extra code: an option's value is written OVER its
`{TAG}`, in place and at the tag's own length (`apply_tag_opts`), so a tag that
has been retyped or deleted stops resolving and its dropdown quietly does
nothing. The dialog watches for that while you type.

## Saving the patch

**Save patch file** downloads the `.savepatch` with the session's edits in it.
The engine rebuilds it from the original bytes, splicing in only the edited
codes, so everything the parse drops — comments, credits, `:file` lines, option
blocks — survives. The bytes never become a JS string on the way out: 245 of
the database's patches are Windows-1252, and decoding plus re-encoding would
corrupt the ™/® in their names, so the worker copies the range straight out of
the wasm heap into the Blob.

A forced type is written into the title as `[SW:…]`, `[BSD:…]` or
`[PYTHON:…]`, but only when the body alone would be read as something else —
stating what the body already implies would add noise, and would display wrong
on console engines older than those prefixes.

Then it re-parses what it built and reports the codes that would read back
differently, which the page passes on in the log. One case remains: a title
carries a single marker, so a code already flagged `[DEFAULT:...]` or
`[INFO:...]` has no room to state a type.

## Byte order

PS3 save data is big-endian; nothing else Apollo covers is. A patch chosen from
the database carries its platform — the directory it lives in — so that decides
directly. A file the user supplies has no directory, so the shared
`apctl_is_big_endian_for()` falls back to matching known PS3 title-ID prefixes
against the file name, then against the patch's own first lines for a file that
has been renamed.

That answer is the default, not the last word: Settings can force big- or
little-endian for every save, and the choice is remembered. Both pages therefore
say which order is in effect whenever one is forced, and say so in red when a
forced order contradicts the patch that is open — see [Settings](#settings).

## Known limitation: the largest saves

Spilling makes the conservative scan retain aggressively, so a Python patch
working on a very large save can exhaust the MicroPython heap and raise
`MemoryError`. Monster Hunter World's 8MB save does, at `PY_HEAP_SIZE` and at
four times it; every other Python patch with a sample in
[save-decrypters](https://github.com/bucanero/save-decrypters) applies
correctly. It fails cleanly rather than producing a bad save.

## Scope

This patches save *data*. It cannot decrypt or re-sign console saves — that
needs the console-side Apollo app. Feed it a decrypted save file.
