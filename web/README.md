# Apollo Save Patcher — web front-end

The Apollo engine compiled to WebAssembly, with a small static site around it.
Everything runs in the browser tab: the page reads your files locally, patches
them in memory and hands the result back as a download. There is no server, no
upload and no telemetry.

Published to GitHub Pages from `main` (see `.github/workflows/pages.yml`).

## Layout

```
src/apollo_wasm.c    Emscripten binding over core/apollo_ctrl.[ch]
../tools/            build-index.py generates the browsable patch index
public/index.html    the page
public/app.js        UI: state + DOM, no framework
public/worker.js     owns the wasm module, runs every engine call
public/cdn.js        where the database is fetched from, shared by both
public/hexedit.js    hex editor, vendored from bucanero/ps2vmc-tool
public/style.css     light + dark
dist/                build output — exactly what gets published
```

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
search the ~2200 patches in
[apollo-patches](https://github.com/bucanero/apollo-patches) by game name or
title ID and fetch the one they pick.

The split between build time and run time is deliberate:

- **The index is built in.** `../tools/build-index.py` reads the second line of
  every patch file (where the game name lives) and writes `dist/patches.json` —
  2240 rows, 24KB gzipped, fetched the first time the dialog opens. Reading
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
gzipped — 31% of the whole page** — while only 35 of 2240 patches import one. It
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
code parse as BSD and fail, with no way to correct it from here until now.

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
has been renamed. It remains a checkbox either way.

## Scope

This patches save *data*. It cannot decrypt or re-sign console saves — that
needs the console-side Apollo app. Feed it a decrypted save file.
