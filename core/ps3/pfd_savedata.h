/*
 * pfd_savedata - the PS3's own savedata encryption, as buffers.
 *
 * This is the layer BELOW everything else Apollo does to a PS3 save, the same
 * role core/psp/ plays for the PSP. A save folder off a PS3 looks like this:
 *
 *   BLUS30724PROFILE/
 *     PARAM.PFD        the protected file database: one entry per protected
 *                      file, each carrying that file's size, its AES key and
 *                      four HMACs, plus two HMACs covering the whole table
 *     PARAM.SFO        listed in the PFD and hashed, but never encrypted
 *     5BC50A32.DAT     encrypted, keyed by a per-game "secure file ID"
 *     ICON0.PNG        not in the PFD at all, so not protected
 *
 * A .savepatch addresses the plaintext inside 5BC50A32.DAT. Handing the patch
 * engine the file as it sits on the memory card gives garbage, so a desktop or
 * browser front-end has to take this layer off first and put it back after.
 *
 * Buffers in, buffers out, stdio-free: there is no filesystem in a browser
 * tab. Progress goes through dbglogger_log(), which apollo_ctrl.c routes to
 * the front-end's log panel.
 *
 * WHAT COUNTS AS PROTECTED
 *
 * The PFD's entry table, and nothing else. A file with an entry is encrypted
 * (bar PARAM.SFO); a file without one is stored in the clear and needs no
 * handling at all. Games that never encrypt anything ship a PFD holding only
 * PARAM.SFO, so apfd_find() answers the question outright -- there is no need
 * to consult the key database to find out, and no way for a stale database to
 * make the answer wrong.
 *
 * WHAT YOU NEED
 *
 *   PARAM.PFD        always. It carries the per-file key, and encryption
 *                    writes back to it -- see apfd_encrypt().
 *   a secure file ID  16 bytes, per game and sometimes per file. apollo-patches
 *                    ships the database (PS3/games.conf); apfd_sfid_from_conf()
 *                    resolves one. PARAM.SFO and the four trophy files use
 *                    built-in keys instead and need none.
 *
 * WHAT YOU DO NOT NEED, TO PATCH A SAVE
 *
 * The console's IDPS. A PFD entry holds four hashes, but pfd_update on the
 * console only ever computes the last three for PARAM.SFO -- every other entry
 * gets hash 0, which is keyed by the secure file ID. And of PARAM.SFO's four,
 * only hash 1 is keyed by the console ID. So patching a game data file and
 * resigning the PFD around it touches nothing console-bound, and the save
 * stays valid on the console it came from.
 *
 * WHAT YOU DO NEED, TO MOVE ONE
 *
 * That IDPS, if the save is to be re-bound to a DIFFERENT console. Naming one
 * through apfd_set_console() makes apfd_update_file() rewrite all four of
 * PARAM.SFO's hashes instead of the first, which is the PFD half of moving a
 * save. It is only the PFD half: PARAM.SFO also carries account fields of its
 * own, and re-signing those is a separate job this does not do.
 *
 * Bounds. Every offset in a PARAM.PFD is read out of the file itself, and one
 * of these reaches you from a browser tab as readily as from your own console,
 * so each is checked against the real length before it is followed.
 */
#ifndef APOLLO_PS3_PFD_SAVEDATA_H
#define APOLLO_PS3_PFD_SAVEDATA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes. 0 is success; everything else is negative, and
 * apfd_strerror() turns it into something a user can read. */
enum {
    APFD_OK           =  0,
    APFD_ERR_PFD      = -1,  /* not a PARAM.PFD, or one that does not parse  */
    APFD_ERR_VERSION  = -2,  /* a PFD version this does not implement        */
    APFD_ERR_NO_ENTRY = -3,  /* that name has no entry in the PFD            */
    APFD_ERR_SIZE     = -4,  /* input too short, or output buffer too small  */
    APFD_ERR_NO_KEY   = -5,  /* no secure file ID for this save in games.conf */
    APFD_ERR_PLAIN    = -6,  /* that entry is not encrypted (PARAM.SFO)      */
    APFD_ERR_ARG      = -7,  /* a NULL where one is not allowed              */
    APFD_ERR_MEM      = -8,  /* out of memory                                */
    APFD_ERR_HASH     = -9,  /* the entry's hash does not match the file      */
};

#define APFD_SFID_LEN       0x10  /* a secure file ID                        */
#define APFD_CONSOLE_ID_LEN 0x10  /* the IDPS                                */
#define APFD_DHK_LEN        0x10  /* a disc hash key                         */
#define APFD_NAME_LEN  65    /* an entry's file name field, NUL included     */
#define APFD_ALIGN     16    /* encrypted files are padded up to this        */

const char *apfd_strerror(int err);

/* ---- which console the save is for -------------------------------------- */

