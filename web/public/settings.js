/*
 * Settings: how saves are read and written.
 *
 * Two kinds of thing live here, and they are different in a way worth keeping
 * straight. The byte order describes THE SAVE IN FRONT OF YOU and is normally
 * detected per patch; the console IDs describe YOUR HARDWARE and are true
 * forever. Every one of them is safe to leave alone -- the defaults mean "keep
 * whatever the save already says", which is what patching one in place wants.
 *
 *   PSP, Fuse ID       Two of a PARAM.SFO's hashes are derived from the
 *                      console's own fuse in savedata modes 4 and 6. The
 *                      PSP's loader does not enforce them, but a GAME may:
 *                      console-locked titles verify them and flag the save
 *                      when they differ (Gran Turismo does).
 *
 *   PS3, console ID    PARAM.SFO's second hash inside PARAM.PFD is keyed by
 *                      the IDPS of one machine. Name one and the PS3 panel
 *                      offers to re-bind a save to it; leave it blank and
 *                      every save keeps the binding it arrived with.
 *
 *   PS3, account ID    The PSN account, and usually the better of the two:
 *                      it is written into the save's own PARAM.SFO, so the
 *                      save loads on ANY PS3 that account has signed in to
 *                      rather than on one machine. Unlike the two above it
 *                      never reaches the wasm module as a setting -- it is
 *                      passed with the call that uses it.
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
const EMPTY = {
    pspFuseId: '', ps3ConsoleId: '', ps3AccountId: '', ps3UserId: 1, byteOrder: 'auto',
};

/*
 * Byte order for save DATA, the CLI's -b/--big-endian flag.
 *
 * 'auto' is the default and the only value that cannot be wrong: PS3 saves are
 * big-endian and everything else Apollo covers is not, and the patch database's
 * own platform tag says which a patch is. The other two are for somebody who
 * knows better than the tag.
 *
 * Forcing one is remembered across visits, which is the point of it and also
 * its only hazard: a forced big-endian left set byte-reverses every PS4 or Vita
 * save afterwards, and the result looks like a patched save rather than an
 * error. Both pages therefore say which order is in effect and why, and say it
 * in amber when a forced one disagrees with the patch that is open.
 */
export const BYTE_ORDERS = ['auto', 'big', 'little'];

/* What the engine should run with for a patch the detection called `detected`. */
export function effectiveBigEndian(detected) {
    switch (current.byteOrder) {
        case 'big':    return true;
        case 'little': return false;
        default:       return !!detected;
    }
}

export const byteOrderForced = () => current.byteOrder !== 'auto';

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
export const hasAccountId = () => /^[0-9A-F]{16}$/i.test(current.ps3AccountId);

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
    const acct = $('set-acct').value.trim();
    const bad  = [];

    if (fuse && !/^[0-9A-F]{16}$/.test(fuse)) bad.push('the Fuse ID needs 16 hex digits');
    if (cid && !/^[0-9A-F]{32}$/.test(cid)) bad.push('the console ID needs 32 hex digits');
    // All-or-nothing like the others: a short account ID is not a shorter
    // account, it is a different one.
    if (acct && !/^[0-9A-Fa-f]{16}$/.test(acct)) bad.push('the account ID needs 16 hex digits');

    $('settings-save').disabled = bad.length > 0;
    setStatus(bad.join('; '), bad.length ? 'bad' : '');
    return bad.length === 0;
}

function render() {
    $('set-order').value = current.byteOrder;
    $('set-fuse').value = current.pspFuseId;
    $('set-cid').value = current.ps3ConsoleId;
    $('set-acct').value = current.ps3AccountId;
    $('set-user').value = String(current.ps3UserId || 1);
    renderOrderWarning();
    validate();
}

function renderOrderWarning() {
    $('set-order-warn').hidden = current.byteOrder === 'auto';
}

/*
 * The store on its own, for a page with no Settings dialog in it.
 *
 * index.html has none -- the console panels it would configure live on the
 * tools page -- but it applies codes, so it needs the byte order. Reading the
 * same key means the two pages cannot disagree about it.
 */
export function loadSettings() {
    return load();
}

/* Change one field and persist it. Returns the new settings. */
export function updateSettings(patch) {
    current = { ...current, ...patch };
    save();
    return settings();
}

export function initSettings(worker, changed) {
    call = worker;
    if (changed) onChange = changed;

    load();
    /* Apply the stored values before anything can use them. Failures are the
     * worker's to report; there is nothing the person can do about them at
     * start-up and the dialog will show the truth when they open it.
     *
     * This half runs on BOTH pages. The dialog below is on the patcher page
     * only -- the tools page has no control for these, it just honours them,
     * and says so when a forced byte order contradicts the patch it has open. */
    push().catch(() => {});

    if (!$('settings-open')) return;

    $('settings-open').addEventListener('click', () => {
        render();
        $('settings').showModal();
    });

    for (const id of ['set-fuse', 'set-cid', 'set-acct', 'set-user'])
        $(id).addEventListener('input', validate);

    /* The byte order takes effect the moment it is picked: it cannot be
     * half-typed, so there is nothing to validate and no reason to make anyone
     * press Save for it. */
    $('set-order').addEventListener('change', (ev) => {
        current.byteOrder = BYTE_ORDERS.includes(ev.target.value) ? ev.target.value : 'auto';
        save();
        renderOrderWarning();
        onChange({ ok: true, byteOrder: current.byteOrder });
        setStatus(current.byteOrder === 'auto'
            ? 'Byte order follows each patch again.'
            : `Byte order forced to ${current.byteOrder}-endian for every save.`,
            current.byteOrder === 'auto' ? 'good' : '');
    });

    $('settings-save').addEventListener('click', async () => {
        if (!validate()) return;

        current = {
            ...current,
            pspFuseId: $('set-fuse').value.trim().toUpperCase(),
            ps3ConsoleId: $('set-cid').value.trim().toUpperCase(),
            ps3AccountId: $('set-acct').value.trim().toLowerCase(),
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
        onChange({ ok: true, byteOrder: current.byteOrder });
        setStatus('Cleared. Byte order follows each patch, and saves keep whatever '
                  + 'console they are already bound to.', 'good');
    });
}
