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
#                 fetched from a CDN the first time a patch needs one, rather
#                 than embedded — which would cost every visitor 133KB gzipped
#                 for something 1.6% of patches use, and would freeze them at
#                 build time while the patches importing them are fetched
#                 live.
#   PS3/ PS4/ ... the ~2250 .savepatch files themselves, which the page lets
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
TOOLS       = $(DIST)/tools.json
# The user guide, rendered from the same markdown the repo serves, so the
# site and the repo cannot drift. tools/make-guide.py handles only the
# subset the guide uses and fails the build on anything else.
GUIDE       = $(DIST)/guide.html
GUIDE_SRC   = ../docs/user-guide.md
# A committed INPUT, not build output: producing it needs the real game saves
# in bucanero/save-decrypters, which are not vendored. `make verify` rewrites
# it; a plain build just reads it, so dist/ can be wiped without losing it.
VERIFIED    = ../tools/verified.json

# ../core/psp/ and ../core/ps3/ are the consoles' own savedata encryption --
# the layer below anything a .savepatch does. They need only mbedTLS's AES and
# SHA1, which this module already links for libapollo's own decrypters, so they
# ride in the same wasm rather than separate ones: ~12KB gzipped against a
# second module's duplicated crypto, second heap and second instantiate.
# ../core/sfo.c is the PARAM.SFO container, which the PSP path now shares with
# every other console rather than carrying its own copy of.
SRCS        = src/apollo_wasm.c ../core/apollo_ctrl.c ../core/sfo.c \
			  ../core/psp/kirk_engine.c ../core/psp/psp_savedata.c \
			  ../core/ps3/pfd_savedata.c
