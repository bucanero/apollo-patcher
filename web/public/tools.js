/*
 * Apollo Save Decrypters & Fixers — one decrypt / re-encrypt pair per game.
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
import { CDN, SAVES_CDN, PSNDB, TMDB, TMDB_KEY } from './cdn.js';
import { splitChain } from './toolkit.js';

const PLATFORM_LABEL = { PS3: 'PS3', PS4: 'PS4', PSV: 'PS Vita', PSP: 'PSP', PS2: 'PS2' };

/* Card art lives in apollo-saves as <PLATFORM>/<TITLEID>/<icon>, and the file
 * name's CASE depends on the console: PS3 and PSP store ICON0.PNG, while PS4,
 * PS Vita, PS2 and PS1 store icon0.png. jsDelivr is case-sensitive, so asking
 * for the wrong one is a silent 404 — which is exactly how every PS4 and Vita
 * card lost its icon the first time round. */
const ICON_FILE = { PS3: 'ICON0.PNG', PSP: 'ICON0.PNG' };
const iconUrl = (platform, id) =>
    `${SAVES_CDN}/${platform}/${id}/${ICON_FILE[platform] || 'icon0.png'}`;

/* Finding a card's art.
 *
 * Three sources, because none covers everything. apollo-saves is filed per
 * title ID and carries almost every PS3 game but few PS4 ones; the psndb
 * mirror of Sony's PS4 metadata has the reverse shape; Sony's own TMDB covers
 * PS3 and is the one that matters once the catalogue opens past the games
 * listed today (of the 837 PS3 patches with no apollo-saves art, about 82%
 * have art there).
 *
 * All of them are tried against every title ID in the card's group, since
 * coverage is per region and a group's first region is not necessarily the one
 * with art — Diablo III's group leads with BLES01921, which has none, while
 * BLUS31188 does. The <img> removes itself when nothing matches and the grid
 * column collapses.
 *
 * This runs from the img's onerror, so the FIRST attempt is still the lazy one
 * the browser scheduled: nothing here fetches for a card nobody scrolled to.
 */
const settle = (img, url) => new Promise((resolve) => {
    img.onload = () => resolve(true);
    img.onerror = () => resolve(false);
    img.src = url;
});

/* The psndb mirror hands back an http:// icon URL; this page is https, so
 * loading it as-is is mixed content and the browser blocks it. The same host
 * answers over https, so upgrade the scheme rather than dropping the icon. */
async function psndbIcon(titleId) {
    try {
        const res = await fetch(`${PSNDB}/${titleId}/${titleId}_00.json`);
        if (!res.ok) return null;
        const url = ((await res.json()).icons || [])[0]?.icon;
        return url ? url.replace(/^http:\/\//, 'https://') : null;
    } catch {
        return null;
    }
}

/* HMAC-SHA1 of "<TITLEID>_00" under the TMDB key, uppercase hex — the path
 * segment IS the digest, so there is no metadata request to make first.
 *
 * crypto.subtle exists only in a secure context (https, or localhost). On a
 * page served over plain http this returns null and the caller moves on,
 * rather than throwing halfway through the chain. */
let tmdbKey = null;
async function tmdbIcon(titleId) {
    if (!globalThis.crypto?.subtle) return null;
    try {
        tmdbKey ??= await crypto.subtle.importKey(
            'raw',
            Uint8Array.from(TMDB_KEY.match(/../g), (b) => parseInt(b, 16)),
            { name: 'HMAC', hash: 'SHA-1' }, false, ['sign']);
        const mac = await crypto.subtle.sign(
            'HMAC', tmdbKey, new TextEncoder().encode(`${titleId}_00`));
        const hex = [...new Uint8Array(mac)]
            .map((b) => b.toString(16).padStart(2, '0')).join('').toUpperCase();
        return `${TMDB}/${titleId}_00_${hex}/ICON0.PNG`;
    } catch {
        return null;
    }
}

window.__nextIcon = async (img) => {
    if (img.dataset.busy) return;
    img.dataset.busy = '1';
    img.onerror = null;

    const platform = img.closest('.card')?.dataset.platform || '';
    const ids = (img.dataset.ids || '').split(',').filter(Boolean);

    /* apollo-saves for the remaining regions (the first was already tried). */
    for (const id of ids.slice(1))
        if (await settle(img, iconUrl(platform, id))) { delete img.dataset.busy; return; }

    /* Then whichever second source covers this console: the psndb mirror for
     * PS4, Sony's TMDB for PS3. Neither answers for the other's titles. */
    const second = platform === 'PS4' ? psndbIcon
                 : platform === 'PS3' ? tmdbIcon
                 : null;
    if (second)
        for (const id of ids) {
            const url = await second(id);
            if (url && await settle(img, url)) { delete img.dataset.busy; return; }
        }

    img.remove();
};

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
    /* One card per TOOL, not per patch. A game ships a patch per region and
     * they usually carry the same codes, so listing them separately buried
     * the same tool five times over and left people wondering whether their
     * region was covered. Patches are folded together by the chain group the
     * verifier assigns: same group means byte-identical codes, so one card can
     * stand for all of them and any member can be the one it loads.
     *
     * Not folded by game name — Metal Gear Solid V keys per region, so its
     * PS3 releases are genuinely different tools and stay on separate cards. */
    const byGroup = new Map();
    for (const [platform, id, name, kinds, files, verified, group] of (await res.json()).tools) {
        const key = `${platform}/${group || id}`;
        const entry = byGroup.get(key);
        if (entry) { entry.ids.push(id); continue; }
        byGroup.set(key, {
            platform, id, name, kinds, verified,
            ids: [id],
            files: files ? files.split(',') : [],
        });
    }
    catalog = [...byGroup.values()];

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
        (!q || r.name.toLowerCase().includes(q) ||
               r.ids.some((id) => id.toLowerCase().includes(q))));
}

