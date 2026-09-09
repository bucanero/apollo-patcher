# Apollo Patcher

Desktop and web front-ends for the [Apollo save-patch engine](https://github.com/bucanero/apollo-lib).

Both apply Apollo `.savepatch` files — Save Wizard codes, BSD scripts and Python
scripts — to save data on a computer. Neither reimplements any patch logic: they
drive the same `libapollo` engine the console apps use.

| Front-end | What it is |
|-----------|------------|
| [`gui/`](gui/README.md) | Native desktop app (Dear ImGui + GLFW) for Windows, macOS and Linux |
| [`web/`](web/README.md) | The engine compiled to WebAssembly, running in a browser tab |

The command-line tools (`patcher`, `dumper`) and the engine itself live in
[apollo-lib](https://github.com/bucanero/apollo-lib).

## Layout

```
core/     apollo_ctrl.[ch] — stdio-free engine facade, shared by both front-ends
gui/      Dear ImGui desktop app
web/      WebAssembly build + static site
```

`core/apollo_ctrl.c` is the only code that adapts libapollo's data model for a
UI. It replaces the CLI's three interactive pieces with callbacks and data:

| CLI (`patcher.c`)              | Front-end equivalent                    |
|--------------------------------|-----------------------------------------|
| `printf` / `dbglogger_log`     | `apctl_set_log_sink()` → log panel      |
| `scanf` in `get_user_options`  | `apctl_opt_set_selected()` ← dropdowns  |
| `is_active_code` arg parsing   | per-row checkboxes                      |

## Building

The engine is not vendored here. Clone [apollo-lib](https://github.com/bucanero/apollo-lib)
next to this repository:

```bash
git clone https://github.com/bucanero/apollo-lib
git clone https://github.com/bucanero/apollo-patcher
```

Point at a checkout elsewhere with `-DAPOLLO_ROOT=/path/to/apollo-lib`
(CMake) or `APOLLO_LIB=/path/to/apollo-lib` (the web Makefile).

### Desktop GUI

Needs CMake ≥ 3.16, a C/C++ toolchain, zlib, OpenGL, and a built
`libmbedcrypto` in the apollo-lib checkout:

```bash
cd ../apollo-lib/mbedtls-2.16.12/library && make static && \
  mkdir -p ../build/library && cp libmbedcrypto.a ../build/library && cd -
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # fetches Dear ImGui + GLFW
cmake --build build -j
```

Targets:
- `apollo_patcher_gui` — the desktop app (macOS: `build/gui/apollo_patcher_gui.app`)
- `apollo_ctrl_test` — headless lister, proves parity with `patcher <file>`
- `-DAPOLLO_BUILD_GUI=OFF` builds only the engine + headless test (no GL needed)

### Web

See [web/README.md](web/README.md).

## License

[Apollo Save Tool](https://github.com/bucanero/apollo-lib) - Copyright (C) 2020-2026 [Damian Parrino](https://twitter.com/dparrino)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
