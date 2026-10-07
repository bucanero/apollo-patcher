/*
 * Headless checks for the shared PARAM.SFO reader (core/sfo.c) and the save
 * identification built on it (core/saveinfo.c).
 *
 *   test_save                    run every self-contained check
 *   test_save --sfo WHERE FILE   identify one real PARAM.SFO and print what
 *                                it says. WHERE is "root" (PSP/PS3, the SFO
 *                                sits beside the data files) or "sce" (PS4 or
 *                                Vita, it sits in sce_sys/).
 *   test_save --scan DIR         walk a tree of real saves the way the desktop
 *                                app's browser does and print the list
 *
 * Why these two are worth a test of their own
 * -------------------------------------------
 * Every offset in an SFO is read out of the file itself, and this parser is
 * the one every front-end uses, the PSP encryption path included. So the
 * bounds checks below are load-bearing: a save arrives from a stranger's
 * memory card as readily as from your own.
 *
 * The identification is the other half. Four consoles write the same container
 * with four different sets of keys, and getting it wrong is not a crash but
 * something worse -- a save listed under the wrong game, patched with that
 * game's codes. The per-console cases below are built from the key sets real
 * saves actually carry (see the comment on each).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include <zlib.h>   /* building the synthetic PNGs below */

#include "sfo.h"
#include "saveinfo.h"
#include "png.h"

/* ---- the harness -------------------------------------------------------- */

static int g_fails;

#define CHECK(what, cond) do {                                        \
        int ok_ = (cond);                                             \
        if (!ok_) g_fails++;                                          \
        printf("  %-58s %s\n", (what), ok_ ? "ok" : "FAILED");        \
    } while (0)

#define CHECK_STR(what, got, want) do {                               \
        int ok_ = strcmp((got), (want)) == 0;                         \
        if (!ok_) g_fails++;                                          \
        printf("  %-58s %s\n", (what), ok_ ? "ok" : "FAILED");        \
        if (!ok_) printf("      got %s, wanted %s\n", (got), (want));  \
    } while (0)

/* ---- building an SFO ---------------------------------------------------- */

typedef struct {
    const char *key;
    unsigned    fmt;
    const void *val;
    uint32_t    used;   /* bytes written; 0 with a string value means strlen+1 */
    uint32_t    max;    /* bytes reserved; 0 means "same as used"              */
} kv_t;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/*
 * Assemble a PARAM.SFO. Keys land in the key table in the order given and
 * values in the data table in the same order, which is what a console does
 * too -- the format does not require it and this parser does not rely on it.
 */
static size_t build_sfo(uint8_t *out, size_t cap, const kv_t *kv, int n)
{
    size_t key_at = 0x14 + 0x10 * (size_t)n, key_len = 0, data_at, total = 0;
    int i;

    for (i = 0; i < n; i++)
        key_len += strlen(kv[i].key) + 1;
    data_at = (key_at + key_len + 3) & ~(size_t)3;

    for (i = 0; i < n; i++) {
        uint32_t used = kv[i].used ? kv[i].used : (uint32_t)(strlen((const char *)kv[i].val) + 1);
        total += kv[i].max ? kv[i].max : used;
    }
    if (data_at + total > cap)
        return 0;

    memset(out, 0, data_at + total);
    put32(out + 0x00, 0x46535000u);   /* "\0PSF" */
    put32(out + 0x04, 0x00000101u);
    put32(out + 0x08, (uint32_t)key_at);
    put32(out + 0x0C, (uint32_t)data_at);
    put32(out + 0x10, (uint32_t)n);

    key_len = 0;
    total   = 0;
    for (i = 0; i < n; i++) {
        uint8_t *e    = out + 0x14 + 0x10 * (size_t)i;
        uint32_t used = kv[i].used ? kv[i].used : (uint32_t)(strlen((const char *)kv[i].val) + 1);
        uint32_t max  = kv[i].max ? kv[i].max : used;

        put16(e + 0x00, (uint16_t)key_len);
        put16(e + 0x02, (uint16_t)kv[i].fmt);
        put32(e + 0x04, used);
        put32(e + 0x08, max);
        put32(e + 0x0C, (uint32_t)total);

        strcpy((char *)out + key_at + key_len, kv[i].key);
        key_len += strlen(kv[i].key) + 1;

        memcpy(out + data_at + total, kv[i].val, used);
        total += max;
    }
    return data_at + total;
}

/* ---- the four consoles -------------------------------------------------- */

/* A PSP save: SAVEDATA_PARAMS is what makes it one. Key set taken from the
 * sample saves in web/dist/sample. */
static size_t psp_sfo(uint8_t *out, size_t cap)
{
    static uint8_t params[0x80];
    const kv_t kv[] = {
        { "SAVEDATA_DIRECTORY", ASFO_FMT_STR, "ULUS10391DATA00", 0, 64 },
        { "SAVEDATA_FILE_LIST", ASFO_FMT_BIN, params, 0x20, 0x20 },
        { "SAVEDATA_PARAMS",    ASFO_FMT_BIN, params, 0x80, 0x80 },
        { "SAVEDATA_TITLE",     ASFO_FMT_STR, "Slot 1", 0, 128 },
        { "TITLE",              ASFO_FMT_STR, "Grand Theft Auto", 0, 128 },
    };

    params[0] = 0x41;
    memcpy(params + 0x40, "SAVEDATA.BIN", 12);
    return build_sfo(out, cap, kv, (int)(sizeof kv / sizeof *kv));
}

/* A PS3 save, from ~/BLUS30917-AUTOSAVE/PARAM.SFO: TITLE, SUB_TITLE,
 * SAVEDATA_DIRECTORY, and no SAVEDATA_PARAMS. */
static size_t ps3_sfo(uint8_t *out, size_t cap)
{
    static uint8_t params[1024];
    const kv_t kv[] = {
        { "ACCOUNT_ID",         ASFO_FMT_BIN, params, 16, 16 },
        { "ATTRIBUTE",          ASFO_FMT_U32, "\1\0\0\0", 4, 4 },
        { "CATEGORY",           ASFO_FMT_STR, "SD", 0, 4 },
        { "DETAIL",             ASFO_FMT_STR, "LOLLIPOP CHAINSAW", 0, 1024 },
        { "PARAMS",             ASFO_FMT_BIN, params, 1024, 1024 },
        { "SAVEDATA_DIRECTORY", ASFO_FMT_STR, "BLUS30917-AUTOSAVE", 0, 64 },
        { "SUB_TITLE",          ASFO_FMT_STR, "SAVE DATA", 0, 128 },
        { "TITLE",              ASFO_FMT_STR, "LOLLIPOP CHAINSAW", 0, 128 },
    };

    return build_sfo(out, cap, kv, (int)(sizeof kv / sizeof *kv));
}

/* A PS4 save, from apollo-lib/tools/dec_JOJOASB.S_CUSA28770: TITLE_ID is the
 * key no other console here has. */
