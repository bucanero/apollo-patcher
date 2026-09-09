/*
 * UI for the Apollo Save Patcher web front-end.
 *
 * No framework and no build step: the page is served exactly as it sits in
 * dist/. All engine work happens in worker.js; this file is only state + DOM.
 */

import { CDN } from './cdn.js';

const TYPE = { 1: 'Save Wizard', 2: 'BSD', 3: 'Python' };

/* APOLLO_CODE_FLAG_* from apollo.h — the same markers the CLI prints. */
const FLAG = {
    PARENT: 1, CHILD: 2, REQUIRED: 4, ALERT: 8,
    EMPTY: 16, DISABLED: 32,
};

const $ = (id) => document.getElementById(id);

const state = {
    patchName: null,
    save: null,          // ArrayBuffer of the original, unpatched save
    saveName: null,
    codes: [],
    patchText: null,     // the .savepatch verbatim, for the raw viewer
    checked: new Set(),  // indices
    options: {},         // index -> [selected value per group]
    patched: null,       // Uint8Array of the last successful run
};

/* ---------------------------------------------------------------------------
 * Worker plumbing
 * ------------------------------------------------------------------------- */

const worker = new Worker('./worker.js', { type: 'module' });
const pending = new Map();
let nextId = 1;

worker.onmessage = (ev) => {
    const { id, log, ...rest } = ev.data;
    if (log && log.length) appendLog(log);

    const resolve = pending.get(id);
    if (resolve) {
        pending.delete(id);
        resolve(rest);
    }
};

worker.onerror = (err) => {
    appendLog([`worker error: ${err.message || err}`]);
};

function call(type, args = {}, transfer = []) {
    const id = nextId++;
    return new Promise((resolve) => {
        pending.set(id, resolve);
        worker.postMessage({ id, type, ...args }, transfer);
    });
}

/* ---------------------------------------------------------------------------
 * Log panel
 * ------------------------------------------------------------------------- */

let logLines = 0;

function appendLog(lines) {
    const pre = $('log');
    pre.textContent += lines.join('\n') + '\n';
    logLines += lines.length;
    $('log-count').textContent = logLines ? `(${logLines})` : '';
    pre.scrollTop = pre.scrollHeight;
}

function clearLog() {
    $('log').textContent = '';
    logLines = 0;
    $('log-count').textContent = '';
}

/* ---------------------------------------------------------------------------
 * File inputs
 * ------------------------------------------------------------------------- */

function wireDropZone(zoneId, inputId, onFile) {
    const zone = $(zoneId);
    const input = $(inputId);

    zone.addEventListener('click', () => input.click());
    zone.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            input.click();
        }
    });
    input.addEventListener('change', () => {
        if (input.files[0]) onFile(input.files[0]);
    });

    for (const type of ['dragenter', 'dragover']) {
        zone.addEventListener(type, (e) => {
            e.preventDefault();
            zone.classList.add('over');
        });
    }
    for (const type of ['dragleave', 'drop']) {
        zone.addEventListener(type, () => zone.classList.remove('over'));
    }
    zone.addEventListener('drop', (e) => {
        e.preventDefault();
        if (e.dataTransfer.files[0]) onFile(e.dataTransfer.files[0]);
    });
}

async function loadPatch(file, displayName) {
    const buffer = await file.arrayBuffer();
    state.patchName = file.name;
    $('patch-name').textContent = displayName || file.name;
    $('drop-patch').classList.add('filled');

    /* Decode before the buffer is handed to the worker — posting it transfers
     * ownership, so it is unreadable here afterwards. Parsing keeps only the
     * codes, and the comments and target lines are often the only
     * documentation a patch has, so keep the text for the raw viewer. */
    state.patchText = decodePatch(buffer);

    clearLog();
    const res = await call('open', { buffer, name: file.name }, [buffer]);
    if (!res.ok) {
        $('patch-name').textContent = res.error || 'Could not read this file';
        $('drop-patch').classList.remove('filled');
        $('workspace').hidden = true;
        state.codes = [];
        state.checked.clear();
        state.patchText = null;
        $('view-patch').hidden = true;
        clearResult();
        return;
    }

    state.codes = res.codes;
    state.options = {};
    res.codes.forEach((c, i) => {
        if (c.options.length) state.options[i] = c.options.map((o) => o.sel);
    });

    /* Prefer the index's name when the patch came from the database: the
     * engine hands back raw bytes, and 245 patches are Windows-1252, so their
     * ™/® characters arrive mojibaked through UTF8ToString. */
    /* Byte order follows the patch, not the previous one: a PS3 patch turns
     * big-endian on, anything else turns it back off. Still a checkbox, so it
     * can be overridden afterwards. */
    const be = !!res.bigEndian;
    if ($('big-endian').checked !== be) {
        $('big-endian').checked = be;
        appendLog([be ? 'PS3 title detected — big-endian data mode enabled'
                      : 'Non-PS3 title — big-endian data mode disabled']);
    }

    $('game-name').textContent = displayName || res.game.trim() || file.name;
    $('code-count').textContent = `${res.codes.length} codes`;
    $('view-patch').hidden = false;
    $('workspace').hidden = false;
    $('intro').hidden = true;
    clearResult();
    selectDefaults();
}

