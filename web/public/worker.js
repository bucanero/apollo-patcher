/*
 * Web Worker that owns the wasm module.
 *
 * Everything the engine does is synchronous and some of it is slow (a Python
 * code allocates a 64MB heap and runs an interpreter), so it all happens here
 * rather than on the main thread — the UI stays responsive and the log panel
 * keeps painting.
 *
 * Protocol: the page posts {id, type, ...} and gets back {id, ok, ...}. Every
 * reply carries `log`, the engine output produced during that call.
 */
import createApollo from './apollo.mjs';
import { CDN } from './cdn.js';

const WORKDIR = '/work';
const PYDIR   = '/python';
let M = null;

async function ready() {
    if (!M) {
        M = await createApollo();
        try {
            M.FS.mkdir(WORKDIR);
        } catch (e) {
            /* already there */
        }
    }
    return M;
}

/* The C log sink pushes complete lines onto globalThis.apolloLog (see
 * web/src/apollo_wasm.c). Take and clear whatever accumulated. */
function drainLog() {
    const lines = globalThis.apolloLog || [];
    globalThis.apolloLog = [];
    return lines;
}

/*
 * Python helper modules.
 *
 * Python patches `import` these (rijndael, umsgpack, per-game decrypters).
 * They used to be baked into the wasm module with --embed-file, which cost
 * every visitor 133KB gzipped — 31% of the download — for something 35 of 2240
 * patches use, and froze them at build time while the patches that import them
 * are fetched live. Now they are fetched too, and written into the in-memory
 * filesystem; MicroPython's import does stat()/open() and does not care that
 * the files arrived after startup.
 *
 * Fetched as a set rather than per import, because the modules import each
 * other (umsgpack pulls in datetime) — resolving that from the outside would
 * break the first time someone adds an import upstream. The cost of that
 * simplification is small: 51 patches contain Python at all, and only 16 of
 * those import nothing but built-ins, so the set is rarely fetched in vain.
 */
let modulesReady = null;

async function fetchModules() {
    /* The list is generated at build time, so nothing has to crawl a
     * directory listing. 300 bytes, and only ever requested when a patch
     * actually contains Python. */
    const listed = await fetch('./python-modules.json');
    if (!listed.ok) throw new Error(`module list unavailable (${listed.status})`);
    const { modules } = await listed.json();

    /* Fetch everything before writing anything: a partial set on disk would
     * surface as a bare ImportError from inside a patch, which looks like a
     * broken patch rather than a failed download. */
    const fetched = await Promise.all(modules.map(async (name) => {
        const res = await fetch(`${CDN}/python/${name}`);
        if (!res.ok) throw new Error(`${name} (${res.status})`);
        return [name, new Uint8Array(await res.arrayBuffer())];
    }));

    try {
        M.FS.mkdir(PYDIR);
    } catch (e) {
        /* already there */
    }
    for (const [name, bytes] of fetched) M.FS.writeFile(`${PYDIR}/${name}`, bytes);
    return fetched.length;
}

function ensureModules() {
    if (!modulesReady) {
        modulesReady = fetchModules().catch((err) => {
            modulesReady = null;      /* let the next attempt retry */
            throw err;
        });
    }
    return modulesReady;
}

function withCString(str, fn) {
    const ptr = M.stringToNewUTF8(str);
    try {
        return fn(ptr);
    } finally {
        M._free(ptr);
    }
}

function withBytes(bytes, fn) {
    const ptr = M._malloc(bytes.length);
    try {
        M.HEAPU8.set(bytes, ptr);
        return fn(ptr, bytes.length);
    } finally {
        M._free(ptr);
    }
}

/* MEMFS path for the save file. The name is only a label here, but keep it
 * recognisable in the log and strip anything that could escape WORKDIR. */
function workPath(name) {
    const safe = (name || 'savedata').replace(/[^\w.\- ]+/g, '_');
    return `${WORKDIR}/${safe}`;
}

const TYPE_PYTHON = 3;    /* APOLLO_CODE_PYTHON */
let codeTypes = [];       /* per-row type from the last open() */

/* What one code looks like now. `flags` comes back with it because emptying a
 * body (or filling an empty one) moves APOLLO_CODE_FLAG_EMPTY, and that is
 * what decides whether the page lets the row be ticked. The type never moves:
 * it was decided by the [...] header at parse time, so codeTypes stays valid
 * across an edit. */
function codeState(index) {
    return {
        text: M.UTF8ToString(M._apw_code_text(index)),
        edited: !!M._apw_code_is_edited(index),
        flags: M._apw_code_flags(index),
    };
}

