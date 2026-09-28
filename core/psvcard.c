/* See psvcard.h. */
#include <stdlib.h>
#include <string.h>

#include <mbedtls/aes.h>
#include <mbedtls/md.h>

#include "psvcard.h"
#include "shiftjis.h"

/* ---- the container's own offsets ---------------------------------------
 *
 * Read field by field rather than through a struct: the layout is fixed by
 * the format, not by this compiler's padding rules, and the same code has to
 * give the same answer in the wasm build.
 */
#define PSV_SALT_OFF   0x08
#define PSV_SALT_LEN   0x14
#define PSV_SIG_OFF    0x1C
#define PSV_SIG_LEN    0x14
#define PSV_HDRSZ_OFF  0x38
#define PSV_TYPE_OFF   0x3C

#define PS1_NAME_OFF   0x64   /* the save's name, 0x20 bytes                */
#define PS1_SIZE_OFF   0x40   /* and the length of the block that follows   */

/* The PS2 body, which begins at APSVC_HDR_LEN. */
#define PS2_HDR_LEN     40    /* displaySize, four pos/size pairs, count    */
#define PS2_DIR_LEN     56    /* the save folder's own entry                */
#define PS2_FILE_LEN    60    /* per file: dates, size, attr, name, pos     */
#define PS2_COUNT_OFF   36    /* numberOfFiles, within the PS2 header       */
#define PS2_SYSPOS_OFF   4
#define PS2_SYSSIZE_OFF  8
#define PS2_DIRNAME_OFF 24    /* within the folder entry                    */
#define PS2_FSIZE_OFF   16    /* within a file entry                        */
#define PS2_FATTR_OFF   20
#define PS2_FNAME_OFF   24
#define PS2_FPOS_OFF    56

/* icon.sys, which names the save and its three icon files. */
#define SYS_SECONDLINE_OFF 6     /* where the title wraps to line two    */
#define SYS_TITLE_OFF   0xC0
#define SYS_TITLE_LEN   68
#define SYS_ICON1_OFF   0x104
#define SYS_ICON2_OFF   0x144
#define SYS_ICON3_OFF   0x184
#define SYS_NAME_LEN    64
#define SYS_MIN_LEN     (SYS_ICON3_OFF + SYS_NAME_LEN)

#define PS1_TITLE_OFF   4     /* within the PS1 block                       */
#define PS1_TITLE_LEN   64

static const uint8_t PSV_MAGIC[4] = { 0x00, 'V', 'S', 'P' };

/* Key material for the signature. From ps3-psvresigner by @dots_tb, with the
 * CBPS group; the same constants apollo-ps4, apollo-vita and apollo-ps3 use. */
static const uint8_t psv_ps2key[0x10] = {
    0xEA, 0x02, 0xCE, 0xEF, 0x5B, 0xB4, 0xD2, 0x99,
    0x8F, 0x61, 0x19, 0x10, 0xD7, 0x7F, 0x51, 0xC6
};
static const uint8_t psv_ps1key[0x10] = {
    0xAB, 0x5A, 0xBC, 0x9F, 0xC1, 0xF4, 0x9D, 0xE6,
    0xA0, 0x51, 0xDB, 0xAE, 0xFA, 0x51, 0x88, 0x59
};
static const uint8_t psv_iv[0x10] = {
    0xB3, 0x0F, 0xFE, 0xED, 0xB7, 0xDC, 0x5E, 0xB7,
    0x13, 0x3D, 0xA6, 0x0D, 0x1B, 0x6B, 0x2C, 0xDC
};

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* Copy a fixed-width, possibly unterminated name field into a C string. */
static void copy_name(char *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    size_t n = src_len < dst_len - 1 ? src_len : dst_len - 1;
    size_t i;

    for (i = 0; i < n && src[i]; i++)
        dst[i] = (char)src[i];
    dst[i] = '\0';
}

static int ci_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static void xor_iv(uint8_t *buf, const uint8_t *iv)
{
    int i;
    for (i = 0; i < 16; i++)
        buf[i] ^= iv[i];
}