static size_t ps4_sfo(uint8_t *out, size_t cap)
{
    static uint8_t params[1024];
    const kv_t kv[] = {
        { "ACCOUNT_ID",         ASFO_FMT_BIN, params, 8, 8 },
        { "CATEGORY",           ASFO_FMT_STR, "sd", 0, 4 },
        { "DETAIL",             ASFO_FMT_STR, "", 1, 1024 },
        { "FORMAT",             ASFO_FMT_STR, "obs", 0, 4 },
        { "MAINTITLE",          ASFO_FMT_STR, "JoJo's Bizarre Adventure", 0, 128 },
        { "PARAMS",             ASFO_FMT_BIN, params, 1024, 1024 },
        { "SAVEDATA_DIRECTORY", ASFO_FMT_STR, "JOJOASB.S", 0, 32 },
        { "SUBTITLE",           ASFO_FMT_STR, "Save Data", 0, 128 },
        { "TITLE_ID",           ASFO_FMT_STR, "CUSA28770", 0, 12 },
    };

    return build_sfo(out, cap, kv, (int)(sizeof kv / sizeof *kv));
}

/*
 * A Vita save, from save-decrypters/vita-re-rev2-unpacker/PCSE00608/SLOT0.
 *
 * The awkward one: no TITLE_ID, TITLE present but empty, and the title ID
 * only at 0x28 inside the 1 KiB PARAMS blob, where it is followed
 * immediately by more fields rather than a NUL.
 */
static size_t psv_sfo(uint8_t *out, size_t cap, int with_params)
{
    static uint8_t params[1024];
    const kv_t all[] = {
        { "ACCOUNT_ID",       ASFO_FMT_BIN, params, 8, 8 },
        { "CATEGORY",         ASFO_FMT_STR, "sd", 0, 4 },
        { "DETAIL",           ASFO_FMT_STR, "", 1, 1024 },
        { "PARAMS",           ASFO_FMT_BIN, params, 1024, 1024 },
        { "PARENT_DIRECTORY", ASFO_FMT_STR, "/PCSE00608", 0, 64 },
        { "SAVEDATA_VER",     ASFO_FMT_STR, "0.00", 0, 8 },
        { "TITLE",            ASFO_FMT_STR, "", 1, 128 },
    };
    kv_t kv[8];
    int n = 0, i;

    memset(params, 0, sizeof params);
    memcpy(params + 0x28, "PCSE00608", 9);
    /* What follows the ID in a real blob, and the reason it cannot be read as
     * a string: there is no terminator. */
    memcpy(params + 0x31, "SLOT0", 5);

    for (i = 0; i < (int)(sizeof all / sizeof *all); i++) {
        if (!with_params && strcmp(all[i].key, "PARAMS") == 0)
            continue;
        kv[n++] = all[i];
    }
    return build_sfo(out, cap, kv, n);
}

/* A game's own PARAM.SFO -- off a disc, or an EBOOT's. These are all over a
 * memory card and must never be listed as saves. */
static size_t game_sfo(uint8_t *out, size_t cap)
{
    const kv_t kv[] = {
        { "BOOTABLE",        ASFO_FMT_U32, "\1\0\0\0", 4, 4 },
        { "CATEGORY",        ASFO_FMT_STR, "MG", 0, 4 },
        { "DISC_ID",         ASFO_FMT_STR, "ULUS10391", 0, 16 },
        { "PSP_SYSTEM_VER",  ASFO_FMT_STR, "3.71", 0, 8 },
        { "TITLE",           ASFO_FMT_STR, "Grand Theft Auto", 0, 128 },
    };

    return build_sfo(out, cap, kv, (int)(sizeof kv / sizeof *kv));
}

/* ---- checks ------------------------------------------------------------- */

static void check_reader(void)
{
    uint8_t  sfo[4096];
    size_t   len = ps3_sfo(sfo, sizeof sfo);
    char     s[256];
    uint8_t  blob[4];
    uint32_t u;

    printf("\nthe SFO reader\n");
    CHECK("an SFO was built at all", len > 0);
    CHECK("asfo_valid accepts it", asfo_valid(sfo, len) == ASFO_OK);

    CHECK("a string value reads back", asfo_string(sfo, len, "TITLE", s, sizeof s) == ASFO_OK);
    CHECK_STR("...with the right text", s, "LOLLIPOP CHAINSAW");

    CHECK("a u32 value reads back", asfo_u32(sfo, len, "ATTRIBUTE", &u) == ASFO_OK);
    CHECK("...with the right number", u == 1);

    CHECK("a blob reads back", asfo_blob(sfo, len, "PARAMS", 0, blob, 4) == ASFO_OK);
    CHECK("a blob past its end is refused",
          asfo_blob(sfo, len, "PARAMS", 1022, blob, 4) == ASFO_ERR_SPACE);

    CHECK("a missing key says so",
          asfo_string(sfo, len, "NO_SUCH_KEY", s, sizeof s) == ASFO_ERR_MISSING);
    CHECK("...and leaves the buffer empty", s[0] == '\0');

    /* A short output buffer must refuse rather than truncate: a truncated
     * title ID would still be 9 characters of something. */
    CHECK("a value too long for the buffer is refused",
          asfo_string(sfo, len, "TITLE", s, 4) == ASFO_ERR_SPACE);
    CHECK("...and leaves the buffer empty", s[0] == '\0');

    /* The u32 reader is typed, so a string does not come back as a number. */
    CHECK("a u32 read of a string is refused",
          asfo_u32(sfo, len, "TITLE", &u) == ASFO_ERR_FORMAT);
}

static void check_bounds(void)
{
    uint8_t sfo[4096];
    size_t  len = ps3_sfo(sfo, sizeof sfo);
    char    s[256];
    uint8_t copy[4096];

    printf("\nbounds, on a file that lies about itself\n");

    CHECK("a NULL buffer is refused", asfo_valid(NULL, 64) == ASFO_ERR_FORMAT);
    CHECK("an empty buffer is refused", asfo_valid(sfo, 0) == ASFO_ERR_FORMAT);
    CHECK("a buffer shorter than the header is refused", asfo_valid(sfo, 0x13) == ASFO_ERR_FORMAT);

    memcpy(copy, sfo, len);
    copy[0] = 'X';
    CHECK("a wrong magic is refused", asfo_valid(copy, len) == ASFO_ERR_FORMAT);

    memcpy(copy, sfo, len);
    put32(copy + 0x04, 0x00000102u);
    CHECK("a wrong version is refused", asfo_valid(copy, len) == ASFO_ERR_FORMAT);

    memcpy(copy, sfo, len);
    put32(copy + 0x08, (uint32_t)len + 1);
    CHECK("a key table past the end is refused", asfo_valid(copy, len) == ASFO_ERR_FORMAT);

    memcpy(copy, sfo, len);
    put32(copy + 0x0C, (uint32_t)len + 1);
    CHECK("a data table past the end is refused", asfo_valid(copy, len) == ASFO_ERR_FORMAT);

    memcpy(copy, sfo, len);
    put32(copy + 0x10, 0xFFFFFFFFu);
    CHECK("an impossible entry count is refused", asfo_valid(copy, len) == ASFO_ERR_FORMAT);

    /* Per-entry lies. The whole file is condemned rather than the entry
     * skipped: "no such key" about a corrupt file would be a lie of our own. */
    memcpy(copy, sfo, len);
    put32(copy + 0x14 + 0x10 * 7 + 0x0C, (uint32_t)len);   /* TITLE's value offset */
    CHECK("a value offset past the end is refused",
          asfo_string(copy, len, "TITLE", s, sizeof s) == ASFO_ERR_FORMAT);

    memcpy(copy, sfo, len);
    put32(copy + 0x14 + 0x10 * 7 + 0x08, 0xFFFFFFFFu);     /* TITLE's reserved size */
    CHECK("a reserved size past the end is refused",
          asfo_string(copy, len, "TITLE", s, sizeof s) == ASFO_ERR_FORMAT);

    memcpy(copy, sfo, len);
    put32(copy + 0x14 + 0x10 * 7 + 0x04, 0xFFFFFFFFu);     /* TITLE's used size */
    CHECK("a used size past its own reservation is refused",
          asfo_string(copy, len, "TITLE", s, sizeof s) == ASFO_ERR_FORMAT);

    /* A key name with no terminator inside the key table: strcmp would walk
     * off the end of the buffer. */
    memcpy(copy, sfo, len);
    memset(copy + 0x14 + 0x10 * 8, 'A', (size_t)len - (0x14 + 0x10 * 8));
    CHECK("an unterminated key name does not run off the end",
          asfo_string(copy, len, "TITLE", s, sizeof s) != ASFO_OK);

    /*
     * Truncation. Every prefix of a valid file is either refused or read
     * correctly -- never read past its end, and never answered with bytes
     * that are not in the buffer. The exactness matters: a reader that
     * returned a half-value would be handing back whatever followed the
     * buffer, and a title ID is nine characters of anything.
     *
     * The loop is also the ASan/UBSan target: under a sanitiser build a read
     * past `at` aborts here rather than being caught by the comparison.
     */
    {
        size_t at;
        int    wrong = 0, parsed = 0;

        for (at = 1; at < len; at++) {
            memcpy(copy, sfo, at);
            if (asfo_string(copy, at, "TITLE", s, sizeof s) == ASFO_OK) {
                parsed++;
                if (strcmp(s, "LOLLIPOP CHAINSAW") != 0) wrong++;
            }
            /* CATEGORY as well as TITLE: values are laid out in key order, so
             * the last key's value survives no truncation at all and on its
             * own would prove only that everything was refused. */
            if (asfo_string(copy, at, "CATEGORY", s, sizeof s) == ASFO_OK) {
                parsed++;
                if (strcmp(s, "SD") != 0) wrong++;
            }
        }
        CHECK("no truncation of a valid file reads past its end", wrong == 0);
        CHECK("...and the ones that do parse are complete", parsed > 0);
    }
}

