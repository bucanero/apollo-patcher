# Apollo Save Patcher — web front-end

The Apollo engine compiled to WebAssembly, with a small static site around it.
Everything runs in the browser tab: the page reads your files locally, patches
them in memory and hands the result back as a download. There is no server, no
upload and no telemetry.

Published to GitHub Pages from `main` (see `.github/workflows/pages.yml`).

## Documentation

| | |
|---|---|
| [The front-ends](../docs/internals/frontends.md#the-web-front-end) | The two pages, the console panels, the worker, the patch browser, the Python modules |
| [Save data](../docs/internals/savedata.md) | What the console panels are actually undoing |
| [User guide](../docs/user-guide.md) | The desktop app — much of it applies here too |

This file is the build: how to compile the module, what the build flags are for,
and how the tools page proves what it offers.

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

`index.html` is the **patcher**: every patch in the database, the whole code
list, tick what you want. The two console savedata panels and Settings live
there too.

`tools.html` is narrower on purpose: one **Decrypt** and one **Re-encrypt** (or
**Fix checksum**) button per game, instead of a code list the user has to
assemble in the right order. It lists only patches `make verify` has vouched
for — see [Verifying the tools](#verifying-the-tools).

Why each page is shaped that way, what `toolkit.js` does with the code order,
and how the console layer folds into a game card are in
[the front-ends](../docs/internals/frontends.md#the-web-front-end).

## The user guide page

`dist/guide.html` is [docs/user-guide.md](../docs/user-guide.md) rendered by
`tools/make-guide.py`, so the site and the repo serve the same document and
cannot drift. It is built by `make` like everything else, and styled by
`public/guide.css` on `style.css`'s own tokens.

The renderer is deliberately **not** a markdown library. The site has no npm
and no bundler — `dist/` is exactly what gets published — and a Python
dependency would put the Pages workflow at the mercy of something nobody here
controls. python3 is already required for `build-index.py`, so this costs
nothing new.

It implements only the subset the guide actually uses, and **fails the build**
on anything else rather than emitting a page with raw markdown in it: an
unrecognised construct names its line number, and leftover `**`, a stray
backtick or an in-page link with no matching heading each stop the build. If
the guide grows a construct it does not handle, teach it the construct or
rewrite the line.

Heading ids follow GitHub's own slug rules, so the guide's Contents links work
identically on the site and in the repo, and a link copied from one lands in
the same place on the other.

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

## Scope

This patches save **data**, in the browser, with no server and no upload.

It **can** take off the PSP's and the PS3's own savedata encryption — that is
what the two console panels are for, and what the optional stage inside each
PSP or PS3 game card does. It **cannot** decrypt a PS4 or Vita save: those
arrive already decrypted from the console-side Apollo app, and there is no
layer here to remove.

For a folder of saves rather than one file at a time, and for PS1/PS2 `.PSV`
containers, use the [desktop app](../gui/README.md).

