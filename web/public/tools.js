/*
 * Apollo Save Tools — one decrypt / re-encrypt pair per game.
 *
 * The patcher page next door exposes the whole code list and lets you pick.
 * This one answers the question most people actually arrive with: "how do I
 * open this save, and how do I put it back". It lists only patches that
 * tools/verify-tools.mjs has run against a real save, so every button here has
 * been shown to reproduce a reference decrypter's output byte for byte.
 *
 * The engine work is identical to the patcher's — the same worker, the same
 * wasm module — so there is no second implementation to keep correct. What is
 * new is only which codes get applied, and that comes from toolkit.js.
 */
import { CDN } from './cdn.js';
import { splitChain } from './toolkit.js';

const PLATFORM_LABEL = { PS3: 'PS3', PS4: 'PS4', PSV: 'PS Vita', PSP: 'PSP', PS2: 'PS2' };

const $ = (id) => document.getElementById(id);
const worker = new Worker('./worker.js', { type: 'module' });

/* Same request/reply envelope the patcher uses. */
let seq = 0;
const pending = new Map();
worker.onmessage = (ev) => {
    const { id, ...rest } = ev.data;
    const resolve = pending.get(id);
    if (resolve) { pending.delete(id); resolve(rest); }
};
const call = (type, args = {}, transfer = []) =>
    new Promise((resolve) => {
        const id = ++seq;
        pending.set(id, resolve);
        worker.postMessage({ id, type, ...args }, transfer);
    });

let catalog = [];
let active = null;      /* { row, codes, chain, bigEndian } */
let save = null;        /* { name, bytes } */

/* ---- listing ---------------------------------------------------------- */

async function boot() {
    const res = await fetch('./tools.json');
    if (!res.ok) { $('count').textContent = 'Could not load the tool list.'; return; }
    catalog = (await res.json()).tools.map(([platform, id, name, kinds, files, verified]) =>
        ({ platform, id, name, kinds, files: files ? files.split(',') : [], verified }));

    const platforms = [...new Set(catalog.map((r) => r.platform))];
    $('platforms').innerHTML = platforms
        .map((p) => `<button type="button" class="chip" data-platform="${p}">${PLATFORM_LABEL[p] || p}</button>`)
        .join('');
    $('platforms').addEventListener('click', (ev) => {
        const chip = ev.target.closest('.chip');
        if (chip) { chip.classList.toggle('on'); render(); }
    });
    $('search').addEventListener('input', render);
    $('clear-filters').addEventListener('click', () => {
        $('search').value = '';
        document.querySelectorAll('.chip.on').forEach((c) => c.classList.remove('on'));
        render();
    });
    render();
}

function visible() {
    const q = $('search').value.trim().toLowerCase();
    const chosen = [...document.querySelectorAll('.chip.on')].map((c) => c.dataset.platform);
    return catalog.filter((r) =>
        (!chosen.length || chosen.includes(r.platform)) &&
        (!q || r.name.toLowerCase().includes(q) || r.id.toLowerCase().includes(q)));
}

function render() {
    const rows = visible();
    $('count').textContent = rows.length === catalog.length
        ? `${catalog.length} games`
        : `${rows.length} of ${catalog.length} games`;
    $('empty').hidden = rows.length > 0;
    $('grid').innerHTML = rows.map((r, i) => `
      <button type="button" class="card" data-i="${catalog.indexOf(r)}">
        <span class="card-plat">${PLATFORM_LABEL[r.platform] || r.platform}</span>
        <span class="card-name">${escapeHtml(r.name)}</span>
        <span class="card-meta">${escapeHtml(r.id)}${r.files.length ? ' · ' + escapeHtml(r.files[0]) : ''}</span>
      </button>`).join('');
}