/*
 * ACCOUNT_ID, the PS4/Vita way: eight raw bytes, little-endian.
 *
 * The interesting case is the PS3, whose ACCOUNT_ID is the same key with the
 * same ASFO_FMT_BIN format and a completely different meaning -- sixteen
 * bytes of ASCII hex. Only the length tells them apart, so both directions
 * are checked against a PS3 file as well as a PS4 one.
 */
static void check_account_id(void)
{
    uint8_t  buf[4096], before[4096];
    uint64_t got = 0xDEADBEEF;
    size_t   len, off;
    uint32_t used;

    printf("\nACCOUNT_ID (PS4/Vita: 8 raw bytes, little-endian)\n");

    /* Read, from a file that has one. The fixture stores zeros. */
    len = ps4_sfo(buf, sizeof buf);
    CHECK("PS4 save: read succeeds", asfo_account_id(buf, len, &got) == ASFO_OK);
    CHECK("PS4 save: an unset account reads as zero", got == 0);

    len = psv_sfo(buf, sizeof buf, 1);
    CHECK("Vita save: read succeeds", asfo_account_id(buf, len, NULL) == ASFO_OK);

    /* Write, then read back. */
    len = ps4_sfo(buf, sizeof buf);
    CHECK("assigning an account succeeds",
          asfo_set_account_id(buf, len, 0x135CD5AC1D86F213ull) == ASFO_OK);
    CHECK("...and reads back the same value",
          asfo_account_id(buf, len, &got) == ASFO_OK && got == 0x135CD5AC1D86F213ull);

    /* The byte order is the one apollo-ps4 and apollo-vita write: a plain
       memcpy of the u64, so the low byte lands first. Spelled out rather than
       round-tripped, because a reader and writer that agreed with each other
       and not with the console would pass a round-trip. */
    CHECK("stored little-endian, low byte first",
          asfo_find(buf, len, "ACCOUNT_ID", &off, &used, NULL, NULL) == ASFO_OK &&
          used == 8 &&
          buf[off + 0] == 0x13 && buf[off + 1] == 0xF2 &&
          buf[off + 2] == 0x86 && buf[off + 3] == 0x1D &&
          buf[off + 4] == 0xAC && buf[off + 5] == 0xD5 &&
          buf[off + 6] == 0x5C && buf[off + 7] == 0x13);

    /* Zero is refused: it would take the owner away rather than set one. */
    memcpy(before, buf, len);
    CHECK("assigning zero is refused", asfo_set_account_id(buf, len, 0) == ASFO_ERR_SPACE);
    CHECK("...and the file is untouched", memcmp(before, buf, len) == 0);

    /* A PS3 file: same key, sixteen ASCII bytes, must not be read as a number
       nor overwritten with one. */
    len = ps3_sfo(buf, sizeof buf);
    memcpy(before, buf, len);
    got = 0xDEADBEEF;
    CHECK("PS3 save: 16-byte ACCOUNT_ID is not read as a number",
          asfo_account_id(buf, len, &got) == ASFO_ERR_FORMAT);
    CHECK("...and the out-parameter is left alone", got == 0xDEADBEEF);
    CHECK("PS3 save: assigning is refused",
          asfo_set_account_id(buf, len, 0x1122334455667788ull) == ASFO_ERR_FORMAT);
    CHECK("...and the file is untouched", memcmp(before, buf, len) == 0);

    /* A PSP save has no such key at all -- neither does a game's own SFO. */
    len = psp_sfo(buf, sizeof buf);
    CHECK("PSP save: reported missing, not malformed",
          asfo_account_id(buf, len, NULL) == ASFO_ERR_MISSING);
    CHECK("PSP save: assigning is reported missing",
          asfo_set_account_id(buf, len, 0x1122334455667788ull) == ASFO_ERR_MISSING);

    /*
     * Truncation. Cutting the file in half is not the test it looks like:
     * ACCOUNT_ID is the first value in the data table, so half a file still
     * holds all eight bytes and reading them is the right answer. The bounds
     * that matter are the VALUE's, so the cut goes through the middle of it.
     */
    len = ps4_sfo(buf, sizeof buf);
    CHECK("the value can be located at all",
          asfo_find(buf, len, "ACCOUNT_ID", &off, NULL, NULL, NULL) == ASFO_OK);
    memcpy(before, buf, len);
    CHECK("a value cut short is not read",
          asfo_account_id(buf, off + 4, NULL) != ASFO_OK);
    CHECK("a value cut short is not written",
          asfo_set_account_id(buf, off + 4, 0x1122334455667788ull) != ASFO_OK);
    CHECK("...and nothing was written before it gave up", memcmp(before, buf, len) == 0);
    CHECK("a file that is not an SFO at all is rejected",
          asfo_account_id((const uint8_t *)"not an sfo", 10, NULL) == ASFO_ERR_FORMAT);
}

/*
 * A Vita add-on-content folder. Real one, from apollo-saves: it sits at
 * addcont/<titleid>/<something>/sce_sys/param.sfo and carries a TITLE_ID,
 * which is the one thing that tells a PS4 save from a Vita one -- so without
 * the CATEGORY test it identifies as a PS4 save.
 */
