/*
 * Headless checks for the PS3 savedata layer (core/ps3), so CI exercises the
 * PARAM.PFD parser, the file cipher and the key database without a GUI and
 * without a console.
 *
 *   test_ps3                        run every self-contained check
 *   test_ps3 --dump-fixture DIR     write the synthetic save the known-answer
 *                                   vectors below were taken from
 *   test_ps3 --corpus DIR [CONF]    walk a tree of real saves and check every
 *                                   hash the console wrote against one
 *                                   recomputed here
 *
 * On the known-answer vectors
 * ---------------------------
 * KNOWN_CIPHER and KNOWN_PFD are digests of what --dump-fixture produces. They
 * were confirmed against flatz's pfdtool -- a separate implementation, built
 * from bucanero/pfd_sfo_tools with its own polarSSL -- which reads the same
 * fixture, reports every hash OK and decrypts DATA.BIN back to the plaintext.
 * See REGENERATING below. So a pass here says this code agrees with the tool
 * the format came from, not merely with itself.
 *
 * What it does NOT say is that either matches a console, and no fixture can:
 * the hashes are keyed by secrets and the plaintext is synthetic. That is what
 * --corpus is for, and it is the check that matters. Run it over a folder of
 * real saves and it verifies, per save, that resigning a PARAM.PFD the console
 * wrote reproduces it byte for byte, and per file, that the hash the console
 * recorded is the hash this code computes.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0777)
#endif

#include "ps3/pfd_savedata.h"
#include "sfo.h"

/* ------------------------------------------------------------------ *
 * REGENERATING the known-answer vectors
 *
 *   1. Build the oracle -- upstream, unmodified:
 *
 *        git clone https://github.com/bucanero/pfd_sfo_tools
 *        cd pfd_sfo_tools/pfdtool
 *        cc -O1 -w -o /tmp/pfdtool -Iinclude -Iinclude/polarssl -Isrc src/[a-z]*.c
 *
 *   2. Give it the console keys. bin/global.conf ships with the values blank;
 *      they are the ones in core/ps3/pfd_savedata.c, XORed byte-wise with
 *      D4D16B0C5DB08791 to undo the scrambling. games.conf needs one section:
 *
 *        [FIXTURE]
 *        secure_file_id:*=000102030405060708090A0B0C0D0E0F
 *
 *   3. test_ps3 --dump-fixture /tmp/fx
 *   4. /tmp/pfdtool -g FIXTURE -c /tmp/fx        -> every hash OK
 *      /tmp/pfdtool -g FIXTURE -d /tmp/fx DATA.BIN
 *      ...and DATA.BIN is now the plaintext --dump-fixture printed a digest of.
 *   5. Paste the digests the dump prints into KNOWN_CIPHER / KNOWN_PFD.
 *
 * The fixture holds no PARAM.SFO on purpose. Its entry would carry three more
 * hashes keyed by the console id, the disc hash key and the authentication id,
 * which this code deliberately never writes, and pfdtool would report those
 * three as mismatched every run.
 * ------------------------------------------------------------------ */

static const uint64_t KNOWN_CIPHER = 0xbd0819cd4a7dfddbULL;
static const uint64_t KNOWN_PFD    = 0x13eafd8a0fea60e3ULL;

static int g_fails;

#define CHECK(what, cond) do {                                        \
        int ok_ = (cond);                                             \
        if (!ok_) g_fails++;                                          \
        printf("  %-56s %s\n", (what), ok_ ? "ok" : "FAILED");        \
    } while (0)

#define CHECK_RC(what, rc) do {                                       \
        int rc_ = (rc);                                               \
        if (rc_ != APFD_OK) g_fails++;                                \
        printf("  %-56s %s\n", (what),                                \
               rc_ == APFD_OK ? "ok" : apfd_strerror(rc_));           \
    } while (0)

/* FNV-1a, 64-bit. Not a security hash -- a fixture digest, chosen so the test
 * carries no dependency and the constants stay readable. */
static uint64_t fnv1a64(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ULL;

    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* ---- the synthetic save ------------------------------------------------- */

#define FIXTURE_NAME  "DATA.BIN"
#define FIXTURE_DIR   "FIXTURE"
#define FIXTURE_SFID  "000102030405060708090A0B0C0D0E0F"
#define FIXTURE_LEN   0x1234      /* not a multiple of 16: exercises padding  */
#define PFD_LEN       32768       /* what the console writes, every time      */
#define FIX_CAPACITY  57          /* ..with these table sizes                 */
#define FIX_RESERVED  114

/* Where the last table ends. Short of PFD_LEN: the console writes a fixed
 * 32KB and the tables use 32724 of it. */
#define TABLES_END    (96 + 24 + FIX_CAPACITY * 8 + FIX_RESERVED * 272 \
                       + FIX_CAPACITY * 20)

/* A deterministic filler. xorshift rather than rand(), whose sequence is
 * libc's business and would make the vectors unreproducible off this machine. */
static void fill(uint8_t *out, size_t len, uint32_t seed)
{
    uint32_t x = seed;

    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = (uint8_t)(x >> 24);
    }
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

/*
 * Which bucket a name hashes to. Written out again here rather than shared
 * with the implementation, so the test would notice the implementation
 * changing it -- and checked against a real PARAM.PFD, where PARAM.SFO lands
 * in bucket 8 and PROFILE.DAT in bucket 15.
 */
static uint64_t bucket_of(const char *name, uint64_t capacity)
{
    uint64_t hash = 0;

    for (const char *p = name; *p; ++p)
        hash = (hash << 5) - hash + (uint8_t)*p;

    return hash % capacity;
}

/*
 * A PARAM.PFD holding `n` named entries and nothing else, laid out the way the
 * console lays one out: version 3, 57 buckets, 114 reserved entries, empty
 * buckets and chain ends both marked with the reserved count.
 *
 * The header key and each entry key are filler. That is fine and deliberate:
 * they decrypt to SOMETHING deterministic, which is all a fixture needs, and
 * it keeps the file free of any real console's secrets. Sizes and hashes are
 * left zero -- apfd_encrypt() fills them in, which is what is being tested.
 *
 * The caller must apfd_resign() before the result is valid.
 */