const handlers = {
    async version() {
        await ready();
        return { version: M.UTF8ToString(M._apw_version()) };
    },

    /* Parse a .savepatch held in memory and return the full code list. */
    async open({ buffer, name, platform }) {
        await ready();
        const bytes = new Uint8Array(buffer);
        const ok = withBytes(bytes, (ptr, len) =>
            withCString(name || 'patch.savepatch', (np) => M._apw_open(ptr, len, np)),
        );
        if (!ok) {
            codeTypes = [];
            return { ok: false, error: 'Could not parse this patch file.' };
        }

        const doc = JSON.parse(M.UTF8ToString(M._apw_codes_json()));
        codeTypes = doc.codes.map((c) => c.type);

        /* PS3 save data is big-endian, and no patch in the database declares
         * the order per code, so the mode is decided here (shared with the
         * desktop GUI — see apctl_is_big_endian_for).
         *
         * A patch picked from the database carries its platform: that is the
         * directory it lives in, so it is authoritative and no title-ID
         * guessing is needed. A file the user supplied has none, so fall back
         * to its name, then to its own first lines for a file that has been
         * renamed. latin1 because a title ID is ASCII and the header may hold
         * stray high bytes ('latin1' is the valid label; 'latin-1' throws). */
        const plat = platform || '';
        const askBe = (text) =>
            withCString(plat, (pp) => withCString(text, (tp) => M._apw_is_be(pp, tp)));

        const head = new TextDecoder('latin1').decode(bytes.subarray(0, 128));
        const bigEndian = !!(plat ? askBe('') : (askBe(name || '') || askBe(head)));

        /* Start pulling the helper modules now, while the user reads the code
         * list, so Apply rarely has to wait. Failures are reported then, not
         * here — the patch may not need them at all. */
        if (codeTypes.some((t) => t === TYPE_PYTHON))
            ensureModules().catch(() => {});

        return { ok: true, bigEndian, ...doc };
    },

    async codeText({ index }) {
        await ready();
        return { text: M.UTF8ToString(M._apw_code_text(index)) };
    },

    /*
     * Replace one code's body for this session.
     *
     * Answers with what the engine actually holds afterwards rather than
     * echoing the request: setting the original text back is not an edit, so
     * `edited` is the engine's own verdict and the page marks the row from it.
     */
    async setCodeText({ index, text }) {
        await ready();
        const ok = withCString(text, (tp) => M._apw_set_code_text(index, tp));
        if (!ok) return { ok: false, error: 'Could not store the edited code.' };

        return codeState(index);
    },

    async revertCode({ index }) {
        await ready();
        M._apw_revert_code(index);
        return codeState(index);
    },

    /*
     * Apply codes to a fresh copy of the save data.
     *
     * The engine patches a file in place, so a second run over an
     * already-patched file would stack changes. Writing the original bytes
     * every time makes Apply idempotent: what you get always reflects exactly
     * the codes currently ticked.
     */
    async apply({ indices, options, save, saveName, bigEndian }) {
        await ready();

        /* Only the codes actually being applied matter: a patch can hold a
         * Python code the user did not tick. */
        if (indices.some((i) => codeTypes[i] === TYPE_PYTHON)) {
            globalThis.apolloLog ||= [];
            globalThis.apolloLog.push('Loading Python helper modules...');
            try {
                await ensureModules();
            } catch (err) {
                return {
                    ok: false,
                    error: `Could not load the Python helper modules (${err.message}). ` +
                           'These codes need them; check your connection and try again.',
                };
            }
        }

        const path = workPath(saveName);
        M.FS.writeFile(path, new Uint8Array(save));

        for (const [index, groups] of Object.entries(options || {}))
            groups.forEach((value, group) => M._apw_set_option(Number(index), group, value));

        const results = withCString(path, (p) =>
            indices.map((index) => ({
                index,
                ok: !!M._apw_apply(index, p, bigEndian ? 1 : 0),
            })),
        );

        /* Patch variables accumulate across applies; drop them after a batch,
         * exactly like the desktop GUI does. */
        M._apw_reset_vars();

        const out = M.FS.readFile(path);
        M.FS.unlink(path);
        return { ok: true, results, patched: out };
    },
};

self.onmessage = async (ev) => {
    const { id, type, ...args } = ev.data;
    try {
        const result = (await handlers[type](args)) || {};
        const reply = { id, ok: result.ok !== false, ...result, log: drainLog() };

        /* Hand the patched bytes over instead of copying them. */
        self.postMessage(reply, result.patched ? [result.patched.buffer] : []);
    } catch (err) {
        self.postMessage({ id, ok: false, error: String(err && err.message || err), log: drainLog() });
    }
};
