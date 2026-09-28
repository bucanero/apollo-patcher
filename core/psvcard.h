/*
 * psvcard - the .PSV container, which is how a PS1 or PS2 save reaches a
 * computer at all.
 *
 * Every other console here writes saves a filesystem can hold: a folder, some
 * files, a PARAM.SFO beside them. The PS1 and PS2 did not. A save lived in
 * blocks on a memory card, and there was no file to copy off it -- which is
 * why this repo's scanner, which looks for a folder containing a PARAM.SFO,
 * has never seen one.
 *
 * The PS3 solved that when it started hosting PS1 and PS2 titles: it exports a
 * memory-card save as a single .PSV file, signed, carrying the save's own
 * directory inside it. That is the format this file reads and writes. A .PSV
 * is therefore not a save -- it is a container HOLDING one, and the difference
 * matters, because the codes in a .savepatch address the files INSIDE it.
 *
 * NAMING. The letters "PSV" mean two unrelated things, which is worth being
 * blunt about because the ambiguity is the kind that compiles:
 *
 *   the PS VITA       the console. ASAVE_PSVITA, and the patch database's own
 *                     "PSV" platform tag, which is the string on disk and so
 *                     cannot be renamed.
 *   a .PSV FILE       this container, which holds a PS1 or PS2 save and has
 *                     nothing to do with the Vita.
 *
 * So the console's enumerator is spelled out in full -- ASAVE_PSVITA, never
 * ASAVE_PSV -- and everything about the container is `apsvc_`/"psvcard" and
 * never plain "psv". Only two things keep the bare spelling, and both name the
 * file format itself rather than either concept: the ".psv" extension, and
 * asave_platform_name(), which still answers "PSV" for the Vita because that
 * is the tag the patch database is keyed by.
 *
 * LAYOUT. A 0x40 header, then a per-console body:
 *
 *   0x00  magic       00 'V' 'S' 'P'
 *   0x08  salt[0x14]  the seed the signing key is derived from
 *   0x1C  sig[0x14]   HMAC-SHA1 over the whole file, this field zeroed
 *   0x38  headerSize  0x2C for a PS2 save, 0x14 for a PS1 one
 *   0x3C  saveType    2 for PS2, 1 for PS1
 *
 *   PS1   one flat 8 KiB or 16 KiB block of memory-card data at 0x84, with
 *         the save's name at 0x64. That block IS the save, icon included.
 *   PS2   a directory: a 40-byte header, the folder's own entry, then one
 *         60-byte record per file (name, size, absolute position). File data
 *         follows contiguously, no padding and no alignment.
 *
 * Checked against every .PSV in the apollo-saves database -- all 2,647 of
 * them, 2,641 PS2 and 6 PS1. Every one parses, every inner directory is
 * consistent, and every signature verifies. The PS1 fields above are constant
 * across all six.
 *
 * SIGNATURES. The key is derived from the file's own salt by AES-128, using a
 * different schedule per console, and the signature is HMAC-SHA1 over the
 * whole file with the signature field zeroed. So one routine both produces and
 * checks it, and re-signing after an edit is the same call.
 *
 * A bad signature is worth REPORTING and not worth refusing over: tools wrote
 * .PSV files before the algorithm was understood, and those saves are
 * otherwise perfectly good. The caller decides.
 *
 * Bounds. Every offset and count in the directory is read out of the file,
 * which arrives from a stranger's memory card as readily as your own, so each
 * is checked against the real length before it is followed. A malformed
 * container is rejected, never trusted and never fatal.
 */
#ifndef APOLLO_PSVCARD_H
#define APOLLO_PSVCARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    APSVC_OK          =  0,
    APSVC_ERR_FORMAT  = -1,  /* not a .PSV, or one whose tables do not fit */
    APSVC_ERR_TYPE    = -2,  /* a .PSV of a save type this does not know   */
    APSVC_ERR_MISSING = -3,  /* no inner file by that name                 */
    APSVC_ERR_SPACE   = -4,  /* the caller's buffer is too small           */
    APSVC_ERR_MEMORY  = -5   /* out of memory building a new container     */
};

/* saveType, as the header stores it. */
#define APSVC_TYPE_PS1  1
#define APSVC_TYPE_PS2  2

/* Signature verdicts, separate from the error codes: three of the four are
 * not failures, and a caller that treats them as such would refuse saves that
 * import perfectly well. */
#define APSVC_SIG_OK        1   /* matches the contents                     */
#define APSVC_SIG_BAD       0   /* present, and does not match              */
#define APSVC_SIG_UNSIGNED (-1) /* all zeros -- never signed                */
#define APSVC_SIG_UNKNOWN  (-2) /* not a .PSV, or a type we cannot check    */

#define APSVC_HDR_LEN       0x40
#define APSVC_PS1_DATA_OFF  0x84   /* where the PS1 block starts            */
#define APSVC_NAME_LEN      33     /* 32 stored bytes and a terminator      */

/*
 * One file inside the container.
 *
 * `off` is into the caller's own buffer, so a reader needs no copy; a PS1
 * container reports exactly one entry, its whole block, which keeps every
 * caller on one code path.
 */