/*
 * Derive the 0x40-byte HMAC key from the file's own salt, then sign the whole
 * buffer into `dest` -- which points INSIDE that buffer, and is zeroed first,
 * because the signature covers itself as zeros. One routine therefore both
 * produces a signature and recomputes one for checking.
 *
 * The two consoles derive the key differently and neither is a plain CBC pass,
 * which is why this is transcribed rather than reduced to something tidier.
 */
static int gen_hash(const uint8_t *input, size_t len,
                    const uint8_t *salt_seed, uint8_t *dest, int type)
{
    mbedtls_aes_context aes;
    uint8_t iv[0x10];
    uint8_t salt[0x40];
    uint8_t work[PSV_SIG_LEN];

    memset(salt, 0, sizeof salt);
    mbedtls_aes_init(&aes);

    if (type == APSVC_TYPE_PS1) {
        memcpy(work, salt_seed, 0x10);

        mbedtls_aes_setkey_dec(&aes, psv_ps1key, 128);
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, work, salt);
        mbedtls_aes_setkey_enc(&aes, psv_ps1key, 128);
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, work, salt + 0x10);

        xor_iv(salt, psv_iv);

        memset(work, 0xFF, sizeof work);
        memcpy(work, salt_seed + 0x10, 4);

        xor_iv(salt + 0x10, work);
    } else if (type == APSVC_TYPE_PS2) {
        memcpy(salt, salt_seed, PSV_SALT_LEN);
        memcpy(iv, psv_iv, sizeof iv);

        mbedtls_aes_setkey_dec(&aes, psv_ps2key, 128);
        mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, sizeof salt, iv, salt, salt);
    } else {
        mbedtls_aes_free(&aes);
        return APSVC_ERR_TYPE;
    }

    memset(salt + PSV_SALT_LEN, 0, sizeof(salt) - PSV_SALT_LEN);
    mbedtls_aes_free(&aes);

    /* The key is exactly one SHA-1 block, so HMAC uses it as it stands and no
     * normalisation happens -- which is why the salt's own length matters. */
    memset(dest, 0, PSV_SIG_LEN);
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
                    salt, sizeof salt, input, len, dest);
    return APSVC_OK;
}

int apsvc_valid(const uint8_t *psv, size_t len)
{
    uint32_t type;

    if (!psv || len < APSVC_PS1_DATA_OFF)
        return APSVC_ERR_FORMAT;
    if (memcmp(psv, PSV_MAGIC, sizeof PSV_MAGIC) != 0)
        return APSVC_ERR_FORMAT;

    type = le32(psv + PSV_TYPE_OFF);
    if (type != APSVC_TYPE_PS1 && type != APSVC_TYPE_PS2)
        return APSVC_ERR_TYPE;

    return APSVC_OK;
}

/*
 * Walk the inner directory.
 *
 * Shared by apsvc_files(), apsvc_find() and apsvc_info() so there is one
 * bounds check rather than three: every count and position below comes out of
 * the file, and a container arrives from wherever a save arrives from.
 */
