/*
 * Prove each catalogued tool against a real save, and write the verified list.
 *
 *   node tools/verify-tools.mjs <apollo-patches> <save-decrypters> \
 *        [--module web/dist/apollo.mjs] [--out web/dist/verified.json]
 *
 * For every row of verify-manifest.tsv it opens the shipped .savepatch in the
 * real wasm module and splits its required codes with the SAME toolkit.js the
 * page uses. Then, depending on what the patch is:
 *
 *   decrypt   applies the decrypt half to the encrypted sample and requires
 *             the output to equal the decrypted sample byte for byte.
 *
 *   checksum  (a row whose expected file is "-") applies the checksum half to
 *             a VALID save and requires the bytes not to change. That is a
 *             real test, not a tautology: the sample carries the checksum the
 *             game itself wrote, so a patch that hashes the wrong range, or
 *             with the wrong algorithm, or writes to the wrong offset, lands
 *             on a different value and fails. The apply must also succeed —
 *             a code that errors out leaves the file alone too, and that is
 *             not the same thing.
 *
 * So "verified" on the site does not mean "someone looked at it": it means
 * this exact patch, through this exact engine build, reproduced a reference
 * plaintext that a separate C tool produced independently.
 *
 * It also cross-checks the catalog's `kinds` against what toolkit.js derives
 * at run time. Those are two implementations of one rule — build-index.py's
 * CODE_* regexes and toolkit.js's — and this is what stops them drifting.
 *
 * After the direct runs it EXPANDS the result: any other patch in the database
 * whose applied chain is byte-identical to a proven one inherits that proof.
 * A game usually ships one patch per region — DmC has five, and all five carry
 * the same decrypt code — and games that share a save format share the code
 * verbatim. Identical code driven by the same engine cannot behave
 * differently, so this is transfer of evidence rather than a guess, and it is
 * the only claim made without a sample. It is also what stops the page listing
 * one region of a game and not the other four.
 *
 * Needs a module built for node (-sENVIRONMENT=node); `make verify` in web/
 * builds one. The wasm is identical to the shipped one either way.
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import crypto from 'node:crypto';
import { splitChain, needsOptions, isRequired, chainTargets, routeChain } from '../web/public/toolkit.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));

function arg(name, fallback) {
    const i = process.argv.indexOf(name);
    return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
}

const [patchesDir, samplesDir] = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const catalogPath = arg('--catalog', '');
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
        return { platform, titleId, enc, dec, bigEndian: be === '1',
                 /* "-" means: this is a checksum fixer, expect no change. */
                 checksumOnly: dec === '-' };
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

/* The applied chain's text, as the engine holds it — the fingerprint evidence
 * transfers across. Line endings are normalised because the database mixes
 * CRLF and LF, and trailing blanks because patch authors do. */
/* A short, stable id for a chain — patches sharing one behave identically, so
 * the page can safely present them as a single tool listing several title IDs.
 * Grouping by GAME NAME instead would be wrong: Metal Gear Solid V ships a
 * different key per region, so NPUB31594 and NPEB02140 are the same game and
 * emphatically not the same tool. */
function chainGroup(fingerprint) {
    return crypto.createHash('sha256').update(fingerprint).digest('hex').slice(0, 8);
}