static size_t vita_dlc_sfo(uint8_t *out, size_t cap)
{
    const kv_t kv[] = {
        { "ATTRIBUTE", ASFO_FMT_U32, "\0\0\0\0", 4, 4 },
        { "CATEGORY",  ASFO_FMT_STR, "ac", 0, 4 },
        { "TITLE_ID",  ASFO_FMT_STR, "PCSA00147", 0, 12 },
    };

    return build_sfo(out, cap, kv, (int)(sizeof kv / sizeof *kv));
}

/*
 * A PS4 save whose PARAMS blob has telltale contents, so a write into one
 * field can be shown NOT to have touched the others.
 *
 *   0x04  user_id      0x11111111
 *   0x08  psid_hmac    0x22 x32
 *   0x2C  title_id_1   "CUSA28770"
 *   0x3C  title_id_2   "OLDTITLE0"   -- what sync_title_id must overwrite
 *   rest               0x33
 */
static size_t ps4_sfo_params(uint8_t *out, size_t cap, uint32_t params_used)
{
    static uint8_t params[1024];
    const kv_t kv[] = {
        { "ACCOUNT_ID",         ASFO_FMT_BIN, params, 8, 8 },
        { "CATEGORY",           ASFO_FMT_STR, "sd", 0, 4 },
        { "PARAMS",             ASFO_FMT_BIN, params, params_used, params_used },
        { "SAVEDATA_DIRECTORY", ASFO_FMT_STR, "JOJOASB.S", 0, 32 },
        { "TITLE_ID",           ASFO_FMT_STR, "CUSA28770", 0, 12 },
    };

    memset(params, 0x33, sizeof params);
    params[0x04] = params[0x05] = params[0x06] = params[0x07] = 0x11;
    memset(params + 0x08, 0x22, 32);
    memcpy(params + 0x2C, "CUSA28770", 9);
    memcpy(params + 0x3C, "OLDTITLE0", 9);
    return build_sfo(out, cap, kv, (int)(sizeof kv / sizeof *kv));
}

/*
 * The PS4's PARAMS blob: the console identity, beside the account.
 *
 * These offsets belong to the PS4 and to nothing else -- a PS3 keeps its
 * account at PARAMS+0x30 and a Vita its title ID at PARAMS+0x28, both inside
 * what is psid_hmac here. The writers cannot tell which console wrote a blob,
 * so what is checked below is that each writes its OWN field and leaves every
 * other byte alone; keeping them off a Vita save is the caller's job.
 */
static void check_ps4_params(void)
{
    uint8_t  buf[4096], before[4096];
    size_t   len, off;
    uint32_t used;

    static const uint8_t PSID[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
    };
    /* HMAC-SHA256(PSID_HMAC_KEY, PSID), computed with Python's hmac/hashlib
       rather than by the code under test -- an independent oracle, so a
       writer and a reader that agreed with each other and with nothing else
       would still fail here. */
    static const uint8_t WANT[32] = {
        0x64, 0xB4, 0x2A, 0x1C, 0x85, 0xD7, 0x77, 0x35,
        0xB4, 0x52, 0xD5, 0x0C, 0x42, 0x37, 0xE6, 0xEA,
        0xAB, 0x08, 0x59, 0x4A, 0x2E, 0x7A, 0xBD, 0x85,
        0xC7, 0x8A, 0xE6, 0x8A, 0xAC, 0x26, 0x9B, 0xE7,
    };

    printf("\nPARAMS (PS4: user_id, psid_hmac, title_id)\n");

    /* ---- the console binding ---- */
    len = ps4_sfo_params(buf, sizeof buf, 1024);
    CHECK("PARAMS is found at all",
          asfo_find(buf, len, "PARAMS", &off, &used, NULL, NULL) == ASFO_OK);
    memcpy(before, buf, len);

    CHECK("the PSID HMAC is written",
          asfo_ps4_set_psid_hmac(buf, len, PSID) == ASFO_OK);
    CHECK("...and matches an independently computed HMAC-SHA256",
          memcmp(buf + off + 0x08, WANT, 32) == 0);
    CHECK("...the file neither grew nor moved", len == ps4_sfo_params(before, sizeof before, 1024));

    /* Everything outside the 32 bytes it owns is untouched. */
    len = ps4_sfo_params(before, sizeof before, 1024);
    CHECK("...nothing before the field changed", memcmp(buf, before, off + 0x08) == 0);
    CHECK("...nothing after the field changed",
          memcmp(buf + off + 0x28, before + off + 0x28, len - off - 0x28) == 0);

    CHECK("a NULL PSID is refused", asfo_ps4_set_psid_hmac(buf, len, NULL) == ASFO_ERR_FORMAT);

    /* ---- the console-local user ---- */
    len = ps4_sfo_params(buf, sizeof buf, 1024);
    CHECK("the user id is written", asfo_ps4_set_user_id(buf, len, 0x01020304) == ASFO_OK);
    CHECK("...little-endian, in its own four bytes",
          buf[off + 0x04] == 0x04 && buf[off + 0x05] == 0x03 &&
          buf[off + 0x06] == 0x02 && buf[off + 0x07] == 0x01);

    len = ps4_sfo_params(buf, sizeof buf, 1024);
    CHECK("a user id of zero is left alone rather than written",
          asfo_ps4_set_user_id(buf, len, 0) == ASFO_OK &&
          buf[off + 0x04] == 0x11 && buf[off + 0x07] == 0x11);

    /* ---- the title ID copy ---- */
    len = ps4_sfo_params(buf, sizeof buf, 1024);
    CHECK("the second title ID starts out different",
          memcmp(buf + off + 0x3C, "OLDTITLE0", 9) == 0);
    CHECK("syncing succeeds", asfo_ps4_sync_title_id(buf, len) == ASFO_OK);
    CHECK("...and copies the first over the second",
          memcmp(buf + off + 0x3C, "CUSA28770", 9) == 0);
    CHECK("...leaving the first as it was",
          memcmp(buf + off + 0x2C, "CUSA28770", 9) == 0);

    /* ---- what is refused ---- */
    len = game_sfo(buf, sizeof buf);                    /* no PARAMS at all */
    CHECK("a param.sfo with no PARAMS is reported missing, not written",
          asfo_ps4_set_psid_hmac(buf, len, PSID) == ASFO_ERR_MISSING);
    CHECK("...for the user id too",
          asfo_ps4_set_user_id(buf, len, 7) == ASFO_ERR_MISSING);
    CHECK("...and for the title copy",
          asfo_ps4_sync_title_id(buf, len) == ASFO_ERR_MISSING);

    /* A blob too short to hold these fields. The PS4's own guard is 0x50, and
       the point is that a Vita's shorter PARAMS cannot be half-written. */
    len = ps4_sfo_params(buf, sizeof buf, 0x20);
    CHECK("a PARAMS shorter than the PS4's fields is refused",
          asfo_ps4_set_psid_hmac(buf, len, PSID) == ASFO_ERR_FORMAT);
    CHECK("...and nothing was written into it",
          asfo_blob(buf, len, "PARAMS", 0x08, before, 4) == ASFO_OK &&
          before[0] == 0x22 && before[3] == 0x22);
}

/* A PS4 save with CATEGORY replaced, for testing that field on its own. */
static size_t ps4_sfo_category(uint8_t *out, size_t cap, const char *cat, uint32_t used)
{
    static uint8_t params[1024];
    const kv_t kv[] = {
        { "ACCOUNT_ID",         ASFO_FMT_BIN, params, 8, 8 },
        { "CATEGORY",           ASFO_FMT_STR, cat, used, 16 },
        { "MAINTITLE",          ASFO_FMT_STR, "JoJo's Bizarre Adventure", 0, 128 },
        { "SAVEDATA_DIRECTORY", ASFO_FMT_STR, "JOJOASB.S", 0, 32 },
        { "TITLE_ID",           ASFO_FMT_STR, "CUSA28770", 0, 12 },
    };

    return build_sfo(out, cap, kv, (int)(sizeof kv / sizeof *kv));
}

