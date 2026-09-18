/*
 * psp_savedata - the PSP's own savedata encryption, as buffers.
 *
 * This is the layer BELOW everything else Apollo does to a PSP save. A file
 * pulled off a Memory Stick is wrapped twice:
 *
 *   MHP2NDG.BIN (1,483,024 bytes)  <- this layer: KIRK, keyed by the game key
 *     MHP2NDG.BIN (1,483,008)      <- Monster Hunter's own encryption, which
 *                                     is what a .savepatch knows how to undo
 *       plaintext
 *
 * The console apps (apollo-psp, apollo-vita) have always handled the outer
 * wrap; the desktop and web front-ends could not, so anyone arriving with a
 * real save got garbage out of the patch engine. That is what this fixes.
 *
 * Everything here is buffer in, buffer out, and stdio-free -- there is no
 * filesystem in a browser tab. Progress goes through dbglogger_log(), which
 * apollo_ctrl.c routes to the front-end's log sink, so PSP output lands in the
 * same panel as the engine's.
 *
 * WHAT YOU NEED, for either direction:
 *
 *   PARAM.SFO   the save's own metadata file, sitting next to the data file.
 *               It carries SAVEDATA_PARAMS, whose first byte selects the
 *               encryption mode, and SAVEDATA_FILE_LIST, which names the
 *               files that are wrapped at all (ICON0.PNG and PIC1.PNG are
 *               not). Encryption also WRITES to it -- see apsp_encrypt().
 *   the game key  16 bytes, per title. apollo-patches ships a database of
 *               them (PSP/gamekeys.txt), keyed by the save directory name,
 *               which apsp_sfo_directory() reads straight out of the SFO. A
 *               key of all zeroes is legitimate and means "unkeyed".
 *
 * Bounds. Every offset in a PARAM.SFO is read out of the file itself, and one
 * of these reaches you from a browser tab as readily as from your own Memory
 * Stick, so each is checked against the real length before it is followed.
 */
#ifndef APOLLO_PSP_SAVEDATA_H
#define APOLLO_PSP_SAVEDATA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes. 0 is success; everything else is negative, and
 * apsp_strerror() turns it into something a user can read. */
enum {
    APSP_OK            =  0,
    APSP_ERR_SFO       = -1,  /* not a PARAM.SFO, or one that does not parse */
    APSP_ERR_NO_PARAM  = -2,  /* the SFO carries no SAVEDATA_PARAMS          */
    APSP_ERR_NO_FILE   = -3,  /* that name is not in SAVEDATA_FILE_LIST      */
    APSP_ERR_SIZE      = -4,  /* input too short, or output buffer too small */
    APSP_ERR_MODE      = -5,  /* SAVEDATA_PARAMS asks for a mode we have not */
    APSP_ERR_ARG       = -6,  /* a NULL where one is not allowed             */
    APSP_ERR_MEM       = -7,  /* out of memory                               */
    APSP_ERR_NO_KEY    = -8,  /* no game key for this save in the database    */
};

#define APSP_KEY_LEN     0x10  /* a game key                                 */
#define APSP_HEADER_LEN  0x10  /* the IV an encrypted file carries up front  */
#define APSP_NAME_LEN    0x0D  /* a FILE_LIST entry's name field, NUL incl.  */

const char *apsp_strerror(int err);

/* ---- PARAM.SFO ---------------------------------------------------------- */

/* Does this parse as a PARAM.SFO with the savedata parameters in it?
 * APSP_OK, or the reason why not. Cheap; call it before showing any UI. */
int apsp_sfo_valid(const uint8_t *sfo, size_t sfo_len);

/*
 * SAVEDATA_DIRECTORY, the save's folder name ("ULUS10391", "ULJM05500DATA00").
 *
 * This is the lookup key for the game-key database: apollo-patches'
 * gamekeys.txt matches an entry against the START of this string, so a game
 * whose saves live in several folders needs one entry. Copies at most
 * out_len-1 bytes and NUL-terminates. Returns APSP_OK or an error.
 */
int apsp_sfo_directory(const uint8_t *sfo, size_t sfo_len, char *out, size_t out_len);

/*
 * The encrypted files, from SAVEDATA_FILE_LIST -- which is the authoritative
 * answer to "what in this folder is wrapped". Enumerate with a count and an
 * index rather than returning the list, so a front-end can render it without
 * owning any memory.
 *
 * apsp_sfo_file_count() returns a count (>= 0) or a negative error.
 */
int apsp_sfo_file_count(const uint8_t *sfo, size_t sfo_len);
int apsp_sfo_file_name(const uint8_t *sfo, size_t sfo_len, int index,
                       char *out, size_t out_len);

/*
 * SAVEDATA_PARAMS[0], the mode byte the originating console wrote (0..255), or
 * a negative error.
 *
 * Worth surfacing because it answers "does this save need a game key". Bit
 * 0x20 selects SD mode 3 and 0x40 selects mode 5; a byte with NEITHER set
 * describes a save that can only be unkeyed, since the one remaining mode is
 * the one an all-zero key selects. A front-end can say so up front instead of
 * asking for a key that does not exist.
 */
int apsp_sfo_mode(const uint8_t *sfo, size_t sfo_len);

/* The bits of the mode byte that name a keyed mode; none set means unkeyed. */
#define APSP_MODE_KEYED 0x60

