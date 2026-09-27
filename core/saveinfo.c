/* See saveinfo.h. */
#include <string.h>

#include "saveinfo.h"
#include "sfo.h"

/*
 * CATEGORY says what an SFO DESCRIBES, and exactly one value per console
 * means "savedata":
 *
 *   MS   PSP -- a Memory Stick save
 *   SD   PS3
 *   sd   PS4 and Vita
 *
 * Measured over every PARAM.SFO in the apollo-saves database: all 2,566 real
 * saves carry one of those three and nothing else does.
 *
 * Anything else describes something that is not a save and must not be listed
 * as one. `ac` is add-on content and `gd` is game data -- and a Vita DLC
 * folder has its own sce_sys/param.sfo with a TITLE_ID in it, which is
 * otherwise indistinguishable from a PS4 save's, so 80 of them in that
 * database were being identified as PS4 saves.
 *
 * Two deliberate looseness's. The comparison ignores case, because the same
 * two letters are upper on a PS3 and lower on a PS4 and a tool that rewrote
 * one either way is still describing a save. And a file with NO CATEGORY at
 * all is passed through to the key-set tests rather than refused: every real
 * save measured has one, but refusing a save over a key it merely omits would
 * be a worse failure than the one this fixes.
 */
static int category_is_savedata(const uint8_t *sfo, size_t sfo_len)
{
    char cat[16];
    char a, b;

    if (asfo_string(sfo, sfo_len, "CATEGORY", cat, sizeof cat) != ASFO_OK || !cat[0])
        return 1;   /* absent, or too long to be one of these: no opinion */
    if (cat[1] == '\0' || cat[2] != '\0')
        return 0;

    a = (cat[0] >= 'A' && cat[0] <= 'Z') ? (char)(cat[0] + 32) : cat[0];
    b = (cat[1] >= 'A' && cat[1] <= 'Z') ? (char)(cat[1] + 32) : cat[1];
    return (a == 's' && b == 'd') || (a == 'm' && b == 's');
}

/* Nine characters of upper-case letters and digits, and nothing else: every
 * title ID in the patch database is exactly that. Checked rather than assumed
 * because the string it comes from is a directory name off a memory card, and
 * an entry with a plausible-looking wrong ID would load some other game's
 * patch. Empty is a fine answer; wrong is not. */
static int title_id_ok(const char *s)
{
    int i;

    for (i = 0; i < 9; i++) {
        const char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return 0;
    }
    return s[9] == '\0';
}

/* The leading title ID of a save directory name: "ULUS10391DATA00" ->
 * "ULUS10391", "BLUS30917-AUTOSAVE" -> "BLUS30917". */
static void title_id_from_dir(const char *dir, char out[ASAVE_TITLE_ID_LEN])
{
    out[0] = '\0';
    if (strlen(dir) < 9)
        return;
    memcpy(out, dir, 9);
    out[9] = '\0';
    if (!title_id_ok(out))
        out[0] = '\0';
}

/*
 * Strip leading and trailing spaces, in place.
 *
 * Games really do write them: Far Cry 3 Blood Dragon's TITLE is
 * " Far Cry(R) 3 Blood Dragon", space and all. A console shows that as-is, but
 * in a sorted list it files the game under space and reads as a rendering
 * fault, so the display strings are trimmed. Only those -- SAVEDATA_DIRECTORY
 * is a lookup key and a filesystem name, and is left exactly as written.
 */
static void trim(char *s)
{
    size_t n = strlen(s), at = 0;

    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    s[n] = '\0';
    while (s[at] == ' ' || s[at] == '\t') at++;
    if (at)
        memmove(s, s + at, n - at + 1);
}

/* The first of these keys that is present and non-empty. Consoles disagree
 * about which one holds the same fact, and several write the key with an
 * empty value rather than leaving it out. */
static void first_of(const uint8_t *sfo, size_t len, char *out, size_t out_len,
                     const char *const *keys, int n)
{
    int i;

    out[0] = '\0';
    for (i = 0; i < n; i++) {
        if (asfo_string(sfo, len, keys[i], out, out_len) == ASFO_OK) {
            trim(out);
            if (out[0])
                return;
        }
        out[0] = '\0';
    }
}

const char *asave_platform_name(asave_platform_t platform)
{
    switch (platform) {
        case ASAVE_PSP: return "PSP";
        case ASAVE_PS3: return "PS3";
        case ASAVE_PS4: return "PS4";
        case ASAVE_PSV: return "PSV";
        default:        return "?";
    }
}