static int walk(const uint8_t *psv, size_t len,
                apsvc_file_t *out, int max, int *count,
                char *dir_name, size_t dir_len)
{
    uint32_t nfiles, i;
    size_t off;
    int written = 0;

    int rc = apsvc_valid(psv, len);
    if (rc != APSVC_OK) return rc;

    if (le32(psv + PSV_TYPE_OFF) == APSVC_TYPE_PS1) {
        /* One flat block, which IS the save. Reported as a single entry so
         * callers need no separate PS1 path. */
        uint32_t size = le32(psv + PS1_SIZE_OFF);

        if (size > len - APSVC_PS1_DATA_OFF)
            return APSVC_ERR_FORMAT;

        if (dir_name)
            copy_name(dir_name, dir_len, psv + PS1_NAME_OFF, 0x20);
        if (count) *count = 1;
        if (out && max > 0) {
            copy_name(out[0].name, sizeof out[0].name, psv + PS1_NAME_OFF, 0x20);
            out[0].off  = APSVC_PS1_DATA_OFF;
            out[0].size = size;
            out[0].attr = 0;
            written = 1;
        }
        return APSVC_OK;
    }

    /* PS2: header, the folder's own entry, then one record per file. */
    if (len < APSVC_HDR_LEN + PS2_HDR_LEN + PS2_DIR_LEN)
        return APSVC_ERR_FORMAT;

    nfiles = le32(psv + APSVC_HDR_LEN + PS2_COUNT_OFF);

    /* Divided rather than multiplied so a huge count cannot overflow. */
    if (nfiles > (len - APSVC_HDR_LEN - PS2_HDR_LEN - PS2_DIR_LEN) / PS2_FILE_LEN)
        return APSVC_ERR_FORMAT;

    if (dir_name)
        copy_name(dir_name, dir_len,
                  psv + APSVC_HDR_LEN + PS2_HDR_LEN + PS2_DIRNAME_OFF, 32);
    if (count) *count = (int)nfiles;

    off = APSVC_HDR_LEN + PS2_HDR_LEN + PS2_DIR_LEN;
    for (i = 0; i < nfiles; i++, off += PS2_FILE_LEN) {
        uint32_t size = le32(psv + off + PS2_FSIZE_OFF);
        uint32_t pos  = le32(psv + off + PS2_FPOS_OFF);

        /* A file has to lie inside the container. Written as a subtraction so
         * pos + size cannot wrap. */
        if (pos > len || size > len - pos)
            return APSVC_ERR_FORMAT;

        if (out && written < max) {
            copy_name(out[written].name, sizeof out[written].name,
                      psv + off + PS2_FNAME_OFF, 32);
            out[written].off  = pos;
            out[written].size = size;
            out[written].attr = le32(psv + off + PS2_FATTR_OFF);
            written++;
        }
    }
    return APSVC_OK;
}

int apsvc_info(const uint8_t *psv, size_t len, apsvc_info_t *out)
{
    int count = 0, rc;

    if (!out) return APSVC_ERR_FORMAT;
    memset(out, 0, sizeof *out);

    rc = walk(psv, len, NULL, 0, &count, out->dir_name, sizeof out->dir_name);
    if (rc != APSVC_OK) return rc;

    out->type       = (int)le32(psv + PSV_TYPE_OFF);
    out->file_count = count;

    if (out->type == APSVC_TYPE_PS2) {
        uint32_t pos  = le32(psv + APSVC_HDR_LEN + PS2_SYSPOS_OFF);
        uint32_t size = le32(psv + APSVC_HDR_LEN + PS2_SYSSIZE_OFF);

        /* The header points at icon.sys, but a caller is about to read a
         * title out of it, so only hand over a pointer that is really there. */
        if (pos && pos <= len && size <= len - pos && size >= SYS_MIN_LEN) {
            out->sys_off  = pos;
            out->sys_size = size;
        }
    }
    return APSVC_OK;
}

int apsvc_files(const uint8_t *psv, size_t len,
                apsvc_file_t *out, int max, int *count)
{
    return walk(psv, len, out, max, count, NULL, 0);
}

int apsvc_find(const uint8_t *psv, size_t len, const char *name,
               apsvc_file_t *out)
{
    apsvc_file_t *all;
    int count = 0, i, rc;

    if (!name || !out) return APSVC_ERR_FORMAT;

    rc = walk(psv, len, NULL, 0, &count, NULL, 0);
    if (rc != APSVC_OK) return rc;
    if (count <= 0) return APSVC_ERR_MISSING;

    all = calloc((size_t)count, sizeof *all);
    if (!all) return APSVC_ERR_MEMORY;

    rc = walk(psv, len, all, count, NULL, NULL, 0);
    if (rc != APSVC_OK) { free(all); return rc; }

    for (i = 0; i < count; i++) {
        if (ci_equal(all[i].name, name)) {
            *out = all[i];
            free(all);
            return APSVC_OK;
        }
    }
    free(all);
    return APSVC_ERR_MISSING;
}