function render() {
    const rows = visible();
    $('count').textContent = rows.length === catalog.length
        ? `${catalog.length} games`
        : `${rows.length} of ${catalog.length} games`;
    $('empty').hidden = rows.length > 0;

    /* Grouped into a section per console rather than one long grid. The
     * catalog already arrives sorted by platform and then by name, so the
     * sections come out in a stable order and each stays alphabetical without
     * sorting anything here. */
    const sections = new Map();
    for (const r of rows) {
        if (!sections.has(r.platform)) sections.set(r.platform, []);
        sections.get(r.platform).push(r);
    }

    $('grid').innerHTML = [...sections].map(([platform, items]) => `
      <section class="platform">
        <h2 class="platform-head">
          ${PLATFORM_LABEL[platform] || platform}
          <span class="platform-count">${items.length}</span>
        </h2>
        <div class="cards">${items.map(cardHtml).join('')}</div>
      </section>`).join('');
}

/* One ID reads as one ID; several read as "and N more", with the full list in
 * the dialog so somebody holding a JP copy can check before they start. */
const idSummary = (r) => r.ids.length > 1 ? `${r.id} +${r.ids.length - 1} more` : r.id;

/* In the dialog the IDs become links to the patch each one came from. Two
 * questions get asked here and only the second needs the source: "is my
 * region covered" and "what is this thing about to do to my save". Pointed at
 * the repository rather than the CDN the page fetches from, so the link opens
 * something readable, with a history and a blame, instead of a raw blob. */
const PATCH_REPO = 'https://github.com/bucanero/apollo-patches/blob/main';
const idLinks = (r) => r.ids.map((id) =>
    `<a href="${PATCH_REPO}/${r.platform}/${encodeURIComponent(id)}.savepatch"`
    + ` target="_blank" rel="noopener">${escapeHtml(id)}</a>`).join(', ');

const cardHtml = (r) => `
  <button type="button" class="card" data-i="${catalog.indexOf(r)}" data-platform="${r.platform}">
    <img class="card-icon" alt="" loading="lazy" decoding="async"
         src="${iconUrl(r.platform, r.ids[0])}"
         data-ids="${escapeHtml(r.ids.join(','))}"
         onerror="window.__nextIcon(this)">
    <span class="card-plat">${PLATFORM_LABEL[r.platform] || r.platform}</span>
    <span class="card-name">${escapeHtml(r.name)}</span>
    <span class="card-meta">${escapeHtml(idSummary(r))}${r.files.length ? ' · ' + escapeHtml(r.files[0]) : ''}</span>
  </button>`;

const escapeHtml = (s) => String(s).replace(/[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

$('grid').addEventListener('click', (ev) => {
    const card = ev.target.closest('.card');
    if (!card) return;
    /* Reuse the icon the card already resolved instead of walking the source
     * chain a second time — by this point it is decoded and in cache. */
    const icon = card.querySelector('.card-icon');
    openTool(catalog[Number(card.dataset.i)],
             icon && icon.complete && icon.naturalWidth ? icon.src : null);
});

/* ---- one game --------------------------------------------------------- */

async function openTool(row, iconSrc) {
    active = null; save = null;
    $('tool-title').textContent = row.name;
    $('tool-sub').innerHTML =
        `${escapeHtml(PLATFORM_LABEL[row.platform] || row.platform)} · `
        + (row.ids.length > 1 ? `covers ${idLinks(row)}` : idLinks(row))
        + (row.files.length ? ` · expects ${escapeHtml(row.files.join(' or '))}` : '');
    const icon = $('tool-icon');
    if (iconSrc) { icon.src = iconSrc; icon.hidden = false; } else { icon.hidden = true; icon.removeAttribute('src'); }

    $('drop').hidden = true;
    $('loaded').hidden = true;
    $('actions').hidden = true;
    $('actions').innerHTML = '';
    $('log-wrap').hidden = true;
    $('log').textContent = '';
    setStatus('');
    setBusy('Loading the patch…');
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
        setBusy(null);
        setStatus(`Could not fetch this patch (${err.message}).`, 'bad');
        return;
    }

    const doc = await call('open', { buffer, name: `${row.id}.savepatch`, platform: row.platform },
                           [buffer]);
    setBusy(null);
    if (!doc.ok) { setStatus(doc.error || 'Could not read this patch.', 'bad'); return; }

    const chain = splitChain(doc.codes || []);
    active = { row, codes: doc.codes, chain, bigEndian: doc.bigEndian };
    $('drop').hidden = !!save;
    $('loaded').hidden = !save;
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
    setStatus('');
    setBusy(`${spec.label}…`);
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

    setBusy(null);
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

/* The dialog is busy in two places and both can take a while — fetching and
 * parsing the patch, and running a code (a Python patch on a few megabytes is
 * tens of seconds). Passing null clears it. */
function setBusy(text) {
    $('busy').hidden = !text;
    if (text) $('busy-text').textContent = text;
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
