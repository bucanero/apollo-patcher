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

const WORKDIR = '/work';
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

const handlers = {
    async version() {
        await ready();
        return { version: M.UTF8ToString(M._apw_version()) };
    },

    /* Parse a .savepatch held in memory and return the full code list. */
    async open({ buffer, name }) {
        await ready();
        const bytes = new Uint8Array(buffer);
        const ok = withBytes(bytes, (ptr, len) =>
            withCString(name || 'patch.savepatch', (np) => M._apw_open(ptr, len, np)),
        );
        if (!ok) return { ok: false, error: 'Could not parse this patch file.' };

        return { ok: true, ...JSON.parse(M.UTF8ToString(M._apw_codes_json())) };
    },

    async codeText({ index }) {
        await ready();
        return { text: M.UTF8ToString(M._apw_code_text(index)) };
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
