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
import { splitChain, isRequired, chainTargets, routeChain,
         chainVariants, splitIndices, chainOptions, optionsReady,
         optionAssignments } from '../web/public/toolkit.js';

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
        const [platform, titleId, enc, dec, be, variant, options] = l.split('\t');
        return { platform, titleId, enc, dec, bigEndian: be === '1',
                 /* Optional 6th field: which of a multi-tool patch's variants
                  * this row is about. MGS HD holds Metal Gear Solid 2's whole
                  * chain and then Metal Gear Solid 3's. */
                 variant: Number(variant || 0),
                 /* Optional 7th field: the answers to the patch's {TAG}
                  * questions, `;`-separated, each `Value` or `TAG=Value`. */
                 options: (options || '').split(';').map((s) => s.trim())
                     .filter((s) => s && s.toLowerCase() !== 'noroundtrip'),
                 /* Opt out of the round trip, for a chain that genuinely cannot
                  * reproduce its own input. Written in the row so the exception
                  * is visible next to the evidence, never inferred. */
                 noRoundTrip: /(^|;)\s*noroundtrip\s*(;|$)/i.test(options || ''),
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

function chainFingerprint(indices, codes) {
    const text = indices
        .map((i) => M.UTF8ToString(M._apw_code_text(i))
            .replace(/\r\n?/g, '\n')
            .split('\n').map((l) => l.replace(/\s+$/, '')).join('\n')
            .trim())
        .join('\n--\n');

    /* A code that references {TAG} does not carry the tag's VALUES: the
     * `{TAG}a=One;b=Two{/TAG}` block lives at file scope, outside every code.
     * So two patches could hold identical code text and still behave
     * differently, and evidence must not transfer between them. Fold the
     * groups in — but only when there are any, so the fingerprints (and the
     * group ids in verified.json) of the other 99% stay exactly as they were. */
    const groups = chainOptions(codes || [], indices);
    if (!groups.length) return text;

    return `${text}\n--options--\n`
         + groups.map((g) => `${g.tag}=${g.values.join('|')}`).join('\n');
}

/*
 * Turn a row's 7th column into a selection chainOptions can drive.
 *
 * Matched on the value's NAME, not its position, so a patch that gains or
 * reorders its values fails this row loudly instead of quietly verifying a
 * different branch than the one the row is about. The tag may be left off when
 * the chain asks exactly one question, which is every case in the database —
 * writing `LA_NOIRE_AES_CBC256_KEY_OPTION=` out in full buys nothing.
 */
function resolveOptions(groups, wanted) {
    const bare = (t) => String(t).replace(/^\{|\}$/g, '');
    const chosen = {};

    for (const spec of wanted) {
        const eq = spec.indexOf('=');
        const tag = eq >= 0 ? spec.slice(0, eq).trim() : '';
        const want = (eq >= 0 ? spec.slice(eq + 1) : spec).trim();

        const hits = tag ? groups.filter((g) => bare(g.tag) === bare(tag)) : groups;
        if (hits.length !== 1)
            return { error: `option "${spec}" matches ${hits.length} of the chain's `
                          + `${groups.length} question(s)` };

        const at = hits[0].values.indexOf(want);
        if (at < 0)
            return { error: `"${want}" is not one of ${hits[0].tag}'s values `
                          + `(${hits[0].values.join(', ')})` };

        chosen[hits[0].key] = at;
    }

    if (!optionsReady(groups, chosen))
        return { error: `the chain asks ${groups.length} interactive question(s) and `
                      + `this row answers ${Object.keys(chosen).length}` };

    return { chosen };
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
    const whole = splitChain(codes);
    const variants = chainVariants(codes, whole.indices);
    if (!variants[row.variant]) {
        console.log(`FAIL  ${label}  no variant ${row.variant} (patch has ${variants.length})`);
        M._apw_close(); fail++; continue;
    }
    const chain = splitIndices(codes, variants[row.variant].indices);
    const steps = row.checksumOnly ? chain.rest : chain.decrypt;
    if (!steps.length) {
        console.log(`FAIL  ${label}  no ${row.checksumOnly ? 'checksum' : 'decrypt'} step`);
        M._apw_close(); fail++; continue;
    }

    /* Any {TAG} question this chain asks, answered from the row's 7th column.
     * The engine starts every group at "nothing chosen" and refuses the apply,
     * so an unanswered one fails here rather than looking like a broken patch
     * further down.
     *
     * Scoped to the WHOLE variant, not just the half being compared: the round
     * trip below drives the other half too, and L.A. Noire asks its question
     * from both its decrypt and its encrypt code. Resolving only the decrypt
     * side left the encrypt code unset and the engine refused it. */
    const groups = chainOptions(codes, variants[row.variant].indices);
    const picked = resolveOptions(groups, row.options);
    if (picked.error) {
        console.log(`FAIL  ${label}  ${picked.error}`);
        M._apw_close(); fail++; continue;
    }
    /* Selections live on the code and survive apw_reset_vars, so setting them
     * once here covers the liveness re-runs too. */
    for (const [index, sel] of Object.entries(optionAssignments(groups, picked.chosen)))
        sel.forEach((value, group) => M._apw_set_option(Number(index), group, value));

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

    /* Run an arbitrary step list over an arbitrary set of buffers. Parameterised
     * rather than closed over `steps` because the round trip below drives the
     * OTHER half of the chain through exactly the same path. */
    const runSteps = (stepList, stepRoutes, bufs) => {
        inputs.forEach((f, i) => M.FS.writeFile(f.path, new Uint8Array(bufs[i])));
        const ok = stepList.map((i) =>
            withCString(inputs[stepRoutes[i]].path, (p) => !!M._apw_apply(i, p, row.bigEndian ? 1 : 0)));
        M._apw_reset_vars();
        const out = inputs.map((f) => Buffer.from(M.FS.readFile(f.path)));
        inputs.forEach((f) => M.FS.unlink(f.path));
        return { ok, out };
    };
    const runChain = (bufs) => runSteps(steps, routes, bufs);

    const save = inputs[0].bytes;
    const first = runChain(inputs.map((f) => f.bytes));
    const applied = first.ok;
    const got = first.out[0];
    /* `dec` may list one plaintext per input, in the same order as `enc`. */
    /* What each output is checked against.
     *
     * `exact` is the bar and the default: the engine's output equals the
     * reference decrypter's, byte for byte. `prefix:` is a deliberate,
     * per-file opt-out for one situation — the reference tool TRIMS the file
     * and the engine does not. Metal Gear Solid HD's MASTER.BIN is the case:
     * `DECRYPT mgs_base64` decodes over `range 0x0000,eof+1` and leaves the
     * buffer its original length, so 32 bytes come back holding the right 21
     * bytes of payload followed by 11 the reference drops. The payload still
     * has to match exactly and in full — this only tolerates bytes PAST it,
     * and only where a row asks for it in writing. */
    const wants = row.checksumOnly
        ? inputs.map((f) => ({ mode: 'exact', buf: Buffer.from(f.bytes) }))
        : row.dec.split(',').map((rel) => {
            const spec = rel.trim();
            const prefix = spec.startsWith('prefix:');
            return {
                mode: prefix ? 'prefix' : 'exact',
                buf: Buffer.from(fs.readFileSync(
                    path.join(samplesDir, prefix ? spec.slice(7) : spec))),
            };
        });
    const want = wants[0];

    const holds = (out, w) => w.mode === 'prefix'
        ? out.length >= w.buf.length && out.subarray(0, w.buf.length).equals(w.buf)
        : out.equals(w.buf);

    /* Every output has to match, not just the first — for a checksum row that
     * means unchanged, for a decrypt row it means equal to its plaintext. */
    const bad = first.out
        .map((b, i) => (i < wants.length && !holds(b, wants[i])) ? i : -1)
        .filter((i) => i >= 0);
    const allHeld = bad.length === 0;
    const fingerprint = chainFingerprint(steps, codes);

    /*
     * ROUND TRIP -- re-encrypt what the decrypt just produced and require the
     * original encrypted sample back, byte for byte.
     *
     * Without this the whole re-encrypt half of the database is unproven. A
     * sweep of every BSD opcode in apollo-patches against what the suite
     * actually exercises found that of 107 applied codes, exactly TWO contained
     * an `encrypt` line -- and both were Resident Evil Remake DECRYPT codes that
     * run `encrypt blowfish_cbc` over a header while unwrapping it. So
     * `encrypt ffxiii`, `encrypt mgs`, `encrypt mgs_base64`,
     * `encrypt monster_hunter` and `encrypt diablo3` were all shipping behind a
     * Re-encrypt button that nothing had ever checked. The forward run proves
     * the key and the algorithm; this proves the way back.
     *
     * Fed from the ENGINE's own decrypt output rather than from the .dec file
     * on disk. That is the property a user depends on -- press Decrypt, edit,
     * press Re-encrypt, get a save the game accepts -- and it is the only form
     * that works for a reference tool that TRIMS: Metal Gear Solid HD's
     * MASTER.BIN comes back 32 bytes holding the 21 the reference writes, so
     * re-encrypting the 21-byte file would be encrypting something the engine
     * never produced.
     *
     * Rows that legitimately cannot round-trip say so in writing, with the
     * `noroundtrip` token in the options column; there is no silent skip.
     */
    let trip = null;
    if (!row.checksumOnly && chain.rest.length && applied.every(Boolean) && allHeld
        && !row.noRoundTrip) {
        const back = runSteps(chain.rest, routeChain(codes, chain.rest, assign), first.out);
        const wrong = back.out
            .map((b, i) => (i < inputs.length && !b.equals(Buffer.from(inputs[i].bytes))) ? i : -1)
            .filter((i) => i >= 0);
        trip = { applied: back.ok.every(Boolean), wrong };
    }

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

    const tripOk = !trip || (trip.applied && !trip.wrong.length);
    const ok = applied.every(Boolean) && allHeld && live && tripOk;
    if (ok) {
        pass++;
        const group = chainGroup(fingerprint);
        /* A patch can hold more than one row: L.A. Noire is proved twice over,
         * once per {TAG} branch, against a different pair of samples each
         * time. The catalog wants the title listed once. */
        if (!results.some((r) => r[0] === row.platform && r[1] === row.titleId))
            results.push([row.platform, row.titleId, chain.kinds, group]);
        proven.set(fingerprint, { label, kinds: chain.kinds, group });
        const slack = wants.reduce((n, w, i) => n +
            (w.mode === 'prefix' ? (first.out[i]?.length || 0) - w.buf.length : 0), 0);
        /* Every file's size, not just the first — a two-file row that reported
         * only "16 bytes" was naming Final Fantasy XIII-2's KEY.DAT and hiding
         * the 560KB save the row is really about. */
        const sizes = first.out.map((b) => b.length).join(' + ');
        console.log(`ok    ${label}  ${doc.game || ''} (${steps.length} ${row.checksumOnly ? 'checksum' : 'decrypt'} code(s), ${sizes} bytes${
            slack ? `, payload matched with ${slack} trailing byte(s) ignored` : ''}${
            trip ? ` + ${chain.rest.length}-code round trip` : ''})`);
    } else {
        fail++;
        let why = !tripOk
            ? (!trip.applied
                ? 'the re-encrypt chain failed to apply'
                : `re-encrypting the decrypt output did not restore the original (${
                    trip.wrong.map((i) => encs[i]).join(', ')})`)
            : !applied.every(Boolean) ? 'a code failed to apply'
            : !live ? 'the checksum code is inert — it did not react to changed data'
            : !allHeld ? (row.checksumOnly
                ? `changed a valid save (${bad.map((i) => encs[i]).join(', ')})`
                : `does not match the plaintext (${bad.map((i) => encs[i]).join(', ')})`)
            : 'output differs';
        if (want.mode !== 'prefix' && got.length !== want.buf.length)
            why = `length ${got.length} != ${want.buf.length}`;
        else if (want.mode === 'prefix' && got.length < want.buf.length)
            why = `output is ${got.length} bytes, shorter than the ${want.buf.length}-byte payload`;
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
        const chain = splitChain(codes);
        for (const steps of [chain.decrypt, chain.rest]) {
            if (!steps.length) continue;
            const fp = chainFingerprint(steps, codes);
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
