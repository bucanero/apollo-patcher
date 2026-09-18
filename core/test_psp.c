/*
 * Headless checks for the PSP savedata layer (core/psp), so CI exercises the
 * crypto and the PARAM.SFO parser without a GUI and without a console.
 *
 *   test_psp                        run every self-contained check
 *   test_psp --dump-fixtures DIR    write the synthetic saves the known-answer
 *                                   vectors below were taken from
 *   test_psp --save DIR FILE KEY    run a real save through both directions:
 *                                   DIR holds PARAM.SFO and FILE, KEY is 32
 *                                   hex digits (or "-" for an unkeyed game)
 *   test_psp --keydb FILE DIR...    look each save directory up in a real
 *                                   gamekeys.txt, the way both front-ends do
 *
 * On the known-answer vectors
 * ---------------------------
 * The digests in KNOWN[] were not produced by this code. They come from the
 * UNMODIFIED upstream implementation -- apollo-psp's psp_decrypter.c and
 * kirk_engine.c, compiled for the host with nothing changed but the three
 * lines it takes to build off a PSP -- run over the fixtures --dump-fixtures
 * writes. So a pass here says the refactor is byte-faithful to the code that
 * ships on the console, not merely self-consistent.
 *
 * To regenerate them after an upstream change, see REGENERATING below.
 *
 * Encryption is deterministic, which is what makes this possible at all: a
 * console picks the savedata IV off KIRK's PRNG, and Apollo uses a fixed one
 * (see EncryptSavedata in psp_savedata.c). Decryption never had a choice.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0777)
#endif

#include "psp/psp_savedata.h"

/* ------------------------------------------------------------------ *
 * REGENERATING the known-answer vectors
 *
 *   1. Build the oracle -- upstream, unmodified:
 *
 *        mkdir /tmp/oracle && cd /tmp/oracle
 *        cp <apollo-psp>/source/{kirk_engine.c,psp_decrypter.c} .
 *        cp <apollo-psp>/include/kirk_engine.h .
 *        # the three build-off-a-PSP edits:
 *        #   kirk_engine.h  <psptypes.h>      -> stdint typedefs for u8..u64
 *        #   kirk_engine.c  pspXploitKernelRead64 -> return 0xFFFFFFFFFFFFFFFF
 *        #   psp_decrypter.c <apollo.h>/<dbglogger.h> -> local read_buffer,
 *        #                   write_buffer, dbglogger_log
 *        # then a main() calling psp_EncryptSavedata(dir, name, key).
 *
 *      Note the fuse: upstream falls back to all-ones when the kernel read
 *      fails and no DATA/FUSEID.BIN exists, which is exactly
 *      KIRK_HOST_FUSE_ID. The oracle and this code therefore agree by
 *      construction, not by coincidence.
 *
 *   2. test_psp --dump-fixtures /tmp/fx
 *   3. For each mode directory, run the oracle's encrypt over PARAM.SFO and
 *      DATA.BIN, then take fnv1a64() of each result (the helper below is 12
 *      lines; the dump prints its own so the two are easy to compare).
 *   4. Paste the pairs into KNOWN[].
 * ------------------------------------------------------------------ */

/* FNV-1a, 64-bit. Not a security hash -- a fixture digest, chosen so the test
 * carries no dependency and the constants stay readable. */