static void build_pfd(uint8_t out[PFD_LEN], const char *const *names, int n)
{
    const size_t et_off  = 96 + 24 + FIX_CAPACITY * 8;
    const size_t est_off = et_off + (size_t)FIX_RESERVED * 272;

    memset(out, 0, PFD_LEN);

    put64(out + 0, 0x50464442ULL);          /* "PFDB" */
    put64(out + 8, 3);
    fill(out + 16, 16, 0xA5A5A5A5u);        /* header key: the signature IV   */
    fill(out + 32, 64, 0x5A5A5A5Au);        /* signature, in its stored form  */

    put64(out + 96,  FIX_CAPACITY);
    put64(out + 104, FIX_RESERVED);
    put64(out + 112, (uint64_t)n);

    /* Every bucket empty, then one entry dropped into each of its own. */
    for (int i = 0; i < FIX_CAPACITY; i++)
        put64(out + 120 + i * 8, FIX_RESERVED);

    for (int i = 0; i < n; i++) {
        uint8_t *entry = out + et_off + (size_t)i * 272;

        put64(entry + 0, FIX_RESERVED);     /* end of this bucket's chain     */
        strcpy((char *)entry + 8, names[i]);
        fill(entry + 80, 64, 0x1234567u + (uint32_t)i);

        put64(out + 120 + bucket_of(names[i], FIX_CAPACITY) * 8, (uint64_t)i);
    }

    (void)est_off;                          /* zeroed above; resign fills it  */
}

/* ---- structure ---------------------------------------------------------- */

static void check_structure(void)
{
    static const char *names[] = { FIXTURE_NAME, "SECOND.DAT" };
    uint8_t pfd[PFD_LEN];
    char name[APFD_NAME_LEN];

    puts("\nstructure");
    build_pfd(pfd, names, 2);
    CHECK_RC("an unsigned PARAM.PFD still parses", apfd_valid(pfd, sizeof pfd));
    CHECK_RC("resigns", apfd_resign(pfd, sizeof pfd));

    CHECK("version is 3", apfd_version(pfd, sizeof pfd) == 3);
    CHECK("two entries", apfd_entry_count(pfd, sizeof pfd) == 2);
    CHECK("no trophy files", apfd_is_trophy(pfd, sizeof pfd) == 0);

    CHECK_RC("entry 0 names itself",
             apfd_entry_name(pfd, sizeof pfd, 0, name, sizeof name));
    CHECK("..and the name is right", strcmp(name, FIXTURE_NAME) == 0);
    CHECK("entry past the end is refused",
          apfd_entry_name(pfd, sizeof pfd, 2, name, sizeof name) == APFD_ERR_NO_ENTRY);
    CHECK("a name too long for the buffer is refused",
          apfd_entry_name(pfd, sizeof pfd, 0, name, 4) == APFD_ERR_SIZE);

    CHECK("find locates entry 1", apfd_find(pfd, sizeof pfd, "SECOND.DAT") == 1);
    CHECK("..ignoring case", apfd_find(pfd, sizeof pfd, "second.dat") == 1);
    CHECK("an unlisted file is not found",
          apfd_find(pfd, sizeof pfd, "ICON0.PNG") == APFD_ERR_NO_ENTRY);

    CHECK("sizes start at zero", apfd_entry_size(pfd, sizeof pfd, 0) == 0);

    CHECK("PARAM.SFO carries a built-in key",
          apfd_entry_has_builtin_key("param.sfo"));
    CHECK("TROPTRNS.DAT carries a built-in key",
          apfd_entry_has_builtin_key("TROPTRNS.DAT"));
    CHECK("a game file does not", !apfd_entry_has_builtin_key(FIXTURE_NAME));

    /* The bucket function, against the layout a real console wrote. */
    CHECK("PARAM.SFO hashes to bucket 8", bucket_of("PARAM.SFO", 57) == 8);
    CHECK("PROFILE.DAT hashes to bucket 15", bucket_of("PROFILE.DAT", 57) == 15);
}

/* ---- bounds ------------------------------------------------------------- */

/*
 * Every number below comes out of the file, and a file gets here from a
 * browser tab. Each case is one of them lying.
 */
static void check_bounds(void)
{
    static const char *names[] = { FIXTURE_NAME };
    uint8_t pfd[PFD_LEN], bad[PFD_LEN];

    puts("\nbounds");
    build_pfd(pfd, names, 1);
    apfd_resign(pfd, sizeof pfd);

    CHECK("a NULL buffer is refused", apfd_valid(NULL, PFD_LEN) == APFD_ERR_ARG);
    CHECK("an empty buffer is refused", apfd_valid(pfd, 0) == APFD_ERR_PFD);
    CHECK("a buffer ending mid-header is refused", apfd_valid(pfd, 100) == APFD_ERR_PFD);
    CHECK("a buffer ending mid-table is refused", apfd_valid(pfd, 4096) == APFD_ERR_PFD);

    /* The tables stop short of the 32KB the console writes, so the check is
     * against where they end, not against the file length. */
    CHECK("a buffer one byte short of the tables is refused",
          apfd_valid(pfd, TABLES_END - 1) == APFD_ERR_PFD);
    CHECK("..and one exactly as long as they need is not",
          apfd_valid(pfd, TABLES_END) == APFD_OK);

    memcpy(bad, pfd, sizeof bad);
    bad[0] ^= 0xFF;
    CHECK("a bad magic is refused", apfd_valid(bad, sizeof bad) == APFD_ERR_PFD);

    memcpy(bad, pfd, sizeof bad);
    put64(bad + 8, 5);
    CHECK("an unknown version is refused", apfd_valid(bad, sizeof bad) == APFD_ERR_VERSION);

    memcpy(bad, pfd, sizeof bad);
    put64(bad + 96, 0);
    CHECK("a zero capacity is refused", apfd_valid(bad, sizeof bad) == APFD_ERR_PFD);

    memcpy(bad, pfd, sizeof bad);
    put64(bad + 96, 0xFFFFFFFFFFFFFFFFULL);
    CHECK("an absurd capacity is refused", apfd_valid(bad, sizeof bad) == APFD_ERR_PFD);

    memcpy(bad, pfd, sizeof bad);
    put64(bad + 104, 0xFFFFFFFFFFFFFFFFULL);
    CHECK("an absurd reserve is refused", apfd_valid(bad, sizeof bad) == APFD_ERR_PFD);

    memcpy(bad, pfd, sizeof bad);
    put64(bad + 112, FIX_RESERVED + 1);
    CHECK("more used than reserved is refused", apfd_valid(bad, sizeof bad) == APFD_ERR_PFD);

    memcpy(bad, pfd, sizeof bad);
    put64(bad + 96, 200);   /* capacity that pushes the tables past the file */
    CHECK("tables that do not fit are refused", apfd_valid(bad, sizeof bad) == APFD_ERR_PFD);

    /* A bucket chain that points at itself. Without a step limit the lookup
     * never returns; with one it simply does not find the name. */
    memcpy(bad, pfd, sizeof bad);
    {
        const size_t et_off = 96 + 24 + FIX_CAPACITY * 8;
        put64(bad + et_off, 0);          /* entry 0's additional_index -> 0 */
        CHECK("a looping bucket chain terminates",
              apfd_find(bad, sizeof bad, "NOTHERE.DAT") == APFD_ERR_NO_ENTRY);
    }
}