/*
 * The identity PARAM.SFO's console-bound hashes are keyed by.
 *
 *   console_id      the IDPS, 16 bytes. All zero means "not set", which is the
 *                   default and leaves every save bound to the console it came
 *                   off.
 *   disc_hash_key   per game, from games.conf; apfd_dhk_from_conf() reads one.
 *                   All zero selects a built-in fallback, which is what all
 *                   but eight of that file's 1819 sections rely on.
 *   user_id         the PS3 user number (1 for the first account). Reaches
 *                   only a TROPHY folder's hashes; savedata ignores it.
 */
typedef struct {
    uint8_t  console_id[APFD_CONSOLE_ID_LEN];
    uint8_t  disc_hash_key[APFD_DHK_LEN];
    uint32_t user_id;
} apfd_console_t;

/*
 * Name the console a save belongs to, or NULL to go back to naming none.
 *
 * This is a deliberate, destructive act: with one set, apfd_update_file() on
 * PARAM.SFO rewrites the three hashes that bind the save, so a save that
 * worked on the console it came from now works on this one INSTEAD. With none
 * set -- the default -- those three are left exactly as they were, which is
 * what patching a save in place wants.
 *
 * A console_id of all zeroes counts as naming none.
 */
void apfd_set_console(const apfd_console_t *console);

/* The console in effect, and whether one is named at all. */
int  apfd_get_console(apfd_console_t *out);

/* ---- reading a PARAM.PFD ------------------------------------------------ */

/* Does this parse as a PARAM.PFD whose tables fit inside the length given?
 * APFD_OK, or the reason why not. Cheap; call it before showing any UI. */
int apfd_valid(const uint8_t *pfd, size_t len);

/* 3 or 4 -- the two the console writes and the only two handled here -- or a
 * negative error. The version selects how the hash key is derived; nothing
 * else about the format moves. */
int apfd_version(const uint8_t *pfd, size_t len);

/* Nonzero when the entries name trophy files rather than savedata. Worth
 * surfacing because a trophy folder is not something a .savepatch addresses,
 * so a front-end can say so instead of offering to patch it. */
int apfd_is_trophy(const uint8_t *pfd, size_t len);

/*
 * The protected files. Enumerate with a count and an index rather than
 * returning a list, so a front-end can render it without owning any memory.
 *
 * apfd_entry_count() returns a count (>= 0) or a negative error.
 * apfd_entry_size() is the file's logical length -- what the game wrote. On
 * disk an encrypted file is that rounded up to APFD_ALIGN.
 */
int      apfd_entry_count(const uint8_t *pfd, size_t len);
int      apfd_entry_name(const uint8_t *pfd, size_t len, int index,
                         char *out, size_t out_len);
long long apfd_entry_size(const uint8_t *pfd, size_t len, int index);

/*
 * The index of the entry called `name`, or APFD_ERR_NO_ENTRY.
 *
 * This is the protected/unprotected test. Names match case-insensitively, so a
 * file renamed on the way off the console is still found.
 */
int apfd_find(const uint8_t *pfd, size_t len, const char *name);

/*
 * Nonzero when this entry carries a built-in key -- PARAM.SFO and the four
 * trophy files. For those, `sfid` is ignored everywhere below and a front-end
 * should not ask for one.
 */
int apfd_entry_has_builtin_key(const char *name);

/* ---- the secure file ID ------------------------------------------------- */

/* Parse 32 hex digits (the games.conf form, and what a user would paste).
 * Returns APFD_OK or an error. */
int apfd_sfid_from_hex(const char *hex, uint8_t sfid[APFD_SFID_LEN]);

/*
 * Look an ID up in apollo-patches' PS3/games.conf, which is 1800-odd sections
 * of
 *
 *     ; "DiRT 3"
 *     [BLUS30724PROFILE/BLUS30975PROFILE/BLES01287PROFILE]
 *     ;disc_hash_key=
 *     secure_file_id:*=166A717AAF32DFF265F28EE3F3491A52
 *
 * `directory` is the save folder name; `file_name` is the file inside it.
 * Both are needed, because both levels of the lookup discriminate.
 *
 * SECTION: the bracketed ids are SAVE DIRECTORY names, not title ids, and a
 * game can have several. One matches when it is a PREFIX of `directory`,
 * case-insensitively, and the LONGEST match wins. That is not a detail: DiRT 3
 * files both BLUS30724 and BLUS30724PROFILE with DIFFERENT keys, so a
 * first-match rule reads a profile save with the game's key and fails every
 * hash in the folder. (apollo-ps3 matches the 9-character title id with
 * strstr() and hits exactly that.) Empty ids -- six sections have one, from a
 * stray separator -- are skipped, since an empty prefix matches everything.
 *
 * FILE: the patterns inside a section are shell wildcards matched against
 * `file_name`, case-insensitively, and the FIRST match in file order wins.
 * Order carries meaning here: Devil May Cry lists `DATA` before `*` and the
 * two keys differ.
 *
 * Returns APFD_OK and fills `sfid`, or APFD_ERR_NO_KEY when nothing matches.
 * `id_out`, when given, receives the section id that matched -- worth showing,
 * since it says WHICH save folder the key was filed under.
 */