static uint64_t fnv1a64(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ULL;
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* ---- the synthetic save ------------------------------------------------- */

#define FIXTURE_NAME  "DATA.BIN"
#define FIXTURE_DIR   "ULUS10391"
#define FIXTURE_LEN   0x2000          /* spans several 0x800 crypt blocks */
#define FLIST_MAX     0xC60           /* what a real PARAM.SFO reserves */
#define PARAMS_MAX    0x80

/* A deterministic plaintext. xorshift rather than rand(), whose sequence is
 * libc's business and would make the vectors unreproducible off this machine. */
static void fixture_data(uint8_t *out, size_t len)
{
    uint32_t x = 0x9E3779B9u;
    size_t i;

    for (i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = (uint8_t)(x >> 24);
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/*
 * Build a PARAM.SFO with the three values that matter, shaped like a real one:
 * SAVEDATA_DIRECTORY, SAVEDATA_FILE_LIST holding a single FIXTURE_NAME entry,
 * and SAVEDATA_PARAMS whose first byte is `params0`.
 *
 * Returns the length written, or 0 if `cap` is too small.
 */
static size_t build_sfo(uint8_t *out, size_t cap, uint8_t params0)
{
    static const char *KEYS[] = { "SAVEDATA_DIRECTORY", "SAVEDATA_FILE_LIST",
                                  "SAVEDATA_PARAMS" };
    const uint32_t maxes[3] = { 64, FLIST_MAX, PARAMS_MAX };
    const uint32_t lens[3]  = { (uint32_t)sizeof FIXTURE_DIR, FLIST_MAX, PARAMS_MAX };
    uint32_t key_off[3], data_off[3];
    uint32_t keys_at, data_at, at;
    size_t total;
    int i;

    keys_at = 0x14 + 3 * 0x10;

    at = 0;
    for (i = 0; i < 3; i++) {
        key_off[i] = at;
        at += (uint32_t)strlen(KEYS[i]) + 1;
    }
    /* Real files pad the key table to a 4-byte boundary. */
    at = (at + 3) & ~3u;
    data_at = keys_at + at;

    at = 0;
    for (i = 0; i < 3; i++) {
        data_off[i] = at;
        at += maxes[i];
    }
    total = data_at + at;
    if (cap < total)
        return 0;

    memset(out, 0, total);
    put32(out + 0x00, 0x46535000u);      /* "\0PSF" */
    put32(out + 0x04, 0x00000101u);
    put32(out + 0x08, keys_at);
    put32(out + 0x0C, data_at);
    put32(out + 0x10, 3);

    for (i = 0; i < 3; i++) {
        uint8_t *e = out + 0x14 + 0x10 * i;

        put16(e + 0x00, (uint16_t)key_off[i]);
        put16(e + 0x02, 0x0204);         /* UTF-8 string; not read by any of this */
        put32(e + 0x04, lens[i]);
        put32(e + 0x08, maxes[i]);
        put32(e + 0x0C, data_off[i]);
        strcpy((char *)out + keys_at + key_off[i], KEYS[i]);
    }

    strcpy((char *)out + data_at + data_off[0], FIXTURE_DIR);
    strcpy((char *)out + data_at + data_off[1], FIXTURE_NAME);  /* one entry */
    out[data_at + data_off[2]] = params0;
    return total;
}

/* ---- the harness -------------------------------------------------------- */

static int g_fails;

#define CHECK(what, cond) do {                                        \
        int ok_ = (cond);                                             \
        if (!ok_) g_fails++;                                          \
        printf("  %-56s %s\n", (what), ok_ ? "ok" : "FAILED");        \
    } while (0)

#define CHECK_RC(what, rc) do {                                       \
        int rc_ = (rc);                                               \
        if (rc_ != APSP_OK) g_fails++;                                \
        printf("  %-56s %s\n", (what),                                \
               rc_ == APSP_OK ? "ok" : apsp_strerror(rc_));           \
    } while (0)

/*
 * The savedata modes, as SAVEDATA_PARAMS[0] spells them.
 *
 * 0x01 is the unkeyed path (the key is all zeroes, so SD mode 1 regardless).
 * 0x21 selects SD mode 3, 0x41 SD mode 5 -- the one every save written by
 * firmware 2.5.2 or later uses, and the one the Monster Hunter save this was
 * first checked against carries.
 */
static const struct {
    const char *name;
    uint8_t     params0;
    int         null_key;
} MODES[] = {
    { "mode 01 (unkeyed)", 0x01, 1 },
    { "mode 21 (SD 3)",    0x21, 0 },
    { "mode 41 (SD 5)",    0x41, 0 },
};
#define NMODES ((int)(sizeof MODES / sizeof MODES[0]))

/* The fixture game key, when the mode uses one. Arbitrary but fixed. */
static const uint8_t FIXTURE_KEY[APSP_KEY_LEN] = {
    0x4A, 0x1F, 0xF3, 0x59, 0xAE, 0xB6, 0xEF, 0xF8,
    0x1C, 0xA8, 0xCB, 0x23, 0xBC, 0xA5, 0x7B, 0xB3,
};

/*
 * Digests of what the UNMODIFIED upstream implementation produces for each
 * mode: the encrypted DATA.BIN, and the PARAM.SFO after encryption rewrote
 * its hashes. See REGENERATING above. Zero means "not yet pinned" and the
 * check reports the value instead of failing, so a first run tells you what
 * to paste in.
 *
 * Taken 2026-09-14 from apollo-psp @ 17cb5ea, built for the host, over the
 * fixtures --dump-fixtures writes (8192 bytes of plaintext -> 8208 encrypted,
 * a 3484-byte PARAM.SFO).
 */
static const struct { uint64_t enc, sfo; } KNOWN[NMODES] = {
    { 0x42d277ecda5919abULL, 0xe72029f9b46d9945ULL },   /* mode 01 */
    { 0xd0b4cf4932a8f665ULL, 0xd6485a8928bb5e27ULL },   /* mode 21 */
    { 0x1303a629d0f210a6ULL, 0x1e154f75049bb9b4ULL },   /* mode 41 */
};

static void key_for(int mode, uint8_t key[APSP_KEY_LEN])
{
    if (MODES[mode].null_key)
        memset(key, 0, APSP_KEY_LEN);
    else
        memcpy(key, FIXTURE_KEY, APSP_KEY_LEN);
}

/* ---- checks ------------------------------------------------------------- */

static void check_sfo_accessors(void)
{
    uint8_t sfo[0x2000];
    size_t len = build_sfo(sfo, sizeof sfo, 0x41);
    char buf[64];

    puts("\nPARAM.SFO accessors");
    CHECK("the fixture builds", len > 0);
    CHECK_RC("it validates", apsp_sfo_valid(sfo, len));

    CHECK_RC("SAVEDATA_DIRECTORY reads back",
             apsp_sfo_directory(sfo, len, buf, sizeof buf));
    CHECK("...and is the directory it was given", strcmp(buf, FIXTURE_DIR) == 0);

    CHECK("one file is listed", apsp_sfo_file_count(sfo, len) == 1);
    CHECK_RC("its name reads back", apsp_sfo_file_name(sfo, len, 0, buf, sizeof buf));
    CHECK("...and is the file it was given", strcmp(buf, FIXTURE_NAME) == 0);

    /* An index past the end is an error, not a read off the end of the list. */
    CHECK("an out-of-range index is refused",
          apsp_sfo_file_name(sfo, len, 7, buf, sizeof buf) < 0);
    /* A buffer too small for the name is refused rather than overrun. */
    CHECK("a too-small output buffer is refused",
          apsp_sfo_file_name(sfo, len, 0, buf, 4) == APSP_ERR_SIZE);
}

/*
 * Malformed input.
 *
 * Every offset in a PARAM.SFO comes out of the file. These are the shapes that
 * walk off the end of the buffer when one is followed unchecked: each must be
 * REFUSED, and none may crash. Run this under ASan to get the second half of
 * that sentence checked as well.
 */
static void check_sfo_bounds(void)
{
    uint8_t sfo[0x2000];
    size_t len = build_sfo(sfo, sizeof sfo, 0x41);
    uint8_t bad[0x2000];
    size_t i;

    puts("\nPARAM.SFO bounds");

    CHECK("a NULL buffer is refused",  apsp_sfo_valid(NULL, 16) < 0);
    CHECK("an empty buffer is refused", apsp_sfo_valid(sfo, 0) < 0);

    /* Truncation at every length below the full file. A prefix is the most
     * likely real-world corruption and the easiest to get wrong. */
    {
        int refused = 1;
        for (i = 1; i < len; i++) {
            memcpy(bad, sfo, i);
            if (apsp_sfo_valid(bad, i) == APSP_OK) { refused = 0; break; }
        }
        CHECK("every truncation is refused", refused);
    }

    memcpy(bad, sfo, len);
    put32(bad + 0x00, 0x41424344u);
    CHECK("a wrong magic is refused", apsp_sfo_valid(bad, len) < 0);

    memcpy(bad, sfo, len);
    put32(bad + 0x04, 0x00000202u);
    CHECK("a wrong version is refused", apsp_sfo_valid(bad, len) < 0);

    memcpy(bad, sfo, len);
    put32(bad + 0x10, 0x10000000u);
    CHECK("an absurd entry count is refused", apsp_sfo_valid(bad, len) < 0);

    memcpy(bad, sfo, len);
    put32(bad + 0x08, (uint32_t)len + 0x1000);
    CHECK("a key table past the end is refused", apsp_sfo_valid(bad, len) < 0);

    memcpy(bad, sfo, len);
    put32(bad + 0x0C, (uint32_t)len + 0x1000);
    CHECK("a data table past the end is refused", apsp_sfo_valid(bad, len) < 0);

    /* The third index entry is SAVEDATA_PARAMS; push its value off the end. */
    memcpy(bad, sfo, len);
    put32(bad + 0x14 + 0x20 + 0x0C, 0xFFFFFF00u);
    CHECK("a value offset past the end is refused", apsp_sfo_valid(bad, len) < 0);

    memcpy(bad, sfo, len);
    put32(bad + 0x14 + 0x20 + 0x08, 0xFFFFFF00u);
    CHECK("a value length past the end is refused", apsp_sfo_valid(bad, len) < 0);

    /* used > reserved is incoherent and would let a read run past the value. */
    memcpy(bad, sfo, len);
    put32(bad + 0x14 + 0x20 + 0x04, PARAMS_MAX + 0x100);
    CHECK("used length above reserved is refused", apsp_sfo_valid(bad, len) < 0);

    /* SAVEDATA_PARAMS shorter than the block whose hashes get written into
     * it: accepting this is a 0x80-byte write into a smaller value. */
    memcpy(bad, sfo, len);
    put32(bad + 0x14 + 0x20 + 0x08, 0x10);
    put32(bad + 0x14 + 0x20 + 0x04, 0x10);
    CHECK("a short SAVEDATA_PARAMS is refused", apsp_sfo_valid(bad, len) < 0);

    /* An unterminated key name, which strcmp() would read past. */
    {
        uint32_t keys = 0x14 + 3 * 0x10;
        memcpy(bad, sfo, len);
        memset(bad + keys, 'A', len - keys);
        CHECK("an unterminated key name is refused", apsp_sfo_valid(bad, len) < 0);
    }
}

static void check_keys(void)
{
    uint8_t key[APSP_KEY_LEN], zero[APSP_KEY_LEN];
    uint8_t dumper[0x600];
    size_t i;

    puts("\ngame keys");
    memset(zero, 0, sizeof zero);

    for (i = 0; i < sizeof dumper; i++)
        dumper[i] = (uint8_t)i;

    CHECK_RC("SGKeyDumper form (0x10 bytes)",
             apsp_key_from_buffer(dumper, 0x10, key));
    CHECK("...is the file verbatim", memcmp(key, dumper, APSP_KEY_LEN) == 0);

    CHECK_RC("SGDeemer form (0x600 bytes)",
             apsp_key_from_buffer(dumper, 0x600, key));
    CHECK("...is the 16 bytes at 0x5DC", memcmp(key, dumper + 0x5DC, APSP_KEY_LEN) == 0);

    CHECK("any other length is refused",
          apsp_key_from_buffer(dumper, 0x20, key) == APSP_ERR_SIZE);

    CHECK_RC("the gamekeys.txt hex form",
             apsp_key_from_hex("4A1FF359AEB6EFF81CA8CB23BCA57BB3", key));
    CHECK("...parses to the right bytes", memcmp(key, FIXTURE_KEY, APSP_KEY_LEN) == 0);
    CHECK("lower case parses the same",
          apsp_key_from_hex("4a1ff359aeb6eff81ca8cb23bca57bb3", key) == APSP_OK
          && memcmp(key, FIXTURE_KEY, APSP_KEY_LEN) == 0);
    CHECK("a short hex string is refused",
          apsp_key_from_hex("4A1FF359", key) == APSP_ERR_SIZE);
    CHECK("a non-hex digit is refused",
          apsp_key_from_hex("4A1FF359AEB6EFF81CA8CB23BCA57BZZ", key) == APSP_ERR_ARG);

    CHECK("all zeroes reads as the null key", apsp_key_is_null(zero));
    CHECK("a real key does not", !apsp_key_is_null(FIXTURE_KEY));
}

/*
 * The gamekeys.txt lookup.
 *
 * Shaped like the real file, including the one pair that makes the matching
 * rule load-bearing: apollo-patches holds both NPJJ30022 and NPJJ30022GAME1
 * with DIFFERENT keys, so a first-match rule is right only while nobody
 * reorders the file. Longest-match is not.
 */
static void check_key_db(void)
{
    static const char DB[] =
        ";-----------------------------------------------\n"
        "; Apollo Save Tool - PSP Game Key Database\n"
        ";\n"
        "NPJJ30022GAME1=00112233445566778899AABBCCDDEEFF\r\n"
        "NPJJ30022=3837363534333231302F2E2D2C2B2A29\n"
        "ULUS10391=4A1FF359AEB6EFF81CA8CB23BCA57BB3\n"
        "\n"
        "; a malformed line and a bare word, neither of which may throw it off\n"
        "NOTAKEY=oops\n"
        "garbage\n"
        "ULJM05800=E305CEFAEB46B031859A275BDF32D863";   /* no trailing newline */

    uint8_t key[APSP_KEY_LEN];
    char id[32];

    puts("\nthe game-key database");

    CHECK_RC("an exact directory matches",
             apsp_key_from_db(DB, sizeof DB - 1, "ULUS10391", key, id, sizeof id));
    CHECK("...with the right key", memcmp(key, FIXTURE_KEY, APSP_KEY_LEN) == 0);
    CHECK("...and reports which entry", strcmp(id, "ULUS10391") == 0);

    /* The real reason the lookup is a prefix match: a game whose saves live in
     * a folder named after the title ID plus a suffix. */
    CHECK_RC("a longer directory matches by prefix",
             apsp_key_from_db(DB, sizeof DB - 1, "ULUS10391SAVE00", key, id, sizeof id));
    CHECK("...to the same key", memcmp(key, FIXTURE_KEY, APSP_KEY_LEN) == 0);

    CHECK("matching is case-insensitive",
          apsp_key_from_db(DB, sizeof DB - 1, "ulus10391", key, NULL, 0) == APSP_OK
          && memcmp(key, FIXTURE_KEY, APSP_KEY_LEN) == 0);

    /* The collision. Both entries prefix this directory; the specific one has
     * to win, whichever order they appear in. */
    CHECK_RC("the ambiguous directory resolves",
             apsp_key_from_db(DB, sizeof DB - 1, "NPJJ30022GAME1", key, id, sizeof id));
    CHECK("...to the LONGER entry, not the first one",
          strcmp(id, "NPJJ30022GAME1") == 0 && key[0] == 0x00 && key[15] == 0xFF);

    CHECK_RC("...and the shorter directory still finds the shorter entry",
             apsp_key_from_db(DB, sizeof DB - 1, "NPJJ30022", key, id, sizeof id));
    CHECK("...with its own key",
          strcmp(id, "NPJJ30022") == 0 && key[0] == 0x38);

    /* The last line carries no newline; a reader that needs one loses it. */
    CHECK_RC("an entry on the final unterminated line is found",
             apsp_key_from_db(DB, sizeof DB - 1, "ULJM05800", key, NULL, 0));
    CHECK("...with the right key", key[0] == 0xE3 && key[15] == 0x63);

    CHECK("a directory nothing covers is not found",
          apsp_key_from_db(DB, sizeof DB - 1, "ZZZZ99999", key, id, sizeof id)
          == APSP_ERR_NO_KEY);
    CHECK("...and reports no entry", id[0] == '\0');

    CHECK("a malformed value is not mistaken for a key",
          apsp_key_from_db(DB, sizeof DB - 1, "NOTAKEY", key, NULL, 0) == APSP_ERR_NO_KEY);
    CHECK("an empty database finds nothing",
          apsp_key_from_db("", 0, "ULUS10391", key, NULL, 0) == APSP_ERR_NO_KEY);
}

/*
 * Encrypt, then decrypt, and require the plaintext back. Run for every mode,
 * because the mode selects a different key schedule inside the SD context and
 * a mistake in one is invisible from the others.
 */
static void check_round_trip(void)
{
    uint8_t *plain = malloc(FIXTURE_LEN);
    uint8_t *enc   = malloc(FIXTURE_LEN + APSP_HEADER_LEN);
    uint8_t *back  = malloc(FIXTURE_LEN);
    uint8_t sfo[0x2000];
    int m;

    puts("\nencrypt -> decrypt round trip");
    fixture_data(plain, FIXTURE_LEN);

    for (m = 0; m < NMODES; m++) {
        size_t sfo_len = build_sfo(sfo, sizeof sfo, MODES[m].params0);
        size_t enc_len = 0, dec_len = 0;
        uint8_t key[APSP_KEY_LEN];
        char what[96];
        int rc;

        key_for(m, key);

        snprintf(what, sizeof what, "%s: encrypts", MODES[m].name);
        rc = apsp_encrypt(sfo, sfo_len, FIXTURE_NAME, plain, FIXTURE_LEN, key,
                          enc, FIXTURE_LEN + APSP_HEADER_LEN, &enc_len);
        CHECK_RC(what, rc);
        if (rc != APSP_OK)
            continue;

        snprintf(what, sizeof what, "%s: grows by exactly the IV", MODES[m].name);
        CHECK(what, enc_len == FIXTURE_LEN + APSP_HEADER_LEN);

        snprintf(what, sizeof what, "%s: ciphertext differs from plaintext", MODES[m].name);
        CHECK(what, memcmp(enc + APSP_HEADER_LEN, plain, FIXTURE_LEN) != 0);

        snprintf(what, sizeof what, "%s: decrypts", MODES[m].name);
        rc = apsp_decrypt(sfo, sfo_len, enc, enc_len, key, back, FIXTURE_LEN, &dec_len);
        CHECK_RC(what, rc);
        if (rc != APSP_OK)
            continue;

        snprintf(what, sizeof what, "%s: round trips byte for byte", MODES[m].name);
        CHECK(what, dec_len == FIXTURE_LEN && memcmp(back, plain, FIXTURE_LEN) == 0);
    }

    free(plain);
    free(enc);
    free(back);
}

/* The vectors proper: our output against upstream's. */
static void check_known_answers(void)
{
    uint8_t *plain = malloc(FIXTURE_LEN);
    uint8_t *enc   = malloc(FIXTURE_LEN + APSP_HEADER_LEN);
    uint8_t sfo[0x2000];
    int m, unpinned = 0;

    puts("\nknown answers (against unmodified upstream)");
    fixture_data(plain, FIXTURE_LEN);

    for (m = 0; m < NMODES; m++) {
        size_t sfo_len = build_sfo(sfo, sizeof sfo, MODES[m].params0);
        size_t enc_len = 0;
        uint8_t key[APSP_KEY_LEN];
        uint64_t got_enc, got_sfo;
        char what[96];

        key_for(m, key);
        if (apsp_encrypt(sfo, sfo_len, FIXTURE_NAME, plain, FIXTURE_LEN, key,
                         enc, FIXTURE_LEN + APSP_HEADER_LEN, &enc_len) != APSP_OK) {
            printf("  %-56s %s\n", MODES[m].name, "FAILED (did not encrypt)");
            g_fails++;
            continue;
        }

        got_enc = fnv1a64(enc, enc_len);
        got_sfo = fnv1a64(sfo, sfo_len);

        if (!KNOWN[m].enc && !KNOWN[m].sfo) {
            printf("  %-56s not pinned\n", MODES[m].name);
            printf("        { 0x%016llxULL, 0x%016llxULL },\n",
                   (unsigned long long)got_enc, (unsigned long long)got_sfo);
            unpinned++;
            continue;
        }

        snprintf(what, sizeof what, "%s: ciphertext matches upstream", MODES[m].name);
        CHECK(what, got_enc == KNOWN[m].enc);
        snprintf(what, sizeof what, "%s: rewritten PARAM.SFO matches upstream", MODES[m].name);
        CHECK(what, got_sfo == KNOWN[m].sfo);
    }

    if (unpinned)
        printf("\n  %d mode(s) not pinned -- see REGENERATING at the top of this file\n",
               unpinned);

    free(plain);
    free(enc);
}

/* Resigning must touch PARAM.SFO and nothing else, and must be idempotent. */
static void check_resign(void)
{
    uint8_t sfo[0x2000], before[0x2000], once[0x2000];
    size_t len = build_sfo(sfo, sizeof sfo, 0x41);

    puts("\nresign");
    memcpy(before, sfo, len);

    CHECK_RC("resigns", apsp_resign(sfo, len));
    CHECK("PARAM.SFO changed", memcmp(sfo, before, len) != 0);

    memcpy(once, sfo, len);
    CHECK_RC("resigns again", apsp_resign(sfo, len));
    CHECK("...to the same bytes", memcmp(sfo, once, len) == 0);
}

/* Bad arguments to the crypto: refused, never a crash or a partial write. */
static void check_crypto_args(void)
{
    uint8_t sfo[0x2000];
    size_t len = build_sfo(sfo, sizeof sfo, 0x41);
    uint8_t in[0x100], out[0x200];
    uint8_t key[APSP_KEY_LEN];

    puts("\ncrypto arguments");
    memset(in, 0x5A, sizeof in);
    memcpy(key, FIXTURE_KEY, sizeof key);

    CHECK("decrypting a NULL input is refused",
          apsp_decrypt(sfo, len, NULL, 0x100, key, out, sizeof out, NULL) == APSP_ERR_ARG);
    CHECK("a file shorter than the IV is refused",
          apsp_decrypt(sfo, len, in, APSP_HEADER_LEN, key, out, sizeof out, NULL) == APSP_ERR_SIZE);
    CHECK("a too-small decrypt output buffer is refused",
          apsp_decrypt(sfo, len, in, sizeof in, key, out, 4, NULL) == APSP_ERR_SIZE);
    CHECK("a too-small encrypt output buffer is refused",
          apsp_encrypt(sfo, len, FIXTURE_NAME, in, sizeof in, key, out, 4, NULL) == APSP_ERR_SIZE);
    CHECK("encrypting a file PARAM.SFO does not list is refused",
          apsp_encrypt(sfo, len, "NOPE.BIN", in, sizeof in, key, out, sizeof out, NULL)
          == APSP_ERR_NO_FILE);

    /* A mode byte naming none of 1/3/5 matches no branch downstream, so it has
     * to be refused rather than carried through as a value. */
    {
        uint8_t odd[0x2000];
        size_t olen = build_sfo(odd, sizeof odd, 0x10);

        CHECK("an unimplemented savedata mode is refused",
              apsp_decrypt(odd, olen, in, sizeof in, key, out, sizeof out, NULL)
              == APSP_ERR_MODE);
    }
}

/* ---- fixtures, for the oracle ------------------------------------------- */

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");

    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return 0;
    }
    fwrite(data, 1, len, f);
    fclose(f);
    return 1;
}