async function loadSave(file) {
    state.save = await file.arrayBuffer();
    state.saveName = file.name;
    showSaveLoaded();
    clearResult();
    refreshApplyButton();
}

function showSaveLoaded() {
    $('save-name').textContent =
        `${state.saveName} · ${formatSize(state.save.byteLength)}`;
    $('drop-save').classList.add('filled');
    $('save-tools').hidden = false;
}

/* ---------------------------------------------------------------------------
 * Hex editor
 *
 * HexEdit is vendored from bucanero/ps2vmc-tool (see hexedit.js) and loaded as
 * a classic script, so it is a global rather than an import.
 * ------------------------------------------------------------------------- */

function editSaveData() {
    if (!state.save) return;

    HexEdit.open({
        title: state.saveName,
        subtitle: `loaded save · ${formatSize(state.save.byteLength)}`,
        data: new Uint8Array(state.save),
        onSave: (edited) => {
            /* slice() so the ArrayBuffer is exactly the file, whatever view
             * the editor hands back. */
            state.save = edited.slice().buffer;
            showSaveLoaded();
            /* Any previous run patched the bytes as they were, so retract it
             * rather than leave a download that no longer matches. */
            clearResult();
            appendLog([`Save data edited by hand — ${formatSize(state.save.byteLength)}`]);
            refreshApplyButton();
        },
    });
}

/* The patched result, read-only: editing it would produce a file no patch
 * chain accounts for, and it is one Download away anyway. */
function viewResultData() {
    if (!state.patched) return;
    HexEdit.open({
        title: state.saveName,
        subtitle: `patched result · ${formatSize(state.patched.length)}`,
        data: state.patched,
        readOnly: true,
    });
}

/*
 * Patch files are mostly UTF-8, but 245 of the ~2240 in the database are
 * Windows-1252 (game names with ™ / ®). Decoding those as UTF-8 would replace
 * the bytes with U+FFFD, so try strict UTF-8 first and fall back — the same
 * rule tools/build-index.py applies.
 */
function decodePatch(buffer) {
    try {
        return new TextDecoder('utf-8', { fatal: true }).decode(buffer);
    } catch (e) {
        return new TextDecoder('windows-1252').decode(buffer);
    }
}

function formatSize(n) {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
    return `${(n / 1024 / 1024).toFixed(1)} MB`;
}

/* ---------------------------------------------------------------------------
 * Code list
 * ------------------------------------------------------------------------- */

/* A parent row is a group header: its children are the rows that follow it,
 * up to the next non-child row. Ticking the parent ticks the whole group. */
function childrenOf(index) {
    const out = [];
    for (let i = index + 1; i < state.codes.length; i++) {
        if (!state.codes[i].child) break;
        out.push(i);
    }
    return out;
}

function selectable(code) {
    return !code.parent && !(code.flags & FLAG.EMPTY);
}

/*
 * Ticking any code pulls in every [R] code in the file.
 *
 * "(Required)" entries are the wrappers a patch needs around whatever else you
 * choose — typically a pair that decompresses a payload before the real codes
 * run and recompresses it afterwards — so picking one code without them
 * produces a save the game cannot read. The desktop GUI has always done this
 * (auto_enable_required in gui/src/main.cpp); the web app had not.
 *
 * Only additive, and only on check: unticking leaves them alone, so they can
 * still be turned off deliberately.
 *
 * Returns true if anything changed, so the caller knows to re-render.
 */
function enableRequired() {
    let changed = false;
    state.codes.forEach((code, i) => {
        if ((code.flags & FLAG.REQUIRED) && selectable(code) && !state.checked.has(i)) {
            state.checked.add(i);
            changed = true;
        }
    });
    return changed;
}

