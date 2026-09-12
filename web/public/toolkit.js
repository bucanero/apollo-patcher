/*
 * What a patch can do, and which of its codes to run for each action.
 *
 * Shared by the tools page and by tools/verify-tools.mjs, so the thing the
 * verifier proves is literally the thing the page does.
 *
 * The engine is the source of truth for the code list; this only decides how
 * to split it. The split rule below is the one validated byte-for-byte
 * against real saves in apollo-lib's tests/test_samples.c.
 */

/* Classify by TITLE, not body. Resident Evil 2/4 Remake's DECRYPT code runs
 * `encrypt blowfish_cbc` over its header as part of unwrapping it, so "has an
 * encrypt op" does not mean "is the encrypt step".
 *
 * Keep these three in step with CODE_* in ../tools/build-index.py, which
 * classifies the same titles at build time for the catalog. They are checked
 * against each other by verify-tools.mjs. */
const MARKER = /^\s*(?:\[)?(?:python|sw|bsd|default|info)\s*:\s*/i;
const DECRYPT = /\b(decrypt|decode|decompress|unpack|extract|inflate|unzip)\b/i;
const ENCRYPT = /\b(encrypt|encode|compress|repack|pack|deflate|zip)\b/i;
const CHECKSUM =
    /(checksum|csum|\bcrc\w*|\bsha\w*|\bmd\d|\bhash|digest|adler\w*|fletcher\w*|murmur\w*|\bdwadd|\bwadd|\bqwadd|\badd\b|\bxor\b|\bsdbm|\bfnv|\bjhash|\bdjb2|signature|\bupdate\b|\bfix\b|\binit\b|\bcalculate\b|\bget\b)/i;

export const FLAG_REQUIRED = 4;

/* ---- which FILE a code is for ----------------------------------------
 *
 * Every code carries the `:file` target it was written under, and the engine
 * reports it verbatim — "HED-DATA", "card00LUNAR*\\GAME.BIN", "data/data0000.bin".
 * For a long time the front-end ignored it and applied every code to the one
 * file the user supplied, which apollo_apply_code lets you do: its `fpath`
 * argument overrides the code's own target. That is right for the common case
 * (one save, one target, whatever the user called it) and wrong for the
 * patches whose required chain genuinely spans two files — Dead Space checksums
 * USR-DATA and writes the result into HED-DATA, so overriding sends the write
 * to the wrong file and quietly corrupts the save.
 *
 * Paths are Windows-flavoured and may name a directory that does not exist on
 * the user's machine, so only the basename is meaningful here. */
export const targetBase = (file) =>
    String(file || '').split(/[\\/]/).filter(Boolean).pop() || '';

/* `~extracted\00000000.dat` is not a file at all: apollo_apply_code reads and
 * writes a BSD variable for those, the blob a preceding Decompress step put
 * there. It never touches the path you pass, so it must never become a slot
 * the user is asked to fill. */
export const isDerived = (code) => String(code.file || '').startsWith('~');

/* Only `*` appears as a wildcard in the database. A bare `*` means "whatever
 * the user brought" and matches anything. */
const escapeRe = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
export function matchesTarget(pattern, name) {
    const p = targetBase(pattern);
    if (!p || p === '*') return true;
    return new RegExp(`^${p.split('*').map(escapeRe).join('.*')}$`, 'i').test(name);
}

/**
 * Split a chain into contiguous per-target blocks: [{ target, indices }].
 */
export function chainBlocks(codes, indices) {
    const blocks = [];
    for (const index of indices) {
        const target = String(codes[index]?.file || '');
        const last = blocks[blocks.length - 1];
        if (last && last.target === target) last.indices.push(index);
        else blocks.push({ target, indices: [index] });
    }
    return blocks;
}

/**
 * Split a required chain into VARIANTS — independent tools that happen to
 * share one patch file.
 *
 * The signal is the chain restarting: a decrypt-kind code that comes after an
 * encrypt or checksum one. Nothing legitimate unwraps again after it has
 * rewrapped, so that is where one tool ends and the next begins.
 *
 *   Black Ops    d e | d e      decrypt+encrypt for GPAD0_CM.PRF, then the
 *                               same pair for GPAD0_SP.PRF
 *   MGS HD       d d c e e | d d e e   the whole Metal Gear Solid 2 chain,
 *                               then the whole Metal Gear Solid 3 one
 *
 * Everything else in the database comes back as a single variant, including
 * the shapes that legitimately span files (Dead Space's c,c,c across HED-DATA
 * and USR-DATA) and the ones that rewrap in an unusual order (Crisis Core's
 * d,e,c). Getting this wrong is not cosmetic: before it existed, "Re-encrypt"
 * on a Metal Gear Solid 2 save ran Metal Gear Solid 3's chain straight after
 * it.
 */
export function chainVariants(codes, indices) {
    const variants = [];
    let current = null;
    let closed = false;          /* has this variant rewrapped yet? */

    for (const index of indices) {
        const kind = classifyCode(codes[index]?.name);
        if (!current || (closed && kind === 'd')) {
            current = { indices: [], label: variantLabel(codes, index) };
            variants.push(current);
            closed = false;
        }
        if (kind === 'e' || kind === 'c') closed = true;
        current.indices.push(index);
    }
    return variants;
}

/* What to call a variant. Patch authors put the games in [Group:...] headings,
 * which the engine reports as non-required rows with parent=1 immediately
 * before the codes; the outermost of that run is the game name. Patches with
 * no headings (Black Ops) fall back to the file the variant works on. */
function variantLabel(codes, index) {
    let at = index - 1;
    while (at >= 0 && codes[at] && !isRequired(codes[at]) && codes[at].parent) at--;
    const heading = codes[at + 1];
    const name = heading && heading !== codes[index] ? String(heading.name || '').trim() : '';
    const cleaned = name.replace(/^[-\s]+|[-\s]+$/g, '');
    return cleaned || targetBase(codes[index]?.file) || '';
}

/**
 * The distinct targets a chain needs from the user, in the order it first
 * wants them. Derived targets are skipped (the engine makes those itself) and
 * so is a bare `*`, which names no particular file.
 *
 * Kept as the WHOLE target string, not the basename. LUNAR Remastered is the
 * reason: its two required codes are `card00LUNAR*\\GAME.BIN` and
 * `card*SAVEDATA\\GAME.BIN` — two different files that share a name and are
 * told apart only by the folder they sit in. Collapsing to basenames would
 * have made that one slot and silently checksummed the wrong save.
 *
 * Zero or one entry means the old behaviour is correct and the user brings a
 * single save under any name. Two or more means the tool genuinely needs that
 * many files.
 */
export function chainTargets(codes, indices) {
    const seen = [];
    for (const index of indices) {
        const code = codes[index];
        if (!code || isDerived(code)) continue;
        const file = String(code.file || '');
        if (file && targetBase(file) !== '*' && !seen.includes(file)) seen.push(file);
    }

    /* Two targets sharing a basename are usually ONE file written two ways:
     * Silent Hill 3 scopes some codes under `BLUS30810_SH3*\\SAVEDATA.DAT` and
     * the rest under a bare `SAVEDATA.DAT`. Keeping both asked for the same
     * save twice. A bare name is a generic reference, so its presence collapses
     * the group.
     *
     * LUNAR Remastered is the case that must NOT collapse: `card00LUNAR*\\
     * GAME.BIN` and `card*SAVEDATA\\GAME.BIN` are two different saves, and
     * every member there carries a folder. So the rule is on qualification,
     * not on the name. */
    const out = [];
    for (const file of seen) {
        const base = targetBase(file);
        const group = seen.filter((f) => targetBase(f) === base);
        if (group.length > 1 && group.some((f) => !/[\\/]/.test(f))) {
            const bare = group.find((f) => !/[\\/]/.test(f));
            if (!out.includes(bare)) out.push(bare);
        } else if (!out.includes(file)) {
            out.push(file);
        }
    }
    return out;
}

/* What to call a target on screen. The basename is the useful part, except
 * when two targets share one — then the folder is the whole point, so show
 * the path as written. */
export function targetLabels(targets) {
    const bases = targets.map(targetBase);
    return targets.map((t, i) =>
        bases.filter((b) => b === bases[i]).length > 1 ? t : bases[i]);
}

/**
 * Which of the user's files each code is applied to: code index -> the
 * POSITION of that file in the input list.
 *
 * Position, not name, because two of a tool's files can share a name — LUNAR
 * Remastered wants two different GAME.BINs — and keying on the name would
 * silently collapse them into one.
 *
 * `assign` maps a target string (as chainTargets returned it) to that
 * position. Anything not covered — a derived target, a bare `*`, a
 * single-file tool — falls back to input 0, which is the override the
 * front-end has always used and what every one-target tool wants.
 */
export function routeChain(codes, indices, assign) {
    const keys = Object.keys(assign || {});
    const routes = {};
    for (const index of indices) {
        const code = codes[index];
        let at;
        if (code && !isDerived(code)) {
            const file = String(code.file || '');
            at = assign[file];
            /* chainTargets collapses a qualified target onto the bare one when
             * they name the same file, so a code may carry the spelling that
             * did not become the key. Fall back to the basename. */
            if (at === undefined) {
                const hit = keys.find((k) => targetBase(k) === targetBase(file));
                if (hit !== undefined) at = assign[hit];
            }
        }
        routes[index] = at === undefined ? 0 : at;
    }
    return routes;
}

/** 'd' | 'e' | 'c' | '' */
export function classifyCode(name) {
    const title = String(name || '').replace(MARKER, '').trim();
    if (DECRYPT.test(title)) return 'd';
    if (ENCRYPT.test(title)) return 'e';
    if (CHECKSUM.test(title)) return 'c';
    return '';
}

/* A code counts as required when the engine flagged it, or when its title says
 * so — the flag comes from the parser seeing "(REQUIRED)", and a handful of
 * patches carry the marker in a spelling the parser does not flag. */
export function isRequired(code) {
    return !!(code.flags & FLAG_REQUIRED) || /\(required\)/i.test(code.name || '');
}

/**
 * Split a patch's required codes into the two actions a user asks for.
 *
 * Codes run in FILE ORDER — that ordering is the patch author's, and it is
 * load-bearing. Silent Hill 3 is decrypt, then update the DWADD checksum, then
 * encrypt; skip the middle and the game rejects the save. Crisis Core computes
 * its checksum over the CIPHERTEXT, which is why its checksum code sits after
 * the encrypt rather than before it.
 *
 * The split point is the first checksum-or-encrypt code. Everything before it
 * unwraps (decrypt, and decompress where a patch does both, and any unlabelled
 * setup step such as "Read Encryption KEY.DAT"); everything from it onward
 * rewraps. Verified against real saves for every shape in the database:
 *
 *   [d, c, e]       Silent Hill 3      -> {d:[0],   rest:[1,2]}
 *   [d, e, c]       Crisis Core FF7    -> {d:[0],   rest:[1,2]}
 *   [d, d, e, e]    FF Pixel Remaster  -> {d:[0,1], rest:[2,3]}
 *   [d, c, e, c]    DBZ Xenoverse 2    -> {d:[0],   rest:[1,2,3]}
 *   [c]             a checksum fixer   -> {d:[],    rest:[0]}
 */
export function splitChain(codes) {
    const required = [];
    codes.forEach((code, index) => { if (isRequired(code)) required.push(index); });
    return splitIndices(codes, required);
}

/**
 * The same split, over an arbitrary list of code indices — one variant's
 * codes, say. splitChain is this applied to every required code.
 */
export function splitIndices(codes, indices) {
    const kinds = indices.map((i) => classifyCode(codes[i]?.name));
    let split = kinds.findIndex((k) => k === 'c' || k === 'e');
    if (split < 0) split = indices.length;

    /*
     * Leading UNLABELLED codes are setup, not decryption, and both halves need
     * them.
     *
     * Final Fantasy XIII-2 opens with "Read Encryption KEY.DAT", which is
     * `set [key]:read(0x0,0x10)` against a second file. It classifies as
     * neither decrypt nor checksum, so it rides at the head of the decrypt
     * chain -- and its whole job is to put [key] where the REST of the chain
     * can find it: `ENCRYPT FFXIII(2, [key])`. Giving it to the decrypt half
     * alone meant Re-encrypt ran with [key] unset and the engine refused the
     * code, which looked like a broken patch rather than a broken split.
     *
     * Re-running it is safe by construction: a prelude reads, it does not
     * write. Five patches in the database have one (the Final Fantasy XIII-2 /
     * Lightning Returns family) and all five reference its variable from the
     * re-encrypt half, so there is no case where this adds a step nobody wants.
     *
     * Bounded by the first DECRYPT code, not by the split point, so a patch
     * whose required codes are all unlabelled keeps the old shape -- one
     * action, not two identical ones.
     */
    const firstDecrypt = kinds.findIndex((k) => k === 'd');
    let prelude = 0;
    if (firstDecrypt > 0 && split > 0)
        while (prelude < firstDecrypt && kinds[prelude] === '') prelude++;

    return {
        /* NOTE these two now OVERLAP by the prelude, so they are no longer a
         * partition and `[...decrypt, ...rest]` is not the chain -- it repeats
         * the prelude, which silently corrupted variant splitting and chain
         * fingerprints the first time round. `indices` is the whole list, in
         * order, for callers that want it. */
        indices,
        decrypt: indices.slice(0, split),
        rest: [...indices.slice(0, prelude), ...indices.slice(split)],
        kinds: [...new Set(kinds.filter(Boolean))].sort().join(''),
    };
}

/* ---- interactive {TAG} choices ---------------------------------------
 *
 * A handful of patches put a `{TAG}value=Label;...{/TAG}` block at file scope
 * and reference {TAG} from inside a REQUIRED code. The engine substitutes the
 * chosen value textually before running the code, and starts with nothing
 * chosen (sel = -1), so an apply with the choice unmade is refused.
 *
 * L.A. Noire is the case that matters: the {TAG} IS the AES key, and its two
 * values are two genuinely different saves — `Game Savedata` and
 * `User Profile`. Nothing but the person holding the file can say which one it
 * is, so the page has to ask. It used to duck the question by refusing to list
 * such patches at all.
 *
 * Keyed by tag AND by the value list, so one choice drives every code that
 * mentions it — L.A. Noire names {ST} in both its decrypt and its encrypt code
 * and must not ask twice — while a tag that somehow carried different values
 * in different codes would still be asked separately rather than silently
 * conflated.
 */
export function chainOptions(codes, indices) {
    const groups = [];
    const seen = new Map();

    for (const index of indices) {
        (codes[index]?.options || []).forEach((opt, group) => {
            const tag = String(opt.tag || '');
            const values = (opt.values || []).map(String);
            const key = `${tag} ${values.join(' ')}`;

            let g = seen.get(key);
            if (!g) {
                g = { key, tag, values, label: optionLabel(values), at: [] };
                seen.set(key, g);
                groups.push(g);
            }
            g.at.push({ index, group });
        });
    }
    return groups;
}

/* What to call the choice on screen.
 *
 * The tag is the patch author's identifier and ranges from cryptic to unusable
 * ({ST}, {SF}, {LA_NOIRE_AES_CBC256_KEY_OPTION}), so it is not a label. The
 * VALUES are written for humans, and when they share an opening they name the
 * thing being chosen for free: "Save Slot 1".."Save Slot 10" gives "Save
 * slot", "User Profile #1".."#8" gives "User profile". When they share
 * nothing they are each self-describing already (L.A. Noire's "Game Savedata"
 * / "User Profile"), and a generic heading over them reads better than a
 * specific one this code would have to invent. */
function optionLabel(values) {
    if (values.length < 2) return 'Option';

    let prefix = values[0];
    for (const v of values.slice(1)) {
        let i = 0;
        while (i < prefix.length && i < v.length && prefix[i] === v[i]) i++;
        prefix = prefix.slice(0, i);
    }
    /* Cut back to a word boundary, so "Save Slot 1" / "Save Slot 10" does not
     * yield "Save Slot 1", then drop what a numbered list leaves hanging. */
    prefix = prefix.replace(/\S*$/, '').replace(/[\s#:_-]+$/, '').trim();

    return prefix.length >= 3
        ? prefix.charAt(0) + prefix.slice(1).toLowerCase()
        : 'Option';
}

/* True once every group chainOptions returned has a value picked. `chosen`
 * maps a group key to an index into that group's values. */
export function optionsReady(groups, chosen) {
    return groups.every((g) => Number.isInteger(chosen?.[g.key]) && chosen[g.key] >= 0);
}

/* The worker's `options` payload: code index -> the value for each of that
 * code's option groups, in group order, which is what
 * apw_set_option(index, group, value) wants. */
export function optionAssignments(groups, chosen) {
    const out = {};
    for (const g of groups) {
        const value = chosen?.[g.key];
        if (!Number.isInteger(value) || value < 0) continue;
        for (const { index, group } of g.at) (out[index] ||= [])[group] = value;
    }
    return out;
}

/* Does this patch's required chain ask anything at all? For callers that only
 * want the yes/no; chainOptions is the one that answers "what". */
export function needsOptions(codes) {
    return codes.some((code) => isRequired(code) && (code.options || []).length > 0);
}