static int dump_fixtures(const char *dir)
{
    uint8_t *plain = malloc(FIXTURE_LEN);
    uint8_t sfo[0x2000];
    char path[512];
    int m;

    fixture_data(plain, FIXTURE_LEN);

    for (m = 0; m < NMODES; m++) {
        size_t len = build_sfo(sfo, sizeof sfo, MODES[m].params0);
        uint8_t key[APSP_KEY_LEN];
        int i;

        snprintf(path, sizeof path, "%s/mode%02X", dir, MODES[m].params0);
        /* The oracle wants a real directory: upstream's encrypt builds
         * "<fpath>PARAM.SFO" and reads the data file beside it. EEXIST is the
         * expected answer on a re-run. */
        if (MKDIR(path) != 0 && errno != EEXIST) {
            fprintf(stderr, "cannot create %s\n", path);
            free(plain);
            return 1;
        }

        {
            char f[600];
            snprintf(f, sizeof f, "%s/PARAM.SFO", path);
            if (!write_file(f, sfo, len)) return 1;
            snprintf(f, sizeof f, "%s/%s", path, FIXTURE_NAME);
            if (!write_file(f, plain, FIXTURE_LEN)) return 1;

            /* The key beside the data, in SGKeyDumper's own 0x10-byte form,
             * so the oracle can be pointed at it without retyping hex. */
            key_for(m, key);
            snprintf(f, sizeof f, "%s/GAMEKEY.BIN", path);
            if (!write_file(f, key, sizeof key)) return 1;
        }

        printf("%s/  PARAM.SFO  %s (%d bytes)  GAMEKEY.BIN  key=", path,
               FIXTURE_NAME, FIXTURE_LEN);
        for (i = 0; i < APSP_KEY_LEN; i++)
            printf("%02X", key[i]);
        printf("\n");
    }

    printf("\nRun the unmodified upstream encrypt over each, then take\n"
           "fnv1a64() of the resulting %s and PARAM.SFO.\n", FIXTURE_NAME);
    free(plain);
    return 0;
}

