# Apollo Patcher — web build (Emscripten).
#
# Produces a static site in dist/ that can be served from anywhere, including
# GitHub Pages. No bundler, no npm: the page loads the Emscripten ES module
# directly, so `dist/` is exactly what gets published.
#
#   make            build dist/
#   make serve      build, then serve dist/ on http://localhost:8000
#   make clean
#
# Requires the Emscripten SDK on PATH, plus a wasm build of libapollo in the
# apollo-lib checkout:
#
#   cd $(APOLLO_LIB)
#   make -f Makefile.wasm mbedtls
#   make -f Makefile.wasm
#
# The checkout is found the same way CMakeLists.txt finds it: in-tree
# (../apollo-lib from here, which is what CI produces) first, then a sibling
# clone (../../apollo-lib, the local-dev layout). Override with
# APOLLO_LIB=/path/to/apollo-lib.

APOLLO_LIB ?= $(patsubst %/include/apollo.h,%,$(firstword \
			  $(wildcard ../apollo-lib/include/apollo.h ../../apollo-lib/include/apollo.h) \
			  ../../apollo-lib/include/apollo.h))

# The patch database, https://github.com/bucanero/apollo-patches. Two things
# come from it:
#
#   python/       helper modules Python patches `import` (rijndael, umsgpack,
#                 and per-game ones). Only their NAMES are built in
#                 (dist/python-modules.json); the modules themselves are
#                 fetched from a CDN the first time a patch needs one. They
#                 used to be embedded, which cost every visitor 133KB gzipped
#                 for something 1.6% of patches use, and froze them at build
#                 time while the patches importing them were fetched live.
#   PS3/ PS4/ ... the ~2200 .savepatch files themselves, which the page lets
#                 users browse. Only an INDEX is built in (dist/patches.json);
#                 the patches are fetched from a CDN at runtime, so a patch fix
#                 upstream does not need a redeploy here.
#
# Found in-tree first, then as a sibling clone, matching APOLLO_LIB above.
# Override with APOLLO_PATCHES=/path/to/apollo-patches.
APOLLO_PATCHES ?= $(patsubst %/python,%,$(firstword \
			  $(wildcard ../apollo-patches/python ../../apollo-patches/python) \
			  ../../apollo-patches/python))

PYTHON_MODULES = $(APOLLO_PATCHES)/python

LIBAPOLLO   = $(APOLLO_LIB)/build-wasm/libapollo.a
LIBMBED     = $(APOLLO_LIB)/mbedtls-2.16.12/build-wasm/library/libmbedcrypto.a

DIST        = dist
MODULE      = $(DIST)/apollo.mjs
INDEX       = $(DIST)/patches.json
MODLIST     = $(DIST)/python-modules.json

SRCS        = src/apollo_wasm.c ../core/apollo_ctrl.c
STATIC      = $(wildcard public/*)

CC          = emcc

CFLAGS      = -O3 -Wall \
			  -D_GNU_SOURCE -D_USE_MBEDTLS \
			  -I$(APOLLO_LIB)/include \
			  -I$(APOLLO_LIB)/tools \
			  -I$(APOLLO_LIB)/mbedtls-2.16.12/include \
			  -I../core \
			  --use-port=zlib

# -sSTACK_SIZE: Emscripten's 64KB default is below MicroPython's own 40KB stack
#   limit, so Python codes fail immediately without this.
# -sALLOW_MEMORY_GROWTH: apollo_apply_py_code() allocates a 64MB Python heap.
# -sMODULARIZE/-sEXPORT_ES6: the worker imports it with a plain `import`.
# -sENVIRONMENT=worker: this module only ever runs inside the Web Worker, so
#   the browser/node bootstrap paths are dead weight.
# -sINVOKE_RUN=0: there is no main(); we only call exported functions.
#
# Note there is no --embed-file: the worker writes the Python helper modules
# into /python at run time (see public/worker.js). libapollo resolves imports
# against "python" relative to the FS root when no host callback is set, and
# MicroPython's import stat()s real paths, so files written after startup work.
LDFLAGS     = -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=worker \
			  -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=4MB -sINVOKE_RUN=0 \
			  -sEXPORTED_RUNTIME_METHODS='["FS","UTF8ToString","stringToNewUTF8","HEAPU8"]' \
			  -sEXPORTED_FUNCTIONS='["_malloc","_free","_apw_version","_apw_is_be","_apw_open","_apw_close","_apw_codes_json","_apw_code_text","_apw_set_code_text","_apw_code_is_edited","_apw_code_flags","_apw_revert_code","_apw_set_option","_apw_apply","_apw_reset_vars"]'

.PHONY: all serve clean

all: $(MODULE) $(INDEX) $(MODLIST) static

$(MODULE): $(SRCS) $(LIBAPOLLO) $(LIBMBED) Makefile | $(DIST)
	$(CC) $(CFLAGS) $(SRCS) $(LIBAPOLLO) $(LIBMBED) $(LDFLAGS) -o $@
	@echo "built $@"

# The browsable index of the database. Reading ~2200 files is trivial here and
# impossible from a browser, so the page fetches this instead of crawling.
$(INDEX): ../tools/build-index.py $(PYTHON_MODULES) | $(DIST)
	@python3 ../tools/build-index.py $(APOLLO_PATCHES) $@

# Just the helper-module names, so the worker knows what to fetch without
# crawling a directory listing. 300 bytes.
$(MODLIST): ../tools/build-index.py $(PYTHON_MODULES) | $(DIST)
	@python3 ../tools/build-index.py $(APOLLO_PATCHES) $@ --format=modules

# Named as a prerequisite above, so a missing checkout stops here with this
# message instead of failing deep inside emcc's file_packager.
$(PYTHON_MODULES):
	@echo "missing $@ — the patch database (Python helper modules and the"
	@echo "browsable patch index both come from it)."
	@echo "clone it next to this repo:"
	@echo "    git clone https://github.com/bucanero/apollo-patches"
	@echo "or pass APOLLO_PATCHES=/path/to/apollo-patches"
	@exit 2

$(LIBAPOLLO) $(LIBMBED):
	@echo "missing $@"
	@echo "using apollo-lib at: $(APOLLO_LIB)"
	@if [ ! -f "$(APOLLO_LIB)/include/apollo.h" ]; then \
		echo "...which is not an apollo-lib checkout. Clone it next to this repo,"; \
		echo "or pass APOLLO_LIB=/path/to/apollo-lib."; \
	else \
		echo "build libapollo for wasm first:"; \
		echo "    cd $(APOLLO_LIB) && make -f Makefile.wasm mbedtls && make -f Makefile.wasm"; \
	fi
	@exit 2

# Static assets are copied rather than referenced so that dist/ is a complete,
# self-contained site — the same tree the Pages workflow uploads.
.PHONY: static
static: | $(DIST)
	@cp $(STATIC) $(DIST)/
	@echo "copied $(words $(STATIC)) static files to $(DIST)/"

$(DIST):
	@mkdir -p $@

serve: all
	@echo "serving $(DIST) on http://localhost:8000 (ctrl-c to stop)"
	@cd $(DIST) && python3 -m http.server 8000

clean:
	@rm -rf $(DIST)
	@echo "Cleaned up!"