function selectDefaults() {
    state.checked = new Set(
        state.codes.map((c, i) => (c.activated && selectable(c) ? i : -1)).filter((i) => i >= 0),
    );
    renderCodes();
    refreshApplyButton();
}

/* Any previous run's output belongs to the files that produced it. Loading a
 * different patch or save must retract it, or Download would hand back a file
 * that has nothing to do with what is on screen. */
function clearResult() {
    state.patched = null;
    $('result').hidden = true;
}

function renderCodes() {
    const list = $('codes');
    const needle = $('filter').value.trim().toLowerCase();
    list.textContent = '';

    state.codes.forEach((code, index) => {
        if (needle && !code.name.toLowerCase().includes(needle)) return;

        const li = document.createElement('li');
        li.className = 'code';
        if (code.child) li.classList.add('child');
        if (code.parent) li.classList.add('group');
        if (code.flags & FLAG.DISABLED) li.classList.add('disabled');

        const row = document.createElement('div');
        row.className = 'code-row';

        if (selectable(code)) {
            const box = document.createElement('input');
            box.type = 'checkbox';
            box.checked = state.checked.has(index);
            box.addEventListener('change', () => {
                toggle(index, box.checked);
                const pulled = box.checked && enableRequired();
                /* Re-render when a required code was pulled in (its box has to
                 * show as ticked), or when this code has option groups — an
                 * unanswered one is flagged only while ticked. */
                if (pulled || code.options.length) renderCodes();
                refreshApplyButton();
            });
            row.append(box);
        } else if (code.parent) {
            const kids = childrenOf(index);
            const on = kids.filter((i) => state.checked.has(i)).length;
            const box = document.createElement('input');
            box.type = 'checkbox';
            box.checked = on > 0 && on === kids.length;
            box.indeterminate = on > 0 && on < kids.length;
            box.addEventListener('change', () => {
                kids.forEach((i) => toggle(i, box.checked));
                if (box.checked) enableRequired();
                renderCodes();
                refreshApplyButton();
            });
            row.append(box);
        } else {
            row.append(Object.assign(document.createElement('span'), { className: 'spacer' }));
        }

        const label = document.createElement('span');
        label.className = 'code-name';
        label.textContent = code.name;
        row.append(label);

        for (const [flag, text, title] of [
            [FLAG.ALERT, '!', 'Note from the patch author'],
            [FLAG.REQUIRED, 'R', 'Required — apply this one too'],
            [FLAG.DISABLED, 'D', 'Marked disabled by the patch author'],
        ]) {
            if (code.flags & flag) {
                const badge = document.createElement('span');
                badge.className = 'marker';
                badge.textContent = text;
                badge.title = title;
                row.append(badge);
            }
        }

        if (TYPE[code.type] && !code.parent) {
            const badge = document.createElement('span');
            badge.className = `badge type-${code.type}`;
            badge.textContent = TYPE[code.type];
            row.append(badge);
        }

        if (!code.parent) {
            const view = document.createElement('button');
            view.type = 'button';
            view.className = 'ghost small';
            view.textContent = 'View';
            view.addEventListener('click', () => showCode(index, code.name));
            row.append(view);
        }

        li.append(row);

        /* Interactive {TAG} options become one dropdown per group.
         *
         * The engine loads these with sel = -1, meaning "the user has not
         * chosen yet" — it is not a default. Defaulting to the first value
         * would silently pick something meaningful (a user profile, a
         * character slot) on the player's behalf, so instead the dropdown
         * starts on a placeholder and Apply stays blocked, matching the
         * desktop GUI. */
        code.options.forEach((group, gi) => {
            const chosen = state.options[index][gi];
            const wrap = document.createElement('div');
            wrap.className = 'option';

            const tag = document.createElement('label');
            tag.textContent = group.tag || 'Option';

            const select = document.createElement('select');
            if (chosen < 0) {
                select.append(new Option('Choose a value…', '-1', true, true));
                if (state.checked.has(index)) select.classList.add('unfilled');
            }
            group.values.forEach((name, vi) => {
                select.append(new Option(name, String(vi), false, vi === chosen));
            });
            select.addEventListener('change', () => {
                state.options[index][gi] = Number(select.value);
                renderCodes();
                refreshApplyButton();
            });

            tag.append(select);
            wrap.append(tag);
            li.append(wrap);
        });

        list.append(li);
    });
}

function toggle(index, on) {
    if (on) state.checked.add(index);
    else state.checked.delete(index);
}