/*
 * CATEGORY: what the SFO describes, as opposed to who wrote it.
 *
 * Only MS (PSP), SD (PS3) and sd (PS4, Vita) are savedata. This is what keeps
 * Vita DLC out of the save list -- an addcont folder's param.sfo carries a
 * TITLE_ID and is otherwise indistinguishable from a PS4 save's, and 80 of
 * them in apollo-saves were being listed as saves before this.
 */
static void check_category(void)
{
    uint8_t      buf[4096];
    asave_info_t info;
    size_t       len;

    printf("\nCATEGORY (what the file describes, not who wrote it)\n");

    len = vita_dlc_sfo(buf, sizeof buf);
    CHECK("Vita add-on content is not a save",
          asave_identify(buf, len, ASAVE_AT_SCE, 0, &info) == ASAVE_ERR_WHICH);

    len = ps4_sfo_category(buf, sizeof buf, "gd", 0);
    CHECK("...nor is game data",
          asave_identify(buf, len, ASAVE_AT_SCE, 0, &info) == ASAVE_ERR_WHICH);

    /* The three that are. */
    len = ps4_sfo_category(buf, sizeof buf, "sd", 0);
    CHECK("CATEGORY sd is a save (PS4, Vita)",
          asave_identify(buf, len, ASAVE_AT_SCE, 0, &info) == ASAVE_OK);
    len = ps3_sfo(buf, sizeof buf);
    CHECK("CATEGORY SD is a save (PS3)",
          asave_identify(buf, len, ASAVE_AT_ROOT, 1, &info) == ASAVE_OK);
    len = psv_sfo(buf, sizeof buf, 1);
    CHECK("a Vita save still identifies",
          asave_identify(buf, len, ASAVE_AT_SCE, 0, &info) == ASAVE_OK &&
          info.platform == ASAVE_PSVITA);

    /* Case is ignored: the same two letters are upper on a PS3 and lower on a
       PS4, and a tool that rewrote one either way still means savedata. */
    len = ps4_sfo_category(buf, sizeof buf, "SD", 0);
    CHECK("CATEGORY case is ignored",
          asave_identify(buf, len, ASAVE_AT_SCE, 0, &info) == ASAVE_OK);
    len = ps4_sfo_category(buf, sizeof buf, "Sd", 0);
    CHECK("...either way round",
          asave_identify(buf, len, ASAVE_AT_SCE, 0, &info) == ASAVE_OK);

    /* Length is not: a prefix or an extension of one of them is not one. */
    len = ps4_sfo_category(buf, sizeof buf, "s", 0);
    CHECK("a one-letter CATEGORY is not one of them",
          asave_identify(buf, len, ASAVE_AT_SCE, 0, &info) == ASAVE_ERR_WHICH);
    len = ps4_sfo_category(buf, sizeof buf, "sdx", 0);
    CHECK("nor is a longer one starting the same way",
          asave_identify(buf, len, ASAVE_AT_SCE, 0, &info) == ASAVE_ERR_WHICH);

    /* Absent, or empty, means no opinion -- the key-set tests still decide.
       A save is not refused for a key it merely omits. */
    len = psp_sfo(buf, sizeof buf);
    CHECK("a save with no CATEGORY at all still identifies",
          asfo_find(buf, len, "CATEGORY", NULL, NULL, NULL, NULL) == ASFO_ERR_MISSING &&
          asave_identify(buf, len, ASAVE_AT_ROOT, 0, &info) == ASAVE_OK &&
          info.platform == ASAVE_PSP);
    len = ps4_sfo_category(buf, sizeof buf, "", 1);
    CHECK("an empty CATEGORY is not held against it",
          asave_identify(buf, len, ASAVE_AT_SCE, 0, &info) == ASAVE_OK);
}

static void check_identify(void)
{
    uint8_t      sfo[8192];
    size_t       len;
    asave_info_t info;

    printf("\nidentifying a save\n");

    len = psp_sfo(sfo, sizeof sfo);
    CHECK("a PSP save identifies",
          asave_identify(sfo, len, ASAVE_AT_ROOT, 0, &info) == ASAVE_OK);
    CHECK("...as PSP", info.platform == ASAVE_PSP);
    CHECK_STR("...with its title ID", info.title_id, "ULUS10391");
    CHECK_STR("...its name", info.name, "Grand Theft Auto");
    CHECK_STR("...its slot", info.detail, "Slot 1");
    CHECK_STR("...and its directory", info.directory, "ULUS10391DATA00");
    CHECK("...and it has a layer below the patch", info.encrypted == 1);

    len = ps3_sfo(sfo, sizeof sfo);
    CHECK("a PS3 save identifies",
          asave_identify(sfo, len, ASAVE_AT_ROOT, 1, &info) == ASAVE_OK);
    CHECK("...as PS3", info.platform == ASAVE_PS3);
    CHECK_STR("...with its title ID", info.title_id, "BLUS30917");
    CHECK_STR("...its name", info.name, "LOLLIPOP CHAINSAW");
    CHECK_STR("...and its slot", info.detail, "SAVE DATA");
    CHECK("...and it has a layer below the patch", info.encrypted == 1);

    /* A PS3 save whose PARAM.PFD was stripped is still a PS3 save. */
    CHECK("...with no PARAM.PFD beside it, still PS3",
          asave_identify(sfo, len, ASAVE_AT_ROOT, 0, &info) == ASAVE_OK
          && info.platform == ASAVE_PS3);

    len = ps4_sfo(sfo, sizeof sfo);
    CHECK("a PS4 save identifies",
          asave_identify(sfo, len, ASAVE_AT_SCE, 0, &info) == ASAVE_OK);
    CHECK("...as PS4", info.platform == ASAVE_PS4);
    CHECK_STR("...with its title ID", info.title_id, "CUSA28770");
    CHECK_STR("...its name", info.name, "JoJo's Bizarre Adventure");
    CHECK_STR("...and its slot", info.detail, "Save Data");
    CHECK("...and it needs no unwrapping", info.encrypted == 0);

    len = psv_sfo(sfo, sizeof sfo, 1);
    CHECK("a Vita save identifies",
          asave_identify(sfo, len, ASAVE_AT_SCE, 0, &info) == ASAVE_OK);
    CHECK("...as PSV", info.platform == ASAVE_PSVITA);
    CHECK_STR("...with the title ID from inside PARAMS", info.title_id, "PCSE00608");
    CHECK_STR("...and no name, which is honest", info.name, "");
    CHECK("...and it needs no unwrapping", info.encrypted == 0);

    /* Without PARAMS, PARENT_DIRECTORY still names it. */
    len = psv_sfo(sfo, sizeof sfo, 0);
    CHECK("a Vita save with no PARAMS falls back to PARENT_DIRECTORY",
          asave_identify(sfo, len, ASAVE_AT_SCE, 0, &info) == ASAVE_OK
          && info.platform == ASAVE_PSVITA);
    CHECK_STR("...to the same title ID", info.title_id, "PCSE00608");

    /*
     * A PS4 save's SFO that is not in sce_sys/ -- which is how one turns up
     * when somebody has copied it out on its own. The keys settle it: no PSP
     * or PS3 SAVEDATA carries TITLE_ID, so this is a PS4 save wherever it is
     * sitting, and saying so is the difference between naming the game and a
     * nameless row (a PS4 save directory is the slot, "Save0001", which yields
     * no title ID at all).
     */
    len = ps4_sfo(sfo, sizeof sfo);
    CHECK("a PS4 SFO found at the save's root is still PS4",
          asave_identify(sfo, len, ASAVE_AT_ROOT, 0, &info) == ASAVE_OK
          && info.platform == ASAVE_PS4);
    CHECK_STR("...with its title ID", info.title_id, "CUSA28770");
    CHECK_STR("...and its name", info.name, "JoJo's Bizarre Adventure");
    CHECK("...and still needs no unwrapping", info.encrypted == 0);

    /* The one that must be rejected. */
    len = game_sfo(sfo, sizeof sfo);
    CHECK("a game's own PARAM.SFO is not a save",
          asave_identify(sfo, len, ASAVE_AT_ROOT, 0, &info) == ASAVE_ERR_WHICH);

    /* And a file that is not an SFO at all. */
    memset(sfo, 0xA5, 64);
    CHECK("a non-SFO is refused",
          asave_identify(sfo, 64, ASAVE_AT_ROOT, 0, &info) == ASAVE_ERR_SFO);
}

