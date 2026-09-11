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
    codes.forEach((code, index) => {
        if (isRequired(code)) required.push({ index, kind: classifyCode(code.name) });
    });

    let split = required.findIndex((c) => c.kind === 'c' || c.kind === 'e');
    if (split < 0) split = required.length;

    return {
        decrypt: required.slice(0, split).map((c) => c.index),
        rest: required.slice(split).map((c) => c.index),
        kinds: [...new Set(required.map((c) => c.kind).filter(Boolean))].sort().join(''),
    };
}

/* A patch whose required codes need an interactive {TAG} choice cannot be
 * driven by two buttons — the engine leaves the option unset and the apply is
 * refused. Both L.A. Noire patches are in this position (the key IS the
 * choice). They are excluded rather than guessed at. */
export function needsOptions(codes) {
    return codes.some((code) => isRequired(code) && (code.options || []).length > 0);
}
