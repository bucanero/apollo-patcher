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

LIBAPOLLO   = $(APOLLO_LIB)/build-wasm/libapollo.a
LIBMBED     = $(APOLLO_LIB)/mbedtls-2.16.12/build-wasm/library/libmbedcrypto.a

DIST        = dist
MODULE      = $(DIST)/apollo.mjs

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
# --embed-file: Python codes `import` helper modules from sys.path. With no host
#   callback libapollo looks in "python" relative to the FS root, so the
#   library's tools/python lands at /python. Embedded rather than preloaded to
#   keep dist/ a fixed set of files (no .data sidecar to fetch).
LDFLAGS     = -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=worker \
			  -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=4MB -sINVOKE_RUN=0 \
			  -sEXPORTED_RUNTIME_METHODS='["FS","UTF8ToString","stringToNewUTF8","HEAPU8"]' \
			  -sEXPORTED_FUNCTIONS='["_malloc","_free","_apw_version","_apw_open","_apw_close","_apw_codes_json","_apw_code_text","_apw_set_option","_apw_apply","_apw_reset_vars"]' \
			  --embed-file $(APOLLO_LIB)/tools/python@/python

.PHONY: all serve clean

all: $(MODULE) static

$(MODULE): $(SRCS) $(LIBAPOLLO) $(LIBMBED) Makefile | $(DIST)
	$(CC) $(CFLAGS) $(SRCS) $(LIBAPOLLO) $(LIBMBED) $(LDFLAGS) -o $@
	@echo "built $@"

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