static void check_title_ids(void)
{
    uint8_t      sfo[8192];
    size_t       len;
    asave_info_t info;
    static const char *const bad[] = {
        "SHORT",              /* under 9 characters               */
        "ulus10391DATA00",    /* lower case                       */
        "ULUS-0391DATA00",    /* punctuation inside the ID itself */
    };
    size_t i;

    printf("\ntitle IDs that must not be believed\n");

    for (i = 0; i < sizeof bad / sizeof *bad; i++) {
        const kv_t kv[] = {
            { "SAVEDATA_DIRECTORY", ASFO_FMT_STR, bad[i], 0, 64 },
            { "SAVEDATA_PARAMS",    ASFO_FMT_BIN, "\x41", 1, 0x80 },
            { "TITLE",              ASFO_FMT_STR, "Some Game", 0, 128 },
        };
        char what[96];

        len = build_sfo(sfo, sizeof sfo, kv, 3);
        snprintf(what, sizeof what, "\"%s\" yields no title ID", bad[i]);
        CHECK(what, asave_identify(sfo, len, ASAVE_AT_ROOT, 0, &info) == ASAVE_OK
                    && info.title_id[0] == '\0');
    }

    /* ...but the save itself is still listed, under its own name, because a
     * folder somebody renamed is still a save worth opening by hand. */
    CHECK("...and the save is still identified", info.platform == ASAVE_PSP);
    CHECK_STR("...with its name intact", info.name, "Some Game");

    /* A name a game wrote with a leading space -- Far Cry 3 Blood Dragon does
     * exactly this -- must not file the save under space in a sorted list. */
    {
        const kv_t kv[] = {
            { "SAVEDATA_DIRECTORY", ASFO_FMT_STR, "BLUS30613-PLAYDATA", 0, 64 },
            { "SUB_TITLE",          ASFO_FMT_STR, "SAVE DATA  ", 0, 128 },
            { "TITLE",              ASFO_FMT_STR, " Far Cry 3 Blood Dragon", 0, 128 },
        };

        len = build_sfo(sfo, sizeof sfo, kv, 3);
        CHECK("a title written with a leading space identifies",
              asave_identify(sfo, len, ASAVE_AT_ROOT, 0, &info) == ASAVE_OK);
        CHECK_STR("...with the space gone", info.name, "Far Cry 3 Blood Dragon");
        CHECK_STR("...and the trailing one too", info.detail, "SAVE DATA");
        CHECK_STR("...while the directory is left exactly as written",
                  info.directory, "BLUS30613-PLAYDATA");
    }
}

/*
 * The title catalogue: game names by title ID, for saves that name no game
 * themselves. Every Vita save is one of those.
 */
static void check_title_db(void)
{
    /* As tools/make-bundle.py writes it: platform, title ID, name, sorted.
     * The awkward rows are deliberate -- a blank name, a record with no name
     * field at all, CRLF, and a title ID in the wrong case. */
    static const char db[] =
        "PSP\tULUS10391\tMonster Hunter Freedom Unite\n"
        "PSV\tPCSB00245\t\n"
        "PSV\tPCSE00608\tResident Evil: Revelations 2\r\n"
        "PSV\tPCSE00996\n"
        "PSV\tPCSG00022\t@field\n";
    char out[128];

    printf("\nthe title catalogue\n");

    CHECK("a Vita title is found",
          asave_name_from_db(db, sizeof db - 1, "PSV", "PCSE00608", out, sizeof out) == ASAVE_OK);
    CHECK_STR("...with its name", out, "Resident Evil: Revelations 2");

    CHECK("a PSP title is found",
          asave_name_from_db(db, sizeof db - 1, "PSP", "ULUS10391", out, sizeof out) == ASAVE_OK);
    CHECK_STR("...with its name", out, "Monster Hunter Freedom Unite");

    CHECK("the last line needs no terminator",
          asave_name_from_db(db, sizeof db - 1, "PSV", "PCSG00022", out, sizeof out) == ASAVE_OK);
    CHECK_STR("...and still reads", out, "@field");

    CHECK("a lower-case title ID still matches",
          asave_name_from_db(db, sizeof db - 1, "PSV", "pcse00608", out, sizeof out) == ASAVE_OK);

    /* The platform is half the key: the catalogue holds the same ID for two
     * consoles in a handful of places, and a PSP save must not be named after
     * a Vita one. */
    CHECK("the wrong platform does not match",
          asave_name_from_db(db, sizeof db - 1, "PSP", "PCSE00608", out, sizeof out) != ASAVE_OK);
    CHECK("...and leaves the buffer empty", out[0] == '\0');

    CHECK("an unknown title says so",
          asave_name_from_db(db, sizeof db - 1, "PSV", "PCSE99999", out, sizeof out) != ASAVE_OK);
    CHECK("a record with an empty name is not a name",
          asave_name_from_db(db, sizeof db - 1, "PSV", "PCSB00245", out, sizeof out) != ASAVE_OK);
    CHECK("a record with no name field is not a name",
          asave_name_from_db(db, sizeof db - 1, "PSV", "PCSE00996", out, sizeof out) != ASAVE_OK);

    /* Refused rather than truncated -- half a game's name is worse than the
     * folder name the caller would otherwise show. */
    CHECK("a name too long for the buffer is refused",
          asave_name_from_db(db, sizeof db - 1, "PSV", "PCSE00608", out, 8) != ASAVE_OK);
    CHECK("...and leaves the buffer empty", out[0] == '\0');

    CHECK("an empty catalogue is survivable",
          asave_name_from_db("", 0, "PSV", "PCSE00608", out, sizeof out) != ASAVE_OK);
    CHECK("a NULL catalogue is survivable",
          asave_name_from_db(NULL, 0, "PSV", "PCSE00608", out, sizeof out) != ASAVE_OK);
    CHECK("an empty title ID matches nothing",
          asave_name_from_db(db, sizeof db - 1, "PSV", "", out, sizeof out) != ASAVE_OK);

    /*
     * Truncation, the same argument as everywhere else here: this file comes
     * out of a zip, and a zip is a file somebody can hand you.
     *
     * What is checked is that anything returned is a PREFIX of the real name,
     * never bytes from past the end. Not that it is the whole name: a file's
     * last line legitimately has no terminator (see above), so a record cut
     * short is indistinguishable from the last one in a complete file, and
     * the honest answer is what is actually there.
     */
    {
        static const char want[] = "Resident Evil: Revelations 2";
        size_t at;
        int    wrong = 0, whole = 0;

        for (at = 0; at < sizeof db - 1; at++) {
            if (asave_name_from_db(db, at, "PSV", "PCSE00608", out, sizeof out) != ASAVE_OK)
                continue;
            if (strncmp(out, want, strlen(out)) != 0) wrong++;
            if (strcmp(out, want) == 0) whole++;
        }
        CHECK("every truncation returns a prefix of the real name, never more", wrong == 0);
        CHECK("...and the untruncated ones return all of it", whole > 0);
    }
}

