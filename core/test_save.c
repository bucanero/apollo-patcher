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
 * now the one every front-end uses -- the PSP encryption path included, which
 * used to carry its own copy. So the bounds checks below are load-bearing:
 * a save arrives from a stranger's memory card as readily as from your own.
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

#include "sfo.h"
#include "saveinfo.h"

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
    CHECK("...as PSV", info.platform == ASAVE_PSV);
    CHECK_STR("...with the title ID from inside PARAMS", info.title_id, "PCSE00608");
    CHECK_STR("...and no name, which is honest", info.name, "");
    CHECK("...and it needs no unwrapping", info.encrypted == 0);

    /* Without PARAMS, PARENT_DIRECTORY still names it. */
    len = psv_sfo(sfo, sizeof sfo, 0);
    CHECK("a Vita save with no PARAMS falls back to PARENT_DIRECTORY",
          asave_identify(sfo, len, ASAVE_AT_SCE, 0, &info) == ASAVE_OK
          && info.platform == ASAVE_PSV);
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
            print_info(root, &info);
            (*found)++;
            free(sfo);
            closedir(d);
            return 1;
        }
    } else if (asave_identify(sfo, len, ASAVE_AT_ROOT, 0, &info) == ASAVE_OK) {
        print_info(root, &info);
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

int main(int argc, char **argv)
{
    if (argc > 3 && strcmp(argv[1], "--sfo") == 0)
        return run_one(argv[2], argv[3]);

    if (argc > 2 && strcmp(argv[1], "--scan") == 0)
        return run_scan(argv[2]);

    printf("PARAM.SFO reader and save identification\n");

    check_reader();
    check_bounds();
    check_identify();
    check_title_ids();

    printf("\nsave checks: %s\n", g_fails ? "FAILED" : "all passed");
    return g_fails ? 1 : 0;
}