/* Codes that are ticked but still have an unanswered option group. */
function unfilledCodes() {
    return [...state.checked].filter((i) => (state.options[i] || []).some((v) => v < 0));
}

function refreshApplyButton() {
    const n = state.checked.size;
    const unfilled = unfilledCodes();

    $('apply').disabled = !(state.save && n) || unfilled.length > 0;
    $('apply').textContent = n ? `Apply ${n} code${n === 1 ? '' : 's'}` : 'Apply';

    const note = $('needs-options');
    note.hidden = unfilled.length === 0;
    if (unfilled.length) {
        note.textContent = unfilled.length === 1
            ? `Choose a value for “${state.codes[unfilled[0]].name}” before applying.`
            : `${unfilled.length} selected codes still need an option value.`;
    }

    if (!state.save && n) $('apply').title = 'Load a save data file first';
    else $('apply').title = '';
}

function showPatchText() {
    if (!state.patchText) return;
    $('dialog-title').textContent = state.patchName || 'Patch file';
    $('dialog-body').textContent = state.patchText;
    $('code-dialog').showModal();
}

async function showCode(index, name) {
    const { text } = await call('codeText', { index });
    $('dialog-title').textContent = name;
    $('dialog-body').textContent = text || '(no code body)';
    $('code-dialog').showModal();
}

/* ---------------------------------------------------------------------------
 * Apply
 * ------------------------------------------------------------------------- */

async function apply() {
    const indices = [...state.checked].sort((a, b) => a - b);
    if (!indices.length || !state.save) return;

    $('apply').disabled = true;
    clearResult();
    clearLog();
    $('log-panel').open = true;

    /* The worker takes ownership of what it is given, so hand it a copy and
     * keep the pristine original for the next run. */
    const copy = state.save.slice(0);
    const res = await call(
        'apply',
        {
            indices,
            options: state.options,
            save: copy,
            saveName: state.saveName,
            bigEndian: $('big-endian').checked,
        },
        [copy],
    );

    refreshApplyButton();

    if (!res.ok) {
        /* Keep the headline short and put the explanation underneath, the same
         * shape as the success case — some of these messages are a sentence or
         * two long. */
        showResult(false, 'Apply failed', res.error || '');
        return;
    }

    state.patched = res.patched;
    const failed = res.results.filter((r) => !r.ok);
    const ok = res.results.length - failed.length;

    showResult(
        failed.length === 0,
        failed.length === 0
            ? `Applied ${ok} code${ok === 1 ? '' : 's'}`
            : `Applied ${ok} of ${res.results.length} codes`,
        failed.length
            ? `Failed: ${failed.map((r) => state.codes[r.index].name).join(', ')}. See the log for details.`
            : `${formatSize(res.patched.length)} ready to download.`,
    );
}

function showResult(good, title, detail) {
    $('result').hidden = false;
    $('result').classList.toggle('bad', !good);
    $('result-title').textContent = title;
    $('result-detail').textContent = detail;
    $('download').hidden = !state.patched;
    $('view-result').hidden = !state.patched;
}