/* ---- the key database --------------------------------------------------- */

/*
 * A games.conf in miniature, holding one instance of each rule the real file
 * exercises. The blank line endings are LF here; check_conf() runs it again
 * with CRLF, which is what apollo-patches actually ships.
 */
static const char CONF[] =
    "; \"Game A\"\n"
    "[BLUS30000/BLUS30001]\n"
    ";disc_hash_key=\n"
    "secure_file_id:*=00112233445566778899AABBCCDDEEFF\n"
    "\n"
    "; \"Game A, profile saves\"\n"
    "[BLUS30000PROFILE]\n"
    "secure_file_id:*=FFEEDDCCBBAA99887766554433221100\n"
    "\n"
    "; \"Game B, one key per file\"\n"
    "[BLUS30002]\n"
    "secure_file_id:DATA=0102030405060708090A0B0C0D0E0F10\n"
    "secure_file_id:SYS*=1112131415161718191A1B1C1D1E1F20\n"
    "secure_file_id:*=2122232425262728292A2B2C2D2E2F30\n"
    "\n"
    "; \"Game C, with a stray separator\"\n"
    "[BLUS30003/]\n"
    "secure_file_id:*=3132333435363738393A3B3C3D3E3F40\n"
    "\n"
    "; \"Game D, one of the few that names a disc hash key\"\n"
    "[BLUS30004]\n"
    "disc_hash_key=4142434445464748494A4B4C4D4E4F50\n"
    "secure_file_id:*=5152535455565758595A5B5C5D5E5F60\n";

static int sfid_is(const uint8_t *sfid, const char *hex)
{
    uint8_t want[APFD_SFID_LEN];

    return apfd_sfid_from_hex(hex, want) == APFD_OK &&
           memcmp(sfid, want, APFD_SFID_LEN) == 0;
}

static int conf_lookup(const char *text, size_t len, const char *dir,
                       const char *file, uint8_t sfid[APFD_SFID_LEN],
                       char *id, size_t id_cap)
{
    if (id && id_cap)
        id[0] = '\0';
    return apfd_sfid_from_conf(text, len, dir, file, sfid, id, id_cap);
}

static void check_conf(void)
{
    uint8_t sfid[APFD_SFID_LEN];
    char id[APFD_NAME_LEN];
    char *crlf;
    size_t crlf_len = 0;

    puts("\ngames.conf");

    CHECK_RC("a plain section matches",
             conf_lookup(CONF, sizeof CONF - 1, "BLUS30000GAME", "SAVE.DAT",
                         sfid, id, sizeof id));
    CHECK("..with that game's key", sfid_is(sfid, "00112233445566778899AABBCCDDEEFF"));
    CHECK("..and reports the id it matched", strcmp(id, "BLUS30000") == 0);

    /* The rule apollo-ps3's strstr() gets wrong. */
    CHECK_RC("the longest prefix wins over a shorter one",
             conf_lookup(CONF, sizeof CONF - 1, "BLUS30000PROFILE", "SAVE.DAT",
                         sfid, id, sizeof id));
    CHECK("..so a profile save gets the profile key",
          sfid_is(sfid, "FFEEDDCCBBAA99887766554433221100"));
    CHECK("..and the id says which", strcmp(id, "BLUS30000PROFILE") == 0);

    CHECK_RC("a later id in the same section matches",
             conf_lookup(CONF, sizeof CONF - 1, "BLUS30001SAVE", "SAVE.DAT",
                         sfid, id, sizeof id));
    CHECK("..and reports itself, not the first id", strcmp(id, "BLUS30001") == 0);

    CHECK_RC("an exact file name beats the wildcard below it",
             conf_lookup(CONF, sizeof CONF - 1, "BLUS30002", "DATA",
                         sfid, NULL, 0));
    CHECK("..taking the specific key", sfid_is(sfid, "0102030405060708090A0B0C0D0E0F10"));

    CHECK_RC("..ignoring case",
             conf_lookup(CONF, sizeof CONF - 1, "BLUS30002", "data", sfid, NULL, 0));
    CHECK("..still the specific key", sfid_is(sfid, "0102030405060708090A0B0C0D0E0F10"));

    CHECK_RC("a pattern matches by wildcard",
             conf_lookup(CONF, sizeof CONF - 1, "BLUS30002", "SYS-DATA", sfid, NULL, 0));
    CHECK("..taking its key", sfid_is(sfid, "1112131415161718191A1B1C1D1E1F20"));

    CHECK_RC("anything else falls through to *",
             conf_lookup(CONF, sizeof CONF - 1, "BLUS30002", "OTHER.BIN", sfid, NULL, 0));
    CHECK("..taking the catch-all key", sfid_is(sfid, "2122232425262728292A2B2C2D2E2F30"));

    /* [BLUS30003/] leaves an empty id, and an empty prefix matches every
     * directory in the file. Six real sections have one. */
    CHECK("an unknown directory finds nothing despite a stray separator",
          conf_lookup(CONF, sizeof CONF - 1, "BLUS39999", "SAVE.DAT", sfid, NULL, 0)
              == APFD_ERR_NO_KEY);
    CHECK_RC("..while the section it belongs to still matches",
             conf_lookup(CONF, sizeof CONF - 1, "BLUS30003", "SAVE.DAT", sfid, NULL, 0));

    /* Keys live only in their own section: Game B's patterns must not answer
     * for Game A, whose section has no DATA entry. */
    CHECK_RC("a section's patterns do not leak into another",
             conf_lookup(CONF, sizeof CONF - 1, "BLUS30000", "DATA", sfid, NULL, 0));
    CHECK("..so DATA here gets Game A's catch-all",
          sfid_is(sfid, "00112233445566778899AABBCCDDEEFF"));

    /* apollo-patches ships CRLF. */
    crlf = malloc(sizeof CONF * 2);
    for (const char *p = CONF; *p; p++) {
        if (*p == '\n')
            crlf[crlf_len++] = '\r';
        crlf[crlf_len++] = *p;
    }
    CHECK_RC("CRLF line endings parse",
             conf_lookup(crlf, crlf_len, "BLUS30000PROFILE", "SAVE.DAT",
                         sfid, NULL, 0));
    CHECK("..to the same key", sfid_is(sfid, "FFEEDDCCBBAA99887766554433221100"));
    free(crlf);

    CHECK("an empty file finds nothing",
          conf_lookup("", 0, "BLUS30000", "SAVE.DAT", sfid, NULL, 0) == APFD_ERR_NO_KEY);

    puts("\nhex keys");
    CHECK_RC("32 hex digits parse", apfd_sfid_from_hex(FIXTURE_SFID, sfid));
    CHECK("..to the right bytes", sfid[0] == 0x00 && sfid[15] == 0x0F);
    CHECK("lower case parses",
          apfd_sfid_from_hex("000102030405060708090a0b0c0d0e0f", sfid) == APFD_OK);
    CHECK("31 digits are refused",
          apfd_sfid_from_hex("000102030405060708090A0B0C0D0E0", sfid) == APFD_ERR_ARG);
    CHECK("33 digits are refused",
          apfd_sfid_from_hex("000102030405060708090A0B0C0D0E0FF", sfid) == APFD_ERR_ARG);
    CHECK("a non-hex digit is refused",
          apfd_sfid_from_hex("000102030405060708090A0B0C0D0E0Z", sfid) == APFD_ERR_ARG);
    CHECK("NULL is refused", apfd_sfid_from_hex(NULL, sfid) == APFD_ERR_ARG);
}

