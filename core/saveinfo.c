/* See saveinfo.h. */
#include <string.h>

#include "saveinfo.h"
#include "sfo.h"

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
     * TITLE_ID is a key no PSP or PS3 SAVEDATA carries -- 0 of the 172 real
     * PS3 saves this was checked against has one -- so when it turns up beside
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
