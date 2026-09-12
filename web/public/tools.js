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
import { splitChain, chainTargets, targetLabels, routeChain, matchesTarget,
         chainVariants, splitIndices, chainOptions, optionsReady,
         optionAssignments } from './toolkit.js';

const PLATFORM_LABEL = { PS3: 'PS3', PS4: 'PS4', PSV: 'PS Vita', PSP: 'PSP', PS2: 'PS2' };

/* Is this the same GAME, spelled differently? Patch titles are written by
 * hand, so the regions of one game disagree about trademark glyphs, case,
 * punctuation, a "PS4 " prefix, and whether to append the Japanese title after
 * a slash, and whether to tag the PSN re-release. Normalising those away keeps
 * a game on one card; anything this does
 * NOT collapse is treated as a different game and gets its own card, which is
 * the safe direction to err — a spurious second card is findable, a game
 * hidden under a sibling's name is not. */
const nameKey = (s) => String(s || '')
    .toLowerCase()
    .replace(/[\u2122\u00ae\u00a9]/g, '')
    .replace(/^ps[1-5]\s+/, '')
    .split('/')[0]
    .replace(/\s*\([^)]*\)/g, '')
    .replace(/[^a-z0-9]+/g, ' ')
    .trim();

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
let active = null;      /* { row, codes, chain, bigEndian, targets, labels } */
let slots = [];         /* one per file the chain needs: { target, label, file } */

/* ---- listing ---------------------------------------------------------- */

