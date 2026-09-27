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
    ASAVE_PSV
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

/* "PSP", "PS3", "PS4", "PSV" -- the patch database's own platform tags, so a
 * caller can match one straight against patchdb_entry_t::platform. "?" for
 * ASAVE_UNKNOWN. */
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
int asave_name_from_db(const char *text, size_t len,
                       const char *platform, const char *title_id,
                       char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* APOLLO_SAVEINFO_H */