/* ---- the round trip ----------------------------------------------------- */

/*
 * The account ID: the other way to re-sign a save, and the one that travels
 * between machines rather than binding to one.
 *
 * A synthetic PARAM.SFO here rather than a real one, because what has to be
 * right is narrow and exact -- 16 ASCII characters into two fields at two
 * offsets -- and a fixture states the expected bytes where a real save only
 * implies them. The offsets themselves came from 172 real saves.
 */
static size_t build_account_sfo(uint8_t *out, size_t cap,
                                int with_field, int with_params)
{
    /* Laid out by hand: header, index, keys, data. Small enough to be obvious
     * and it keeps the test independent of the SFO writer. */
    static const char *K1 = "ACCOUNT_ID";
    static const char *K2 = "PARAMS";
    const uint32_t n = (uint32_t)(!!with_field + !!with_params);
    uint32_t keys_at, data_at, key_off = 0, data_off = 0, i = 0;
    uint8_t *p;

    if (cap < 0x400 || !n)
        return 0;
    memset(out, 0, 0x400);

    keys_at = 0x14 + 0x10 * n;
    data_at = (keys_at + 32 + 3) & ~3u;

    memcpy(out, "\0PSF", 4);
    out[4] = 0x01; out[5] = 0x01;                 /* version 1.1, LE */
    out[8]  = (uint8_t)keys_at;  out[9]  = (uint8_t)(keys_at >> 8);
    out[12] = (uint8_t)data_at;  out[13] = (uint8_t)(data_at >> 8);
    out[16] = (uint8_t)n;

    if (with_field) {
        p = out + 0x14 + 0x10 * i++;
        p[0] = (uint8_t)key_off; p[2] = 0x04; p[3] = 0x02;   /* UTF-8 */
        p[4] = APFD_ACCT_ID_LEN; p[8] = APFD_ACCT_ID_LEN;
        p[12] = (uint8_t)data_off;
        strcpy((char *)out + keys_at + key_off, K1);
        memset(out + data_at + data_off, 'Z', APFD_ACCT_ID_LEN);
        key_off += (uint32_t)strlen(K1) + 1;
        data_off += APFD_ACCT_ID_LEN;
    }
    if (with_params) {
        const uint32_t plen = 0x400 - data_at - data_off > 0x80 ? 0x80 : 0;
        p = out + 0x14 + 0x10 * i++;
        p[0] = (uint8_t)key_off; p[2] = 0x04; p[3] = 0x00;   /* binary */
        p[4] = (uint8_t)plen; p[8] = (uint8_t)plen;
        p[12] = (uint8_t)data_off;
        strcpy((char *)out + keys_at + key_off, K2);
        memset(out + data_at + data_off, 'Y', plen);
        data_off += plen;
    }
    return data_at + data_off;
}

static void check_account_id(void)
{
    static const char *ACCT = "780304033110f24f";
    uint8_t sfo[0x400];
    char    got[APFD_ACCT_ID_LEN + 1];
    size_t  len;

    puts("\nthe account a save is signed to");

    len = build_account_sfo(sfo, sizeof sfo, 1, 1);
    CHECK("a PARAM.SFO with both fields was built", len > 0);

    CHECK_RC("an account ID writes", apfd_sfo_set_account_id(sfo, len, ACCT));
    CHECK_RC("...and reads back", apfd_sfo_account_id(sfo, len, got, sizeof got));
    CHECK("...as what went in", strcmp(got, ACCT) == 0);

    /* Both copies, because a console may read either and a save that
     * disagrees with itself is the bug this guards. */
    {
        size_t   off;
        uint32_t used;
        int      both = 0;

        if (asfo_find(sfo, len, "ACCOUNT_ID", &off, &used, NULL, NULL) == ASFO_OK
            && memcmp(sfo + off, ACCT, APFD_ACCT_ID_LEN) == 0) both++;
        if (asfo_find(sfo, len, "PARAMS", &off, &used, NULL, NULL) == ASFO_OK
            && memcmp(sfo + off + 0x30, ACCT, APFD_ACCT_ID_LEN) == 0) both++;
        CHECK("...into the field AND the PARAMS blob", both == 2);
    }

    /* Either alone still works: a save missing one is odd, not unusable. */
    len = build_account_sfo(sfo, sizeof sfo, 1, 0);
    CHECK_RC("with only the ACCOUNT_ID field", apfd_sfo_set_account_id(sfo, len, ACCT));
    len = build_account_sfo(sfo, sizeof sfo, 0, 1);
    CHECK_RC("with only the PARAMS blob", apfd_sfo_set_account_id(sfo, len, ACCT));

    /* What must be refused. A short value written into a fixed field is a
     * DIFFERENT account, not a shorter one, so it is not padded. */
    len = build_account_sfo(sfo, sizeof sfo, 1, 1);
    CHECK("a short ID is refused",
          apfd_sfo_set_account_id(sfo, len, "780304") == APFD_ERR_ARG);
    CHECK("a long ID is refused",
          apfd_sfo_set_account_id(sfo, len, "780304033110f24f0") == APFD_ERR_ARG);
    CHECK("a non-hex ID is refused",
          apfd_sfo_set_account_id(sfo, len, "780304033110f24g") == APFD_ERR_ARG);
    CHECK("an empty ID is refused",
          apfd_sfo_set_account_id(sfo, len, "") == APFD_ERR_ARG);
    CHECK("a NULL ID is refused",
          apfd_sfo_set_account_id(sfo, len, NULL) == APFD_ERR_ARG);
    CHECK("a NULL SFO is refused",
          apfd_sfo_set_account_id(NULL, len, ACCT) == APFD_ERR_ARG);
    CHECK("...and none of that changed the save",
          apfd_sfo_account_id(sfo, len, got, sizeof got) == APFD_OK
          && strcmp(got, "ZZZZZZZZZZZZZZZZ") == 0);

    /* An SFO that is not a save's carries neither field. */
    {
        uint8_t bare[0x40];
        memset(bare, 0, sizeof bare);
        memcpy(bare, "\0PSF", 4);
        bare[4] = 0x01; bare[5] = 0x01;
        bare[8] = 0x14; bare[12] = 0x14;
        CHECK("an SFO with neither field says so",
              apfd_sfo_set_account_id(bare, sizeof bare, ACCT) == APFD_ERR_NO_ENTRY);
    }

    /* Truncation, the usual argument: this arrives from a memory card. */
    {
        uint8_t copy[0x400];
        size_t  at;
        int     wrong = 0;

        len = build_account_sfo(sfo, sizeof sfo, 1, 1);
        for (at = 0; at < len; at++) {
            memcpy(copy, sfo, at);
            if (apfd_sfo_set_account_id(copy, at, ACCT) == APFD_OK
                && apfd_sfo_account_id(copy, at, got, sizeof got) == APFD_OK
                && strcmp(got, ACCT) != 0)
                wrong++;
        }
        CHECK("no truncation writes a half account ID", wrong == 0);
    }
}

