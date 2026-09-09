# Apollo Patcher — web front-end

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
`make APOLLO_LIB=/path/to/apollo-lib PYTHON_MODULES=/path/to/python`.

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
- **`--embed-file <patches>/python@/python`** — Python codes `import` helper
  modules (`rijndael`, `umsgpack`, and per-game ones). With no host callback the
  engine looks in `python` relative to the filesystem root, so the patch
  database's `python/` is embedded there. Those modules live in
  [apollo-patches](https://github.com/bucanero/apollo-patches), not in the
  library, because they version together with the patches that import them.
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

## Scope

This patches save *data*. It cannot decrypt or re-sign console saves — that
needs the console-side Apollo app. Feed it a decrypted save file.