/* ---- a real save -------------------------------------------------------- */

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) {
        fprintf(stderr, "cannot read %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }

    buf = malloc((size_t)n ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf)
        *len = (size_t)n;
    return buf;
}

/*
 * Decrypt a real console save, then put it back and require the plaintext to
 * survive. This is the check that cannot be faked by a self-consistent bug:
 * the input was written by a PSP, not by this code.
 *
 * The ciphertext will NOT match the original -- the IV differs (see
 * psp_savedata.c) -- so what is asserted is the plaintext, both ways round.
 */
static int run_real_save(const char *dir, const char *name, const char *hexkey)
{
    char path[1024];
    uint8_t *sfo = NULL, *enc = NULL, *plain = NULL, *again = NULL, *back = NULL;
    size_t sfo_len = 0, enc_len = 0, plain_len = 0, again_len = 0, back_len = 0;
    uint8_t key[APSP_KEY_LEN];
    char dirname[64];
    int rc, ret = 1;

    snprintf(path, sizeof path, "%s/PARAM.SFO", dir);
    if (!(sfo = slurp(path, &sfo_len))) goto done;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!(enc = slurp(path, &enc_len))) goto done;

    if (!hexkey || strcmp(hexkey, "-") == 0)
        memset(key, 0, sizeof key);
    else if ((rc = apsp_key_from_hex(hexkey, key)) != APSP_OK) {
        fprintf(stderr, "bad key: %s\n", apsp_strerror(rc));
        goto done;
    }

    printf("\nreal save: %s/%s (%zu bytes), PARAM.SFO %zu bytes\n",
           dir, name, enc_len, sfo_len);

    CHECK_RC("PARAM.SFO validates", apsp_sfo_valid(sfo, sfo_len));
    if (apsp_sfo_directory(sfo, sfo_len, dirname, sizeof dirname) == APSP_OK)
        printf("  SAVEDATA_DIRECTORY: %s\n", dirname);
    printf("  files listed: %d\n", apsp_sfo_file_count(sfo, sfo_len));

    plain_len = apsp_decrypted_size(enc_len);
    plain = malloc(plain_len);
    CHECK_RC("decrypts", apsp_decrypt(sfo, sfo_len, enc, enc_len, key,
                                      plain, plain_len, &plain_len));

    /* Re-encrypting rewrites PARAM.SFO, so work on a copy -- the decrypt
     * above and the one below have to see the same mode byte. */
    again_len = apsp_encrypted_size(plain_len);
    again = malloc(again_len);
    {
        uint8_t *sfo2 = malloc(sfo_len);
        memcpy(sfo2, sfo, sfo_len);
        CHECK_RC("re-encrypts", apsp_encrypt(sfo2, sfo_len, name, plain, plain_len,
                                             key, again, again_len, &again_len));
        CHECK("PARAM.SFO was rewritten", memcmp(sfo2, sfo, sfo_len) != 0);

        back_len = apsp_decrypted_size(again_len);
        back = malloc(back_len);
        CHECK_RC("decrypts again", apsp_decrypt(sfo2, sfo_len, again, again_len, key,
                                                back, back_len, &back_len));
        free(sfo2);
    }

    CHECK("the plaintext survives the round trip",
          back_len == plain_len && memcmp(back, plain, plain_len) == 0);

    /* Hand the plaintext over so it can be checked against a reference
     * decrypter -- that is what turns this from a round trip into a proof. */
    snprintf(path, sizeof path, "%s/%s.dec", dir, name);
    if (write_file(path, plain, plain_len))
        printf("  wrote %s (%zu bytes)\n", path, plain_len);

    ret = 0;
done:
    free(sfo); free(enc); free(plain); free(again); free(back);
    return ret;
}

