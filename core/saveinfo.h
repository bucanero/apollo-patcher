/*
 * saveinfo - what console wrote this save, and what is it called.
 *
 * A front-end that lets someone point at a folder full of saves has to answer
 * two questions per candidate, and neither is as simple as the file name:
 *
 *   WHICH CONSOLE   decides whether there is an encryption layer under the
 *                   patch (PSP and PS3 have one; PS4 and Vita saves come off
 *                   the console already decrypted) and which patches apply
 *   WHICH GAME      decides which patch to load, and is what a list shows
 *
 * Both come out of the save's own PARAM.SFO, and each console fills one in
 * differently -- which is the whole reason this file exists rather than the
 * caller reading three keys and guessing:
 *
 *   PSP   PARAM.SFO beside the data files. SAVEDATA_PARAMS is the giveaway:
 *         no other console has it. Title ID is the first 9 characters of
 *         SAVEDATA_DIRECTORY ("ULUS10391DATA00"), name is TITLE.
 *   PS3   PARAM.SFO beside the data files, no SAVEDATA_PARAMS. Title ID is
 *         again SAVEDATA_DIRECTORY's first 9 ("BLUS30917-AUTOSAVE"), name is
 *         TITLE. PARAM.PFD sits beside it and holds the encryption.
 *   PS4   sce_sys/param.sfo. Says TITLE_ID outright, and MAINTITLE is the
 *         game's real name -- the only console here that stores it plainly.
 *   PSV   sce_sys/param.sfo, and it says neither. TITLE is usually empty and
 *         there is no TITLE_ID key at all; the title ID is 9 bytes at 0x28
 *         inside the binary PARAMS blob, with PARENT_DIRECTORY ("/PCSE00608")
 *         as the fallback. So a Vita save is NAMELESS until the patch
 *         database is asked about its title ID -- which the caller does, and
 *         is why the name here is allowed to come back empty.
 *
 * PS1 and PS2 are the exception to all of the above, because they predate
 * PARAM.SFO entirely: those consoles kept saves in memory-card blocks, and a
 * save only becomes a file when a PS3 exports it as a .PSV. So they are
 * identified from the container instead -- asave_identify_psvcard(), which reads
 * what core/psvcard.c parsed rather than an SFO that was never written.
 *
 * Nothing in here touches a filesystem: the caller reads the bytes and says
 * where they came from. That keeps the classification testable against a
 * handful of real SFOs, and usable from the web build, which has no
 * filesystem to walk in the first place.
 */
#ifndef APOLLO_SAVEINFO_H
#define APOLLO_SAVEINFO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ASAVE_OK        =  0,
    ASAVE_ERR_SFO   = -1,   /* not a PARAM.SFO                              */
    ASAVE_ERR_WHICH = -2    /* parses, but names no console this recognises */
};

typedef enum {
    ASAVE_UNKNOWN = 0,
    ASAVE_PSP,
    ASAVE_PS3,
    ASAVE_PS4,
    /* Spelled out, not ASAVE_PSV: a .PSV FILE is an unrelated thing holding a
     * PS1 or PS2 save (core/psvcard.h), and the two would otherwise be one
     * typo apart with nothing to catch it. */
    ASAVE_PSVITA,
    /* The two that have no PARAM.SFO at all: see asave_identify_psvcard(). */
    ASAVE_PS1,
    ASAVE_PS2
} asave_platform_t;

/* Where the SFO was found, which is itself evidence: the two consoles that
 * keep it in sce_sys/ are exactly the two whose saves are already decrypted. */
typedef enum {
    ASAVE_AT_ROOT = 0,   /* <save>/PARAM.SFO      -- PSP or PS3 */
    ASAVE_AT_SCE  = 1    /* <save>/sce_sys/param.sfo -- PS4 or PSV */
} asave_where_t;

#define ASAVE_TITLE_ID_LEN  10   /* 9 characters and a terminator */
#define ASAVE_NAME_LEN     128
#define ASAVE_DIR_LEN       65

