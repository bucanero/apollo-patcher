/*
 * Settings: which console a save is being written FOR.
 *
 * Both values here change what Apollo WRITES when it puts a save back, and
 * neither affects reading one. That is the whole shape of this dialog, and the
 * reason it is safe to leave empty: a blank field means "whatever the save
 * already says", which is what patching a save in place wants.
 *
 *   PSP, Fuse ID       Two of a PARAM.SFO's hashes are derived from the
 *                      console's own fuse in savedata modes 4 and 6. A PSP
 *                      loads a save whose values differ, so this is only
 *                      needed to reproduce one console's output byte for byte.
 *
 *   PS3, console ID    PARAM.SFO's second hash inside PARAM.PFD is keyed by
 *                      the IDPS of one machine. Name one and the PS3 panel
 *                      offers to re-bind a save to it; leave it blank and
 *                      every save keeps the binding it arrived with.
 *
 * The values live in the wasm module, which belongs to the worker, so they are
 * pushed there on every change and once at start-up. localStorage keeps them
 * across visits -- they are a property of the person's console, not of the
 * save they happen to be holding.
 */
const $ = (id) => document.getElementById(id);

const KEY = 'apollo.settings';

/* The defaults are "say nothing", which is why both are empty strings rather
 * than the values they stand in for. */
const EMPTY = { pspFuseId: '', ps3ConsoleId: '', ps3UserId: 1 };

let current = { ...EMPTY };
let call = null;
let onChange = () => {};

function load() {
    try {
        const raw = localStorage.getItem(KEY);
        if (raw) current = { ...EMPTY, ...JSON.parse(raw) };
    } catch {
        /* A private window, or storage the browser refuses. Not worth saying
         * anything about: the settings simply do not persist. */
    }
    return current;
}

function save() {
    try {
        localStorage.setItem(KEY, JSON.stringify(current));
    } catch { /* as above */ }
}

/* What is set right now. Read by the PS3 panel, which offers its re-bind
 * action only when a console has been named. */
export const settings = () => ({ ...current });
export const hasConsoleId = () => /^[0-9A-F]{32}$/i.test(current.ps3ConsoleId);

/* Push to the worker and report what it says is actually in effect -- which is
 * not always what was asked for, since it rejects a malformed value. */
async function push() {
    const res = await call('settings', {
        pspFuseId: current.pspFuseId,
        ps3ConsoleId: current.ps3ConsoleId,
        ps3UserId: current.ps3UserId,
    });
    onChange(res);
    return res;
}

function setStatus(text, tone) {
    const el = $('settings-status');
    el.textContent = text || '';
    el.className = `status${tone ? ' ' + tone : ''}`;
}

/* Both fields are hex and both are all-or-nothing: a half-typed value is not
 * "no value", it is a value that would bind a save to the wrong machine. */
function validate() {
    const fuse = $('set-fuse').value.trim().toUpperCase();
    const cid  = $('set-cid').value.trim().toUpperCase();
    const bad  = [];

    if (fuse && !/^[0-9A-F]{16}$/.test(fuse)) bad.push('the Fuse ID needs 16 hex digits');
    if (cid && !/^[0-9A-F]{32}$/.test(cid)) bad.push('the console ID needs 32 hex digits');

    $('settings-save').disabled = bad.length > 0;
    setStatus(bad.join('; '), bad.length ? 'bad' : '');
    return bad.length === 0;
}

function render() {
    $('set-fuse').value = current.pspFuseId;
    $('set-cid').value = current.ps3ConsoleId;
    $('set-user').value = String(current.ps3UserId || 1);
    validate();
}

export function initSettings(worker, changed) {
    call = worker;
    if (changed) onChange = changed;

    load();
    /* Apply the stored values before anything can use them. Failures are the
     * worker's to report; there is nothing the person can do about them at
     * start-up and the dialog will show the truth when they open it. */
    push().catch(() => {});

    $('settings-open').addEventListener('click', () => {
        render();
        $('settings').showModal();
    });

    for (const id of ['set-fuse', 'set-cid', 'set-user'])
        $(id).addEventListener('input', validate);

    $('settings-save').addEventListener('click', async () => {
        if (!validate()) return;

        current = {
            pspFuseId: $('set-fuse').value.trim().toUpperCase(),
            ps3ConsoleId: $('set-cid').value.trim().toUpperCase(),
            ps3UserId: Math.max(1, parseInt($('set-user').value, 10) || 1),
        };
        save();

        const res = await push();
        if (!res.ok) { setStatus(res.error, 'bad'); return; }

        setStatus(res.ps3.set
            ? `Saved. PS3 saves will be re-bound to ${res.ps3.consoleId}.`
            : 'Saved. Saves keep whatever console they are already bound to.',
            'good');
    });

    $('settings-clear').addEventListener('click', async () => {
        current = { ...EMPTY };
        save();
        render();
        await push();
        setStatus('Cleared. Saves keep whatever console they are already bound to.', 'good');
    });
}