function chainFingerprint(indices) {
    return indices
        .map((i) => M.UTF8ToString(M._apw_code_text(i))
            .replace(/\r\n?/g, '\n')
            .split('\n').map((l) => l.replace(/\s+$/, '')).join('\n')
            .trim())
        .join('\n--\n');
}

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
const proven = new Map();   /* chain fingerprint -> the row that proved it */
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
    const steps = row.checksumOnly ? chain.rest : chain.decrypt;
    if (!steps.length) {
        console.log(`FAIL  ${label}  no ${row.checksumOnly ? 'checksum' : 'decrypt'} step`);
        M._apw_close(); fail++; continue;
    }

    /* One sample, or several. A comma-separated `enc` is a multi-target row:
     * the samples are listed in the same order as chainTargets() wants them,
     * because the targets that need telling apart (LUNAR's two GAME.BIN) are
     * exactly the ones a filename cannot distinguish. */
    const encs = row.enc.split(',').map((x) => x.trim()).filter(Boolean);
    const targets = chainTargets(codes, steps);
    if (encs.length > 1 && encs.length !== targets.length) {
        console.log(`FAIL  ${label}  ${encs.length} samples for ${targets.length} target(s)`);
        M._apw_close(); fail++; continue;
    }

    const inputs = encs.map((rel, i) => ({
        path: `/verify${i}.bin`,
        bytes: fs.readFileSync(path.join(samplesDir, rel)),
    }));
    const assign = {};
    if (encs.length > 1) targets.forEach((t, i) => { assign[t] = i; });
    const routes = routeChain(codes, steps, assign);

    const runChain = (bufs) => {
        inputs.forEach((f, i) => M.FS.writeFile(f.path, new Uint8Array(bufs[i])));
        const ok = steps.map((i) =>
            withCString(inputs[routes[i]].path, (p) => !!M._apw_apply(i, p, row.bigEndian ? 1 : 0)));
        M._apw_reset_vars();
        const out = inputs.map((f) => Buffer.from(M.FS.readFile(f.path)));
        inputs.forEach((f) => M.FS.unlink(f.path));
        return { ok, out };
    };

    const save = inputs[0].bytes;
    const first = runChain(inputs.map((f) => f.bytes));
    const applied = first.ok;
    const got = first.out[0];
    const wants = row.checksumOnly
        ? inputs.map((f) => Buffer.from(f.bytes))
        : [Buffer.from(fs.readFileSync(path.join(samplesDir, row.dec)))];
    const want = wants[0];
    /* Every file has to come back untouched, not just the first. */
    const allHeld = !row.checksumOnly || first.out.every((b, i) => b.equals(wants[i]));
    const fingerprint = chainFingerprint(steps);

    /* Liveness, for checksum rows only.
     *
     * "Applying the fixer left this valid save untouched" is only evidence if
     * the fixer actually wrote something. A code whose range falls outside the
     * file, or that the engine declines for any other reason, also leaves the
     * bytes alone — and would sail through as a pass. So flip a byte and
     * require the output to differ from that flipped input: the checksum has
     * to move when the data under it moves.
     *
     * WHERE the byte is flipped matters, and one fixed position is not enough.
     * Plenty of checksums guard a header-sized prefix of a much larger file —
     * Far Cry 5 covers 0x10..0x892A3 of a 2MB memory.dat, Alien: Isolation
     * 0x20..0x86F of a 10KB STHEFILE — so a poke at the midpoint lands outside
     * the covered range and the checksum correctly does not move. Read as
     * "inert", that failed two patches that are in fact fine. Sweep a spread
     * of offsets instead and take the first one that moves it; only a code
     * that reacts NOWHERE is inert. */
    let live = true;
    if (row.checksumOnly && applied.every(Boolean) && allHeld) {
        live = false;
        /* Every input gets poked, not only the first: Dead Space computes over
         * USR-DATA and writes the result into HED-DATA, so a change to the
         * file that is never written to is the one that has to move the
         * other. */
        outer:
        for (let f = 0; f < inputs.length; f++) {
            const n = inputs[f].bytes.length;
            const spots = [...new Set([0x20, 0x40, n >> 5, n >> 4, n >> 3, n >> 2,
                                       n >> 1, n - (n >> 3), n - 1])]
                .filter((o) => o >= 0 && o < n);
            for (const spot of spots) {
                const bufs = inputs.map((x) => Buffer.from(x.bytes));
                bufs[f][spot] ^= 0xff;
                const { ok: ok2, out } = runChain(bufs);
                if (ok2.every(Boolean) && out.some((b, i) => !b.equals(bufs[i]))) {
                    live = true;
                    break outer;
                }
            }
        }
    }
    M._apw_close();

    const ok = applied.every(Boolean) && got.equals(want) && allHeld && live;
    if (ok) {
        pass++;
        const group = chainGroup(fingerprint);
        results.push([row.platform, row.titleId, chain.kinds, group]);
        proven.set(fingerprint, { label, kinds: chain.kinds, group });
        console.log(`ok    ${label}  ${doc.game || ''} (${steps.length} ${row.checksumOnly ? 'checksum' : 'decrypt'} code(s), ${got.length} bytes)`);
    } else {
        fail++;
        let why = !applied.every(Boolean) ? 'a code failed to apply'
            : !live ? 'the checksum code is inert — it did not react to changed data'
            : !allHeld ? 'a file other than the first came back changed'
            : row.checksumOnly ? 'the checksum code changed a valid save'
            : 'output differs';
        if (got.length !== want.length) why = `length ${got.length} != ${want.length}`;
        console.log(`FAIL  ${label}  ${why}`);
    }
}

console.log(`\n${pass} verified, ${fail} failed, ${rows.length} in manifest`);

/* ---- expansion: same chain, same proof ---- */
if (catalogPath && !fail) {
    const catalog = JSON.parse(fs.readFileSync(catalogPath, 'utf8')).tools;
    const already = new Set(results.map((r) => `${r[0]}/${r[1]}`));
    let added = 0;

    for (const [platform, titleId] of catalog) {
        if (already.has(`${platform}/${titleId}`)) continue;
        const file = path.join(patchesDir, platform, `${titleId}.savepatch`);
        if (!fs.existsSync(file)) continue;

        const doc = openPatch(file);
        if (!doc) continue;
        const codes = doc.codes || [];
        if (needsOptions(codes)) { M._apw_close(); continue; }

        const chain = splitChain(codes);
        for (const steps of [chain.decrypt, chain.rest]) {
            if (!steps.length) continue;
            const fp = chainFingerprint(steps);
            const hit = proven.get(fp);
            if (!hit) continue;
            results.push([platform, titleId, chain.kinds, hit.group]);
            already.add(`${platform}/${titleId}`);
            added++;
            console.log(`ok    ${platform}/${titleId}  ${doc.game || ''} — same chain as ${hit.label}`);
            break;
        }
        M._apw_close();
    }
    console.log(`\n${added} more by identical chain; ${results.length} verified in total`);
}

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