typedef struct {
    asave_platform_t platform;
    char title_id[ASAVE_TITLE_ID_LEN];   /* "ULUS10391", or empty if unknown */
    char name[ASAVE_NAME_LEN];           /* the game, as the SFO names it    */
    char detail[ASAVE_NAME_LEN];         /* the slot: "AUTOSAVE", "Slot 1"   */
    char directory[ASAVE_DIR_LEN];       /* SAVEDATA_DIRECTORY, when present */
    int  encrypted;                      /* is there a console layer below   */
} asave_info_t;

/*
 * Identify one save from its PARAM.SFO.
 *
 * `has_pfd` says whether a PARAM.PFD sits beside it; pass 0 when unknown. It
 * only ever confirms PS3 -- a root SFO without SAVEDATA_PARAMS is taken as
 * PS3 either way, because a PS3 save whose PFD was stripped is still a PS3
 * save and still has patches.
 *
 * ASAVE_ERR_WHICH for an SFO that parses but is not a save's. Two kinds:
 *
 *   - a game's own PARAM.SFO, from a disc or a homebrew EBOOT, which carries
 *     BOOTABLE and PSP_SYSTEM_VER and turns up all over a memory card. Kept
 *     out by requiring SAVEDATA_DIRECTORY.
 *   - anything whose CATEGORY names something other than savedata -- add-on
 *     content (`ac`), game data (`gd`). Vita DLC in particular has its own
 *     sce_sys/param.sfo carrying a TITLE_ID, which is otherwise exactly what
 *     tells a PS4 save from a Vita one.
 */
int asave_identify(const uint8_t *sfo, size_t sfo_len,
                   asave_where_t where, int has_pfd, asave_info_t *out);

/* "PSP", "PS3", "PS4", "PSV", "PS1", "PS2" -- the patch database's own
 * platform tags, so a caller can match one straight against
 * patchdb_entry_t::platform. "?" for ASAVE_UNKNOWN.
 *
 * "PSV" here is the PS VITA: it is the tag the database is keyed by, so it is
 * a string on disk and stays as it is even though the enumerator behind it is
 * ASAVE_PSVITA. A save out of a .PSV FILE answers "PS1" or "PS2". */
const char *asave_platform_name(asave_platform_t platform);

/*
 * A game's name, looked up by title ID in the bundled catalogue.
 *
 * For the saves that name no game themselves. A Vita save is the reason this
 * exists: it carries no TITLE_ID key and its TITLE is usually empty, so
 * without a catalogue it can only be listed under its folder. The patch
 * database knows the 123 Vita titles it has patches for; the catalogue knows
 * 4581.
 *
 * `text` is titles.tsv out of apollo-patches.zip, which tools/make-bundle.py
 * normalises from the per-console files in the patch checkout. One record per
 * line, tab separated, sorted:
 *
 *     PSV<TAB>PCSE00608<TAB>Resident Evil: Revelations 2
 *
 * Buffer in, buffer out, like every other database lookup here: the front-end
 * owns the file, and the web build has no filesystem to read one from.
 *
 * ASAVE_ERR_WHICH when the catalogue has no such title; `out` is emptied.
 */
/*
 * Identify a PS1 or PS2 save from the .PSV container holding it.
 *
 * The container is the only evidence there is -- no PARAM.SFO was ever
 * written for these -- so this takes what core/psvcard.c read out of it:
 * `dir_name` is the save's own memory-card directory ("BASLUS-20216"), `type`
 * is APSVC_TYPE_PS1 or APSVC_TYPE_PS2, and `title` is the name the console's
 * save list showed, already converted out of Shift-JIS.
 *
 * The title ID comes from `dir_name` and is AUTHORITATIVE: it can disagree
 * with the folder the save was filed under, and when it does the container is
 * right (see apsvc_title_id).
 *
 * `title` becomes the DETAIL, not the name -- it is the slot ("Devil May Cry
 * SaveData"), and the game's own name comes from the catalogue like a Vita's
 * does. It may be NULL.
 *
 * `encrypted` is always 0: neither console encrypted its saves, and the
 * container's signature is a signature, not a cipher.
 *
 * ASAVE_ERR_WHICH when `dir_name` names no title ID this recognises.
 */
int asave_identify_psvcard(const char *dir_name, int type, const char *title,
                       asave_info_t *out);

int asave_name_from_db(const char *text, size_t len,
                       const char *platform, const char *title_id,
                       char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* APOLLO_SAVEINFO_H */