STATIC      = $(wildcard public/*)

CC          = emcc

# Resolve `node` to a real executable rather than letting a recipe do a bare
# PATH lookup. emsdk puts <emsdk> on PATH and <emsdk>/node is a DIRECTORY (the
# bundled Node install); GNU make execs a simple recipe line itself rather than
# through a shell, so its PATH search stops at that directory and fails with
#
#     make: node: Permission denied        (make then reports Error 127)
#
# A shell skips directories, which is why this bites only the one recipe that
# runs node directly, and why it never reproduces on macOS — no such directory
# is on PATH there. Same helper as apollo-lib's Makefile.wasm, which hit it
# first with cmake.
#
# $(wildcard $(d)/$(1)/.) is non-empty only when $(d)/$(1) IS a directory, so
# this takes the first PATH entry holding a $(1) that is not one, and falls
# back to the bare name so a genuinely missing tool still reports itself.
find_exe = $(firstword $(foreach d,$(subst :, ,$(PATH)), \
			  $(if $(wildcard $(d)/$(1)/.),,$(wildcard $(d)/$(1)))) $(1))

NODE       ?= $(call find_exe,node)

# wasm-opt, for the --spill-pointers step below. emsdk puts it on PATH; a
# Homebrew emscripten keeps it in libexec/binaryen/bin, reachable only after
# resolving the emcc symlink. Override WASM_OPT if it lives anywhere else.
#
# Backticks rather than $(...) for the command substitution on purpose: inside
# $(shell ...) make would read the closing paren as the end of its own call.
EMCC_REAL  := $(shell readlink -f `command -v emcc` 2>/dev/null || command -v emcc 2>/dev/null)

WASM_OPT   ?= $(firstword \
			  $(shell command -v wasm-opt 2>/dev/null) \
			  $(wildcard $(dir $(EMCC_REAL))../libexec/binaryen/bin/wasm-opt) \
			  $(wildcard $(dir $(EMCC_REAL))../binaryen/bin/wasm-opt) \
			  $(wildcard $(dir $(EMCC_REAL))../bin/wasm-opt) \
			  $(wildcard $(dir $(EMCC_REAL))wasm-opt))

# wasm-opt has to be told the same feature set the linker emitted or it refuses
# the module.
WASM_FEATURES = --enable-bulk-memory --enable-bulk-memory-opt \
				--enable-call-indirect-overlong --enable-multivalue \
				--enable-mutable-globals --enable-nontrapping-float-to-int \
				--enable-reference-types --enable-sign-ext

CFLAGS      = -O3 -Wall \
			  -D_GNU_SOURCE -D_USE_MBEDTLS \
			  -I$(APOLLO_LIB)/include \
			  -I$(APOLLO_LIB)/tools \
			  -I$(APOLLO_LIB)/mbedtls-2.16.12/include \
			  -I../core \
			  -I../core/psp \
			  -I../core/ps3 \
			  --use-port=zlib

# -sSTACK_SIZE: Emscripten's 64KB default is below MicroPython's own 40KB stack
#   limit, so Python codes fail immediately without this.
# -sALLOW_MEMORY_GROWTH: apollo_apply_py_code() allocates a 64MB Python heap.
# -sMODULARIZE/-sEXPORT_ES6: the worker imports it with a plain `import`.
# -sENVIRONMENT=worker: this module only ever runs inside the Web Worker, so
#   the browser/node bootstrap paths are dead weight.
# -sINVOKE_RUN=0: there is no main(); we only call exported functions.
# --profiling-funcs: keeps the wasm name section, which the --spill-pointers
#   step below needs to locate __stack_pointer. It is NOT for profiling here and
#   it is not -g: no DWARF, so emcc still runs the full -O3 binaryen pipeline.
#   The names are stripped again right after spilling.
#
# Note there is no --embed-file: the worker writes the Python helper modules
# into /python at run time (see public/worker.js). libapollo resolves imports
# against "python" relative to the FS root when no host callback is set, and
# MicroPython's import stat()s real paths, so files written after startup work.
LDFLAGS     = -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=worker \
			  --profiling-funcs \
			  -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=4MB -sINVOKE_RUN=0 \
			  -sEXPORTED_RUNTIME_METHODS='["FS","UTF8ToString","stringToNewUTF8","HEAPU8"]' \
			  -sEXPORTED_FUNCTIONS='["_malloc","_free","_apw_version","_apw_is_be","_apw_open","_apw_close","_apw_codes_json","_apw_code_text","_apw_set_code_text","_apw_code_is_edited","_apw_code_flags","_apw_set_code_type","_apw_code_type","_apw_export_patch","_apw_export_size","_apw_export_mismatches","_apw_revert_code","_apw_set_option","_apw_apply","_apw_reset_vars","_apw_psp_sfo_json","_apw_psp_key_from_file","_apw_psp_key_from_db","_apw_psp_decrypt","_apw_psp_encrypt","_apw_psp_resign","_apw_psp_set_fuse_id","_apw_psp_fuse_id","_apw_savedata_out","_apw_savedata_out_size","_apw_psp_error","_apw_ps3_pfd_json","_apw_ps3_sfo_directory","_apw_ps3_key_from_hex","_apw_ps3_key_from_db","_apw_ps3_decrypt","_apw_ps3_encrypt","_apw_ps3_verify","_apw_ps3_resign","_apw_ps3_error","_apw_ps3_set_console","_apw_ps3_console","_apw_ps3_dhk_from_db","_apw_ps3_rebind","_apw_ps3_account_id","_apw_ps3_account_resign"]'

.PHONY: all serve verify clean

all: $(MODULE) $(INDEX) $(MODLIST) $(TOOLS) $(GUIDE) static

# The --spill-pointers step is not optional: without it the MicroPython path
# corrupts memory in this module.
#
# MicroPython's GC scans the C stack conservatively (micropy_gc_collect() in
# apollo-lib walks from &regs up to vm.stack_top). Under wasm that finds almost
# nothing -- locals live in wasm locals, not addressable memory, so a measured
# run had 1276 bytes of shadow stack for the whole live VM call chain. Live
# objects go unseen, get swept, and the next free of one aborts the module with
# `assert(!"bad free")`. Applying a Python patch here failed outright before
# this step existed; ~50 patches in the database contain Python codes.
#
# It has to be a SEPARATE wasm-opt invocation rather than
# -sBINARYEN_EXTRA_PASSES=--spill-pointers, because that route cannot work:
# the pass finds the stack pointer by NAME, so without the name section it
# stops with "Fatal: getStackSpace: failed to find the stack pointer" -- and
# asking emcc for names with -g makes it report "running limited binaryen
# optimizations because DWARF info requested" and silently drop the extra
# passes. --profiling-funcs above threads between the two.
#
# Cost: the spilled module is ~920KB against ~600KB, 317KB gzipped against
# 243KB. That is the price of a correct MicroPython.
$(MODULE): $(SRCS) $(LIBAPOLLO) $(LIBMBED) Makefile | $(DIST)
	$(CC) $(CFLAGS) $(SRCS) $(LIBAPOLLO) $(LIBMBED) $(LDFLAGS) -o $@
	@[ -x "$(WASM_OPT)" ] || { \
		echo "wasm-opt not found — needed for the --spill-pointers step."; \
		echo "set WASM_OPT=/path/to/wasm-opt"; exit 2; }
	$(WASM_OPT) --spill-pointers --strip-debug \
		$(DIST)/apollo.wasm -o $(DIST)/apollo.wasm $(WASM_FEATURES)
	@echo "built $@ (--spill-pointers applied)"

# The browsable index of the database. Reading ~2250 files is trivial here and
# impossible from a browser, so the page fetches this instead of crawling.
$(INDEX): ../tools/build-index.py $(PYTHON_MODULES) | $(DIST)
	@python3 ../tools/build-index.py $(APOLLO_PATCHES) $@

# The tools page's catalog: one row per patch that can decrypt, re-encrypt or
# fix a checksum, of the ones proven against a real save.
#
# --verified-only is the whole scoping decision for the first release. The
# catalog knows about 1156 patches; 187 of them have been run end to end by
# `make verify`, and only those are listed. Drop the flag to publish the rest
# as an unverified long tail — but then the page promises something nobody has
# checked.
#
# ../tools/verified.json is a committed input (see above), so a clean checkout
# builds the page without needing the game saves.
$(TOOLS): ../tools/build-index.py $(VERIFIED) | $(DIST)
	@python3 ../tools/build-index.py $(APOLLO_PATCHES) $@ \
		--format=tools --verified=$(VERIFIED) --verified-only

$(VERIFIED):
	@echo "missing $@ — the list of patches proven against a real save."
	@echo "regenerate it with:  make verify SAMPLES=/path/to/save-decrypters"
	@exit 2

# Just the helper-module names, so the worker knows what to fetch without
# crawling a directory listing. 300 bytes.
$(MODLIST): ../tools/build-index.py $(PYTHON_MODULES) | $(DIST)
	@python3 ../tools/build-index.py $(APOLLO_PATCHES) $@ --format=modules

# The guide page. Depends on the renderer as well as the markdown, so editing
# either rebuilds it. `static` copies guide.css alongside, like every other
# stylesheet in public/.
$(GUIDE): $(GUIDE_SRC) ../tools/make-guide.py | $(DIST)
	@python3 ../tools/make-guide.py $(GUIDE_SRC) $@

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

# Prove the catalogued tools against real saves, and write dist/verified.json.
#
# Needs a clone of https://github.com/bucanero/save-decrypters for the samples
# (not vendored — they are real game saves):
#
#   make verify SAMPLES=/path/to/save-decrypters
#
# The unfiltered catalog goes to the verify scratch dir (removed before the
# site is published): the expansion pass needs to see every patch, not just
# the ones already believed good.
#
# The module is rebuilt for node because the shipped one is -sENVIRONMENT=worker
# and will not load outside one. Only the JS bootstrap differs; the .wasm is
# byte-identical, so what this proves is what ships.
VERIFY_DIST = $(DIST)/node
VERIFY_MOD  = $(VERIFY_DIST)/apollo.mjs

verify: $(INDEX) | $(DIST)
	@[ -n "$(SAMPLES)" ] || { echo "set SAMPLES=/path/to/save-decrypters"; exit 2; }
	@mkdir -p $(VERIFY_DIST)
	$(CC) $(CFLAGS) $(SRCS) $(LIBAPOLLO) $(LIBMBED) \
		$(subst -sENVIRONMENT=worker,-sENVIRONMENT=node,$(LDFLAGS)) -o $(VERIFY_MOD)
	@[ -x "$(WASM_OPT)" ] || { echo "wasm-opt not found; set WASM_OPT=..."; exit 2; }
	@$(WASM_OPT) --spill-pointers --strip-debug \
		$(VERIFY_DIST)/apollo.wasm -o $(VERIFY_DIST)/apollo.wasm $(WASM_FEATURES)
	@cmp -s $(VERIFY_DIST)/apollo.wasm $(DIST)/apollo.wasm \
		&& echo "verify module wasm == shipped wasm" \
		|| echo "NOTE: verify wasm differs from dist/apollo.wasm (rebuild dist?)"
	@python3 ../tools/build-index.py $(APOLLO_PATCHES) $(VERIFY_DIST)/all-tools.json \
		--format=tools
	$(NODE) ../tools/verify-tools.mjs $(APOLLO_PATCHES) $(SAMPLES) \
		--module $(VERIFY_MOD) --catalog $(VERIFY_DIST)/all-tools.json \
		--out $(VERIFIED)

# Static assets are copied rather than referenced so that dist/ is a complete,
# self-contained site — the same tree the Pages workflow uploads.
.PHONY: static
# The patcher ships as BOTH index.html and patcher.html, so a deployment can
# make either page the landing one without rewriting a single link. This site
# keeps the patcher at /; the save-decrypters site copies tools.html over
# index.html and gets the tools at / instead. tools.html links to
# ./patcher.html, which is correct in both.
# The guide's images come from docs/ rather than public/, and are copied here
# rather than in the $(GUIDE) rule: that rule only reruns when the markdown or
# the renderer changes, so a newly ADDED screenshot would never reach dist/.
# This target always runs.
static: | $(DIST)
	@cp $(STATIC) $(DIST)/
	@cp $(DIST)/index.html $(DIST)/patcher.html
	@mkdir -p $(DIST)/images && cp ../docs/images/* $(DIST)/images/
	@echo "copied $(words $(STATIC)) static files to $(DIST)/ (+ patcher.html, images/)"

$(DIST):
	@mkdir -p $@

serve: all
	@echo "serving $(DIST) on http://localhost:8000 (ctrl-c to stop)"
	@cd $(DIST) && python3 -m http.server 8000

clean:
	@rm -rf $(DIST)
	@echo "Cleaned up!"