int apsvc_verify(uint8_t *psv, size_t len)
{
    uint8_t stored[PSV_SIG_LEN], computed[PSV_SIG_LEN];
    int i, signed_at_all = 0;

    if (apsvc_valid(psv, len) != APSVC_OK)
        return APSVC_SIG_UNKNOWN;

    memcpy(stored, psv + PSV_SIG_OFF, sizeof stored);
    for (i = 0; i < (int)sizeof stored; i++)
        if (stored[i]) { signed_at_all = 1; break; }

    if (!signed_at_all)
        return APSVC_SIG_UNSIGNED;

    /* gen_hash() writes into the file's own signature field, which is also
     * what it has to hash as zeros. Let it, then put the original back so the
     * caller's buffer comes out exactly as it went in. */
    if (gen_hash(psv, len, psv + PSV_SALT_OFF, psv + PSV_SIG_OFF,
                 (int)le32(psv + PSV_TYPE_OFF)) != APSVC_OK) {
        memcpy(psv + PSV_SIG_OFF, stored, sizeof stored);
        return APSVC_SIG_UNKNOWN;
    }
    memcpy(computed, psv + PSV_SIG_OFF, sizeof computed);
    memcpy(psv + PSV_SIG_OFF, stored, sizeof stored);

    return memcmp(stored, computed, sizeof stored) == 0
         ? APSVC_SIG_OK : APSVC_SIG_BAD;
}

int apsvc_sign(uint8_t *psv, size_t len)
{
    int rc = apsvc_valid(psv, len);
    if (rc != APSVC_OK) return rc;

    return gen_hash(psv, len, psv + PSV_SALT_OFF, psv + PSV_SIG_OFF,
                    (int)le32(psv + PSV_TYPE_OFF));
}

/*
 * Shift one of the PS2 header's four pos/size pairs to follow an edit.
 *
 * The header points separately at icon.sys and at the three icon files that
 * icon.sys names. They are positions into the same data area, so an edit that
 * changes a file's length moves the ones that sit after it -- and a pair
 * pointing AT the edited file needs its size corrected instead.
 *
 * Matched by position rather than by name: a name match would have to read
 * icon.sys to learn the three icon names, and a container whose icon.sys is
 * itself the file being replaced would then be consulting the copy it is
 * about to overwrite.
 */
static void shift_pair(uint8_t *h, int pos_off, int size_off,
                       uint32_t at, uint32_t old_size, long delta)
{
    uint32_t pos = le32(h + pos_off);

    if (!pos) return;                      /* the pair is unused */
    if (pos == at) {
        put32(h + size_off, (uint32_t)((long)old_size + delta));
    } else if (pos > at) {
        put32(h + pos_off, (uint32_t)((long)pos + delta));
    }
}

int apsvc_replace(const uint8_t *psv, size_t len,
                  const char *name, const uint8_t *data, uint32_t size,
                  uint8_t **out, size_t *out_len)
{
    apsvc_file_t *all;
    int count = 0, i, found = -1, rc;

    if (!name) return APSVC_ERR_FORMAT;

    rc = walk(psv, len, NULL, 0, &count, NULL, 0);
    if (rc != APSVC_OK) return rc;
    if (count <= 0) return APSVC_ERR_MISSING;

    all = calloc((size_t)count, sizeof *all);
    if (!all) return APSVC_ERR_MEMORY;
    rc = walk(psv, len, all, count, NULL, NULL, 0);
    if (rc != APSVC_OK) { free(all); return rc; }

    for (i = 0; i < count; i++)
        if (ci_equal(all[i].name, name)) { found = i; break; }
    free(all);

    if (found < 0) return APSVC_ERR_MISSING;
    return apsvc_replace_at(psv, len, found, data, size, out, out_len);
}