int asave_identify(const uint8_t *sfo, size_t sfo_len,
                   asave_where_t where, int has_pfd, asave_info_t *out)
{
    static const char *const name_psp[] = { "TITLE", "SAVEDATA_TITLE" };
    static const char *const det_psp[]  = { "SAVEDATA_TITLE", "SAVEDATA_DETAIL", "DETAIL" };
    static const char *const name_ps3[] = { "TITLE" };
    static const char *const det_ps3[]  = { "SUB_TITLE", "SUBTITLE", "DETAIL" };
    static const char *const name_ps4[] = { "MAINTITLE", "TITLE" };
    static const char *const det_ps4[]  = { "SUBTITLE", "DETAIL" };
    static const char *const det_psv[]  = { "SAVEDATA_TITLE", "DETAIL" };

    (void)has_pfd;   /* see below */

    if (!out)
        return ASAVE_ERR_SFO;
    memset(out, 0, sizeof *out);

    if (asfo_valid(sfo, sfo_len) != ASFO_OK)
        return ASAVE_ERR_SFO;

    /* Before anything else, because it applies to every console and the tests
     * below cannot tell a Vita save from Vita DLC on their own. */
    if (!category_is_savedata(sfo, sfo_len))
        return ASAVE_ERR_WHICH;

    if (where == ASAVE_AT_SCE) {
        /* PS4 says its title ID outright; the Vita has no such key. That one
         * difference is the whole test -- more reliable than the ID's own
         * prefix, which would have to be kept up to date with every new
         * range Sony issues. */
        if (asfo_string(sfo, sfo_len, "TITLE_ID",
                        out->title_id, sizeof out->title_id) == ASFO_OK) {
            out->platform = ASAVE_PS4;
            first_of(sfo, sfo_len, out->name, sizeof out->name, name_ps4, 2);
            first_of(sfo, sfo_len, out->detail, sizeof out->detail, det_ps4, 2);
            asfo_string(sfo, sfo_len, "SAVEDATA_DIRECTORY",
                        out->directory, sizeof out->directory);
        } else {
            char parent[ASAVE_DIR_LEN];

            out->platform = ASAVE_PSV;
            /* 9 bytes at 0x28 inside PARAMS. asfo_blob rather than a string
             * read because the blob is not NUL-terminated at that point --
             * the ID runs straight into the fields after it. */
            if (asfo_blob(sfo, sfo_len, "PARAMS", 0x28,
                          (uint8_t *)out->title_id, 9) == ASFO_OK)
                out->title_id[9] = '\0';

            /* "/PCSE00608" -- the fallback, for a save whose PARAMS blob is
             * short or missing. */
            if (!title_id_ok(out->title_id) &&
                asfo_string(sfo, sfo_len, "PARENT_DIRECTORY",
                            parent, sizeof parent) == ASFO_OK && parent[0] == '/')
                title_id_from_dir(parent + 1, out->title_id);

            /* TITLE is usually empty on a Vita save. Left that way rather
             * than filled with something invented: the caller looks the title
             * ID up in the patch database, which does know the name. */
            asfo_string(sfo, sfo_len, "TITLE", out->name, sizeof out->name);
            trim(out->name);
            first_of(sfo, sfo_len, out->detail, sizeof out->detail, det_psv, 2);
        }

        if (!title_id_ok(out->title_id))
            out->title_id[0] = '\0';
        out->encrypted = 0;
        return ASAVE_OK;
    }

    /* At the save's own root, so PSP or PS3 -- and every save of either has a
     * SAVEDATA_DIRECTORY. Requiring it is what keeps a GAME's PARAM.SFO out
     * of the list: memory cards and hard drives are full of those, from discs
     * and homebrew EBOOTs, and they carry BOOTABLE and a version instead. */
    if (asfo_string(sfo, sfo_len, "SAVEDATA_DIRECTORY",
                    out->directory, sizeof out->directory) != ASFO_OK
        || !out->directory[0])
        return ASAVE_ERR_WHICH;

    /*
     * A PS4 save's SFO that is not in sce_sys/.
     *
     * TITLE_ID is a key no PSP, PS3 or Vita SAVEDATA carries -- across the
     * apollo-saves database, 0 of 632 PS3, 0 of 1,262 PSP and 0 of 333 Vita
     * saves has one, while all 339 PS4 saves do -- so when it turns up beside
     * a SAVEDATA_DIRECTORY the file is a PS4 save's, wherever it is sitting.
     * Believing the keys over the location matters because a PS3 title ID
     * comes from the directory name, and a PS4 save's directory is named after
     * the slot ("Save0001"), which yields no ID at all: the alternative is a
     * nameless row for a save this can name exactly.
     */
    if (asfo_string(sfo, sfo_len, "TITLE_ID",
                    out->title_id, sizeof out->title_id) == ASFO_OK
        && title_id_ok(out->title_id)) {
        out->platform = ASAVE_PS4;
        first_of(sfo, sfo_len, out->name, sizeof out->name, name_ps4, 2);
        first_of(sfo, sfo_len, out->detail, sizeof out->detail, det_ps4, 2);
        out->encrypted = 0;
        return ASAVE_OK;
    }

    title_id_from_dir(out->directory, out->title_id);

    /* SAVEDATA_PARAMS is the PSP's savedata mode and hashes. No other console
     * writes it, and every PSP save has it -- apsp_* refuses a save without
     * one outright. */
    if (asfo_find(sfo, sfo_len, "SAVEDATA_PARAMS", NULL, NULL, NULL, NULL) == ASFO_OK) {
        out->platform = ASAVE_PSP;
        first_of(sfo, sfo_len, out->name, sizeof out->name, name_psp, 2);
        first_of(sfo, sfo_len, out->detail, sizeof out->detail, det_psp, 3);
    } else {
        /* A PARAM.PFD beside it would confirm this, but its absence does not
         * deny it: a save that has been unwrapped, or copied without the PFD,
         * is still a PS3 save and still has patches. So `has_pfd` is not
         * consulted -- it is taken on the caller's word. */
        out->platform = ASAVE_PS3;
        first_of(sfo, sfo_len, out->name, sizeof out->name, name_ps3, 1);
        first_of(sfo, sfo_len, out->detail, sizeof out->detail, det_ps3, 3);
    }

    out->encrypted = 1;
    return ASAVE_OK;
}