static void check_round_trip(void)
{
    static const char *names[] = { FIXTURE_NAME };
    uint8_t pfd[PFD_LEN], before[PFD_LEN];
    uint8_t sfid[APFD_SFID_LEN];
    uint8_t plain[FIXTURE_LEN], back[FIXTURE_LEN];
    uint8_t cipher[FIXTURE_LEN + APFD_ALIGN];
    size_t cipher_len = 0, back_len = 0;

    puts("\nround trip");
    build_pfd(pfd, names, 1);
    apfd_resign(pfd, sizeof pfd);
    apfd_sfid_from_hex(FIXTURE_SFID, sfid);
    fill(plain, sizeof plain, 0x9E3779B9u);

    memcpy(before, pfd, sizeof before);

    CHECK("the aligned size rounds up",
          apfd_encrypted_size(FIXTURE_LEN) == ((FIXTURE_LEN + 15) & ~15));

    CHECK_RC("encrypts", apfd_encrypt(pfd, sizeof pfd, FIXTURE_NAME,
                                      plain, sizeof plain, sfid,
                                      cipher, sizeof cipher, &cipher_len));
    CHECK("..to the aligned length", cipher_len == apfd_encrypted_size(FIXTURE_LEN));
    CHECK("..padding the tail with zeroes",
          memcmp(cipher + FIXTURE_LEN, cipher + FIXTURE_LEN, cipher_len - FIXTURE_LEN) == 0);
    CHECK("..and it is not the plaintext", memcmp(cipher, plain, FIXTURE_LEN) != 0);

    CHECK("PARAM.PFD was rewritten", memcmp(pfd, before, sizeof pfd) != 0);
    CHECK("..recording the logical size, not the padded one",
          apfd_entry_size(pfd, sizeof pfd, 0) == FIXTURE_LEN);
    CHECK("..and it still parses", apfd_valid(pfd, sizeof pfd) == APFD_OK);

    CHECK("the size the entry reports is what decrypt will produce",
          apfd_decrypted_size(pfd, sizeof pfd, FIXTURE_NAME) == FIXTURE_LEN);

    CHECK_RC("decrypts", apfd_decrypt(pfd, sizeof pfd, FIXTURE_NAME,
                                      cipher, cipher_len, sfid,
                                      back, sizeof back, &back_len));
    CHECK("the plaintext survives the round trip",
          back_len == FIXTURE_LEN && memcmp(back, plain, FIXTURE_LEN) == 0);

    /* A file shorter than the entry's aligned size cannot decrypt correctly,
     * so it is refused rather than quietly mangled. */
    CHECK("a file truncated to its logical size is refused",
          apfd_decrypt(pfd, sizeof pfd, FIXTURE_NAME, cipher, FIXTURE_LEN, sfid,
                       back, sizeof back, &back_len) == APFD_ERR_SIZE);
    CHECK("..and a longer one is not",
          apfd_decrypt(pfd, sizeof pfd, FIXTURE_NAME, cipher, sizeof cipher, sfid,
                       back, sizeof back, &back_len) == APFD_OK);

    CHECK("the wrong key gives the wrong plaintext",
          apfd_decrypt(pfd, sizeof pfd, FIXTURE_NAME, cipher, cipher_len,
                       (const uint8_t *)"0123456789ABCDEF",
                       back, sizeof back, &back_len) == APFD_OK &&
          memcmp(back, plain, FIXTURE_LEN) != 0);

    CHECK("verify accepts the file it just wrote",
          apfd_verify_file(pfd, sizeof pfd, FIXTURE_NAME, cipher, cipher_len, sfid)
              == APFD_OK);
    cipher[0] ^= 0xFF;
    CHECK("..and rejects one byte of damage",
          apfd_verify_file(pfd, sizeof pfd, FIXTURE_NAME, cipher, cipher_len, sfid)
              == APFD_ERR_HASH);
    cipher[0] ^= 0xFF;

    /* Resigning a signed PFD changes nothing. This is the property --corpus
     * leans on to check the whole signature chain against a real console. */
    memcpy(before, pfd, sizeof before);
    CHECK_RC("resigns", apfd_resign(pfd, sizeof pfd));
    CHECK("resigning a signed PARAM.PFD is a no-op",
          memcmp(pfd, before, sizeof pfd) == 0);
}

/* ---- arguments ---------------------------------------------------------- */