typedef struct {
    char     name[APSVC_NAME_LEN];
    size_t   off;
    uint32_t size;
    uint32_t attr;
} apsvc_file_t;

/*
 * What a container says about itself.
 *
 * `dir_name` is the save's own directory as the memory card held it --
 * "BASLUS-20216" -- which is both what the title ID is derived from and what
 * a .savepatch names its target files under.
 */
typedef struct {
    int    type;                     /* APSVC_TYPE_PS1 or APSVC_TYPE_PS2    */
    char   dir_name[APSVC_NAME_LEN];
    int    file_count;
    size_t sys_off;                  /* icon.sys, PS2 only; 0 when absent   */
    uint32_t sys_size;
} apsvc_info_t;

/* Does this parse as a .PSV at all? APSVC_OK, or an error. Cheap: header
 * only, no directory walk. */
int apsvc_valid(const uint8_t *psv, size_t len);

/* Read the header and the inner directory. */
int apsvc_info(const uint8_t *psv, size_t len, apsvc_info_t *out);

/*
 * List the inner files. `max` is how many `out` holds; the real count is
 * always written to `count` so a caller can size a second call. Pass out=NULL
 * to ask for the count alone.
 */
int apsvc_files(const uint8_t *psv, size_t len,
                apsvc_file_t *out, int max, int *count);

/* Find one inner file by name, case-insensitively -- a .savepatch and a
 * memory card do not always agree on case. APSVC_ERR_MISSING when absent. */
int apsvc_find(const uint8_t *psv, size_t len, const char *name,
               apsvc_file_t *out);

/*
 * Check the signature. The buffer is written to and restored, so it must be
 * writable; its contents are unchanged on return. Returns an APSVC_SIG_*.
 */
int apsvc_verify(uint8_t *psv, size_t len);

/* Re-sign in place, after an edit. APSVC_OK, or APSVC_ERR_TYPE for a save
 * type whose key schedule is not known. */
int apsvc_sign(uint8_t *psv, size_t len);

/*
 * Replace one inner file and rebuild the container around it.
 *
 * The replacement may be a different size: every position after it moves, and
 * the PS2 header's own totals are recomputed, which is why this returns a new
 * buffer instead of editing in place. The result is signed and ready to write.
 *
 * Everything the console chose and this code has no opinion on is preserved:
 * two real containers leave a gap between files and nine carry a displaySize
 * that is not the sum of their contents, so the bytes either side of the edit
 * are copied and only the offsets after it are shifted. A replacement of the
 * same size therefore reproduces the input byte for byte.
 *
 * `*out` is malloc'd and belongs to the caller; free it with apsvc_free().
 * APSVC_ERR_MISSING when the container holds no such file.
 *
 * The name form takes the FIRST file of that name. Four real containers hold
 * two files called settings.dat, so a caller that has a particular one in mind
 * -- which anything working from an apsvc_files() listing does -- should use
 * the index form instead.
 */
int apsvc_replace(const uint8_t *psv, size_t len,
                  const char *name, const uint8_t *data, uint32_t size,
                  uint8_t **out, size_t *out_len);

/* The same, naming the file by its index in apsvc_files() order. */
int apsvc_replace_at(const uint8_t *psv, size_t len,
                     int index, const uint8_t *data, uint32_t size,
                     uint8_t **out, size_t *out_len);

void apsvc_free(uint8_t *buf);

/* A human-readable reason, for the log. Never NULL. */
const char *apsvc_strerror(int err);

/*
 * The title ID a container belongs to: "SLUS20216", derived from the save's
 * own directory name.
 *
 * The name is B, a region letter, the disc's code and its number --
 * "BASLUS-20216" -- optionally with a P after the code and with or without the
 * dash, and often with the slot's own name run on after it. Everything from
 * the code onward is the title ID.
 *
 * This is AUTHORITATIVE, and it can disagree with the folder a save was filed
 * under. Of 2,647 real containers, 2,646 agree; the one that does not is a
 * Final Fantasy Chronicles save, a two-in-one disc whose halves carry
 * different IDs (SLUS-01360 and SLUS-01363). The container is right.
 *
 * `out` must hold at least 10 bytes. APSVC_ERR_FORMAT when the name is not of
 * that shape.
 */
int apsvc_title_id(const char *dir_name, char *out, size_t out_len);

/*
 * The save's own name, as the console showed it in its save list, converted
 * from Shift-JIS to UTF-8.
 *
 *   PS2   the title in icon.sys, 68 bytes at 0xC0
 *   PS1   the title in the block header, 64 bytes at offset 4
 *
 * A PS2 title is stored as one run of bytes with icon.sys saying where the
 * console wrapped it onto its second line, so "Devil May Cry" and "SaveData"
 * arrive as "Devil May CrySaveData"; 1,895 of them wrap, and the break comes
 * back as a space.
 *
 * Every one of the 2,641 PS2 containers carries one, so a PS2 save names
 * itself and needs no catalogue -- unlike a Vita save, which names nothing.
 * The text is the SLOT ("Devil May Cry Slot 1"), not the game, so a caller
 * still wants the catalogue for the game's own name.
 *
 * APSVC_ERR_MISSING when there is no title to read.
 */
int apsvc_title(const uint8_t *psv, size_t len, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* APOLLO_PSVCARD_H */