/* One field of a tab-separated line: its start and length, or 0 at the end of
 * the line. `at` is advanced past the separator. */
static size_t field(const char *text, size_t len, size_t *at, const char **start)
{
    size_t begin = *at;

    *start = text + begin;
    while (*at < len && text[*at] != '\t' && text[*at] != '\n' && text[*at] != '\r')
        (*at)++;
    {
        const size_t n = *at - begin;
        if (*at < len && text[*at] == '\t')
            (*at)++;
        return n;
    }
}

/* Case-insensitive compare of a field against a NUL-terminated string. */
static int same_text(const char *a, size_t a_len, const char *b)
{
    size_t i;

    for (i = 0; i < a_len; i++) {
        char x = a[i], y = b[i];

        if (!y)
            return 0;
        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y)
            return 0;
    }
    return b[a_len] == '\0';
}

int asave_name_from_db(const char *text, size_t len,
                       const char *platform, const char *title_id,
                       char *out, size_t out_len)
{
    size_t at = 0;

    if (!out || !out_len)
        return ASAVE_ERR_WHICH;
    out[0] = '\0';
    if (!text || !platform || !title_id || !title_id[0])
        return ASAVE_ERR_WHICH;

    while (at < len) {
        const char *plat, *id, *name;
        size_t plat_len, id_len, name_len;

        plat_len = field(text, len, &at, &plat);
        id_len   = field(text, len, &at, &id);
        name_len = field(text, len, &at, &name);

        /* To the end of the line, whatever is left of it: a name is the last
         * field, but a fourth one would otherwise be read as the next
         * record's platform. make-bundle.py does not emit one -- this is so
         * that a file which did could not make this parse nonsense. */
        while (at < len && text[at] != '\n' && text[at] != '\r')
            at++;
        while (at < len && (text[at] == '\n' || text[at] == '\r'))
            at++;

        if (!name_len || !same_text(plat, plat_len, platform) || !same_text(id, id_len, title_id))
            continue;

        /* Refused rather than truncated: half a game's name in a list is
         * worse than the folder name it would otherwise show. */
        if (name_len >= out_len)
            return ASAVE_ERR_WHICH;
        memcpy(out, name, name_len);
        out[name_len] = '\0';
        return ASAVE_OK;
    }
    return ASAVE_ERR_WHICH;
}
