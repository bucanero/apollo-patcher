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
 * They are fetched at run time and written into the in-memory filesystem;
 * MicroPython's import does stat()/open() and does not care that the files
 * arrived after startup.
 *
 * Fetched rather than embedded with --embed-file, which would cost every
 * visitor 133KB gzipped — 31% of the download — for something 35 of 2247
 * patches use, and would freeze them at build time while the patches that
 * import them are fetched live.
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
function workPath(name, slot = 0) {
    const safe = (name || 'savedata').replace(/[^\w.\- ]+/g, '_');
    if (!slot) return `${WORKDIR}/${safe}`;
    /* A tool can want two files with the SAME name in different folders
     * (LUNAR Remastered's two GAME.BINs), so give every input past the first
     * its own directory rather than mangling the name the engine sees. */
    const dir = `${WORKDIR}/${slot}`;
    try { M.FS.mkdir(dir); } catch { /* already there */ }
    return `${dir}/${safe}`;
}

const sameBytes = (a, b) =>
    a.length === b.length && a.every((v, i) => v === b[i]);

const TYPE_PYTHON = 3;    /* APOLLO_CODE_PYTHON */
let codeTypes = [];       /* per-row type from the last open() */

/* What one code looks like now: whatever the caller just did, this is the
 * engine's own answer.
 *
 * `flags` comes back because emptying a body (or filling an empty one) moves
 * APOLLO_CODE_FLAG_EMPTY, which decides whether the page lets the row be
 * ticked. `type` comes back because the type selector moves it (and revert
 * moves it back) — and codeTypes has to follow, since it is what decides
 * whether the Python helper modules get fetched at all. */
function codeState(index) {
    const type = M._apw_code_type(index);
    codeTypes[index] = type;

    /* Switched to Python after load: start the modules now, the same way
     * open() does for a patch that already contained one. Apply awaits it. */
    if (type === TYPE_PYTHON) ensureModules().catch(() => {});

    return {
        text: M.UTF8ToString(M._apw_code_text(index)),
        edited: !!M._apw_code_is_edited(index),
        flags: M._apw_code_flags(index),
        type,
    };
}

/* ---- PSP savedata ------------------------------------------------------
 *
 * The PSP's own encryption, which wraps a save BELOW anything a .savepatch
 * touches. No session, no patch: these run for any PSP save at all.
 *
 * Buffers are copied into the wasm heap by hand rather than through
 * withBytes(), because each call needs two or three live at once. Note that
 * M.HEAPU8 is re-read after every allocation and after every call: the module
 * is built with ALLOW_MEMORY_GROWTH, and a malloc that grows memory detaches
 * whatever view was captured before it.
 */

function alloc(bytes) {
    const ptr = M._malloc(bytes.length || 1);
    M.HEAPU8.set(bytes, ptr);
    return ptr;
}

/* Copy a heap range out. Always a copy: the wasm side frees this buffer on
 * the next PSP call, and growth can detach the view before then anyway. */
function heapCopy(ptr, len) {
    return new Uint8Array(M.HEAPU8.subarray(ptr, ptr + len));
}

/*
 * Whatever the last PSP or PS3 savedata call produced, or null.
 *
 * One buffer serves both consoles, and that is safe because of how these
 * handlers are shaped: each awaits ready() once and then runs straight through
 * -- allocate, call, read the result -- with no further await. Two messages
 * arriving together therefore cannot interleave between a call filling this
 * buffer and its handler reading it.
 */
function savedataOut() {
    const ptr = M._apw_savedata_out();
    const len = M._apw_savedata_out_size();
    return ptr && len > 0 ? heapCopy(ptr, len) : null;
}

const pspError = (rc) => M.UTF8ToString(M._apw_psp_error(rc));
const ps3Error = (rc) => M.UTF8ToString(M._apw_ps3_error(rc));