async function boot() {
    const res = await fetch('./tools.json');
    if (!res.ok) { $('count').textContent = 'Could not load the tool list.'; return; }
    /* One card per GAME, folded across the regions of that game.
     *
     * Two things have to be true at once. A game ships a patch per region and
     * they usually carry the same codes, so listing those separately buried
     * the same tool five times over and left people wondering whether their
     * region was covered — hence the chain group, which the verifier assigns
     * and which means byte-identical codes. But a chain group is a TOOL, and
     * one tool often spans several games: the LEGO checksum covers four of
     * them, RE7's covers Village, the Naughty Dog decrypt covers Uncharted and
     * The Last of Us together. Folding those into one card hid real games
     * behind a sibling's name, which is the harder problem — people look for
     * their game by name and by box art, not by algorithm.
     *
     * So the key is chain group AND game, and it is the game that wins ties.
     * MGS V still splits by region on top of that, because it keys per region
     * and lands in different chain groups to begin with. */
    const byGroup = new Map();
    for (const [platform, id, name, kinds, files, verified, group, alt] of (await res.json()).tools) {
        /* A patch can cover more than one game and say so under its title —
         * the Metal Gear Solid HD Collection handles both MGS2 and MGS3. The
         * card is named for the first; keep the rest searchable so the second
         * game is not invisible. */
        const titles = [name, ...(alt ? alt.split(';') : [])].filter(Boolean);
        const key = `${platform}/${group || id}/${nameKey(name)}`;
        const entry = byGroup.get(key);
        if (entry) {
            entry.ids.push(id);
            /* Same game, different spelling on the box: "RESIDENT EVIL 0 HD /
             * Biohazard Zero" and "... / Zero", "The Last of Us Part II" and
             * "The Last of Us: Part II". nameKey() folds those onto one card;
             * keep the raw titles so a search for either spelling finds it. */
            for (const t of titles) if (!entry.names.includes(t)) entry.names.push(t);
            continue;
        }
        byGroup.set(key, {
            platform, id, name, kinds, verified,
            ids: [id],
            names: titles,
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
        (!q || r.names.some((n) => n.toLowerCase().includes(q)) ||
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
    active = null; slots = [];
    $('tool-title').textContent = row.name;
    $('tool-sub').innerHTML =
        `${escapeHtml(PLATFORM_LABEL[row.platform] || row.platform)} · `
        + (row.ids.length > 1 ? `covers ${idLinks(row)}` : idLinks(row))
        + (row.files.length ? ` · expects ${escapeHtml(row.files.join(' or '))}` : '');
    $('tool-also').textContent = row.names.length > 1
        ? `Also listed as: ${row.names.slice(1).join(' · ')}`
        : '';
    $('tool-also').hidden = row.names.length < 2;
    const icon = $('tool-icon');
    if (iconSrc) { icon.src = iconSrc; icon.hidden = false; } else { icon.hidden = true; icon.removeAttribute('src'); }

    $('drop').hidden = true;
    $('slots').hidden = true;
    $('options').hidden = true;
    $('options').innerHTML = '';
    $('outputs').hidden = true;
    $('outputs').innerHTML = '';
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
    /* `chosen` is this session's answers to the patch's {TAG} questions, keyed
     * by option group. It survives switching variants, since the tag block is
     * file-scoped and the question does not change. */
    active = { row, codes: doc.codes, chain, bigEndian: doc.bigEndian,
               options: [], chosen: {} };
    /* Every file the required chain needs, in the order it wants them. One
     * for almost every tool; two or three for the patches whose codes work
     * across separate files. */
    const all = chain.indices;

    /* Some patches hold several independent tools: Black Ops a decrypt+encrypt
     * pair per profile file, MGS HD the whole Metal Gear Solid 2 chain and
     * then the whole Metal Gear Solid 3 one. Only one of them ever applies to
     * the save in front of you. */
    active.variants = chainVariants(doc.codes || [], all);
    selectVariant(0);
    setStatus('');
    renderActions();
}

/* Choose which of the patch's tools to drive, and rebuild everything that
 * depends on it: the files it wants, and the buttons it offers. */
function selectVariant(at) {
    active.at = at;
    const indices = active.variants[at].indices;
    active.chain = splitIndices(active.codes, indices);

    const targets = chainTargets(active.codes, indices);
    const labels = targetLabels(targets);
    active.targets = targets;
    active.labels = labels;
    slots = targets.length > 1
        ? targets.map((target, i) => ({ target, label: labels[i], file: null }))
        : [{ target: targets.length === 1 && active.variants.length > 1 ? '' : (targets[0] || ''),
             label: labels[0] || '', file: null }];

    /* What this variant's codes still want answered. Asked per variant rather
     * than per patch because a variant the user is not running must not block
     * the buttons with a question about the other one. */
    active.options = chainOptions(active.codes, indices);

    renderVariants();
    renderOptions();
    renderSlots();
    renderActions();
}

/* The {TAG} questions, as one <select> each.
 *
 * Deliberately left unanswered: the engine starts at sel = -1 and so does
 * this. Defaulting to the first value would be a guess about which save the
 * user is holding, and for L.A. Noire a wrong guess decrypts with the wrong
 * AES key and hands back garbage that looks like output. A <select> rather
 * than the chips used for variants, because these lists run to ten entries. */
function renderOptions() {
    const groups = active.options;
    $('options').hidden = !groups.length;
    if (!groups.length) return;

    $('options').classList.toggle('unset', !optionsReady(groups, active.chosen));
    $('options').innerHTML = groups.map((g) => {
        const sel = active.chosen[g.key];
        const opts = g.values.map((v, i) =>
            `<option value="${i}"${i === sel ? ' selected' : ''}>${escapeHtml(v)}</option>`).join('');
        return `
      <div class="option">
        <p class="option-head">
          <span class="option-label">${escapeHtml(g.label)}</span>
          <span class="option-tag" title="${escapeHtml(g.tag)}">${escapeHtml(g.tag)}</span>
        </p>
        <select class="option-pick" data-key="${escapeHtml(g.key)}"
                aria-label="${escapeHtml(g.label)}">
          <option value=""${Number.isInteger(sel) ? '' : ' selected'}>Choose…</option>
          ${opts}
        </select>
      </div>`;
    }).join('')
      + '<p class="option-why">This patch needs the answer before it can run — '
      + 'it decides what the codes actually do.</p>';
}

$('options').addEventListener('change', (ev) => {
    const pick = ev.target.closest('.option-pick');
    if (!pick || !active) return;
    const at = Number(pick.value);
    if (pick.value === '' || !Number.isInteger(at)) delete active.chosen[pick.dataset.key];
    else active.chosen[pick.dataset.key] = at;

    $('options').classList.toggle('unset', !optionsReady(active.options, active.chosen));
    setStatus('');
    syncActionState();
});

/* Only shown when there is a choice to make. Black Ops labels its tools by the
 * profile file; MGS HD by the game, read off the patch's own [Group:] heading. */
function renderVariants() {
    const many = active.variants.length > 1;
    $('variants').hidden = !many;
    if (!many) return;
    $('variants').innerHTML = active.variants.map((v, i) => `
      <button type="button" class="chip variant${i === active.at ? ' on' : ''}" data-i="${i}">
        ${escapeHtml(v.label || `Tool ${i + 1}`)}
      </button>`).join('');
}

$('variants').addEventListener('click', (ev) => {
    const chip = ev.target.closest('.variant');
    if (!chip || !active) return;
    const keep = slots[0]?.file;
    selectVariant(Number(chip.dataset.i));
    /* Keep the file they already chose, if the new tool wants just the one. */
    if (keep && slots.length === 1) { slots[0].file = keep; renderSlots(); }
    setStatus('');
});

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
    const on = slots.length > 0 && slots.every((s) => s.file)
        && optionsReady(active?.options || [], active?.chosen);
    $('actions').querySelectorAll('.action').forEach((b) => { b.disabled = !on; });
}

$('actions').addEventListener('click', (ev) => {
    const btn = ev.target.closest('.action');
    if (!btn || btn.disabled) return;
    const spec = JSON.parse($('actions').dataset.spec || '[]');
    run(spec.find((s) => s.key === btn.dataset.key));
});

async function run(spec) {
    if (!spec || !active || !slots.length || slots.some((s) => !s.file)) return;
    if (!optionsReady(active.options, active.chosen)) return;


    setStatus('');
    $('outputs').hidden = true;
    $('outputs').innerHTML = '';
    setBusy(`${spec.label}…`);
    $('actions').querySelectorAll('.action').forEach((b) => { b.disabled = true; });

    /* Always start from the bytes the user loaded, so the actions are
     * idempotent: pressing Decrypt twice gives the same file, not a save
     * decrypted twice. */
    const files = slots.map((s) => ({
        name: s.file.name,
        buffer: s.file.bytes.slice().buffer,
    }));

    /* Which file each code lands on. For a one-slot tool this is every code
     * on the only file, which is what the front-end has always done. */
    const assign = {};
    slots.forEach((s, i) => { if (s.target) assign[s.target] = i; });

    const res = await call('apply', {
        indices: spec.indices,
        files,
        routes: routeChain(active.codes, spec.indices, assign),
        /* The {TAG} answers, spread back over every code that mentions them —
         * L.A. Noire asks once and both its decrypt and encrypt code need
         * setting. */
        options: optionAssignments(active.options, active.chosen),
        bigEndian: active.bigEndian,
    }, files.map((f) => f.buffer));

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

    /* The engine can touch more than one file, and a page cannot reliably
     * start several downloads in a row — browsers block the second. So a
     * single output downloads as it always did, and several are listed with a
     * Save button each. Unchanged files are still offered, greyed: "this one
     * did not need fixing" is useful to see. */
    const out = res.files || [{ name: slots[0].file.name, bytes: res.patched, changed: true }];
    if (out.length === 1) {
        download(out[0].bytes, suggestName(slots[0].file.name, spec.key));
        setStatus(`${spec.label} done — ${out[0].bytes.length.toLocaleString()} bytes downloaded.`, 'good');
        return;
    }

    $('outputs').innerHTML = out.map((f, i) => `
      <div class="output${f.changed ? '' : ' same'}">
        <span class="output-name">${escapeHtml(active.labels[i] || f.name)}</span>
        <span class="output-note">${f.changed
            ? `${f.bytes.length.toLocaleString()} bytes · updated`
            : 'unchanged'}</span>
        <button type="button" class="linkish output-save" data-i="${i}">Save</button>
      </div>`).join('');
    $('outputs').hidden = false;
    lastOutputs = out.map((f, i) => ({ ...f, as: suggestName(slots[i].file.name, spec.key) }));
    const n = out.filter((f) => f.changed).length;
    setStatus(`${spec.label} done — ${n} of ${out.length} file(s) changed. Save each below.`, 'good');
}

let lastOutputs = [];
$('outputs').addEventListener('click', (ev) => {
    const btn = ev.target.closest('.output-save');
    if (!btn) return;
    const f = lastOutputs[Number(btn.dataset.i)];
    if (f) download(f.bytes, f.as);
});

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

/* The slot list doubles as the "what do I need" answer and the "what have I
 * given it" answer, so it is rendered for every tool — it just has one row and
 * no target name for the usual single-file case. */
function renderSlots() {
    const multi = slots.length > 1;
    const filled = slots.filter((s) => s.file).length;

    $('drop-main').textContent = multi
        ? `Drop the ${slots.length} files here`
        : 'Drop your save file here';
    $('drop-sub').textContent = multi
        ? `${active.labels.join(', ')} · matched by name, or choose each below · nothing is uploaded`
        : 'or click to choose · nothing is uploaded';
    $('file').multiple = multi;

    /* Single-file keeps the old behaviour: the drop zone gives way once the
     * save is in. Multi-file keeps it, because more files are still wanted. */
    $('drop').hidden = !multi && filled === slots.length;
    $('slots').hidden = !multi && !filled;

    $('slots').innerHTML = slots.map((s, i) => `
      <li class="slot${s.file ? ' filled' : ''}">
        ${multi ? `<span class="slot-target">${escapeHtml(s.label)}</span>` : ''}
        <span class="slot-file">${s.file
            ? `<strong>${escapeHtml(s.file.name)}</strong> <span class="dim">${s.file.bytes.length.toLocaleString()} bytes</span>`
            : '<span class="dim">no file yet</span>'}</span>
        <button type="button" class="linkish slot-pick" data-i="${i}">${s.file ? 'change' : 'choose'}</button>
      </li>`).join('');

    syncActionState();
}

/* Which slot a dropped file belongs in. Its own name against the target it
 * was written for first — that is what makes dropping HED-DATA and USR-DATA
 * together just work — then the first empty slot, so a file the patch names
 * differently than the user's console did still lands somewhere. */
function slotFor(name, taken) {
    const byName = slots.findIndex((s, i) =>
        !taken.has(i) && s.target && matchesTarget(s.target, name));
    if (byName >= 0) return byName;
    const empty = slots.findIndex((s, i) => !taken.has(i) && !s.file);
    if (empty >= 0) return empty;
    /* Every slot is full: this is somebody swapping the file they already
     * chose. Overwrite rather than drop it on the floor, which is what
     * happened when a one-slot tool was handed a second file. */
    return slots.findIndex((s, i) => !taken.has(i));
}

async function loadFiles(list, only) {
    const files = [...(list || [])];
    if (!files.length || !slots.length) return;

    /* When the tools differ only by which file they are for — Black Ops'
     * two profiles — the file the user brought says which one they meant, so
     * pick it for them rather than making them read the chips. */
    if (active?.variants?.length > 1 && slots.length === 1 && only === undefined) {
        const at = active.variants.findIndex((v) =>
            v.indices.some((i) => matchesTarget(active.codes[i]?.file, files[0].name)));
        if (at >= 0 && at !== active.at) selectVariant(at);
    }

    const taken = new Set();
    for (const file of files) {
        const at = only !== undefined ? only : slotFor(file.name, taken);
        if (at < 0) continue;
        taken.add(at);
        slots[at].file = { name: file.name, bytes: new Uint8Array(await file.arrayBuffer()) };
        if (only !== undefined) break;
    }
    setStatus('');
    renderSlots();
}

/* Which slot the next pick goes into; undefined means "work it out from the
 * file's own name". */
let pickInto;

/* The <input type=file> sits INSIDE the drop zone, so the synthetic click that
 * opens it bubbles straight back here. Left unguarded that re-entered this
 * handler, reset pickInto and threw away the slot the user had just chosen —
 * which looked like "the second slot refuses files", because picking for slot
 * 0 happened to be what the fallback did anyway. */
$('drop').addEventListener('click', (ev) => {
    if (ev.target === $('file')) return;
    pickInto = undefined;
    $('file').click();
});
$('drop').addEventListener('keydown', (ev) => {
    if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); pickInto = undefined; $('file').click(); }
});
$('slots').addEventListener('click', (ev) => {
    const btn = ev.target.closest('.slot-pick');
    if (!btn) return;
    pickInto = Number(btn.dataset.i);
    $('file').multiple = false;   /* one file, into the slot that was asked for */
    $('file').click();
});
$('file').addEventListener('change', (ev) => {
    loadFiles(ev.target.files, pickInto);
    ev.target.value = '';
    $('file').multiple = slots.length > 1;
});
for (const type of ['dragenter', 'dragover']) {
    $('drop').addEventListener(type, (ev) => { ev.preventDefault(); $('drop').classList.add('over'); });
}
for (const type of ['dragleave', 'drop']) {
    $('drop').addEventListener(type, (ev) => { ev.preventDefault(); $('drop').classList.remove('over'); });
}
$('drop').addEventListener('drop', (ev) => loadFiles(ev.dataTransfer?.files));

$('tool').addEventListener('close', () => { active = null; slots = []; });

boot();