/* ---- save icons --------------------------------------------------------- */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Append one chunk, CRC and all. Real files carry a correct CRC even though
 * the decoder does not check it, so the fixtures do too. */
static size_t chunk(uint8_t *out, const char *type, const uint8_t *body, size_t len)
{
    uLong crc;

    put_be32(out, (uint32_t)len);
    memcpy(out + 4, type, 4);
    if (len) memcpy(out + 8, body, len);
    crc = crc32(0, out + 4, (uInt)(4 + len));
    put_be32(out + 8 + len, (uint32_t)crc);
    return 12 + len;
}

/*
 * A PNG built to order: `rows` is height scanlines of `stride` bytes, each of
 * which this prefixes with a filter byte of 0 (None). Everything the decoder
 * has to get right is in how those bytes are then interpreted, which is what
 * the callers below vary.
 */
static size_t make_png(uint8_t *out, size_t cap, int w, int h, int depth, int color,
                       int interlace, const uint8_t *palette, size_t pal_len,
                       const uint8_t *rows, size_t stride)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    uint8_t  ihdr[13];
    uint8_t  raw[8192], packed[16384];
    uLongf   packed_len = sizeof packed;
    size_t   at = 0, raw_len = 0;
    int      y;

    if ((size_t)h * (stride + 1) > sizeof raw || cap < 1024)
        return 0;

    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8]  = (uint8_t)depth;
    ihdr[9]  = (uint8_t)color;
    ihdr[10] = 0;   /* deflate  */
    ihdr[11] = 0;   /* adaptive */
    ihdr[12] = (uint8_t)interlace;

    for (y = 0; y < h; y++) {
        raw[raw_len++] = 0;                       /* filter: None */
        memcpy(raw + raw_len, rows + (size_t)y * stride, stride);
        raw_len += stride;
    }
    if (compress2(packed, &packed_len, raw, (uLong)raw_len, 9) != Z_OK)
        return 0;

    memcpy(out, sig, sizeof sig);
    at = sizeof sig;
    at += chunk(out + at, "IHDR", ihdr, sizeof ihdr);
    if (pal_len) at += chunk(out + at, "PLTE", palette, pal_len);
    at += chunk(out + at, "IDAT", packed, packed_len);
    at += chunk(out + at, "IEND", NULL, 0);
    return at;
}

static void check_icons(void)
{
    uint8_t  png[4096];
    uint8_t *rgba = NULL;
    size_t   len;
    int      w = 0, h = 0;

    printf("\nsave icons\n");

    /* 8-bit RGB and 8-bit RGBA: between them 4,809 of the 5,047 real icons in
       apollo-saves. Palette is the next 223 and is covered below. */
    {
        const uint8_t rows[2 * 6] = { 255,0,0,  0,255,0,
                                      0,0,255,  255,255,255 };
        len = make_png(png, sizeof png, 2, 2, 8, 2, 0, NULL, 0, rows, 6);
        CHECK("an 8-bit RGB icon decodes", len > 0
              && apng_decode(png, len, &rgba, &w, &h) == APNG_OK);
        CHECK("...at the right size", w == 2 && h == 2);
        CHECK("...with the first pixel opaque red",
              rgba && rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);
        CHECK("...and the last pixel white",
              rgba && rgba[12] == 255 && rgba[13] == 255 && rgba[14] == 255);
        apng_free(rgba); rgba = NULL;
    }
    {
        const uint8_t rows[2 * 8] = { 1,2,3,0,    4,5,6,128,
                                      7,8,9,255,  10,11,12,64 };
        len = make_png(png, sizeof png, 2, 2, 8, 6, 0, NULL, 0, rows, 8);
        CHECK("an 8-bit RGBA icon decodes", len > 0
              && apng_decode(png, len, &rgba, &w, &h) == APNG_OK);
        CHECK("...and alpha survives",
              rgba && rgba[3] == 0 && rgba[7] == 128 && rgba[11] == 255 && rgba[15] == 64);
        apng_free(rgba); rgba = NULL;
    }

    /* The rest of the non-interlaced format, which no save icon here uses but
     * which the decoder claims to read. */
    {
        /* Four palette entries, and one byte holding four 2-bit indices:
         * 0x1B is 00 01 10 11, so the row is entry 0, 1, 2, 3 in order. Every
         * pixel after the first is the part the unpacking has to get right. */
        const uint8_t pal[12] = { 255,0,0,  0,255,0,  0,0,255,  255,255,255 };
        const uint8_t rows[1] = { 0x1B };
        len = make_png(png, sizeof png, 4, 1, 2, 3, 0, pal, sizeof pal, rows, 1);
        CHECK("a 2-bit palette icon decodes", len > 0
              && apng_decode(png, len, &rgba, &w, &h) == APNG_OK);
        CHECK("...at the right size", w == 4 && h == 1);
        CHECK("...unpacking all four indices out of the one byte",
              rgba
              && rgba[0]  == 255 && rgba[1]  == 0   && rgba[2]  == 0     /* red   */
              && rgba[4]  == 0   && rgba[5]  == 255 && rgba[6]  == 0     /* green */
              && rgba[8]  == 0   && rgba[9]  == 0   && rgba[10] == 255   /* blue  */
              && rgba[12] == 255 && rgba[13] == 255 && rgba[14] == 255); /* white */
        CHECK("...opaque, with no tRNS in the file", rgba && rgba[3] == 255);
        apng_free(rgba); rgba = NULL;
    }
    {
        const uint8_t rows[2 * 4] = { 0x12,0x34, 0xAB,0xCD,
                                      0x00,0xFF, 0xFF,0x00 };
        len = make_png(png, sizeof png, 2, 2, 16, 0, 0, NULL, 0, rows, 4);
        CHECK("a 16-bit greyscale icon decodes", len > 0
              && apng_decode(png, len, &rgba, &w, &h) == APNG_OK);
        CHECK("...taking the high byte of each sample",
              rgba && rgba[0] == 0x12 && rgba[4] == 0xAB && rgba[3] == 255);
        apng_free(rgba); rgba = NULL;
    }

    /* What it must refuse, and refuse without crashing or leaking a buffer. */
    {
        const uint8_t rows[6] = { 1,2,3, 4,5,6 };
        len = make_png(png, sizeof png, 2, 1, 8, 2, 1 /* Adam7 */, NULL, 0, rows, 6);
        CHECK("an interlaced PNG is refused, not guessed at",
              len > 0 && apng_decode(png, len, &rgba, &w, &h) == APNG_ERR_SUPPORT);
        CHECK("...and hands back no buffer", rgba == NULL);

        len = make_png(png, sizeof png, 2, 1, 8, 2, 0, NULL, 0, rows, 6);
        png[1] = 'X';
        CHECK("a wrong signature is refused",
              apng_decode(png, len, &rgba, &w, &h) == APNG_ERR_FORMAT);
        png[1] = 'P';

        CHECK("a NULL buffer is refused",
              apng_decode(NULL, 64, &rgba, &w, &h) == APNG_ERR_FORMAT);
        CHECK("an empty buffer is refused",
              apng_decode(png, 0, &rgba, &w, &h) == APNG_ERR_FORMAT);

        /*
         * Every truncation. The decoder allocates from IHDR and fills from
         * the inflate output, so a file that stops before the end of the
         * image data must fail rather than hand back a half-filled buffer of
         * whatever malloc had.
         *
         * A file cut inside the trailing IEND chunk is the exception and must
         * still decode: all the image data is present by then, and refusing
         * would throw away an icon over twelve missing bytes.
         */
        {
            const size_t iend_at = len - 12;
            uint8_t      copy[4096];
            size_t       at;
            int          early = 0, late = 0, wrong = 0;

            for (at = 1; at < len; at++) {
                memcpy(copy, png, at);
                if (apng_decode(copy, at, &rgba, &w, &h) != APNG_OK)
                    continue;
                if (at < iend_at) early++;
                else              late++;
                if (w != 2 || h != 1 || rgba[0] != 1 || rgba[3] != 255) wrong++;
                apng_free(rgba);
                rgba = NULL;
            }
            CHECK("no truncation into the image data decodes", early == 0);
            CHECK("...but one that loses only IEND still does", late == 12);
            CHECK("...and what it decodes is the whole image", wrong == 0);
        }

        /* A file that claims an enormous image must not try to allocate it. */
        len = make_png(png, sizeof png, 2, 1, 8, 2, 0, NULL, 0, rows, 6);
        put_be32(png + 16, 0x7FFFFFFF);          /* IHDR width */
        CHECK("an absurd width is refused by size, not by malloc",
              apng_decode(png, len, &rgba, &w, &h) == APNG_ERR_SIZE);
        CHECK("...and hands back no buffer", rgba == NULL);
    }
}

