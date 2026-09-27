/*
 * The PS3 savedata panel.
 *
 * A PS3 save is wrapped twice, like a PSP one. The console encrypts each
 * protected file with a per-game key before the game's own encryption comes
 * into it, so a file copied straight off a hard drive is opaque to every tool
 * on the page next door -- the patch engine reads it, finds noise where a
 * header should be, and hands back something that looks like output and is not.
 *
 * This is the layer underneath. It is deliberately NOT a card in the game
 * grid: it is not per-game, and it applies to every PS3 save there is,
 * including saves with no patch at all.
 *
 * What differs from the PSP, and shows up all over this file:
 *
 *   PARAM.PFD, not PARAM.SFO   The protected file database. It lists the
 *                              files that are encrypted, carries each one's
 *                              key and hash, and is REWRITTEN when a file is
 *                              re-encrypted.
 *   keys are per file          games.conf gives most games one key for
 *                              everything, but some name a key per file.
 *   the folder is the lookup   games.conf is filed under SAVE DIRECTORY
 *                              names, and PARAM.PFD does not contain one.
 *                              PARAM.SFO does, so dropping it in fills the
 *                              field; otherwise the user says.
 *
 * All of the work happens in the worker, in the same wasm module the rest of
 * the page uses (core/ps3/, compiled in). Nothing is uploaded.
 */
import { CDN } from './cdn.js';
import { hasConsoleId, hasAccountId, settings } from './settings.js';

const $ = (id) => document.getElementById(id);

