/*
 * UI for the Apollo Patcher web front-end.
 *
 * No framework and no build step: the page is served exactly as it sits in
 * dist/. All engine work happens in worker.js; this file is only state + DOM.
 */

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

async function loadPatch(file) {
    const buffer = await file.arrayBuffer();
    state.patchName = file.name;
    $('patch-name').textContent = file.name;
    $('drop-patch').classList.add('filled');

    clearLog();
    const res = await call('open', { buffer, name: file.name }, [buffer]);
    if (!res.ok) {
        $('patch-name').textContent = res.error || 'Could not read this file';
        $('drop-patch').classList.remove('filled');
        $('workspace').hidden = true;
        state.codes = [];
        state.checked.clear();
        clearResult();
        return;
    }

    state.codes = res.codes;
    state.options = {};
    res.codes.forEach((c, i) => {
        if (c.options.length) state.options[i] = c.options.map((o) => o.sel);
    });

    $('game-name').textContent = res.game.trim() || file.name;
    $('code-count').textContent = `${res.codes.length} codes`;
    $('workspace').hidden = false;
    $('intro').hidden = true;
    clearResult();
    selectDefaults();
}

async function loadSave(file) {
    state.save = await file.arrayBuffer();
    state.saveName = file.name;
    $('save-name').textContent = `${file.name} · ${formatSize(state.save.byteLength)}`;
    $('drop-save').classList.add('filled');
    clearResult();
    refreshApplyButton();
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
                /* Ticking a code with option groups changes how they render
                 * (an unanswered one is flagged only while ticked). */
                if (code.options.length) renderCodes();
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
        showResult(false, res.error || 'Apply failed', '');
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

call('version').then(({ version }) => {
    if (version) $('version').textContent = `Apollo engine ${version}`;
});