const toHex = (bytes) =>
    [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('').toUpperCase();



const handlers = {
    async version() {
        await ready();
        return { version: M.UTF8ToString(M._apw_version()) };
    },

    /*
     * What a PARAM.SFO says about its save: the directory name (which is the
     * game-key lookup), the mode byte, and the list of files that are
     * encrypted at all. Everything the PSP panel needs to render itself.
     */
    async pspInfo({ sfo }) {
        await ready();
        const bytes = new Uint8Array(sfo);
        const ptr = alloc(bytes);
        try {
            const info = JSON.parse(
                M.UTF8ToString(M._apw_psp_sfo_json(ptr, bytes.length)));
            return info.ok ? info : { ok: false, error: info.error };
        } finally {
            M._free(ptr);
        }
    },

    /* A game key out of a dumper's file (SGKeyDumper 0x10, SGDeemer 0x600).
     * The recognised shapes live in C so the page and the desktop app agree. */
    async pspKeyFromFile({ buffer }) {
        await ready();
        const bytes = new Uint8Array(buffer);
        const src = alloc(bytes);
        const dst = M._malloc(16);
        try {
            const rc = M._apw_psp_key_from_file(src, bytes.length, dst);
            if (rc !== 0) return { ok: false, error: pspError(rc) };
            const key = heapCopy(dst, 16);
            return {
                ok: true,
                key,
                hex: toHex(key),
            };
        } finally {
            M._free(src);
            M._free(dst);
        }
    },

    /*
     * A game key out of apollo-patches' PSP/gamekeys.txt, by save directory.
     *
     * The page fetches the file; the matching happens in C, so the prefix rule
     * and its longest-match tie-break are shared with the desktop app instead
     * of reimplemented here. (That tie-break is load-bearing: the database
     * holds both NPJJ30022 and NPJJ30022GAME1, with different keys.)
     */
    async pspKeyFromDb({ text, directory }) {
        await ready();
        const bytes = new TextEncoder().encode(text);
        const tp = alloc(bytes);
        const dp = M.stringToNewUTF8(directory || '');
        const kp = M._malloc(16);
        const ip = M._malloc(64);
        try {
            const rc = M._apw_psp_key_from_db(tp, bytes.length, dp, kp, ip, 64);
            if (rc !== 0) return { ok: false, error: pspError(rc) };
            const key = heapCopy(kp, 16);
            /* `entry`, not `id`: the reply envelope is {id, ok, ...result},
             * so a result field called `id` overwrites the message id and the
             * page never matches the reply to its request. */
            return {
                ok: true,
                key,
                entry: M.UTF8ToString(ip),
                hex: toHex(key),
            };
        } finally {
            M._free(tp); M._free(dp); M._free(kp); M._free(ip);
        }
    },

    async pspDecrypt({ sfo, data, name, key }) {
        await ready();
        const s = new Uint8Array(sfo);
        const d = new Uint8Array(data);
        const k = new Uint8Array(key);
        const sp = alloc(s), dp = alloc(d), kp = alloc(k);
        try {
            const rc = M._apw_psp_decrypt(sp, s.length, dp, d.length, kp);
            if (rc !== 0) return { ok: false, error: pspError(rc) };
            return { ok: true, files: [{ name, bytes: savedataOut(), changed: true }] };
        } finally {
            M._free(sp); M._free(dp); M._free(kp);
        }
    },

    /*
     * Encryption rewrites PARAM.SFO as well as producing the data file -- the
     * file's own hash goes into its SAVEDATA_FILE_LIST entry, and the two
     * SFO-wide hashes are regenerated over the result. Both come back, in the
     * same `files` shape apply() uses, because a save put back with a stale
     * PARAM.SFO does not load and the page must not offer only one of them.
     */
    async pspEncrypt({ sfo, data, name, key }) {
        await ready();
        const s = new Uint8Array(sfo);
        const d = new Uint8Array(data);
        const k = new Uint8Array(key);
        const sp = alloc(s), dp = alloc(d), kp = alloc(k);
        /* The name has to cross as a C string, not a JS one: it is the
         * SAVEDATA_FILE_LIST entry the hash gets written into. */
        const np = M.stringToNewUTF8(name || '');
        try {
            const rc = M._apw_psp_encrypt(sp, s.length, np, dp, d.length, kp);
            if (rc !== 0) return { ok: false, error: pspError(rc) };
            /* The SFO was rewritten in place, at the pointer we passed in. */
            return {
                ok: true,
                files: [
                    { name, bytes: savedataOut(), changed: true },
                    { name: 'PARAM.SFO', bytes: heapCopy(sp, s.length), changed: true },
                ],
            };
        } finally {
            M._free(sp); M._free(dp); M._free(kp); M._free(np);
        }
    },

    /* Regenerate the PARAM.SFO hashes alone -- what an already-plaintext save
     * needs after something edited it. */
    async pspResign({ sfo }) {
        await ready();
        const s = new Uint8Array(sfo);
        const sp = alloc(s);
        try {
            const rc = M._apw_psp_resign(sp, s.length);
            if (rc !== 0) return { ok: false, error: pspError(rc) };
            const bytes = heapCopy(sp, s.length);
            return {
                ok: true,
                files: [{ name: 'PARAM.SFO', bytes, changed: !sameBytes(bytes, s) }],
            };
        } finally {
            M._free(sp);
        }
    },

    /* ---- PS3 savedata --------------------------------------------------
     *
     * Same shape as the PSP handlers above, one console up. Two differences
     * show up in every signature: the metadata file is PARAM.PFD rather than
     * PARAM.SFO, and the key is per file as well as per game, so `sfid` rides
     * along with the name. A null `sfid` is passed to C as a null pointer and
     * means "this entry has a built-in key" -- PARAM.SFO and the trophy files.
     *
     * Names cross as C strings rather than JS ones: they are looked up against
     * the PFD's own 65-byte name fields.
     */

    /*
     * What a PARAM.PFD says about its save: version, whether it is a trophy
     * folder, and the entries. That entry list IS the answer to what is
     * protected, so the panel offers exactly those files and no others.
     */
    async ps3Info({ pfd }) {
        await ready();
        const bytes = new Uint8Array(pfd);
        const ptr = alloc(bytes);
        try {
            const info = JSON.parse(
                M.UTF8ToString(M._apw_ps3_pfd_json(ptr, bytes.length)));
            return info.ok ? info : { ok: false, error: info.error };
        } finally {
            M._free(ptr);
        }
    },

    /*
     * A secure file ID out of apollo-patches' PS3/games.conf.
     *
     * The page fetches the file; the matching happens in C, so both rules the
     * database needs live with the desktop app rather than being reimplemented
     * here: the section is the LONGEST directory prefix (DiRT 3 files
     * BLUS30724 and BLUS30724PROFILE separately, with different keys), and the
     * file pattern is the FIRST match in file order (Devil May Cry lists DATA
     * before *).
     */
    async ps3KeyFromDb({ text, directory, file }) {
        await ready();
        const bytes = new TextEncoder().encode(text);
        const tp = alloc(bytes);
        const dp = M.stringToNewUTF8(directory || '');
        const fp = M.stringToNewUTF8(file || '');
        const kp = M._malloc(16);
        const ip = M._malloc(80);
        try {
            const rc = M._apw_ps3_key_from_db(tp, bytes.length, dp, fp, kp, ip, 80);
            if (rc !== 0) return { ok: false, error: ps3Error(rc) };
            const key = heapCopy(kp, 16);
            /* `entry`, not `id`: the reply envelope is {id, ok, ...result}. */
            return { ok: true, key, entry: M.UTF8ToString(ip), hex: toHex(key) };
        } finally {
            M._free(tp); M._free(dp); M._free(fp); M._free(kp); M._free(ip);
        }
    },

    /*
     * SAVEDATA_DIRECTORY out of a PS3 PARAM.SFO, which is what games.conf
     * files its sections under. PARAM.PFD does not carry it, so a save folder
     * that is not simply its title id -- DiRT 3's profile saves -- can only be
     * identified from the SFO, or by the user saying so.
     */
    async ps3Folder({ sfo }) {
        await ready();
        const bytes = new Uint8Array(sfo);
        const ptr = alloc(bytes);
        try {
            return { ok: true, folder: M.UTF8ToString(M._apw_ps3_sfo_directory(ptr, bytes.length)) };
        } finally {
            M._free(ptr);
        }
    },

    /* Which PSN account a PARAM.SFO is currently signed to, as 16 hex digits.
     * "" when the file carries neither of the two fields that hold it. */
    async ps3AccountId({ sfo }) {
        await ready();
        const bytes = new Uint8Array(sfo);
        const ptr = alloc(bytes);
        try {
            return { ok: true, account: M.UTF8ToString(M._apw_ps3_account_id(ptr, bytes.length)) };
        } finally {
            M._free(ptr);
        }
    },

    /* 32 hex digits the user typed, validated by the same parser the database
     * lookup uses. */
    async ps3KeyFromHex({ hex }) {
        await ready();
        const hp = M.stringToNewUTF8((hex || '').trim());
        const kp = M._malloc(16);
        try {
            const rc = M._apw_ps3_key_from_hex(hp, kp);
            if (rc !== 0) return { ok: false, error: ps3Error(rc) };
            const key = heapCopy(kp, 16);
            return { ok: true, key, hex: toHex(key) };
        } finally {
            M._free(hp); M._free(kp);
        }
    },

    /*
     * Does the PARAM.PFD's recorded hash match this file? Offered before
     * anything else touches the save: one that already disagrees was damaged
     * before it got here, and patching would sign the damage into place.
     */
    async ps3Verify({ pfd, data, name, sfid }) {
        await ready();
        const p = new Uint8Array(pfd);
        const d = new Uint8Array(data);
        const pp = alloc(p), dp = alloc(d);
        const kp = sfid ? alloc(new Uint8Array(sfid)) : 0;
        const np = M.stringToNewUTF8(name || '');
        try {
            const rc = M._apw_ps3_verify(pp, p.length, np, dp, d.length, kp);
            return rc === 0 ? { ok: true } : { ok: false, error: ps3Error(rc) };
        } finally {
            M._free(pp); M._free(dp); M._free(np); if (kp) M._free(kp);
        }
    },

    async ps3Decrypt({ pfd, data, name, sfid }) {
        await ready();
        const p = new Uint8Array(pfd);
        const d = new Uint8Array(data);
        const pp = alloc(p), dp = alloc(d);
        const kp = sfid ? alloc(new Uint8Array(sfid)) : 0;
        const np = M.stringToNewUTF8(name || '');
        try {
            const rc = M._apw_ps3_decrypt(pp, p.length, np, dp, d.length, kp);
            if (rc !== 0) return { ok: false, error: ps3Error(rc) };
            return { ok: true, files: [{ name, bytes: savedataOut(), changed: true }] };
        } finally {
            M._free(pp); M._free(dp); M._free(np); if (kp) M._free(kp);
        }
    },

    /*
     * Encryption rewrites PARAM.PFD as well as producing the data file -- the
     * entry's size and hash change, and the signatures over the whole table
     * are regenerated to match. Both come back, in the same `files` shape
     * apply() uses, because a save put back with a stale PARAM.PFD does not
     * load and the page must not offer only one of them.
     */
    async ps3Encrypt({ pfd, data, name, sfid }) {
        await ready();
        const p = new Uint8Array(pfd);
        const d = new Uint8Array(data);
        const pp = alloc(p), dp = alloc(d);
        const kp = sfid ? alloc(new Uint8Array(sfid)) : 0;
        const np = M.stringToNewUTF8(name || '');
        try {
            const rc = M._apw_ps3_encrypt(pp, p.length, np, dp, d.length, kp);
            if (rc !== 0) return { ok: false, error: ps3Error(rc) };
            /* The PFD was rewritten in place, at the pointer we passed in. */
            return {
                ok: true,
                files: [
                    { name, bytes: savedataOut(), changed: true },
                    { name: 'PARAM.PFD', bytes: heapCopy(pp, p.length), changed: true },
                ],
            };
        } finally {
            M._free(pp); M._free(dp); M._free(np); if (kp) M._free(kp);
        }
    },

    /* Regenerate the PARAM.PFD signatures alone -- what a PFD whose entries
     * something else edited needs before the console will accept it. */
    async ps3Resign({ pfd }) {
        await ready();
        const p = new Uint8Array(pfd);
        const pp = alloc(p);
        try {
            const rc = M._apw_ps3_resign(pp, p.length);
            if (rc !== 0) return { ok: false, error: ps3Error(rc) };
            const bytes = heapCopy(pp, p.length);
            return {
                ok: true,
                files: [{ name: 'PARAM.PFD', bytes, changed: !sameBytes(bytes, p) }],
            };
        } finally {
            M._free(pp);
        }
    },

    /* ---- which console a save is for ------------------------------------
     *
     * Two settings, one per console, both of which change what the wrapping
     * side WRITES and neither of which affects unwrapping:
     *
     *   the PSP's Fuse ID    reaches the two PARAM.SFO hashes that savedata
     *                        modes 4 and 6 derive from the console's own fuse.
     *                        Not enforced by the console, but console-locked
     *                        games check it themselves (Gran Turismo does)
     *   the PS3's console ID reaches PARAM.SFO's second hash in PARAM.PFD,
     *                        which is what binds a save to one machine
     *
     * They live in the wasm module, which is this worker's alone, so the page
     * pushes them here whenever they change and at start-up. Sending an empty
     * string for either clears it back to the default.
     */
    async settings({ pspFuseId, ps3ConsoleId, ps3UserId }) {
        await ready();
        const errors = [];

        if (pspFuseId !== undefined) {
            const hp = M.stringToNewUTF8(pspFuseId || '');
            const rc = M._apw_psp_set_fuse_id(hp);
            M._free(hp);
            if (rc !== 0) errors.push(`Fuse ID: ${pspError(rc)}`);
        }

        if (ps3ConsoleId !== undefined) {
            const cp = M.stringToNewUTF8(ps3ConsoleId || '');
            const rc = M._apw_ps3_set_console(cp, ps3UserId | 0);
            M._free(cp);
            if (rc !== 0) errors.push(`Console ID: ${ps3Error(rc)}`);
        }

        /* Report what is actually in effect rather than what was asked for, so
         * the page shows the truth after a rejected value. */
        return {
            ok: errors.length === 0,
            error: errors.join('; ') || undefined,
            pspFuseId: M.UTF8ToString(M._apw_psp_fuse_id()),
            ps3: JSON.parse(M.UTF8ToString(M._apw_ps3_console())),
        };
    },

    /* The disc hash key a games.conf section names, '' when it names none. */
    async ps3Dhk({ text, directory }) {
        await ready();
        const bytes = new TextEncoder().encode(text);
        const tp = alloc(bytes);
        const dp = M.stringToNewUTF8(directory || '');
        try {
            return { ok: true, hex: M.UTF8ToString(M._apw_ps3_dhk_from_db(tp, bytes.length, dp)) };
        } finally {
            M._free(tp); M._free(dp);
        }
    },

    /*
     * Re-bind a save to the console in Settings: rewrite the three PARAM.SFO
     * hashes that name a machine, and resign the PARAM.PFD around them.
     *
     * Only PARAM.PFD comes back. PARAM.SFO is read, not written -- its own
     * account fields are a separate binding this does not touch.
     */
    /*
     * Sign a save to a PSN account: write the ID into PARAM.SFO and bring
     * PARAM.PFD's record of it back into agreement.
     *
     * BOTH files come back, and both have to be kept. A PARAM.SFO carrying a
     * new account beside a PARAM.PFD that still hashes the old one is a save
     * that does not load -- which is why this is one call and not two.
     */
    async ps3AccountResign({ pfd, sfo, account }) {
        await ready();
        const p = new Uint8Array(pfd);
        const f = new Uint8Array(sfo);
        const pp = alloc(p), fp = alloc(f);
        const ap = M.stringToNewUTF8(account || '');
        try {
            const rc = M._apw_ps3_account_resign(pp, p.length, fp, f.length, ap);
            if (rc !== 0) return { ok: false, error: ps3Error(rc) };
            const sfoBytes = heapCopy(fp, f.length);
            const pfdBytes = heapCopy(pp, p.length);
            return {
                ok: true,
                files: [
                    { name: 'PARAM.SFO', bytes: sfoBytes, changed: !sameBytes(sfoBytes, f) },
                    { name: 'PARAM.PFD', bytes: pfdBytes, changed: !sameBytes(pfdBytes, p) },
                ],
            };
        } finally {
            M._free(pp); M._free(fp); M._free(ap);
        }
    },

    async ps3Rebind({ pfd, sfo, dhk }) {
        await ready();
        const p = new Uint8Array(pfd);
        const f = new Uint8Array(sfo);
        const pp = alloc(p), fp = alloc(f);
        const dp = M.stringToNewUTF8(dhk || '');
        try {
            const rc = M._apw_ps3_rebind(pp, p.length, fp, f.length, dp);
            if (rc !== 0) return { ok: false, error: ps3Error(rc) };
            const bytes = heapCopy(pp, p.length);
            return {
                ok: true,
                files: [{ name: 'PARAM.PFD', bytes, changed: !sameBytes(bytes, p) }],
            };
        } finally {
            M._free(pp); M._free(fp); M._free(dp);
        }
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

    /* `codeType` rather than `type`: the message envelope owns `type`. */
    async setCodeType({ index, codeType }) {
        await ready();
        if (!M._apw_set_code_type(index, codeType))
            return { ok: false, error: 'That is not a code type this engine runs.' };

        return codeState(index);
    },

    async revertCode({ index }) {
        await ready();
        M._apw_revert_code(index);
        return codeState(index);
    },

    /*
     * The patch file with this session's edits in it.
     *
     * Comes back as bytes, never as a string: 245 of the database's patches
     * are Windows-1252, so decoding and re-encoding would corrupt the ™/®
     * characters in their game names. The C side hands over a heap range and
     * this copies it out.
     *
     * `mismatches` is the engine's own verdict on what the file cannot carry
     * (a forced Save Wizard/BSD type, mostly) -- worth telling the user
     * before they walk away with the file.
     */
    async exportPatch() {
        await ready();

        const ptr = M._apw_export_patch();
        const len = M._apw_export_size();
        if (!ptr || len <= 0) return { ok: false, error: 'Could not rebuild the patch file.' };

        const bytes = new Uint8Array(M.HEAPU8.subarray(ptr, ptr + len));

        /* Row indices, out of a scratch int32 block. 32 is plenty: the page
         * only names the first few. Read through an Int32Array view of the
         * heap, since HEAP32 itself is not among the exported runtime
         * methods. */
        const max = 32;
        const rowsPtr = M._malloc(max * 4);
        const rows = [];
        try {
            const n = M._apw_export_mismatches(rowsPtr, max);
            const view = new Int32Array(M.HEAPU8.buffer, rowsPtr, max);
            for (let i = 0; i < Math.min(n, max); i++) rows.push(view[i]);
        } finally {
            M._free(rowsPtr);
        }

        return { patch: bytes, mismatches: rows };
    },

    /*
     * Apply codes to a fresh copy of the save data.
     *
     * The engine patches a file in place, so a second run over an
     * already-patched file would stack changes. Writing the original bytes
     * every time makes Apply idempotent: what you get always reflects exactly
     * the codes currently ticked.
     */
    async apply({ indices, options, save, saveName, bigEndian, files, routes }) {
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

        /* One file or several. The single-file form is the default; `files`
         * + `routes` is what the tools page sends for a patch whose required
         * chain spans two targets (Dead Space writes the checksum of USR-DATA
         * into HED-DATA). Routing is decided by the
         * caller, in toolkit.js, so the page and the verifier agree. */
        const inputs = files?.length
            ? files.map((f) => ({ name: f.name, bytes: new Uint8Array(f.buffer) }))
            : [{ name: saveName, bytes: new Uint8Array(save) }];

        const paths = inputs.map((f, i) => {
            const p = workPath(f.name, i);
            M.FS.writeFile(p, f.bytes);
            return p;
        });

        for (const [index, groups] of Object.entries(options || {}))
            groups.forEach((value, group) => M._apw_set_option(Number(index), group, value));

        /* Applied one at a time rather than under a single withCString,
         * because each code can land on a different file. Variables are NOT
         * reset between them — that is load-bearing: Dead Space accumulates
         * its SDBM across three codes, and a decompressed blob lives in a
         * variable until the code that recompresses it runs. */
        const results = indices.map((index) => {
            const target = paths[routes?.[index] ?? 0] ?? paths[0];
            return {
                index,
                ok: withCString(target, (p) => !!M._apw_apply(index, p, bigEndian ? 1 : 0)),
            };
        });

        /* Patch variables accumulate across applies; drop them after a batch,
         * exactly like the desktop GUI does. */
        M._apw_reset_vars();

        const patchedFiles = inputs.map((f, i) => {
            const bytes = M.FS.readFile(paths[i]);
            M.FS.unlink(paths[i]);
            return { name: f.name, bytes, changed: !sameBytes(bytes, f.bytes) };
        });

        return {
            ok: true,
            results,
            /* Single-file callers (the patcher, and every one-target tool)
             * keep reading `patched`; multi-file ones read `files`. */
            patched: patchedFiles[0].bytes,
            files: patchedFiles,
        };
    },
};

self.onmessage = async (ev) => {
    const { id, type, ...args } = ev.data;
    try {
        const result = (await handlers[type](args)) || {};
        const reply = { id, ok: result.ok !== false, ...result, log: drainLog() };

        /* Hand any bytes over instead of copying them: the patched save (or
         * saves), and the rebuilt patch file. All freshly allocated here.
         *
         * Deduped by buffer, because `patched` IS `files[0].bytes` for the
         * single-file callers that still read it, and postMessage throws if
         * the same ArrayBuffer is listed twice. */
        const owned = [...new Set(
            [result.patched, result.patch, ...(result.files || []).map((f) => f.bytes)]
                .filter(Boolean)
                .map((b) => b.buffer))];

        self.postMessage(reply, owned);
    } catch (err) {
        self.postMessage({ id, ok: false, error: String(err && err.message || err), log: drainLog() });
    }
};