const escapeHtml = (s) => String(s).replace(/[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

/* ---- the key database --------------------------------------------------- */

/*
 * apollo-patches ships PS3/games.conf: 1800-odd sections of
 *
 *     ; "DiRT 3"
 *     [BLUS30724PROFILE/BLUS30975PROFILE/BLES01287PROFILE]
 *     secure_file_id:*=166A717AAF32DFF265F28EE3F3491A52
 *
 * 280KB, fetched from the CDN like the patches are, so a key added upstream
 * reaches users without redeploying this site. It is 23 times the size of the
 * PSP's key file, so it is fetched when the panel opens rather than at load.
 *
 * Only the FETCH happens here. The matching -- longest directory prefix for
 * the section, first pattern in file order for the file -- is
 * apfd_sfid_from_conf() in C, shared with the desktop app, because both rules
 * are load-bearing and two implementations of them would be one too many.
 */
let keyDb = null;

async function loadKeyDb() {
    const res = await fetch(`${CDN}/PS3/games.conf`);
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return res.text();
}

export function ensurePs3KeyDb() {
    keyDb ??= loadKeyDb().catch((err) => {
        keyDb = null;           /* let the next attempt retry */
        throw err;
    });
    return keyDb;
}

/* ---- the shared calls ---------------------------------------------------
 *
 * Used by the standalone panel below AND by the per-game tool dialog in
 * tools.js, which runs the same console layer around a patch's own codes. One
 * copy, so the two cannot drift on which files get wrapped or where the key
 * comes from.
 */

let call = null;         /* the worker envelope, handed in by tools.js */

/*
 * What a PARAM.PFD says: { version, trophy, files } or { error }.
 *
 * `files` is the authoritative answer to what is protected. A game that
 * encrypts nothing ships a PFD listing only PARAM.SFO, so there is no need to
 * consult the key database to find out, and no way for a stale database to
 * make the answer wrong.
 */
export async function ps3PfdInfo(bytes) {
    const res = await call('ps3Info', { pfd: bytes.slice().buffer });
    if (!res.ok)
        return { error: res.error || 'That does not look like a PARAM.PFD.', log: res.log };
    return {
        version: res.version | 0,
        trophy: !!res.trophy,
        files: res.entries || [],
        log: res.log,
    };
}

/* The entry for a name, or undefined. */
export const ps3Entry = (info, name) =>
    info?.files.find((e) => e.name.toLowerCase() === String(name).toLowerCase());

/* Does this file need a key from the database? Only encrypted entries do, and
 * PARAM.SFO and the trophy files carry built-in keys. */
export function ps3NeedsKey(info, name) {
    const e = ps3Entry(info, name);
    return !!e && e.encrypted && !e.builtin;
}

/*
 * Which PARAM.PFD entry a loaded file stands for: its own name when the PFD
 * lists it, else the only encrypted entry when there is just one, else
 * nothing. People rename saves on the way off a console, so a miss is normal
 * and the caller offers a picker rather than an error.
 */
export function ps3ListedFor(info, name, current = '') {
    if (!info) return '';
    if (name && ps3Entry(info, name)) return ps3Entry(info, name).name;

    const encrypted = info.files.filter((e) => e.encrypted);
    if (encrypted.length === 1) return encrypted[0].name;
    return ps3Entry(info, current)?.name || '';
}

/* SAVEDATA_DIRECTORY out of a PARAM.SFO, which is what the key lookup needs
 * and PARAM.PFD does not carry. */
export async function ps3FolderFromSfo(bytes) {
    const res = await call('ps3Folder', { sfo: bytes.slice().buffer });
    return res.ok ? (res.folder || '') : '';
}

/*
 * The secure file ID for one file, from the database, without asking.
 *
 * Returns { key, hex, note } -- note being what to tell the user about where
 * it came from. A file that needs no key gets key null and no note, and so
 * does one the database does not cover; `note` is how the two are told apart,
 * and needsKey() is the question to ask first.
 */
export async function ps3KeyFor(info, name, folder) {
    if (!ps3NeedsKey(info, name)) return { key: null, hex: '', note: '' };
    if (!folder)
        return { key: null, hex: '',
                 note: 'name the save folder to look this up' };

    try {
        const text = await ensurePs3KeyDb();
        const hit = await call('ps3KeyFromDb', { text, directory: folder, file: name });
        if (hit.ok)
            return { key: hit.key, hex: hit.hex,
                     note: `found in the Apollo database (${hit.entry})` };
        return { key: null, hex: '',
                 note: 'not in the Apollo database — supply one below' };
    } catch (err) {
        return { key: null, hex: '',
                 note: `could not reach the key database (${err.message})` };
    }
}

/*
 * Unwrap / wrap one file's console layer.
 *
 * Decrypt returns { bytes } or { error }. Encrypt returns { bytes, meta } --
 * the rewritten PARAM.PFD is not optional, and a caller that drops it hands
 * the user a save that will not load.
 */
export async function ps3NativeDecrypt(pfdBytes, file, key) {
    const res = await call('ps3Decrypt', {
        pfd: pfdBytes.slice().buffer,
        data: file.bytes.slice().buffer,
        name: file.name,
        sfid: key ? key.slice().buffer : null,
    });
    return res.ok ? { bytes: res.files[0].bytes, log: res.log }
                  : { error: res.error, log: res.log };
}

export async function ps3NativeEncrypt(pfdBytes, file, listedName, key) {
    const res = await call('ps3Encrypt', {
        pfd: pfdBytes.slice().buffer,
        data: file.bytes.slice().buffer,
        name: listedName,
        sfid: key ? key.slice().buffer : null,
    });
    if (!res.ok) return { error: res.error, log: res.log };
    return { bytes: res.files[0].bytes, meta: res.files[1].bytes, log: res.log };
}

/* Does the PARAM.PFD's recorded hash match this file as it sits on disk? */
export async function ps3Verify(pfdBytes, file, key) {
    const res = await call('ps3Verify', {
        pfd: pfdBytes.slice().buffer,
        data: file.bytes.slice().buffer,
        name: file.name,
        sfid: key ? key.slice().buffer : null,
    });
    return res.ok ? { ok: true } : { ok: false, error: res.error };
}

/* ---- the panel's own state ---------------------------------------------- */

let pfd = null;          /* { name, bytes } -- PARAM.PFD                     */
let data = null;         /* { name, bytes } -- the save file itself          */
let sfo = null;          /* { name, bytes } -- PARAM.SFO, when one was given */
let info = null;         /* what the PFD says: version, trophy, files        */
let folder = '';         /* the save directory, for the key lookup           */
let listed = '';         /* which PARAM.PFD entry `data` stands for          */
let key = null;          /* 16 bytes, or null when none is known             */
let keyNote = '';        /* where that key came from, for the user           */
let keyHex = '';         /* what is in the hex box, valid or not             */
let keyFor = '';         /* folder + entry the key belongs to                */
let match = null;        /* null, or whether `data` matches its PFD entry    */
let outputs = [];

/* ---- rendering ---------------------------------------------------------- */

function setStatus(text, tone) {
    const el = $('ps3-status');
    el.textContent = text || '';
    el.className = `status${tone ? ' ' + tone : ''}`;
}

function setBusy(text) {
    $('ps3-busy').hidden = !text;
    if (text) $('ps3-busy-text').textContent = text;
}

function showLog(lines) {
    if (!lines?.length) return;
    $('ps3-log').textContent = lines.join('\n');
    $('ps3-log-wrap').hidden = false;
}

function renderSlots() {
    const rows = [
        { label: 'PARAM.PFD', file: pfd, want: 'the save folder’s PARAM.PFD' },
        { label: 'Save file', file: data, want: 'the file to decrypt or re-encrypt' },
    ];
    /* PARAM.SFO is not a slot -- it is taken for the folder name, and kept
     * because re-binding hashes it. Shown only once one has arrived, so the
     * two files that ARE required stay the obvious ask. */
    if (sfo)
        rows.push({ label: 'PARAM.SFO', file: sfo, want: '', extra: true });
    const filled = rows.filter((r) => r.file && !r.extra).length;

    $('ps3-drop').hidden = filled === 2;
    $('ps3-slots').hidden = !filled;
    $('ps3-slots').innerHTML = rows.map((r, i) => `
      <li class="slot${r.file ? ' filled' : ''}">
        <span class="slot-target">${escapeHtml(r.label)}</span>
        <span class="slot-file">${r.file
            ? `<strong>${escapeHtml(r.file.name)}</strong> <span class="dim">${r.file.bytes.length.toLocaleString()} bytes</span>`
            : `<span class="dim">${escapeHtml(r.want)}</span>`}</span>
        ${r.extra ? '<span class="dim">for the folder name</span>'
                  : `<button type="button" class="linkish ps3-pick" data-i="${i}">${r.file ? 'change' : 'choose'}</button>`}
      </li>`).join('');
}

function renderInfo() {
    const box = $('ps3-info');

    box.hidden = !info;
    if (!info) return;

    /* Only the encrypted entries are offered. PARAM.SFO is listed in every
     * PFD and is never encrypted, so putting it in the picker would only
     * invite someone to pick it. */
    const encrypted = info.files.filter((e) => e.encrypted);
    const options = encrypted.map((e) =>
        `<option value="${escapeHtml(e.name)}"${e.name === listed ? ' selected' : ''}>${escapeHtml(e.name)}</option>`).join('');
    const mismatch = data && listed && data.name.toLowerCase() !== listed.toLowerCase();

    box.innerHTML = `
      <dl class="psp-facts">
        <dt>Save folder</dt>
        <dd><input id="ps3-folder" type="text" spellcheck="false" autocomplete="off"
                   placeholder="e.g. BLUS30724PROFILE" aria-label="Save folder name"
                   value="${escapeHtml(folder)}"></dd>
        <dt>Database</dt>
        <dd>version ${info.version}${info.trophy ? ' <span class="dim">· trophy folder</span>' : ''}</dd>
        <dt>Protected files</dt>
        <dd>${encrypted.length
            ? `<select id="ps3-listed" aria-label="Which listed file">${options}</select>`
            : '<span class="good-text">none</span> <span class="dim">— this game stores saves in the clear</span>'}</dd>
        <dt class="ps3-match-dt"${match === null ? ' hidden' : ''}>Recorded hash</dt>
        <dd class="ps3-match"${match === null ? ' hidden' : ''}>${matchHtml()}</dd>
      </dl>
      ${mismatch ? `<p class="psp-note">Your file is named
         <strong>${escapeHtml(data.name)}</strong>, which PARAM.PFD does not
         list. Pick the entry it belongs to above.</p>` : ''}`;
}

function renderKey() {
    const box = $('ps3-key');

    /* Nothing to ask for until we know which entry we are working on, and
     * nothing to ask for at all when that entry carries a built-in key. */
    box.hidden = !info || !listed || !ps3NeedsKey(info, listed);
    if (box.hidden) return;

    box.innerHTML = `
      <p class="psp-key-head">
        <span class="option-label">Secure file ID</span>
        ${key ? `<span class="psp-key-state good-text">${escapeHtml(keyNote)}</span>`
              : `<span class="psp-key-state">${escapeHtml(keyNote || 'not found — supply one below')}</span>`}
      </p>
      <div class="psp-key-inputs">
        <input id="ps3-key-hex" type="text" inputmode="latin" spellcheck="false"
               autocomplete="off" maxlength="32" placeholder="32 hex digits"
               aria-label="Secure file ID, as 32 hex digits"
               value="${escapeHtml(keyHex)}">
      </div>
      <p class="psp-key-why">
        The console keys each game separately, and some games key each file
        separately again. Apollo ships the IDs it knows, filed under the save
        folder name — so naming the folder above is usually all it takes.
      </p>`;
}

function renderActions() {
    const needs = info && listed && ps3NeedsKey(info, listed);
    const ready = pfd && data && info && listed && (!needs || !!key);
    const buttons = [
        { key: 'decrypt', label: 'Decrypt', on: ready,
          hint: 'Unwrap the console’s layer so a patch or an editor can read it.' },
        { key: 'encrypt', label: 'Re-encrypt', on: ready,
          hint: 'Put an edited save back. Updates PARAM.PFD too — keep both.' },
        { key: 'resign', label: 'Resign PARAM.PFD', on: !!pfd,
          hint: 'Recompute the PARAM.PFD signatures alone, leaving every file as it is.' },
    ];

    /* Offered only once an account has been named in Settings. Listed before
     * the console because it is the one to reach for: an account ID travels
     * between machines where an IDPS does not. */
    if (hasAccountId())
        buttons.push({
            key: 'account',
            label: 'Sign to your account',
            on: !!pfd && !!sfo,
            hint: sfo
                ? 'Write your PSN account into PARAM.SFO and update PARAM.PFD to match, '
                  + 'so the save loads on any PS3 signed in to that account.'
                : 'Add the folder’s PARAM.SFO as well — the account fields live in it.',
        });

    /* Offered only once a console has been named in Settings, because without
     * one there is nothing to re-bind TO, and a button that always sat there
     * disabled would raise a question it could not answer. */
    if (hasConsoleId())
        buttons.push({
            key: 'rebind',
            label: 'Re-bind to your console',
            on: !!pfd && !!sfo,
            hint: sfo
                ? 'Rewrite the PARAM.SFO hashes that name a console, so the save belongs to yours.'
                : 'Add the folder’s PARAM.SFO as well — the hashes are taken over it.',
        });

    $('ps3-actions').hidden = !pfd;
    $('ps3-actions').innerHTML = buttons.map((b) => `
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

const isPfdName = (name) => /^param\.pfd$/i.test(name);
const isSfoName = (name) => /^param\.sfo$/i.test(name);

/* The folder a File came from, when the browser knows. Set by a directory
 * picker and by dropping a folder; empty for a plain file choice. */
const folderOf = (file) => (file.webkitRelativePath || '').split('/')[0] || '';

async function readPfd() {
    setBusy('Reading PARAM.PFD…');
    const res = await ps3PfdInfo(pfd.bytes);
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
    listed = ps3ListedFor(info, data?.name, listed);
}

/*
 * Find the secure file ID without asking, when we can. The save folder plus
 * the file name is exactly what games.conf is filed under, so for a game
 * Apollo knows there is nothing for the user to do beyond naming the folder.
 */
async function resolveKey() {
    if (!info || !listed || !ps3NeedsKey(info, listed)) {
        key = null;
        keyNote = '';
        return;
    }

    /* A key typed for one file is not another file's key. The panel keeps a
     * hand-entered one across loading files -- decrypt, edit, come back and
     * re-encrypt is the common path -- but only while it is the same entry of
     * the same save. */
    const want = `${folder}/${listed}`;
    if (keyFor !== want) {
        key = null;
        keyHex = '';
        keyNote = '';
    }

    /* A key the user supplied by hand for THIS file outranks the database:
     * they are holding the console it came off, and we are not. */
    if (keyHex.length === 32) return;

    const found = await ps3KeyFor(info, listed, folder);
    keyFor = want;
    key = found.key;
    keyHex = found.hex;
    keyNote = found.note;
}

/* Whether the loaded file still matches what PARAM.PFD recorded for it. A
 * save that already disagrees was damaged before it got here, and patching it
 * would sign the damage into place -- worth saying before anything else. */
async function checkMatch() {
    match = null;
    if (!pfd || !data || !info || !listed) return;
    if (ps3NeedsKey(info, listed) && !key) return;

    /* Only when the loaded file is the length the console left it: the hash
     * covers the padded ciphertext, so a file somebody already decrypted will
     * not match and saying so would be alarming rather than useful. When the
     * entry's size happens to be a whole number of blocks the two lengths are
     * the same and the question is genuinely ambiguous -- the check runs, and
     * its wording allows for that. */
    const entry = ps3Entry(info, listed);
    if (!entry || data.bytes.length !== ((entry.size + 15) & ~15)) return;

    const res = await ps3Verify(pfd.bytes, { ...data, name: listed }, key);
    match = res.ok;
}

async function loadFiles(list, into) {
    const files = [...(list || [])];
    if (!files.length) return;

    let pfdChanged = false;
    for (const f of files) {
        const bytes = new Uint8Array(await f.arrayBuffer());
        folder ||= folderOf(f);

        /* A PARAM.SFO is not one of the two slots -- it is here only because
         * it names the save folder, which PARAM.PFD does not and the key
         * lookup needs. Dropping the whole folder therefore fills the field
         * with no typing. */
        if (into === undefined && isSfoName(f.name)) {
            sfo = { name: f.name, bytes };
            folder = (await ps3FolderFromSfo(bytes)) || folder;
            continue;
        }

        const slot = into !== undefined ? into : (isPfdName(f.name) ? 0 : 1);
        if (slot === 0) { pfd = { name: f.name, bytes }; pfdChanged = true; }
        else            { data = { name: f.name, bytes }; }
        if (into !== undefined) break;
    }

    $('ps3-outputs').hidden = true;
    outputs = [];
    setStatus('');

    if (pfdChanged || !info) {
        if (pfd) await readPfd();
    } else {
        pickListed();
        await resolveKey();
    }
    await checkMatch();
    renderAll();
}

/* ---- running ------------------------------------------------------------ */

/* Decrypting SAVEDATA gives SAVEDATA.dec; re-encrypting that gives SAVEDATA
 * back, rather than piling suffixes up. Matches the per-game tools next door. */
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
    if (!pfd) return;

    setStatus('');
    $('ps3-outputs').hidden = true;
    $('ps3-outputs').innerHTML = '';
    outputs = [];
    $('ps3-actions').querySelectorAll('.action').forEach((b) => { b.disabled = true; });
    setBusy({ resign: 'Resigning…', rebind: 'Re-binding…', account: 'Signing…',
              decrypt: 'Decrypting…', encrypt: 'Re-encrypting…' }[what]);

    let res;
    if (what === 'resign') {
        res = await call('ps3Resign', { pfd: pfd.bytes.slice().buffer });
    } else if (what === 'account') {
        res = await call('ps3AccountResign', {
            pfd: pfd.bytes.slice().buffer,
            sfo: sfo.bytes.slice().buffer,
            account: settings().ps3AccountId,
        });
    } else if (what === 'rebind') {
        /* The disc hash key keys one of the three hashes and is per GAME, not
         * per console, so it is looked up here rather than kept in Settings.
         * Most games name none and fall back to a built-in one. */
        let dhk = '';
        try {
            const hit = await call('ps3Dhk', { text: await ensurePs3KeyDb(), directory: folder });
            dhk = hit.ok ? hit.hex : '';
        } catch { /* no database reachable; the fallback key still applies */ }

        res = await call('ps3Rebind', {
            pfd: pfd.bytes.slice().buffer,
            sfo: sfo.bytes.slice().buffer,
            dhk,
        });
    } else {
        /* Always start from the bytes that were loaded, so the actions are
         * idempotent -- pressing Decrypt twice gives the same file, not a save
         * decrypted twice. The PFD entry name is what identifies the file,
         * whatever the user called it on disk. */
        const file = { name: listed, bytes: data.bytes };
        const done = what === 'decrypt'
            ? await ps3NativeDecrypt(pfd.bytes, file, key)
            : await ps3NativeEncrypt(pfd.bytes, file, listed, key);

        res = done.error
            ? { ok: false, error: done.error, log: done.log }
            : { ok: true, log: done.log,
                files: done.meta
                    ? [{ name: listed, bytes: done.bytes },
                       { name: 'PARAM.PFD', bytes: done.meta }]
                    : [{ name: listed, bytes: done.bytes }] };
    }

    setBusy(null);
    showLog(res.log);
    renderActions();

    if (!res.ok) { setStatus(res.error || 'That did not work.', 'bad'); return; }

    /* Decryption yields one file and downloads straight away. Encryption
     * yields two -- the data file AND the rewritten PARAM.PFD -- and a page
     * cannot reliably start two downloads in a row, so both are listed with a
     * Save button each. Handing over only the data file would be handing over
     * a save that does not load. */
    const files = (res.files || []).map((f, i) => ({
        ...f,
        as: i === 0 && (what === 'decrypt' || what === 'encrypt')
            ? suggestName(what === 'encrypt' ? listed : data.name, what)
            : f.name,
    }));

    if (files.length === 1) {
        download(files[0].bytes, files[0].as);
        setStatus(what === 'rebind'
            ? 'Re-bound. Save the new PARAM.PFD over the one in the save folder.'
            : `Done — ${files[0].bytes.length.toLocaleString()} bytes downloaded as ${files[0].as}.`,
            'good');
        return;
    }

    outputs = files;
    $('ps3-outputs').innerHTML = files.map((f, i) => `
      <div class="output">
        <span class="output-name">${escapeHtml(f.as)}</span>
        <span class="output-note">${f.bytes.length.toLocaleString()} bytes${i ? ' · rewritten' : ''}</span>
        <button type="button" class="linkish ps3-save" data-i="${i}">Save</button>
      </div>`).join('');
    $('ps3-outputs').hidden = false;
    setStatus(what === 'account'
        ? 'Signed to your account. Save BOTH files back into the save folder — '
          + 'PARAM.SFO carries the account and PARAM.PFD hashes it, and a save '
          + 'with only one of them will not load.'
        : 'Re-encrypted. Save BOTH files back into the save folder — '
          + 'PARAM.PFD changed too, and the save will not load without it.', 'good');
}

/* ---- wiring ------------------------------------------------------------- */

let pickInto;

/* Settings can be changed while the panel is open, and two of the actions
 * exist only when an account or a console is named there. */
export function ps3SettingsChanged() {
    if ($('ps3').open) renderActions();
}

export function initPs3(worker) {
    call = worker;

    /* As in psp.js: the panel lives on the patcher page, but the tools page
     * needs `call` set for the stage inside its per-game dialogs. */
    if (!$('ps3-open')) return;

    $('ps3-open').addEventListener('click', () => {
        setStatus('');
        $('ps3-log-wrap').hidden = true;
        $('ps3-log').textContent = '';
        renderAll();
        $('ps3').showModal();
        /* Start the key database now, while they find their files: it is
         * 280KB and the lookup blocks on it. */
        ensurePs3KeyDb().catch(() => {});
    });

    $('ps3-drop').addEventListener('click', (ev) => {
        if (ev.target === $('ps3-file')) return;
        pickInto = undefined;
        $('ps3-file').multiple = true;
        $('ps3-file').click();
    });
    $('ps3-drop').addEventListener('keydown', (ev) => {
        if (ev.key === 'Enter' || ev.key === ' ') {
            ev.preventDefault();
            pickInto = undefined;
            $('ps3-file').click();
        }
    });
    for (const type of ['dragenter', 'dragover'])
        $('ps3-drop').addEventListener(type, (ev) => {
            ev.preventDefault(); $('ps3-drop').classList.add('over');
        });
    for (const type of ['dragleave', 'drop'])
        $('ps3-drop').addEventListener(type, (ev) => {
            ev.preventDefault(); $('ps3-drop').classList.remove('over');
        });
    $('ps3-drop').addEventListener('drop', (ev) => loadFiles(ev.dataTransfer?.files));

    $('ps3-file').addEventListener('change', (ev) => {
        loadFiles(ev.target.files, pickInto);
        ev.target.value = '';
    });

    $('ps3-slots').addEventListener('click', (ev) => {
        const btn = ev.target.closest('.ps3-pick');
        if (!btn) return;
        pickInto = Number(btn.dataset.i);
        $('ps3-file').multiple = false;
        $('ps3-file').click();
    });

    $('ps3-info').addEventListener('change', async (ev) => {
        if (ev.target.id !== 'ps3-listed') return;
        listed = ev.target.value;
        await resolveKey();
        await checkMatch();
        renderInfo();
        renderKey();
        renderActions();
    });

    /* The save folder is the database lookup, so a change re-resolves the key.
     * Debounced, because it is typed a character at a time and each attempt
     * runs a search over 1800 sections. */
    let folderTimer;
    $('ps3-info').addEventListener('input', (ev) => {
        if (ev.target.id !== 'ps3-folder') return;
        folder = ev.target.value.trim();
        keyFor = '';                 /* a different folder, a different key */
        clearTimeout(folderTimer);
        folderTimer = setTimeout(async () => {
            keyHex = '';
            await resolveKey();
            await checkMatch();
            renderKey();
            renderActions();
            /* The hash line lives in the info box, which holds the field being
             * typed into, so only that one line is replaced. */
            renderMatchLine();
        }, 250);
    });

    /* Typing a key: accepted only at full length, so a half-typed one never
     * reads as "no key" and silently blocks. */
    $('ps3-key').addEventListener('input', async (ev) => {
        if (ev.target.id !== 'ps3-key-hex') return;
        keyHex = ev.target.value.trim().toUpperCase();
        keyFor = `${folder}/${listed}`;
        if (/^[0-9A-F]{32}$/.test(keyHex)) {
            const parsed = await call('ps3KeyFromHex', { hex: keyHex });
            key = parsed.ok ? parsed.key : null;
            keyNote = parsed.ok ? 'entered by hand' : parsed.error;
        } else {
            key = null;
            keyNote = keyHex ? 'needs 32 hex digits' : '';
        }
        /* Update the state line in place rather than re-rendering: rebuilding
         * the input would drop the caret mid-key. */
        const state = $('ps3-key').querySelector('.psp-key-state');
        state.textContent = keyNote;
        state.classList.toggle('good-text', !!key);
        await checkMatch();
        renderMatchLine();
        renderActions();
    });

    $('ps3-actions').addEventListener('click', (ev) => {
        const btn = ev.target.closest('.action');
        if (btn && !btn.disabled) run(btn.dataset.key);
    });

    $('ps3-outputs').addEventListener('click', (ev) => {
        const btn = ev.target.closest('.ps3-save');
        if (!btn) return;
        const f = outputs[Number(btn.dataset.i)];
        if (f) download(f.bytes, f.as);
    });

    /* Closing drops the save, but keeps the key database and a typed key --
     * somebody working through several files of one game should not have to
     * re-enter it. */
    $('ps3').addEventListener('close', () => {
        pfd = data = sfo = info = null;
        listed = '';
        match = null;
        outputs = [];
        $('ps3-outputs').hidden = true;
    });
}

const matchHtml = () => (match
    ? '<span class="good-text">matches your file</span>'
    : '<span class="bad-text">does not match your file</span> <span class="dim">\u2014 the save was edited without its PARAM.PFD, or the key is wrong</span>');

/*
 * The one line in the info box that depends on the key. Updated on its own
 * rather than through renderInfo(), because the box also holds the folder and
 * the box being rebuilt mid-word would drop the caret.
 */
function renderMatchLine() {
    const dt = $('ps3-info').querySelector('.ps3-match-dt');
    const dd = $('ps3-info').querySelector('.ps3-match');
    if (!dt || !dd) return;

    dt.hidden = dd.hidden = match === null;
    if (match !== null) dd.innerHTML = matchHtml();
}
