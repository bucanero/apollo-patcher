/*
 * sfo - reading a PARAM.SFO, whichever console wrote it.
 *
 * Every PlayStation since the PSP describes a save with the same little file:
 * a 0x14-byte header, a table of index entries, a key table of NUL-terminated
 * names and a data table of values. What is IN it differs per console -- a PSP
 * save carries SAVEDATA_PARAMS, a PS4 one carries MAINTITLE and TITLE_ID, a
 * Vita one carries neither and hides its title ID inside a binary blob -- but
 * the container is identical, so the container is parsed in one place.
 *
 * This is that place. It answers "what is under this key" and nothing more;
 * deciding which keys matter is saveinfo.c's job, and writing back into a
 * value is psp_savedata.c's (which is why the lookup hands out an OFFSET and
 * not a pointer: the same call then serves both without a const-cast).
 *
 * Bounds. Every offset in an SFO is read out of the file itself, and one of
 * these reaches you from a browser tab or a stranger's memory card as readily
 * as from your own, so each is checked against the real length before it is
 * followed. A malformed file is rejected, never trusted and never fatal.
 */
#ifndef APOLLO_SFO_H
#define APOLLO_SFO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ASFO_OK          =  0,
    ASFO_ERR_FORMAT  = -1,  /* not an SFO, or one whose tables do not fit  */
    ASFO_ERR_MISSING = -2,  /* parses, but carries no such key             */
    ASFO_ERR_SPACE   = -3   /* the value does not fit the caller's buffer  */
};

/* The `fmt` field of an index entry. Only these three are ever used. */
#define ASFO_FMT_BIN  0x0004u   /* raw bytes: ACCOUNT_ID, PARAMS             */
#define ASFO_FMT_STR  0x0204u   /* UTF-8, NUL-terminated within `used`       */
#define ASFO_FMT_U32  0x0404u   /* little-endian uint32                      */

/* Does this parse as an SFO at all? ASFO_OK, or ASFO_ERR_FORMAT. */
int asfo_valid(const uint8_t *sfo, size_t sfo_len);

/*
 * Locate one key's value. Every out-parameter is optional.
 *
 *   off    absolute offset into the caller's buffer
 *   used   bytes actually written (a string's own NUL is inside this)
 *   max    bytes reserved, which is what a rewrite has to fit in
 *   fmt    one of ASFO_FMT_*
 *
 * ASFO_ERR_MISSING when the file is fine and the key is simply absent, which
 * is the ordinary answer for a key another console's SFO would have had.
 */
int asfo_find(const uint8_t *sfo, size_t sfo_len, const char *key,
              size_t *off, uint32_t *used, uint32_t *max, unsigned *fmt);

/*
 * Copy a string value out, always NUL-terminating `out`.
 *
 * Copied rather than returned in place because a value is only NUL-terminated
 * when whoever wrote it left room: reading one in place walks into the next
 * value when it did not. ASFO_ERR_SPACE if it will not fit, and `out` is left
 * empty rather than half-written.
 *
 * The format is not checked. A binary value comes back as its bytes up to the
 * first NUL, which is what reading a title ID out of a Vita's PARAMS blob
 * wants (see asfo_blob).
 */
int asfo_string(const uint8_t *sfo, size_t sfo_len, const char *key,
                char *out, size_t out_len);

/*
 * Copy `len` bytes from `at` inside a binary value -- the Vita's PARAMS, whose
 * interesting fields sit at fixed offsets inside 1 KiB of otherwise opaque
 * data. ASFO_ERR_SPACE when the value is shorter than at + len.
 */
int asfo_blob(const uint8_t *sfo, size_t sfo_len, const char *key,
              size_t at, uint8_t *out, size_t len);

/* Read a ASFO_FMT_U32 value. ASFO_ERR_FORMAT if it is not four bytes. */
int asfo_u32(const uint8_t *sfo, size_t sfo_len, const char *key, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APOLLO_SFO_H */
