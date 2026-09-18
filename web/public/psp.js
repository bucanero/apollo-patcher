/*
 * The PSP savedata panel.
 *
 * A PSP save is wrapped twice. The console encrypts it with a per-title game
 * key before the game's own encryption ever comes into it, so a file copied
 * straight off a Memory Stick is opaque to every tool on the page next door --
 * the patch engine reads it, finds noise where a header should be, and hands
 * back something that looks like output and is not.
 *
 * This is the layer underneath. It is deliberately NOT a card in the game
 * grid: it is not per-game, it applies to every PSP save there is, including
 * the ~60 PSP titles the patch database covers but the tool catalog does not,
 * and saves with no patch at all.
 *
 * All of the work happens in the worker, in the same wasm module the rest of
 * the page uses (core/psp/, compiled in). Nothing is uploaded.
 */
import { CDN } from './cdn.js';

const $ = (id) => document.getElementById(id);

const escapeHtml = (s) => String(s).replace(/[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

/* SAVEDATA_PARAMS bits that name a keyed mode; neither set means the save
 * can only be unkeyed. Mirrors APSP_MODE_KEYED in core/psp/psp_savedata.h. */
const MODE_KEYED = 0x60;

export const NULL_KEY = new Uint8Array(16);

/* Does this save need a game key at all? A PARAM.SFO whose mode byte sets
 * neither keyed bit describes a save the console wrote unkeyed, and asking
 * for a key that does not exist is the quickest way to lose someone. */
export const needsKey = (info) => !!info && (info.mode & MODE_KEYED) !== 0;

/* ---- the game-key database --------------------------------------------- */

/*
 * apollo-patches ships PSP/gamekeys.txt: 280-odd lines of
 *
 *     ; a comment
 *     ULUS10391=4A1FF359AEB6EFF81CA8CB23BCA57BB3
 *
 * 16KB, fetched from the CDN like the patches are, so a key added upstream
 * reaches users without redeploying this site.
 *
 * Only the FETCH happens here. The matching -- prefix against the save
 * directory, longest entry wins -- is apsp_key_from_db() in C, shared with the
 * desktop app, because the tie-break is load-bearing and two implementations
 * of it would be one too many: the database holds both NPJJ30022 and
 * NPJJ30022GAME1 with different keys.
 */
let keyDb = null;

async function loadKeyDb() {
    const res = await fetch(`${CDN}/PSP/gamekeys.txt`);
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return res.text();
}

function ensureKeyDb() {
    keyDb ??= loadKeyDb().catch((err) => {
        keyDb = null;           /* let the next attempt retry */
        throw err;
    });
    return keyDb;
}

const hexToBytes = (hex) =>
    Uint8Array.from(hex.match(/../g), (b) => parseInt(b, 16));

/* ---- the shared calls ---------------------------------------------------
 *
 * Used by the standalone panel below AND by the per-game tool dialog in
 * tools.js, which runs the same console layer around a patch's own codes. One
 * copy, so the two cannot drift on which files get wrapped or where the key
 * comes from.
 */

let call = null;         /* the worker envelope, handed in by tools.js */

/* Whatever `call` was given at init -- exported so tools.js can reach the
 * worker through the same envelope without threading it everywhere. */

/*
 * What a PARAM.SFO says: { directory, mode, files } or { error }.
 */
export async function pspSfoInfo(bytes) {
    const res = await call('pspInfo', { sfo: bytes.slice().buffer });
    if (!res.ok)
        return { error: res.error || 'That does not look like a PARAM.SFO.', log: res.log };
    return {
        directory: res.directory || '',
        mode: res.mode | 0,
        files: res.files || [],
        log: res.log,
    };
}

/*
 * The game key for a save, from the database, without asking.
 *
 * Returns { key, hex, note } -- note being what to tell the user about where
 * it came from. An unkeyed save gets the null key and no note. A keyed save
 * the database does not cover gets the null key and a note saying so; the
 * caller decides whether to ask for one.
 */
export async function pspKeyFor(info) {
    if (!needsKey(info)) return { key: NULL_KEY, hex: '', note: '' };

    try {
        const text = await ensureKeyDb();
        const hit = await call('pspKeyFromDb', { text, directory: info.directory || '' });
        if (hit.ok)
            return { key: hit.key, hex: hit.hex,
                     note: `found in the Apollo database (${hit.entry})` };
        return { key: NULL_KEY, hex: '',
                 note: 'not in the Apollo database \u2014 supply one below' };
    } catch (err) {
        return { key: NULL_KEY, hex: '',
                 note: `could not reach the key database (${err.message})` };
    }
}

/*
 * Unwrap / wrap one file's console layer.
 *
 * Decrypt returns { bytes } or { error }. Encrypt returns { bytes, sfo } --
 * the rewritten PARAM.SFO is not optional, and a caller that drops it hands
 * the user a save that will not load.
 */
export async function pspNativeDecrypt(sfoBytes, file, key) {
    const res = await call('pspDecrypt', {
        sfo: sfoBytes.slice().buffer,
        data: file.bytes.slice().buffer,
        name: file.name,
        key: key.slice().buffer,
    });
    return res.ok ? { bytes: res.files[0].bytes, log: res.log }
                  : { error: res.error, log: res.log };
}

export async function pspNativeEncrypt(sfoBytes, file, listedName, key) {
    const res = await call('pspEncrypt', {
        sfo: sfoBytes.slice().buffer,
        data: file.bytes.slice().buffer,
        name: listedName,
        key: key.slice().buffer,
    });
    if (!res.ok) return { error: res.error, log: res.log };
    return { bytes: res.files[0].bytes, sfo: res.files[1].bytes, log: res.log };
}

/*
 * Which SAVEDATA_FILE_LIST entry a loaded file stands for: its own name when
 * the SFO lists it, else the only entry when there is just one, else nothing.
 * People rename saves on the way off a console, so a miss is normal and the
 * caller offers a picker rather than an error.
 */
export function pspListedFor(info, name, current = '') {
    if (!info) return '';
    if (name && info.files.includes(name)) return name;
    if (info.files.length === 1) return info.files[0];
    return info.files.includes(current) ? current : '';
}

/* ---- the panel's own state ---------------------------------------------- */

let sfo = null;          /* { name, bytes } -- PARAM.SFO                     */
let data = null;         /* { name, bytes } -- the save file itself          */
let info = null;         /* what the SFO says: directory, mode, files        */
let listed = '';         /* which SAVEDATA_FILE_LIST entry `data` stands for */
let key = NULL_KEY;      /* 16 bytes                                         */
let keyNote = '';        /* where that key came from, for the user           */
let keyHex = '';         /* what is in the hex box, valid or not             */
let keyFor = '';         /* the save directory that key belongs to           */
let outputs = [];

/* ---- rendering ---------------------------------------------------------- */

function setStatus(text, tone) {
    const el = $('psp-status');
    el.textContent = text || '';
    el.className = `status${tone ? ' ' + tone : ''}`;
}

function setBusy(text) {
    $('psp-busy').hidden = !text;
    if (text) $('psp-busy-text').textContent = text;
}

function showLog(lines) {
    if (!lines?.length) return;
    $('psp-log').textContent = lines.join('\n');
    $('psp-log-wrap').hidden = false;
}

function renderSlots() {
    const rows = [
        { label: 'PARAM.SFO', file: sfo, want: 'the save folder’s PARAM.SFO' },
        { label: 'Save file', file: data, want: 'the file to decrypt or re-encrypt' },
    ];
    const filled = rows.filter((r) => r.file).length;

    $('psp-drop').hidden = filled === rows.length;
    $('psp-slots').hidden = !filled;
    $('psp-slots').innerHTML = rows.map((r, i) => `
      <li class="slot${r.file ? ' filled' : ''}">
        <span class="slot-target">${escapeHtml(r.label)}</span>
        <span class="slot-file">${r.file
            ? `<strong>${escapeHtml(r.file.name)}</strong> <span class="dim">${r.file.bytes.length.toLocaleString()} bytes</span>`
            : `<span class="dim">${escapeHtml(r.want)}</span>`}</span>
        <button type="button" class="linkish psp-pick" data-i="${i}">${r.file ? 'change' : 'choose'}</button>
      </li>`).join('');
}

function renderInfo() {
    const box = $('psp-info');

    box.hidden = !info;
    if (!info) return;

    /* Which SAVEDATA_FILE_LIST entry the loaded file is. Decryption does not
     * care -- it reads only the mode byte -- but re-encryption writes the
     * file's hash into that entry, so a file the SFO does not list cannot be
     * put back. Offered as a picker rather than an error, because people
     * rename saves on the way off a console all the time. */
    const options = info.files.map((f) =>
        `<option value="${escapeHtml(f)}"${f === listed ? ' selected' : ''}>${escapeHtml(f)}</option>`).join('');
    const mismatch = data && listed && data.name !== listed;

    box.innerHTML = `
      <dl class="psp-facts">
        <dt>Save directory</dt>
        <dd>${escapeHtml(info.directory || '—')}</dd>
        <dt>Encryption</dt>
        <dd>${needsKey(info)
            ? `keyed <span class="dim">(mode 0x${info.mode.toString(16).toUpperCase().padStart(2, '0')})</span>`
            : `<span class="good-text">unkeyed</span> <span class="dim">— no game key needed</span>`}</dd>
        <dt>Encrypted files</dt>
        <dd>${info.files.length
            ? `<select id="psp-listed" aria-label="Which listed file">${options}</select>`
            : '<span class="dim">none listed</span>'}</dd>
      </dl>
      ${mismatch ? `<p class="psp-note">Your file is named
         <strong>${escapeHtml(data.name)}</strong>, which PARAM.SFO does not list.
         Decrypting is unaffected; to put it back, pick the entry it belongs to
         above.</p>` : ''}`;
}

function renderKey() {
    const box = $('psp-key');

    /* Nothing to ask for until we know what the save is, and nothing to ask
     * for at all when it is unkeyed. */
    box.hidden = !info || !needsKey(info);
    if (box.hidden) return;

    const have = !key.every((b) => b === 0);
    box.innerHTML = `
      <p class="psp-key-head">
        <span class="option-label">Game key</span>
        ${have ? `<span class="psp-key-state good-text">${escapeHtml(keyNote)}</span>`
               : `<span class="psp-key-state">${escapeHtml(keyNote || 'not found — supply one below')}</span>`}
      </p>
      <div class="psp-key-inputs">
        <input id="psp-key-hex" type="text" inputmode="latin" spellcheck="false"
               autocomplete="off" maxlength="32" placeholder="32 hex digits"
               aria-label="Game key, as 32 hex digits"
               value="${escapeHtml(keyHex)}">
        <button type="button" class="linkish" id="psp-key-file">or load a key file…</button>
        <input id="psp-key-input" type="file" hidden>
      </div>
      <p class="psp-key-why">
        The console keys each game separately. Apollo ships the keys it knows;
        for anything else, dump it from your own PSP with SGKeyDumper or
        SGDeemer and drop the file here.
      </p>`;
}

function renderActions() {
    const ready = sfo && data && info && (!needsKey(info) || !key.every((b) => b === 0));
    const buttons = [
        { key: 'decrypt', label: 'Decrypt', on: ready,
          hint: 'Unwrap the console’s layer so a patch or an editor can read it.' },
        { key: 'encrypt', label: 'Re-encrypt', on: ready && info?.files.includes(listed),
          hint: 'Put an edited save back. Updates PARAM.SFO too — keep both.' },
        { key: 'resign', label: 'Resign PARAM.SFO', on: !!sfo,
          hint: 'Recompute the PARAM.SFO hashes alone, for an unencrypted save.' },
    ];

    $('psp-actions').hidden = !sfo;
    $('psp-actions').innerHTML = buttons.map((b) => `
      <button type="button" class="action" data-key="${b.key}"${b.on ? '' : ' disabled'}>
        <span class="action-label">${b.label}</span>
        <span class="action-hint">${b.hint}</span>
      </button>`).join('');
}

function renderAll() {
    renderSlots();
    renderInfo();
    renderKey();
    renderActions();
}

/* ---- loading ------------------------------------------------------------ */

/* PARAM.SFO is unmistakable by name, so a pair dropped together sorts itself;
 * anything else is the save file. */
const isSfoName = (name) => /^param\.sfo$/i.test(name);

async function readSfo() {
    setBusy('Reading PARAM.SFO…');
    const res = await pspSfoInfo(sfo.bytes);
    setBusy(null);
    showLog(res.log);

    if (res.error) {
        info = null;
        listed = '';
        setStatus(res.error, 'bad');
        return;
    }

    info = res;
    pickListed();
    setStatus('');
    await resolveKey();
}

function pickListed() {
    listed = pspListedFor(info, data?.name, listed);
}

/*
 * Find the game key without asking, when we can. The save directory out of
 * PARAM.SFO is exactly what gamekeys.txt is filed under, so for a game Apollo
 * knows there is nothing for the user to do at all.
 */
async function resolveKey() {
    if (!info || !needsKey(info)) {
        key = NULL_KEY;
        keyNote = '';
        return;
    }
    /* A key typed for one game is not another game's key. The dialog keeps a
     * hand-entered key across loading files -- decrypt, edit, come back and
     * re-encrypt is the common path -- but only while it is the same save. */
    if (keyFor !== info.directory) {
        key = NULL_KEY;
        keyHex = '';
        keyNote = '';
    }

    /* A key the user supplied by hand for THIS save outranks the database:
     * they are holding the console it came off, and we are not. */
    if (keyHex.length === 32) return;

    const found = await pspKeyFor(info);
    keyFor = info.directory;
    key = found.key;
    keyHex = found.hex;
    keyNote = found.note;
}

async function loadFiles(list, into) {
    const files = [...(list || [])];
    if (!files.length) return;

    let sfoChanged = false;
    for (const f of files) {
        const bytes = new Uint8Array(await f.arrayBuffer());
        const slot = into !== undefined ? into : (isSfoName(f.name) ? 0 : 1);

        if (slot === 0) { sfo = { name: f.name, bytes }; sfoChanged = true; }
        else            { data = { name: f.name, bytes }; }
        if (into !== undefined) break;
    }

    $('psp-outputs').hidden = true;
    outputs = [];
    setStatus('');

    if (sfoChanged || !info) {
        if (sfo) await readSfo();
    } else {
        pickListed();
    }
    renderAll();
}

/* ---- running ------------------------------------------------------------ */

/* Decrypting SAVEDATA.BIN gives SAVEDATA.BIN.dec; re-encrypting that gives
 * SAVEDATA.BIN back, rather than piling suffixes up. Matches the per-game
 * tools next door. */
function suggestName(name, what) {
    if (what === 'decrypt') return `${name}.dec`;
    return name.endsWith('.dec') ? name.slice(0, -4) : name;
}

function download(bytes, name) {
    const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
    const a = document.createElement('a');
    a.href = url; a.download = name;
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10_000);
}

async function run(what) {
    if (!sfo) return;

    setStatus('');
    $('psp-outputs').hidden = true;
    $('psp-outputs').innerHTML = '';
    outputs = [];
    $('psp-actions').querySelectorAll('.action').forEach((b) => { b.disabled = true; });
    setBusy(what === 'resign' ? 'Resigning…'
                              : `${what === 'decrypt' ? 'Decrypting' : 'Re-encrypting'}…`);

    /* Always start from the bytes that were loaded, so the actions are
     * idempotent -- pressing Decrypt twice gives the same file, not a save
     * decrypted twice. */
    const args = { sfo: sfo.bytes.slice().buffer };
    if (what !== 'resign') {
        args.data = data.bytes.slice().buffer;
        args.name = what === 'encrypt' ? listed : data.name;
        args.key = key.slice().buffer;
    }

    const type = { decrypt: 'pspDecrypt', encrypt: 'pspEncrypt', resign: 'pspResign' }[what];
    const res = await call(type, args, Object.values(args).filter((v) => v instanceof ArrayBuffer));

    setBusy(null);
    showLog(res.log);
    renderActions();

    if (!res.ok) { setStatus(res.error || 'That did not work.', 'bad'); return; }

    /* Decryption yields one file and downloads straight away. Encryption
     * yields two -- the data file AND the rewritten PARAM.SFO -- and a page
     * cannot reliably start two downloads in a row, so both are listed with a
     * Save button each. Handing over only the data file would be handing over
     * a save that does not load. */
    const files = (res.files || []).map((f, i) => ({
        ...f,
        as: i === 0 && what !== 'resign'
            ? suggestName(what === 'encrypt' ? listed : data.name, what)
            : f.name,
    }));

    if (files.length === 1) {
        download(files[0].bytes, files[0].as);
        setStatus(`Done — ${files[0].bytes.length.toLocaleString()} bytes downloaded as ${files[0].as}.`, 'good');
        return;
    }

    outputs = files;
    $('psp-outputs').innerHTML = files.map((f, i) => `
      <div class="output">
        <span class="output-name">${escapeHtml(f.as)}</span>
        <span class="output-note">${f.bytes.length.toLocaleString()} bytes${i ? ' · rewritten' : ''}</span>
        <button type="button" class="linkish psp-save" data-i="${i}">Save</button>
      </div>`).join('');
    $('psp-outputs').hidden = false;
    setStatus('Re-encrypted. Save BOTH files back into the save folder — '
              + 'PARAM.SFO changed too, and the save will not load without it.', 'good');
}

/* ---- wiring ------------------------------------------------------------- */

let pickInto;

export function initPsp(worker) {
    call = worker;

    $('psp-open').addEventListener('click', () => {
        setStatus('');
        $('psp-log-wrap').hidden = true;
        $('psp-log').textContent = '';
        renderAll();
        $('psp').showModal();
        /* Start the key database now, while they find their files. */
        ensureKeyDb().catch(() => {});
    });

    $('psp-drop').addEventListener('click', (ev) => {
        if (ev.target === $('psp-file')) return;
        pickInto = undefined;
        $('psp-file').multiple = true;
        $('psp-file').click();
    });
    $('psp-drop').addEventListener('keydown', (ev) => {
        if (ev.key === 'Enter' || ev.key === ' ') {
            ev.preventDefault();
            pickInto = undefined;
            $('psp-file').click();
        }
    });
    for (const type of ['dragenter', 'dragover'])
        $('psp-drop').addEventListener(type, (ev) => {
            ev.preventDefault(); $('psp-drop').classList.add('over');
        });
    for (const type of ['dragleave', 'drop'])
        $('psp-drop').addEventListener(type, (ev) => {
            ev.preventDefault(); $('psp-drop').classList.remove('over');
        });
    $('psp-drop').addEventListener('drop', (ev) => loadFiles(ev.dataTransfer?.files));

    $('psp-file').addEventListener('change', (ev) => {
        loadFiles(ev.target.files, pickInto);
        ev.target.value = '';
    });

    $('psp-slots').addEventListener('click', (ev) => {
        const btn = ev.target.closest('.psp-pick');
        if (!btn) return;
        pickInto = Number(btn.dataset.i);
        $('psp-file').multiple = false;
        $('psp-file').click();
    });

    $('psp-info').addEventListener('change', (ev) => {
        if (ev.target.id !== 'psp-listed') return;
        listed = ev.target.value;
        renderInfo();
        renderActions();
    });

    /* Typing a key: accepted only at full length, so a half-typed one never
     * reads as "no key" and silently decrypts in the unkeyed mode. */
    $('psp-key').addEventListener('input', (ev) => {
        if (ev.target.id !== 'psp-key-hex') return;
        keyHex = ev.target.value.trim().toUpperCase();
        keyFor = info?.directory || '';
        if (/^[0-9A-F]{32}$/.test(keyHex)) {
            key = hexToBytes(keyHex);
            keyNote = 'entered by hand';
        } else {
            key = NULL_KEY;
            keyNote = keyHex ? 'needs 32 hex digits' : '';
        }
        /* Update the state line in place rather than re-rendering: rebuilding
         * the input would drop the caret mid-key. */
        const state = $('psp-key').querySelector('.psp-key-state');
        state.textContent = keyNote;
        state.classList.toggle('good-text', keyHex.length === 32);
        renderActions();
    });

    $('psp-key').addEventListener('click', (ev) => {
        if (ev.target.id === 'psp-key-file') $('psp-key-input').click();
    });

    $('psp-key').addEventListener('change', async (ev) => {
        if (ev.target.id !== 'psp-key-input') return;
        const file = ev.target.files?.[0];
        ev.target.value = '';
        if (!file) return;

        const res = await call('pspKeyFromFile', { buffer: await file.arrayBuffer() });
        showLog(res.log);
        if (!res.ok) {
            setStatus(`${res.error} — expected SGKeyDumper's 16 bytes or SGDeemer's 1536.`, 'bad');
            return;
        }
        key = res.key;
        keyHex = res.hex;
        keyFor = info?.directory || '';
        keyNote = `read from ${file.name}`;
        setStatus('');
        renderKey();
        renderActions();
    });

    $('psp-actions').addEventListener('click', (ev) => {
        const btn = ev.target.closest('.action');
        if (btn && !btn.disabled) run(btn.dataset.key);
    });

    $('psp-outputs').addEventListener('click', (ev) => {
        const btn = ev.target.closest('.psp-save');
        if (!btn) return;
        const f = outputs[Number(btn.dataset.i)];
        if (f) download(f.bytes, f.as);
    });

    /* Closing drops the save, but keeps the key database and a typed key --
     * somebody working through several files of one game should not have to
     * re-enter it. */
    $('psp').addEventListener('close', () => {
        sfo = data = info = null;
        listed = '';
        outputs = [];
        $('psp-outputs').hidden = true;
    });
}