function download() {
    if (!state.patched) return;
    const url = URL.createObjectURL(new Blob([state.patched], { type: 'application/octet-stream' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = state.saveName;
    a.click();
    URL.revokeObjectURL(url);
}

/* ---------------------------------------------------------------------------
 * Patch database browser
 *
 * dist/patches.json is generated at build time from a checkout of
 * apollo-patches (see web/tools/build-index.py) — ~2200 rows, 24KB gzipped,
 * fetched the first time the dialog opens. The patches themselves are fetched
 * from a CDN on demand, so an upstream patch fix reaches users without a
 * redeploy here.
 * ------------------------------------------------------------------------- */

const MAX_ROWS = 200;   /* rendering all 2200 is pointless; refine instead */

const db = { rows: null, loading: null, platform: null, generated: null };

async function loadIndex() {
    if (db.rows) return db.rows;
    if (!db.loading) {
        db.loading = (async () => {
            const res = await fetch('./patches.json');
            if (!res.ok) throw new Error(`index unavailable (${res.status})`);
            const doc = await res.json();
            db.rows = doc.patches.map(([platform, id, name]) => ({
                platform, id, name,
                /* precomputed once: the filter runs on every keystroke */
                haystack: `${name} ${id}`.toLowerCase(),
            }));
            db.generated = doc.generated;
            db.counts = doc.counts || {};
            return db.rows;
        })();
    }
    return db.loading;
}

function dbPlatforms() {
    const host = $('db-plats');
    if (host.childElementCount) return;

    const make = (label, value) => {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = 'chip';
        b.textContent = label;
        b.setAttribute('aria-pressed', String(db.platform === value));
        b.addEventListener('click', () => {
            db.platform = value;
            [...host.children].forEach((c) => c.setAttribute('aria-pressed', 'false'));
            b.setAttribute('aria-pressed', 'true');
            dbRender();
        });
        host.append(b);
    };

    make('All', null);
    for (const p of Object.keys(db.counts || {})) make(p, p);
}

function dbRender() {
    const list = $('db-results');
    const needle = $('db-search').value.trim().toLowerCase();
    list.textContent = '';

    let matches = db.rows || [];
    if (db.platform) matches = matches.filter((r) => r.platform === db.platform);
    if (needle) matches = matches.filter((r) => r.haystack.includes(needle));

    for (const row of matches.slice(0, MAX_ROWS)) {
        const li = document.createElement('li');
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'db-row';

        const name = document.createElement('span');
        name.className = 'db-name';
        name.textContent = row.name;

        const plat = document.createElement('span');
        plat.className = 'badge';
        plat.textContent = row.platform;

        const id = document.createElement('span');
        id.className = 'db-id';
        id.textContent = row.id;

        btn.append(name, plat, id);
        btn.addEventListener('click', () => dbPick(row));
        li.append(btn);
        list.append(li);
    }

    const shown = Math.min(matches.length, MAX_ROWS);
    const more = matches.length - shown;
    $('db-status').textContent = matches.length
        ? `${matches.length} match${matches.length === 1 ? '' : 'es'}` +
          (more > 0 ? ` — showing ${shown}, keep typing to narrow` : '')
        : 'No match';
}

async function openDb() {
    const dialog = $('db-dialog');
    $('db-status').textContent = 'Loading the index…';
    $('db-results').textContent = '';
    dialog.showModal();

    try {
        await loadIndex();
    } catch (err) {
        $('db-status').textContent =
            `${err.message}. You can still drop a .savepatch file instead.`;
        return;
    }

    dbPlatforms();
    dbRender();
    $('db-search').focus();
}

/* Fetch one patch and hand it to the normal load path. */
async function dbPick(row) {
    /* The dialog stays open until the fetch lands, so a second Enter or click
     * would start a competing download. */
    if (db.fetching) return;
    db.fetching = true;

    const url = `${CDN}/${row.platform}/${row.id}.savepatch`;
    $('db-status').textContent = `Fetching ${row.id}…`;

    try {
        const res = await fetch(url);
        if (!res.ok) throw new Error(`${res.status}`);
        const buffer = await res.arrayBuffer();

        $('db-dialog').close();
        await loadPatch(new File([buffer], `${row.id}.savepatch`), row.name);
    } catch (err) {
        $('db-status').textContent =
            `Could not fetch ${row.id} (${err.message}). It may have been renamed ` +
            `upstream since this index was built — dropping the file still works.`;
    } finally {
        db.fetching = false;
    }
}

/* ---------------------------------------------------------------------------
 * Wire up
 * ------------------------------------------------------------------------- */

wireDropZone('drop-patch', 'file-patch', loadPatch);
wireDropZone('drop-save', 'file-save', loadSave);

$('filter').addEventListener('input', renderCodes);
$('select-default').addEventListener('click', selectDefaults);
$('select-none').addEventListener('click', () => {
    state.checked.clear();
    renderCodes();
    refreshApplyButton();
});
$('apply').addEventListener('click', apply);
$('download').addEventListener('click', download);
$('dialog-close').addEventListener('click', () => $('code-dialog').close());

$('open-db').addEventListener('click', openDb);
$('view-patch').addEventListener('click', showPatchText);
$('view-save').addEventListener('click', editSaveData);
$('view-result').addEventListener('click', viewResultData);
$('db-close').addEventListener('click', () => $('db-dialog').close());
$('db-search').addEventListener('input', dbRender);
$('db-search').addEventListener('keydown', (e) => {
    /* Enter takes the top hit — the common case is typing a game and pressing
     * return, not reaching for the mouse. */
    if (e.key === 'Enter') {
        e.preventDefault();
        $('db-results').querySelector('.db-row')?.click();
    }
});

call('version').then(({ version }) => {
    if (version) $('version').textContent = `Apollo engine ${version}`;
});