const escapeHtml = (s) => String(s).replace(/[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

$('grid').addEventListener('click', (ev) => {
    const card = ev.target.closest('.card');
    if (card) openTool(catalog[Number(card.dataset.i)]);
});

/* ---- one game --------------------------------------------------------- */

async function openTool(row) {
    active = null; save = null;
    $('tool-title').textContent = row.name;
    $('tool-sub').textContent = `${PLATFORM_LABEL[row.platform] || row.platform} · ${row.id}`
        + (row.files.length ? ` · expects ${row.files.join(' or ')}` : '');
    $('drop').hidden = false;
    $('loaded').hidden = true;
    $('actions').hidden = true;
    $('actions').innerHTML = '';
    $('log-wrap').hidden = true;
    $('log').textContent = '';
    setStatus('Loading the patch…');
    $('tool').showModal();

    /* Patches come from the CDN at run time so an upstream fix reaches users
     * without redeploying this site. */
    const url = `${CDN}/${row.platform}/${row.id}.savepatch`;
    let buffer;
    try {
        const res = await fetch(url);
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        buffer = await res.arrayBuffer();
    } catch (err) {
        setStatus(`Could not fetch this patch (${err.message}).`, 'bad');
        return;
    }

    const doc = await call('open', { buffer, name: `${row.id}.savepatch`, platform: row.platform },
                           [buffer]);
    if (!doc.ok) { setStatus(doc.error || 'Could not read this patch.', 'bad'); return; }

    const chain = splitChain(doc.codes || []);
    active = { row, codes: doc.codes, chain, bigEndian: doc.bigEndian };
    setStatus('');
    renderActions();
}

function renderActions() {
    const { chain } = active;
    const buttons = [];
    if (chain.decrypt.length)
        buttons.push({ key: 'decrypt', label: 'Decrypt', indices: chain.decrypt,
                       hint: 'Unlock the save so an editor can read it.' });
    if (chain.rest.length) {
        const rewraps = chain.kinds.includes('e');
        buttons.push({ key: 'rest',
                       label: rewraps ? 'Re-encrypt' : 'Fix checksum',
                       indices: chain.rest,
                       hint: rewraps
                           ? 'Put an edited save back, checksums included.'
                           : 'Recompute the integrity hash after an edit.' });
    }
    $('actions').innerHTML = buttons.map((b) => `
      <button type="button" class="action" data-key="${b.key}" disabled>
        <span class="action-label">${b.label}</span>
        <span class="action-hint">${b.hint}</span>
      </button>`).join('');
    $('actions').hidden = false;
    $('actions').dataset.spec = JSON.stringify(buttons.map(({ key, indices, label }) => ({ key, indices, label })));
    syncActionState();
}

function syncActionState() {
    const on = !!save;
    $('actions').querySelectorAll('.action').forEach((b) => { b.disabled = !on; });
}

$('actions').addEventListener('click', (ev) => {
    const btn = ev.target.closest('.action');
    if (!btn || btn.disabled) return;
    const spec = JSON.parse($('actions').dataset.spec || '[]');
    run(spec.find((s) => s.key === btn.dataset.key));
});

async function run(spec) {
    if (!spec || !save || !active) return;
    setStatus(`${spec.label}…`);
    $('actions').querySelectorAll('.action').forEach((b) => { b.disabled = true; });

    /* Always start from the bytes the user loaded, so the actions are
     * idempotent: pressing Decrypt twice gives the same file, not a save
     * decrypted twice. */
    const copy = save.bytes.slice().buffer;
    const res = await call('apply', {
        indices: spec.indices,
        save: copy,
        saveName: save.name,
        bigEndian: active.bigEndian,
    }, [copy]);

    if (res.log?.length) {
        $('log').textContent = res.log.join('\n');
        $('log-wrap').hidden = false;
    }
    syncActionState();

    if (!res.ok) { setStatus(res.error || 'That did not work.', 'bad'); return; }
    const failed = (res.results || []).filter((r) => !r.ok);
    if (failed.length) {
        const names = failed.map((r) => active.codes[r.index]?.name).filter(Boolean);
        setStatus(`The engine refused ${failed.length} step(s): ${names.join('; ')}`, 'bad');
        return;
    }

    download(res.patched, suggestName(save.name, spec.key));
    setStatus(`${spec.label} done — ${res.patched.length.toLocaleString()} bytes downloaded.`, 'good');
}

/* Decrypting SAVEDATA.DAT gives SAVEDATA.DAT.dec; re-encrypting that gives
 * SAVEDATA.DAT back, rather than piling suffixes up. */
function suggestName(name, key) {
    if (key === 'decrypt') return `${name}.dec`;
    return name.endsWith('.dec') ? name.slice(0, -4) : `${name}.enc`;
}

function download(bytes, name) {
    const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
    const a = document.createElement('a');
    a.href = url; a.download = name;
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10_000);
}

function setStatus(text, tone) {
    const el = $('status');
    el.textContent = text;
    el.className = `status${tone ? ' ' + tone : ''}`;
}

/* ---- picking a file --------------------------------------------------- */

async function loadFile(file) {
    if (!file) return;
    save = { name: file.name, bytes: new Uint8Array(await file.arrayBuffer()) };
    $('loaded-name').textContent = file.name;
    $('loaded-size').textContent = `${save.bytes.length.toLocaleString()} bytes`;
    $('drop').hidden = true;
    $('loaded').hidden = false;
    setStatus('');
    syncActionState();
}

$('drop').addEventListener('click', () => $('file').click());
$('drop').addEventListener('keydown', (ev) => {
    if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); $('file').click(); }
});
$('file').addEventListener('change', (ev) => loadFile(ev.target.files[0]));
$('pick-again').addEventListener('click', () => {
    save = null;
    $('drop').hidden = false;
    $('loaded').hidden = true;
    syncActionState();
});
for (const type of ['dragenter', 'dragover']) {
    $('drop').addEventListener(type, (ev) => { ev.preventDefault(); $('drop').classList.add('over'); });
}
for (const type of ['dragleave', 'drop']) {
    $('drop').addEventListener(type, (ev) => { ev.preventDefault(); $('drop').classList.remove('over'); });
}
$('drop').addEventListener('drop', (ev) => loadFile(ev.dataTransfer?.files?.[0]));

$('tool').addEventListener('close', () => { active = null; save = null; });

boot();