int apsvc_replace_at(const uint8_t *psv, size_t len,
                     int index, const uint8_t *data, uint32_t size,
                     uint8_t **out, size_t *out_len)
{
    apsvc_file_t *all = NULL;
    uint8_t *buf = NULL;
    size_t total, tail_off, tail_len;
    long delta;
    int count = 0, i, found = index, rc, type;
    uint32_t at, old_size;

    if (!out || !out_len || (!data && size))
        return APSVC_ERR_FORMAT;
    *out = NULL;
    *out_len = 0;

    rc = apsvc_valid(psv, len);
    if (rc != APSVC_OK) return rc;
    type = (int)le32(psv + PSV_TYPE_OFF);

    rc = walk(psv, len, NULL, 0, &count, NULL, 0);
    if (rc != APSVC_OK) return rc;
    if (index < 0 || index >= count) return APSVC_ERR_MISSING;

    all = calloc((size_t)count, sizeof *all);
    if (!all) return APSVC_ERR_MEMORY;
    rc = walk(psv, len, all, count, NULL, NULL, 0);
    if (rc != APSVC_OK) { free(all); return rc; }

    at       = (uint32_t)all[found].off;
    old_size = all[found].size;
    delta    = (long)size - (long)old_size;

    if (type == APSVC_TYPE_PS1) {
        /* One block, and the console expects it at its original length: the
         * memory card allocated whole blocks for it. A code that grew or shrank
         * the save would produce something the PS3 will not import, so the
         * size is held and the caller is told. */
        free(all);
        if (delta != 0)
            return APSVC_ERR_SPACE;

        buf = malloc(len);
        if (!buf) return APSVC_ERR_MEMORY;
        memcpy(buf, psv, len);
        memcpy(buf + APSVC_PS1_DATA_OFF, data, size);

        rc = apsvc_sign(buf, len);
        if (rc != APSVC_OK) { free(buf); return rc; }
        *out = buf;
        *out_len = len;
        return APSVC_OK;
    }

    /*
     * PS2. Rebuilt by MOVING what comes after the edited file, not by laying
     * the container out afresh, so that everything the console chose and this
     * code has no opinion on survives untouched.
     *
     * That is not pedantry. Of 2,641 real containers, two leave a gap between
     * files and nine carry a displaySize that is not the sum of their
     * contents -- the PS3 rounds it to how much memory card the save actually
     * occupied. A rebuild that recomputed both would quietly rewrite eleven
     * saves it was only asked to patch, and the only way to notice would be a
     * console refusing one. Copying the bytes either side of the edit and
     * shifting the offsets by the size difference keeps all of it, and makes
     * a same-size replacement produce a byte-identical file by construction.
     */
    tail_off = at + old_size;
    tail_len = len - tail_off;
    total    = (size_t)((long)len + delta);

    buf = malloc(total);
    if (!buf) { free(all); return APSVC_ERR_MEMORY; }

    memcpy(buf, psv, at);                             /* header, directory,
                                                       * earlier files, gaps */
    memcpy(buf + at, data, size);
    memcpy(buf + at + size, psv + tail_off, tail_len);

    /* The edited file's own size, and the position of every file after it. */
    for (i = 0; i < count; i++) {
        size_t rec = APSVC_HDR_LEN + PS2_HDR_LEN + PS2_DIR_LEN
                   + (size_t)i * PS2_FILE_LEN;

        if (i == found)
            put32(buf + rec + PS2_FSIZE_OFF, size);
        else if (all[i].off > at)
            put32(buf + rec + PS2_FPOS_OFF,
                  (uint32_t)((long)all[i].off + delta));
    }

    /* displaySize is how much of the memory card the save took up; the edit
     * changed it by exactly the size difference and by nothing else. */
    put32(buf + APSVC_HDR_LEN,
          (uint32_t)((long)le32(psv + APSVC_HDR_LEN) + delta));

    shift_pair(buf + APSVC_HDR_LEN, PS2_SYSPOS_OFF, PS2_SYSSIZE_OFF,
               at, old_size, delta);
    shift_pair(buf + APSVC_HDR_LEN, 12, 16, at, old_size, delta);
    shift_pair(buf + APSVC_HDR_LEN, 20, 24, at, old_size, delta);
    shift_pair(buf + APSVC_HDR_LEN, 28, 32, at, old_size, delta);

    free(all);

    rc = apsvc_sign(buf, total);
    if (rc != APSVC_OK) { free(buf); return rc; }

    *out = buf;
    *out_len = total;
    return APSVC_OK;
}