/* ---- the game key ------------------------------------------------------- */

/*
 * Pull a key out of a dumper's output file. Two shapes are recognised, both
 * of which people actually have:
 *
 *   0x10 bytes   SGKeyDumper -- the key, and nothing else.
 *   0x600 bytes  SGDeemer    -- the key at 0x5DC.
 *
 * Anything else is APSP_ERR_SIZE and `key` is left alone.
 */
int apsp_key_from_buffer(const uint8_t *buf, size_t len, uint8_t key[APSP_KEY_LEN]);

/* Parse 32 hex digits (the gamekeys.txt form). Returns APSP_OK or an error. */
int apsp_key_from_hex(const char *hex, uint8_t key[APSP_KEY_LEN]);

/*
 * Look a key up in apollo-patches' PSP/gamekeys.txt, which is 280-odd lines of
 *
 *     ; a comment
 *     ULUS10391=4A1FF359AEB6EFF81CA8CB23BCA57BB3
 *
 * `directory` is the save folder name -- SAVEDATA_DIRECTORY, which
 * apsp_sfo_directory() reads straight out of the PARAM.SFO. An entry matches
 * when its id is a PREFIX of that, case-insensitively, so one entry covers a
 * game whose saves live in several folders ("ULJM05500DATA00" and
 * "ULJM05500SAVE01" alike).
 *
 * The LONGEST match wins. That is not a detail: the database holds both
 * NPJJ30022 and NPJJ30022GAME1, with different keys, and a first-match rule
 * answers correctly only for as long as nobody reorders the file. (apollo-psp
 * matches in file order and gets away with it because the specific entry
 * happens to be written first.)
 *
 * Returns APSP_OK and fills `key`, or APSP_ERR_NO_KEY when nothing matches.
 * `id_out`, when given, receives the entry id that matched -- worth showing,
 * since it says WHICH game the key was filed under.
 */
int apsp_key_from_db(const char *text, size_t len, const char *directory,
                     uint8_t key[APSP_KEY_LEN], char *id_out, size_t id_cap);

/* All zeroes. Not an error: an unkeyed game encrypts in mode 1. */
int apsp_key_is_null(const uint8_t key[APSP_KEY_LEN]);

/* ---- sizes -------------------------------------------------------------- */

/* What the other direction produces. Both return 0 when the input cannot be
 * a valid length, which for decrypt means "shorter than the IV it must carry". */
size_t apsp_decrypted_size(size_t encrypted_len);
size_t apsp_encrypted_size(size_t plain_len);

/* ---- decrypt / encrypt -------------------------------------------------- */

/*
 * Unwrap one savedata file. `out` needs apsp_decrypted_size(in_len) bytes.
 *
 * Only SAVEDATA_PARAMS is read from the SFO -- the file's own name does not
 * come into it, so a file renamed on the way out of the console still works.
 * The SFO is not modified.
 */
int apsp_decrypt(const uint8_t *sfo, size_t sfo_len,
                 const uint8_t *in, size_t in_len,
                 const uint8_t key[APSP_KEY_LEN],
                 uint8_t *out, size_t out_cap, size_t *out_len);

/*
 * Wrap one savedata file back up. `out` needs apsp_encrypted_size(in_len).
 *
 * THIS REWRITES THE PARAM.SFO, in place, and the caller has to keep it: the
 * file's own hash lands in its SAVEDATA_FILE_LIST entry, and the two
 * SFO-wide hashes are regenerated over the result. A save put back with a
 * stale PARAM.SFO will not load. So encryption always produces TWO outputs --
 * the data file and the SFO -- and a front-end that offers only the first is
 * handing the user a broken save.
 *
 * `name` must appear in SAVEDATA_FILE_LIST, because that entry is where the
 * hash goes; APSP_ERR_NO_FILE if it does not.
 *
 * On the hashes: for savedata modes 4 and 6 one of them is derived from the
 * console's Fuse ID, which a desktop or a browser does not have (see
 * kirk_engine.h). The value written here therefore differs from the one the
 * originating PSP wrote, and the PSP loads the save anyway. Decryption is
 * unaffected: it never touches the fuse.
 */
int apsp_encrypt(uint8_t *sfo, size_t sfo_len, const char *name,
                 const uint8_t *in, size_t in_len,
                 const uint8_t key[APSP_KEY_LEN],
                 uint8_t *out, size_t out_cap, size_t *out_len);

/*
 * Regenerate the PARAM.SFO hashes without touching any data file, in place.
 *
 * This is what makes an already-plaintext save loadable again after something
 * else edited it -- a save that was never encrypted, or one whose data file a
 * patch changed without changing its length. Same fuse caveat as above.
 */
int apsp_resign(uint8_t *sfo, size_t sfo_len);

/* ---- Fuse ID ------------------------------------------------------------ */

/*
 * Override the Fuse ID the KIRK engine runs with. Defaults to
 * KIRK_HOST_FUSE_ID (all ones), which is also what apollo-psp falls back to
 * on a console where the kernel read failed.
 *
 * Reaches only the mode-4/6 PARAM.SFO hashes described above; there is no
 * reason to set it except to reproduce a specific console's output.
 */
void     apsp_set_fuse_id(uint64_t fuse_id);
uint64_t apsp_get_fuse_id(void);

#ifdef __cplusplus
}
#endif

#endif /* APOLLO_PSP_SAVEDATA_H */