/*
 * The game-key database, against the real file rather than the fixture above.
 *
 * Worth a mode of its own because this is the one piece of the desktop app's
 * PSP support that is not UI: it reads PSP/gamekeys.txt out of the bundle and
 * calls exactly this. A prefix rule is easy to get subtly wrong against 280
 * real entries in a way no fixture catches.
 */
static int run_keydb(const char *path, int argc, char **argv)
{
    size_t len = 0;
    uint8_t *text = slurp(path, &len);
    int i;

    if (!text)
        return 1;

    printf("%s: %zu bytes\n", path, len);
    for (i = 0; i < argc; i++) {
        uint8_t key[APSP_KEY_LEN];
        char entry[64];
        int rc = apsp_key_from_db((const char *)text, len, argv[i],
                                  key, entry, sizeof entry);

        printf("  %-20s ", argv[i]);
        if (rc != APSP_OK) {
            printf("%s\n", apsp_strerror(rc));
            continue;
        }
        printf("%-16s ", entry);
        for (int b = 0; b < APSP_KEY_LEN; b++)
            printf("%02X", key[b]);
        putchar('\n');
    }

    free(text);
    return 0;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "--dump-fixtures") == 0)
        return dump_fixtures(argv[2]);

    if (argc > 2 && strcmp(argv[1], "--keydb") == 0)
        return run_keydb(argv[2], argc - 3, argv + 3);

    if (argc > 3 && strcmp(argv[1], "--save") == 0) {
        int rc = run_real_save(argv[2], argv[3], argc > 4 ? argv[4] : "-");
        printf("\nreal-save checks: %s\n", g_fails ? "FAILED" : "all passed");
        return rc || g_fails ? 1 : 0;
    }

    printf("PSP savedata checks (fuse %016llX)\n",
           (unsigned long long)apsp_get_fuse_id());

    check_sfo_accessors();
    check_sfo_bounds();
    check_keys();
    check_key_db();
    check_crypto_args();
    check_round_trip();
    check_resign();
    check_known_answers();

    printf("\nPSP checks: %s\n", g_fails ? "FAILED" : "all passed");
    return g_fails ? 1 : 0;
}