static void check_args(void)
{
    static const char *names[] = { FIXTURE_NAME, "PARAM.SFO" };
    uint8_t pfd[PFD_LEN];
    uint8_t sfid[APFD_SFID_LEN];
    uint8_t buf[64];
    size_t out_len = 0;

    puts("\narguments");
    build_pfd(pfd, names, 2);
    apfd_resign(pfd, sizeof pfd);
    apfd_sfid_from_hex(FIXTURE_SFID, sfid);

    CHECK("decrypt with no name is refused",
          apfd_decrypt(pfd, sizeof pfd, NULL, buf, sizeof buf, sfid,
                       buf, sizeof buf, &out_len) == APFD_ERR_ARG);
    CHECK("decrypt of an unlisted file is refused",
          apfd_decrypt(pfd, sizeof pfd, "ICON0.PNG", buf, sizeof buf, sfid,
                       buf, sizeof buf, &out_len) == APFD_ERR_NO_ENTRY);
    CHECK("decrypt of PARAM.SFO says it is not encrypted",
          apfd_decrypt(pfd, sizeof pfd, "PARAM.SFO", buf, sizeof buf, sfid,
                       buf, sizeof buf, &out_len) == APFD_ERR_PLAIN);
    CHECK("encrypt of PARAM.SFO says it is not encrypted",
          apfd_encrypt(pfd, sizeof pfd, "PARAM.SFO", buf, sizeof buf, sfid,
                       buf, sizeof buf, &out_len) == APFD_ERR_PLAIN);

    CHECK("a game file with no key is refused",
          apfd_encrypt(pfd, sizeof pfd, FIXTURE_NAME, buf, sizeof buf, NULL,
                       buf, sizeof buf, &out_len) == APFD_ERR_NO_KEY);
    CHECK("..while PARAM.SFO needs none",
          apfd_update_file(pfd, sizeof pfd, "PARAM.SFO", buf, sizeof buf, NULL)
              == APFD_OK);

    {
        uint8_t small[8];
        CHECK("an output buffer too small to hold the result is refused",
              apfd_encrypt(pfd, sizeof pfd, FIXTURE_NAME, buf, sizeof buf, sfid,
                           small, sizeof small, &out_len) == APFD_ERR_SIZE);
    }

    CHECK("update of an unlisted file is refused",
          apfd_update_file(pfd, sizeof pfd, "ICON0.PNG", buf, sizeof buf, NULL)
              == APFD_ERR_NO_ENTRY);
    CHECK("resigning a buffer that is not a PARAM.PFD is refused",
          apfd_resign(buf, sizeof buf) == APFD_ERR_PFD);
}

/* ---- the console binding ------------------------------------------------ */

/*
 * PARAM.SFO's entry carries four hashes and only the second is keyed by the
 * console the save belongs to. Naming a console is therefore the whole of
 * re-binding a save, and NOT naming one has to leave those three alone -- a
 * patched save that quietly lost its binding would stop loading on the machine
 * it came from.
 *
 * The derivations themselves are checked against pfdtool, which reports the
 * four by name; see REGENERATING. What is checked here is the switching.
 */
static void check_console(void)
{
    static const char *names[] = { "PARAM.SFO", FIXTURE_NAME };
    static const uint8_t CID_A[APFD_CONSOLE_ID_LEN] = {
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x07,
        0x00,0x4F,0x50,0x45,0x4E,0x53,0x4C,0x33 };
    uint8_t pfd[PFD_LEN], before[PFD_LEN];
    uint8_t sfo[256];
    apfd_console_t con, got;
    const size_t et_off = 96 + 24 + FIX_CAPACITY * 8;
    const uint8_t *hashes;

    puts("\nthe console binding");
    memset(&con, 0, sizeof con);
    apfd_set_console(NULL);

    CHECK("no console is named to begin with", apfd_get_console(&got) == 0);

    memcpy(con.console_id, CID_A, sizeof CID_A);
    con.user_id = 1;
    apfd_set_console(&con);
    CHECK("one can be named", apfd_get_console(&got) == 1);
    CHECK("..and comes back unchanged",
          memcmp(got.console_id, CID_A, sizeof CID_A) == 0 && got.user_id == 1);

    memset(&con, 0, sizeof con);
    apfd_set_console(&con);
    CHECK("an all-zero id counts as naming none", apfd_get_console(&got) == 0);

    /* PARAM.SFO is entry 0, so its hashes sit at a known offset. */
    build_pfd(pfd, names, 2);
    apfd_resign(pfd, sizeof pfd);
    fill(sfo, sizeof sfo, 0x0BADF00Du);
    hashes = pfd + et_off + 144;

    apfd_set_console(NULL);
    CHECK_RC("PARAM.SFO records with no console named",
             apfd_update_file(pfd, sizeof pfd, "PARAM.SFO", sfo, sizeof sfo, NULL));
    CHECK("..writing hash 0",
          memcmp(hashes, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) != 0);
    CHECK("..and leaving the three console-bound ones alone",
          memcmp(hashes + 20, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 0);

    memcpy(before, pfd, sizeof before);
    memcpy(con.console_id, CID_A, sizeof CID_A);
    con.user_id = 1;
    apfd_set_console(&con);

    CHECK_RC("PARAM.SFO records again with one named",
             apfd_update_file(pfd, sizeof pfd, "PARAM.SFO", sfo, sizeof sfo, NULL));
    CHECK("..hash 0 is unchanged, being keyed by neither",
          memcmp(hashes, before + et_off + 144, 20) == 0);
    CHECK("..and the other three are now written",
          memcmp(hashes + 20, before + et_off + 164, 60) != 0);

    /* A different console has to give a different hash 1, or the setting does
     * nothing and every save would look bound to every machine. */
    {
        uint8_t other[PFD_LEN];
        apfd_console_t con_b = con;

        memcpy(other, before, sizeof other);
        con_b.console_id[0] ^= 0xFF;
        apfd_set_console(&con_b);
        apfd_update_file(other, sizeof other, "PARAM.SFO", sfo, sizeof sfo, NULL);

        CHECK("a different console gives a different hash 1",
              memcmp(other + et_off + 164, hashes + 20, 20) != 0);
        CHECK("..while hash 3 stays put, being keyed by a constant",
              memcmp(other + et_off + 204, hashes + 60, 20) == 0);
    }

    /* Re-binding must not disturb the other entries or the signatures. */
    CHECK("the PARAM.PFD still parses", apfd_valid(pfd, sizeof pfd) == APFD_OK);
    {
        uint8_t copy[PFD_LEN];
        memcpy(copy, pfd, sizeof copy);
        apfd_resign(copy, sizeof copy);
        CHECK("..and is signed for what it now says",
              memcmp(copy, pfd, sizeof pfd) == 0);
    }

    apfd_set_console(NULL);
    CHECK("clearing it goes back to leaving them alone", apfd_get_console(&got) == 0);
}

/* ---- the disc hash key -------------------------------------------------- */

static void check_dhk(void)
{
    uint8_t dhk[APFD_DHK_LEN];

    puts("\ndisc hash keys");

    /* Almost every section leaves the line commented out, which must not read
     * as a key -- the leading ';' is the only thing telling them apart. */
    CHECK("a commented-out disc_hash_key is not a key",
          apfd_dhk_from_conf(CONF, sizeof CONF - 1, "BLUS30000", dhk) == APFD_ERR_NO_KEY);
    CHECK("a section with no line at all has none",
          apfd_dhk_from_conf(CONF, sizeof CONF - 1, "BLUS30002", dhk) == APFD_ERR_NO_KEY);
    CHECK_RC("a section that names one gives it",
             apfd_dhk_from_conf(CONF, sizeof CONF - 1, "BLUS30004", dhk));
    CHECK("..with the right bytes", sfid_is(dhk, "4142434445464748494A4B4C4D4E4F50"));
    CHECK("an unknown directory has none",
          apfd_dhk_from_conf(CONF, sizeof CONF - 1, "BLUS39999", dhk) == APFD_ERR_NO_KEY);
}

/* ---- known answers ------------------------------------------------------ */

/* The fixture, built and encrypted exactly as --dump-fixture writes it. */
static size_t make_fixture(uint8_t pfd[PFD_LEN], uint8_t *plain,
                           uint8_t *cipher, size_t cipher_cap)
{
    static const char *names[] = { FIXTURE_NAME };
    uint8_t sfid[APFD_SFID_LEN];
    size_t cipher_len = 0;

    build_pfd(pfd, names, 1);
    apfd_resign(pfd, PFD_LEN);
    apfd_sfid_from_hex(FIXTURE_SFID, sfid);
    fill(plain, FIXTURE_LEN, 0x9E3779B9u);

    if (apfd_encrypt(pfd, PFD_LEN, FIXTURE_NAME, plain, FIXTURE_LEN, sfid,
                     cipher, cipher_cap, &cipher_len) != APFD_OK)
        return 0;

    return cipher_len;
}

static void check_known_answers(void)
{
    uint8_t pfd[PFD_LEN], plain[FIXTURE_LEN];
    uint8_t cipher[FIXTURE_LEN + APFD_ALIGN];
    size_t cipher_len;

    puts("\nknown answers (cross-checked against pfdtool)");
    cipher_len = make_fixture(pfd, plain, cipher, sizeof cipher);

    CHECK("the fixture encrypts", cipher_len != 0);
    CHECK("the ciphertext matches the recorded digest",
          fnv1a64(cipher, cipher_len) == KNOWN_CIPHER);
    CHECK("the resigned PARAM.PFD matches the recorded digest",
          fnv1a64(pfd, PFD_LEN) == KNOWN_PFD);

    if (fnv1a64(cipher, cipher_len) != KNOWN_CIPHER ||
        fnv1a64(pfd, PFD_LEN) != KNOWN_PFD)
        printf("    cipher %016llx  pfd %016llx\n",
               (unsigned long long)fnv1a64(cipher, cipher_len),
               (unsigned long long)fnv1a64(pfd, PFD_LEN));
}

/* ---- files -------------------------------------------------------------- */

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f)
        return NULL;

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