void apsvc_free(uint8_t *buf)
{
    free(buf);
}

const char *apsvc_strerror(int err)
{
    switch (err) {
    case APSVC_OK:          return "ok";
    case APSVC_ERR_FORMAT:  return "not a .PSV container, or a damaged one";
    case APSVC_ERR_TYPE:    return "a .PSV holding a save type this does not know";
    case APSVC_ERR_MISSING: return "the container holds no file by that name";
    case APSVC_ERR_SPACE:   return "a PS1 save cannot change size";
    case APSVC_ERR_MEMORY:  return "out of memory";
    default:                return "unknown error";
    }
}

int apsvc_title_id(const char *dir_name, char *out, size_t out_len)
{
    const char *p = dir_name;
    size_t i = 0;
    int digits = 0;

    if (!dir_name || !out || out_len < 10)
        return APSVC_ERR_FORMAT;
    out[0] = '\0';

    /* B, then the region letter. */
    if (p[0] != 'B') return APSVC_ERR_FORMAT;
    p++;
    if (!((*p >= 'A' && *p <= 'Z'))) return APSVC_ERR_FORMAT;
    p++;

    /* Four letters of disc code, and the P some discs carry after it. */
    for (i = 0; i < 4; i++) {
        if (!(p[i] >= 'A' && p[i] <= 'Z')) return APSVC_ERR_FORMAT;
        out[i] = p[i];
    }
    p += 4;
    if (*p == 'P') p++;
    if (*p == '-') p++;

    /* Five digits. Anything after them is the slot's own name, which several
     * saves run straight on ("BASLUS-01360FF4"), so the scan stops here. */
    for (digits = 0; digits < 5; digits++) {
        if (!(p[digits] >= '0' && p[digits] <= '9')) return APSVC_ERR_FORMAT;
        out[4 + digits] = p[digits];
    }
    out[9] = '\0';
    return APSVC_OK;
}

/* Trailing spaces are padding, not part of the name: the console right-fills
 * a fixed field, and a list shows the difference. */
static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                 s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

int apsvc_title(const uint8_t *psv, size_t len, char *out, size_t out_len)
{
    apsvc_info_t info;
    int rc;

    if (!out || out_len == 0) return APSVC_ERR_FORMAT;
    out[0] = '\0';

    rc = apsvc_info(psv, len, &info);
    if (rc != APSVC_OK) return rc;

    if (info.type == APSVC_TYPE_PS1) {
        uint32_t size = le32(psv + PS1_SIZE_OFF);
        if (size < PS1_TITLE_OFF + PS1_TITLE_LEN)
            return APSVC_ERR_MISSING;
        asjis_to_utf8(psv + APSVC_PS1_DATA_OFF + PS1_TITLE_OFF,
                      PS1_TITLE_LEN, out, out_len);
    } else {
        const uint8_t *t;
        size_t n, split;

        if (!info.sys_off) return APSVC_ERR_MISSING;
        t = psv + info.sys_off + SYS_TITLE_OFF;

        for (n = 0; n < SYS_TITLE_LEN && t[n]; n++)
            ;

        /*
         * icon.sys says where the console broke the name across the two lines
         * it had room for, and the break is not otherwise marked -- the title
         * is one run of bytes, so "Devil May Cry" and "SaveData" arrive as
         * "Devil May CrySaveData". 1,895 of the 2,641 real titles wrap.
         *
         * Joined with a space rather than a newline: this ends up in a list,
         * in a window title and in a log line, and a name with a newline in it
         * breaks all three.
         */
        split = le16(psv + info.sys_off + SYS_SECONDLINE_OFF);
        if (split > 0 && split < n) {
            size_t w = asjis_to_utf8(t, split, out, out_len);
            if (w + 1 < out_len) {
                out[w++] = ' ';
                asjis_to_utf8(t + split, n - split, out + w, out_len - w);
            }
        } else {
            asjis_to_utf8(t, n, out, out_len);
        }
    }

    rtrim(out);
    return out[0] ? APSVC_OK : APSVC_ERR_MISSING;
}