/* ---- real files --------------------------------------------------------- */

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE    *f = fopen(path, "rb");
    uint8_t *buf;
    long     n;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void print_info(const char *path, const asave_info_t *info)
{
    printf("  %-8s %-9s %-28s %-20s %s\n",
           asave_platform_name(info->platform),
           info->title_id[0] ? info->title_id : "-",
           info->name[0] ? info->name : "(unnamed)",
           info->detail[0] ? info->detail : "-",
           path);
}

/* --accounts: print the account rather than the name for each save found, so
 * a whole folder of real ones can be diffed against an independent reader. */
static int g_show_accounts;

/* "-" covers both "no such key" and "the key is the PS3's sixteen-byte one". */
static void print_account(const char *path, const uint8_t *sfo, size_t len)
{
    uint64_t id;

    if (asfo_account_id(sfo, len, &id) == ASFO_OK)
        printf("  %016llX  %s\n", (unsigned long long)id, path);
    else
        printf("  %-16s  %s\n", "-", path);
}

static int run_one(const char *where, const char *path)
{
    asave_where_t at = strcmp(where, "sce") == 0 ? ASAVE_AT_SCE : ASAVE_AT_ROOT;
    asave_info_t  info;
    size_t        len;
    uint8_t      *sfo = slurp(path, &len);
    int           rc;

    if (!sfo) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    rc = asave_identify(sfo, len, at, 0, &info);
    if (rc != ASAVE_OK) {
        printf("  %s: %s\n", path,
               rc == ASAVE_ERR_SFO ? "not a PARAM.SFO" : "not a save's PARAM.SFO");
    } else {
        print_info(path, &info);
    }
    free(sfo);
    return rc == ASAVE_OK ? 0 : 1;
}

/*
 * Walk a tree the way the desktop app's browser does: a directory holding
 * PARAM.SFO is a PSP or PS3 save, one holding sce_sys/param.sfo is a PS4 or
 * Vita one, and a directory that is a save is not descended into.
 */
static int walk(const char *root, int depth, int *found)
{
    DIR           *d = opendir(root);
    struct dirent *e;
    char           path[2048];
    asave_info_t   info;
    size_t         len;
    uint8_t       *sfo;
    struct stat    st;

    if (!d)
        return 0;

    snprintf(path, sizeof path, "%s/PARAM.SFO", root);
    sfo = slurp(path, &len);
    if (!sfo) {
        snprintf(path, sizeof path, "%s/sce_sys/param.sfo", root);
        sfo = slurp(path, &len);
        if (sfo && asave_identify(sfo, len, ASAVE_AT_SCE, 0, &info) == ASAVE_OK) {
            if (g_show_accounts) print_account(root, sfo, len);
            else                 print_info(root, &info);
            (*found)++;
            free(sfo);
            closedir(d);
            return 1;
        }
    } else if (asave_identify(sfo, len, ASAVE_AT_ROOT, 0, &info) == ASAVE_OK) {
        if (g_show_accounts) print_account(root, sfo, len);
        else                 print_info(root, &info);
        (*found)++;
        free(sfo);
        closedir(d);
        return 1;
    }
    free(sfo);

    if (depth > 0) {
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            snprintf(path, sizeof path, "%s/%s", root, e->d_name);
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
                walk(path, depth - 1, found);
        }
    }
    closedir(d);
    return 0;
}

static int run_scan(const char *root)
{
    int found = 0;

    printf("saves under %s\n", root);
    walk(root, 8, &found);
    printf("\n%d save%s found\n", found, found == 1 ? "" : "s");
    return found ? 0 : 1;
}

/*
 * Decode one real icon and print what came out, as "WxH crc32", so an
 * independent decoder can be run over the same file and the two compared.
 */
static int run_icon(const char *path)
{
    size_t   len;
    uint8_t *data = slurp(path, &len);
    uint8_t *rgba = NULL;
    int      w = 0, h = 0, rc;

    if (!data) { fprintf(stderr, "cannot read %s\n", path); return 2; }

    rc = apng_decode(data, len, &rgba, &w, &h);
    if (rc != APNG_OK) {
        printf("- - %s: %s\n", path, apng_strerror(rc));
        free(data);
        return 1;
    }
    printf("%dx%d %08lx %s\n", w, h,
           (unsigned long)crc32(0, rgba, (uInt)((size_t)w * h * 4)), path);
    apng_free(rgba);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "--icon") == 0)
        return run_icon(argv[2]);

    if (argc > 3 && strcmp(argv[1], "--sfo") == 0)
        return run_one(argv[2], argv[3]);

    if (argc > 2 && strcmp(argv[1], "--scan") == 0)
        return run_scan(argv[2]);

    if (argc > 2 && strcmp(argv[1], "--accounts") == 0) {
        g_show_accounts = 1;
        return run_scan(argv[2]);
    }

    printf("PARAM.SFO reader and save identification\n");

    check_reader();
    check_bounds();
    check_account_id();
    check_ps4_params();
    check_category();
    check_identify();
    check_title_ids();
    check_title_db();
    check_icons();

    printf("\nsave checks: %s\n", g_fails ? "FAILED" : "all passed");
    return g_fails ? 1 : 0;
}