int apfd_sfid_from_conf(const char *text, size_t len,
                        const char *directory, const char *file_name,
                        uint8_t sfid[APFD_SFID_LEN], char *id_out, size_t id_cap);

/*
 * The disc hash key the same section names, for apfd_console_t. Matters only
 * when a console is named, and only for the eight games that carry one --
 * APFD_ERR_NO_KEY is the ordinary answer and means "use the fallback".
 */
int apfd_dhk_from_conf(const char *text, size_t len, const char *directory,
                       uint8_t dhk[APFD_DHK_LEN]);

/* ---- sizes -------------------------------------------------------------- */

/* What the other direction produces. apfd_decrypted_size() is the entry's
 * logical size, so it needs the PFD; it returns a negative error if the name
 * has no entry. apfd_encrypted_size() is just the alignment. */
long long apfd_decrypted_size(const uint8_t *pfd, size_t len, const char *name);
size_t    apfd_encrypted_size(size_t plain_len);

/* ---- decrypt / encrypt -------------------------------------------------- */

/*
 * Unwrap one protected file. `out` needs apfd_decrypted_size() bytes, and
 * receives exactly that many -- the alignment padding is dropped, because it
 * is not part of the file the game wrote.
 *
 * `in` is the file as it sits on disk, which is the aligned length -- anything
 * shorter is APFD_ERR_SIZE. The cipher works on whole blocks, so a truncated
 * last block corrupts the plaintext bytes before it as well, and those are
 * inside the file.
 *
 * The PFD is not modified. APFD_ERR_PLAIN for PARAM.SFO, which is listed but
 * never encrypted.
 */
int apfd_decrypt(const uint8_t *pfd, size_t pfd_len, const char *name,
                 const uint8_t *in, size_t in_len,
                 const uint8_t sfid[APFD_SFID_LEN],
                 uint8_t *out, size_t out_cap, size_t *out_len);

/*
 * Wrap one protected file back up. `out` needs apfd_encrypted_size(in_len)
 * bytes and receives that many, zero-padded to the alignment.
 *
 * THIS REWRITES THE PARAM.PFD, in place, and the caller has to keep it: the
 * entry's size and hash change, and the signatures over the whole table are
 * regenerated to match. A save put back with a stale PARAM.PFD will not load.
 * So encryption always produces TWO outputs -- the data file and the PFD --
 * and a front-end that offers only the first is handing back a broken save.
 *
 * On padding: the console leaves whatever was in memory in the bytes past the
 * file's logical end, and this writes zeroes there instead. The hash is
 * recomputed over what we write, so the save is valid either way; it does mean
 * re-encrypting an untouched file does not reproduce the original bytes.
 */
int apfd_encrypt(uint8_t *pfd, size_t pfd_len, const char *name,
                 const uint8_t *in, size_t in_len,
                 const uint8_t sfid[APFD_SFID_LEN],
                 uint8_t *out, size_t out_cap, size_t *out_len);

/*
 * Record a listed file that is NOT encrypted, in place -- PARAM.SFO, or a game
 * that lists plaintext files. The entry's size becomes `len` and its hash is
 * taken over those same bytes, which is only right when the file is stored as
 * it is; an encrypted file goes through apfd_encrypt(), which knows that the
 * entry records the logical size while the hash covers the padded bytes.
 *
 * APFD_ERR_NO_ENTRY if the file is not listed, which is the right answer: an
 * unlisted file is unprotected and needs nothing recorded.
 *
 * For PARAM.SFO this is also the re-binding call: with a console named through
 * apfd_set_console(), it rewrites all four of that entry's hashes rather than
 * the first, moving the save to that console.
 */
int apfd_update_file(uint8_t *pfd, size_t pfd_len, const char *name,
                     const uint8_t *plain, size_t plain_len,
                     const uint8_t sfid[APFD_SFID_LEN]);

/*
 * Does the entry's recorded hash match these bytes? `disk` is the file exactly
 * as it sits on disk -- ciphertext for an encrypted entry, since the console
 * hashes what it can read rather than what it means.
 *
 * APFD_OK when they agree, APFD_ERR_HASH when they do not, or the usual errors.
 * Worth calling before patching: a save whose PARAM.PFD already disagrees with
 * its files was damaged before it got here, and re-signing would bless it.
 */
int apfd_verify_file(const uint8_t *pfd, size_t pfd_len, const char *name,
                     const uint8_t *disk, size_t disk_len,
                     const uint8_t sfid[APFD_SFID_LEN]);

/*
 * Regenerate the signatures over the entry table, in place, without touching
 * any entry.
 *
 * apfd_encrypt() and apfd_update_file() both end with this, so it is only
 * needed on its own to repair a PFD whose entries something else edited.
 */
int apfd_resign(uint8_t *pfd, size_t pfd_len);

#ifdef __cplusplus
}
#endif

#endif /* APOLLO_PS3_PFD_SAVEDATA_H */
