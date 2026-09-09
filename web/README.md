# Apollo Patcher — web front-end

The Apollo engine compiled to WebAssembly, with a small static site around it.
Everything runs in the browser tab: the page reads your files locally, patches
them in memory and hands the result back as a download. There is no server, no
upload and no telemetry.

Published to GitHub Pages from `main` (see `.github/workflows/pages.yml`).

## Layout

```
src/apollo_wasm.c   Emscripten binding over core/apollo_ctrl.[ch]
public/index.html   the page
public/app.js       UI: state + DOM, no framework
public/worker.js    owns the wasm module, runs every engine call
public/style.css    light + dark
dist/               build output — exactly what gets published
```

## Building

Requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/) on
`PATH` and a wasm build of libapollo in the apollo-lib checkout:

```bash
cd ../../apollo-lib
make -f Makefile.wasm mbedtls    # once
make -f Makefile.wasm            # build-wasm/libapollo.a
cd -
make          # -> dist/
make serve    # build, then serve dist/ on http://localhost:8000
```

Override the library location with `make APOLLO_LIB=/path/to/apollo-lib`.

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
- **`--embed-file tools/python@/python`** — Python codes `import` helper modules
  from `sys.path`. With no host callback the engine looks in `python` relative
  to the filesystem root, so the library's `tools/python` is embedded there.
- **Interactive `{TAG}` options** start unset (the engine loads them as `-1`,
  meaning "not chosen"), so `Apply` stays blocked until every option group on a
  ticked code has a value — the same rule the desktop GUI enforces. Defaulting
  to the first value would silently choose a user profile or character slot on
  the player's behalf.

## Scope

This patches save *data*. It cannot decrypt or re-sign console saves — that
needs the console-side Apollo app. Feed it a decrypted save file.
