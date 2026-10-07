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

/*
 * The PSN account a save is signed to, as PS4 and Vita write it: ACCOUNT_ID,
 * eight raw bytes, little-endian, the same 64-bit number the console reports
 * for the signed-in user.
 *
 * A PS3 writes the same key as SIXTEEN bytes of ASCII hex instead, so the
 * length is checked and a PS3 save comes back ASFO_ERR_FORMAT rather than
 * being read as a number it does not hold -- see apfd_sfo_account_id() for
 * that side. ASFO_ERR_MISSING when the key is absent, which is what a PSP
 * save and a PS4 application's own param.sfo both give.
 *
 * Zero is a real stored value and is returned as zero. It means nobody: a
 * decrypted or shared save usually carries it, and the writer refuses it for
 * the same reason.
 */
#define ASFO_ACCT_BIN_LEN 8

int asfo_account_id(const uint8_t *sfo, size_t sfo_len, uint64_t *out);

/*
 * Assign one, in place. The value is rewritten at its own length, so the file
 * neither grows nor moves and every other offset in it stays good.
 *
 * ASFO_ERR_SPACE for an id of zero: writing it would strip the save of its
 * owner rather than give it one, and apollo-ps4 and apollo-vita both refuse
 * it too.
 */
int asfo_set_account_id(uint8_t *sfo, size_t sfo_len, uint64_t id);

/*
 * The PS4's PARAMS blob -- the console identity a save carries beside its
 * account, and the other half of what apollo-ps4's patch_sfo() writes.
 *
 * Layout, which is what apollo-ps4's sfo_param_params_t declares:
 *
 *   0x00  u32   unknown
 *   0x04  u32   user_id        the console-local user the save belongs to
 *   0x08  [32]  psid_hmac      HMAC-SHA256 of the console's OpenPSID
 *   0x28  u32   unknown
 *   0x2C  [16]  title_id_1
 *   0x3C  [16]  title_id_2
 *   0x4C  u32   unknown
 *   0x50  ...   the rest, 1024 bytes in all
 *
 * THESE ARE PS4 OFFSETS AND NOTHING ELSE'S. Every console writes a key called
 * PARAMS and no two agree on what is in it: a PS3 keeps its account ID at
 * +0x30 and a Vita its title ID at +0x28, both of which land inside the
 * psid_hmac field above. A caller must know it has a PS4 save; there is
 * nothing in the blob itself that says so, so these cannot check it for you.
 *
 * Each returns ASFO_OK, ASFO_ERR_MISSING when the save carries no PARAMS, or
 * ASFO_ERR_FORMAT when what it carries is not a binary value long enough to
 * hold these fields -- the same 0x50 floor apollo-ps4 guards with.
 */
#define ASFO_PS4_PARAMS_MIN   0x50
#define ASFO_PSID_LEN         16
#define ASFO_PSID_HMAC_LEN    32

/*
 * The console-local user number. Zero is "leave it alone" rather than a value
 * to write, which is how apollo-ps4's sfo_patch_user_id() reads it too.
 */
int asfo_ps4_set_user_id(uint8_t *sfo, size_t sfo_len, uint32_t user_id);

/*
 * Bind the save to a console: psid_hmac = HMAC-SHA256(published key, psid),
 * over all ASFO_PSID_LEN bytes of the console's OpenPSID.
 *
 * This is what a PS4 checks for a save that names NO account -- a save with an
 * account is bound by that instead -- so writing an account is both the better
 * fix and the one that makes this moot. It is written anyway, because
 * apollo-ps4 writes it and a save that satisfies both checks travels further.
 */
int asfo_ps4_set_psid_hmac(uint8_t *sfo, size_t sfo_len,
                           const uint8_t *psid /* ASFO_PSID_LEN bytes */);

/*
 * Copy title_id_1 over title_id_2, which is what apollo-ps4 does unconditionally
 * before the rest. A save moved between titles' folders carries the old one in
 * the second slot.
 */
int asfo_ps4_sync_title_id(uint8_t *sfo, size_t sfo_len);

#ifdef __cplusplus
}
#endif

#endif /* APOLLO_SFO_H */