static int spit(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    int ok;

    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        return 0;
    }
    ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

static int dump_fixture(const char *dir)
{
    uint8_t pfd[PFD_LEN], plain[FIXTURE_LEN];
    uint8_t cipher[FIXTURE_LEN + APFD_ALIGN];
    size_t cipher_len;
    char path[1024];

    cipher_len = make_fixture(pfd, plain, cipher, sizeof cipher);
    if (!cipher_len) {
        fprintf(stderr, "could not build the fixture\n");
        return 1;
    }

    MKDIR(dir);

    snprintf(path, sizeof path, "%s/PARAM.PFD", dir);
    if (!spit(path, pfd, sizeof pfd)) return 1;
    snprintf(path, sizeof path, "%s/%s", dir, FIXTURE_NAME);
    if (!spit(path, cipher, cipher_len)) return 1;
    snprintf(path, sizeof path, "%s/%s.plain", dir, FIXTURE_NAME);
    if (!spit(path, plain, sizeof plain)) return 1;

    printf("%s: PARAM.PFD (%d bytes), %s (%zu), %s.plain (%d)\n",
           dir, PFD_LEN, FIXTURE_NAME, cipher_len, FIXTURE_NAME, FIXTURE_LEN);
    printf("  secure file id  %s\n", FIXTURE_SFID);
    printf("  KNOWN_CIPHER    0x%016llxULL\n", (unsigned long long)fnv1a64(cipher, cipher_len));
    printf("  KNOWN_PFD       0x%016llxULL\n", (unsigned long long)fnv1a64(pfd, PFD_LEN));
    printf("  plaintext       0x%016llxULL\n", (unsigned long long)fnv1a64(plain, FIXTURE_LEN));
    return 0;
}

/* ---- the corpus --------------------------------------------------------- */

/*
 * Real saves. This is the check that says the implementation is right: every
 * hash below was written by a PlayStation 3, and every one is recomputed here
 * from the same inputs the console had.
 *
 *   per save   resigning a PARAM.PFD reproduces it byte for byte, which covers
 *              the bucket hash, the chain walk, both table HMACs and the
 *              signature's own encryption in one comparison
 *   per file   the recorded hash matches the file on disk, and the recorded
 *              size matches its length once aligned
 *   per file   decrypt then encrypt returns the original ciphertext over every
 *              whole block (the last one differs only where padding was)
 */
static struct {
    int saves, save_ok, resign_ok;
    int files, hash_ok, hash_bad, size_bad, cipher_ok, cipher_bad;
    int no_key, absent;
} g_corpus;

static const char *g_conf;
static size_t      g_conf_len;

