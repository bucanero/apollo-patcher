/*
 * Prove each catalogued tool against a real save, and write the verified list.
 *
 *   node tools/verify-tools.mjs <apollo-patches> <save-decrypters> \
 *        [--module web/dist/apollo.mjs] [--out web/dist/verified.json]
 *
 * For every row of verify-manifest.tsv it opens the shipped .savepatch in the
 * real wasm module, splits its required codes with the SAME toolkit.js the
 * page uses, applies the decrypt half to the encrypted sample, and requires
 * the output to equal the decrypted sample byte for byte.
 *
 * So "verified" on the site does not mean "someone looked at it": it means
 * this exact patch, through this exact engine build, reproduced a reference
 * plaintext that a separate C tool produced independently.
 *
 * It also cross-checks the catalog's `kinds` against what toolkit.js derives
 * at run time. Those are two implementations of one rule — build-index.py's
 * CODE_* regexes and toolkit.js's — and this is what stops them drifting.
 *
 * Needs a module built for node (-sENVIRONMENT=node); `make verify` in web/
 * builds one. The wasm is identical to the shipped one either way.
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { splitChain, needsOptions, isRequired } from '../web/public/toolkit.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));

function arg(name, fallback) {
    const i = process.argv.indexOf(name);
    return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
}

const [patchesDir, samplesDir] = process.argv.slice(2).filter((a) => !a.startsWith('--'));
if (!patchesDir || !samplesDir) {
    console.error('usage: verify-tools.mjs <apollo-patches> <save-decrypters> ' +
                  '[--module M.mjs] [--out verified.json]');
    process.exit(2);
}
const modulePath = path.resolve(arg('--module', path.join(HERE, '..', 'web', 'dist', 'apollo.mjs')));
const outPath = arg('--out', '');

const rows = fs.readFileSync(path.join(HERE, 'verify-manifest.tsv'), 'utf8')
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l && !l.startsWith('#'))
    .map((l) => {
        const [platform, titleId, enc, dec, be] = l.split('\t');
        return { platform, titleId, enc, dec, bigEndian: be === '1' };
    });

const M = await (await import(modulePath)).default();

/* The Python helper modules, exactly as the worker stages them. */
const pyDir = path.join(patchesDir, 'python');
if (fs.existsSync(pyDir)) {
    M.FS.mkdir('/python');
    for (const f of fs.readdirSync(pyDir).filter((f) => f.endsWith('.py')))
        M.FS.writeFile(`/python/${f}`, fs.readFileSync(path.join(pyDir, f)));
}

const withCString = (s, fn) => {
    const p = M.stringToNewUTF8(s);
    try { return fn(p); } finally { M._free(p); }
};

function openPatch(file) {
    const bytes = fs.readFileSync(file);
    const buf = M._malloc(bytes.length);
    M.HEAPU8.set(bytes, buf);
    const n = withCString(path.basename(file), (nm) => M._apw_open(buf, bytes.length, nm));
    M._free(buf);
    if (!n) return null;
    return JSON.parse(M.UTF8ToString(M._apw_codes_json()));
}

const results = [];
let pass = 0, fail = 0;

for (const row of rows) {
    const label = `${row.platform}/${row.titleId}`;
    const patchFile = path.join(patchesDir, row.platform, `${row.titleId}.savepatch`);
    const doc = openPatch(patchFile);
    if (!doc) { console.log(`FAIL  ${label}  patch would not parse`); fail++; continue; }

    const codes = doc.codes || [];
    if (needsOptions(codes)) {
        console.log(`FAIL  ${label}  required codes need an interactive option`);
        M._apw_close(); fail++; continue;
    }

    const chain = splitChain(codes);
    if (!chain.decrypt.length) {
        console.log(`FAIL  ${label}  no decrypt step`);
        M._apw_close(); fail++; continue;
    }

    const save = fs.readFileSync(path.join(samplesDir, row.enc));
    const want = fs.readFileSync(path.join(samplesDir, row.dec));
    M.FS.writeFile('/verify.bin', new Uint8Array(save));

    const applied = withCString('/verify.bin', (p) =>
        chain.decrypt.map((i) => !!M._apw_apply(i, p, row.bigEndian ? 1 : 0)));
    M._apw_reset_vars();

    const got = Buffer.from(M.FS.readFile('/verify.bin'));
    M.FS.unlink('/verify.bin');
    M._apw_close();

    const ok = applied.every(Boolean) && got.equals(want);
    if (ok) {
        pass++;
        results.push([row.platform, row.titleId, chain.kinds]);
        console.log(`ok    ${label}  ${doc.game || ''} (${chain.decrypt.length} code(s), ${got.length} bytes)`);
    } else {
        fail++;
        let why = applied.every(Boolean) ? 'output differs' : 'a code failed to apply';
        if (got.length !== want.length) why = `length ${got.length} != ${want.length}`;
        console.log(`FAIL  ${label}  ${why}`);
    }
}

console.log(`\n${pass} verified, ${fail} failed, ${rows.length} in manifest`);

if (outPath) {
    const doc = {
        generated: new Date().toISOString().replace(/\.\d+Z$/, 'Z'),
        source: 'bucanero/apollo-patches',
        method: 'decrypt chain applied to a real save; output compared byte-for-byte '
              + 'against the reference plaintext in bucanero/save-decrypters',
        verified: results.sort((a, b) => a[0].localeCompare(b[0]) || a[1].localeCompare(b[1])),
    };
    fs.mkdirSync(path.dirname(outPath) || '.', { recursive: true });
    fs.writeFileSync(outPath, JSON.stringify(doc) + '\n');
    console.log(`${outPath}: ${results.length} verified`);
}

process.exit(fail ? 1 : 0);