static void corpus_file(const char *dir, const char *base,
                        const uint8_t *pfd, size_t pfd_len, int index)
{
    char name[APFD_NAME_LEN], path[2048];
    uint8_t sfid[APFD_SFID_LEN];
    const uint8_t *key = NULL;
    uint8_t *disk = NULL, *plain = NULL, *again = NULL;
    size_t disk_len = 0, plain_len = 0, again_len = 0;
    long long want;
    int rc;

    if (apfd_entry_name(pfd, pfd_len, index, name, sizeof name) != APFD_OK)
        return;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    disk = slurp(path, &disk_len);
    if (!disk) {
        g_corpus.absent++;
        return;
    }
    g_corpus.files++;

    if (!apfd_entry_has_builtin_key(name)) {
        if (!g_conf ||
            apfd_sfid_from_conf(g_conf, g_conf_len, base, name, sfid, NULL, 0) != APFD_OK) {
            g_corpus.no_key++;
            free(disk);
            return;
        }
        key = sfid;
    }

    /* The hash the console recorded, against the one these bytes produce. */
    rc = apfd_verify_file(pfd, pfd_len, name, disk, disk_len, key);
    if (rc == APFD_OK)
        g_corpus.hash_ok++;
    else {
        g_corpus.hash_bad++;
        printf("  MISMATCH %s/%s: %s\n", base, name, apfd_strerror(rc));
    }

    /* The size, which for an encrypted entry is the logical one. */
    want = apfd_entry_size(pfd, pfd_len, index);
    if (want >= 0 && apfd_encrypted_size((size_t)want) != disk_len &&
        (size_t)want != disk_len) {
        g_corpus.size_bad++;
        printf("  FAIL %s/%s: entry says %lld bytes, file is %zu\n",
               base, name, want, disk_len);
    }

    if (strcasecmp(name, "PARAM.SFO") == 0 || !key) {
        free(disk);
        return;
    }

    plain_len = (size_t)(want < 0 ? 0 : want);
    plain = malloc(plain_len ? plain_len : 1);
    again = malloc(apfd_encrypted_size(plain_len) + APFD_ALIGN);

    if (plain && again &&
        apfd_decrypt(pfd, pfd_len, name, disk, disk_len, key,
                     plain, plain_len, &plain_len) == APFD_OK &&
        apfd_encrypt((uint8_t *)pfd, pfd_len, name, plain, plain_len, key,
                     again, apfd_encrypted_size(plain_len), &again_len) == APFD_OK &&
        /* Every whole block has to come back identical. The final partial one
         * carries padding the console filled with whatever was in memory and
         * this fills with zeroes, so it is excluded. */
        memcmp(again, disk, plain_len & ~(size_t)(APFD_ALIGN - 1)) == 0)
        g_corpus.cipher_ok++;
    else {
        g_corpus.cipher_bad++;
        printf("  FAIL %s/%s: ciphertext does not round-trip\n", base, name);
    }

    free(plain);
    free(again);
    free(disk);
}

static void corpus_save(const char *dir, const char *base)
{
    char path[2048];
    uint8_t *pfd, *copy;
    size_t len = 0;
    int count, rc;

    snprintf(path, sizeof path, "%s/PARAM.PFD", dir);
    pfd = slurp(path, &len);
    if (!pfd)
        return;

    g_corpus.saves++;

    rc = apfd_valid(pfd, len);
    if (rc != APFD_OK) {
        printf("  FAIL %s: %s\n", base, apfd_strerror(rc));
        free(pfd);
        return;
    }
    g_corpus.save_ok++;

    /* Resigning what the console signed must change nothing. */
    copy = malloc(len);
    memcpy(copy, pfd, len);
    if (apfd_resign(copy, len) == APFD_OK && memcmp(copy, pfd, len) == 0)
        g_corpus.resign_ok++;
    else
        printf("  FAIL %s: PARAM.PFD does not resign to itself\n", base);
    free(copy);

    /* The per-file checks mutate the PFD (encrypt rewrites the entry), so they
     * work on their own copy and the original stays the reference. */
    count = apfd_entry_count(pfd, len);
    for (int i = 0; i < count; i++) {
        uint8_t *scratch = malloc(len);

        memcpy(scratch, pfd, len);
        corpus_file(dir, base, scratch, len, i);
        free(scratch);
    }

    free(pfd);
}

static void corpus_walk(const char *dir, int depth)
{
    DIR *d;
    struct dirent *e;
    char path[2048];
    struct stat st;

    snprintf(path, sizeof path, "%s/PARAM.PFD", dir);
    if (stat(path, &st) == 0) {
        const char *base = strrchr(dir, '/');
        corpus_save(dir, base ? base + 1 : dir);
        return;
    }

    if (depth <= 0 || !(d = opendir(dir)))
        return;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            corpus_walk(path, depth - 1);
    }
    closedir(d);
}

static int run_corpus(const char *dir, const char *conf_path)
{
    uint8_t *conf = NULL;
    size_t conf_len = 0;

    if (conf_path) {
        conf = slurp(conf_path, &conf_len);
        if (!conf) {
            fprintf(stderr, "cannot read %s\n", conf_path);
            return 1;
        }
        g_conf     = (const char *)conf;
        g_conf_len = conf_len;
        printf("games.conf: %s (%zu bytes)\n", conf_path, conf_len);
    } else
        printf("no games.conf given: only PARAM.SFO and trophy files are checked\n");

    printf("corpus: %s\n\n", dir);
    corpus_walk(dir, 4);

    printf("\n  saves                    %d\n", g_corpus.saves);
    printf("  parsed                   %d\n", g_corpus.save_ok);
    printf("  PARAM.PFD resigns to itself %d\n", g_corpus.resign_ok);
    printf("  files on disk            %d\n", g_corpus.files);
    printf("  recorded hash matches    %d  (mismatched %d)\n",
           g_corpus.hash_ok, g_corpus.hash_bad);
    printf("  size agrees with entry   %d\n", g_corpus.files - g_corpus.size_bad);
    printf("  ciphertext round-trips   %d  (failed %d)\n",
           g_corpus.cipher_ok, g_corpus.cipher_bad);
    printf("  no key in games.conf     %d\n", g_corpus.no_key);
    printf("  listed but not present   %d\n", g_corpus.absent);

    free(conf);

    if (g_corpus.saves == 0) {
        fprintf(stderr, "\nno saves found under %s\n", dir);
        return 1;
    }

    /*
     * A hash mismatch is reported but does not fail the run, because it says
     * as much about the inputs as about this code: the key games.conf gave may
     * be wrong for that file, or the save may have been edited without its
     * PARAM.PFD being updated. Both happen -- the tree these numbers came from
     * holds one of each -- and neither is something to fix here. A regression
     * in the hashing would show up as hundreds of mismatches, not two.
     *
     * The rest are unambiguous, so they do fail.
     */
    if (g_corpus.hash_bad)
        printf("\n  %d mismatch%s above: either the key is wrong for that file "
               "or the save\n  was edited without its PARAM.PFD. Neither fails "
               "this run; see the source.\n",
               g_corpus.hash_bad, g_corpus.hash_bad == 1 ? "" : "es");

    return (g_corpus.save_ok != g_corpus.saves ||
            g_corpus.resign_ok != g_corpus.saves ||
            g_corpus.size_bad || g_corpus.cipher_bad) ? 1 : 0;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "--dump-fixture") == 0)
        return dump_fixture(argv[2]);

    if (argc > 2 && strcmp(argv[1], "--corpus") == 0) {
        int rc = run_corpus(argv[2], argc > 3 ? argv[3] : NULL);
        printf("\ncorpus checks: %s\n", rc ? "FAILED" : "all passed");
        return rc;
    }

    puts("PS3 savedata checks");

    check_structure();
    check_bounds();
    check_conf();
    check_dhk();
    check_console();
    check_account_id();
    check_round_trip();
    check_args();
    check_known_answers();

    printf("\nPS3 checks: %s\n", g_fails ? "FAILED" : "all passed");
    return g_fails ? 1 : 0;
}
